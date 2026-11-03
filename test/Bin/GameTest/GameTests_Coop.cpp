#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Testing/Game/GameTest.h"

#include "Engine/Engine.h"
#include "Engine/EngineGlobals.h"
#include "Engine/MapEnums.h"
#include "Engine/Party.h"
#include "Engine/RemoteParty.h"
#include "Engine/SaveLoad.h"
#include "Engine/Coop/CoopClient.h"
#include "Engine/Coop/CoopServer.h"
#include "Engine/Coop/CoopSession.h"
#include "Engine/Graphics/Renderer/Renderer.h"
#include "Engine/Objects/Actor.h"
#include "Engine/Objects/Character.h"
#include "Engine/Objects/Chest.h"
#include "Engine/Objects/SpriteObject.h"

#include "GUI/GUIButton.h"
#include "GUI/GUIWindow.h"
#include "GUI/UI/UIChest.h"

#include "Library/Platform/Application/PlatformApplication.h"
#include "Library/Platform/Interface/PlatformEvents.h"

#include "Utility/ScopeGuard.h"

#include "GameTestCommon.h"

static bool hasCoopStandIn() {
    return std::ranges::any_of(pActors, [](const Actor &actor) { return actor.attributes & ACTOR_COOP_STAND_IN; });
}

static void typeText(EngineController &game, std::string_view text) {
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            game.pressAndReleaseKey(static_cast<PlatformKey>(std::to_underlying(PlatformKey::KEY_DIGIT_0) + (c - '0')));
        } else if (c >= 'a' && c <= 'z') {
            game.pressAndReleaseKey(static_cast<PlatformKey>(std::to_underlying(PlatformKey::KEY_A) + (c - 'a')));
        } else {
            assert(c == '.');
            game.pressAndReleaseKey(PlatformKey::KEY_PERIOD);
        }
    }
}

static void eraseText(EngineController &game, int count) {
    for (int i = 0; i < count; i++)
        game.pressAndReleaseKey(PlatformKey::KEY_BACKSPACE);
}

static int groundItemCount(ItemId item) {
    return std::ranges::count_if(pSpriteObjects, [&](const SpriteObject &sprite) {
        return sprite.uObjectDescID != 0 && sprite.containing_item.itemId == item;
    });
}

static bool hasSprite(SpriteId sprite) {
    return std::ranges::any_of(pSpriteObjects, [&](const SpriteObject &object) { return object.uObjectDescID != 0 && object.spriteId == sprite; });
}

static bool hasGuiButton(std::string_view id) {
    return std::ranges::any_of(lWindowList, [&](GUIWindow *window) {
        return std::ranges::any_of(window->vButtons, [&](GUIButton *button) { return button->id == id; });
    });
}

static int remotePartyBillboardCount(const Vec3f &pos) {
    int result = 0;
    for (int i = 0; i < uNumBillboardsToDraw; i++)
        if (pBillboardRenderList[i].worldPos == pos && pBillboardRenderList[i].object_pid == Pid())
            result++;
    return result;
}

static void checkRemotePartiesDrawn(EngineController &game, TestController &test, CommonTapeRecorder &tapes,
                                    MapId map, Vec3f partyPos, int partyYawDegrees, Vec3f forward) {
    // A remote party in front of the camera is drawn, and one behind it is not.
    Vec3f frontPos = partyPos + forward;
    Vec3f behindPos = partyPos - forward;
    auto frontTape = tapes.custom([&] { return remotePartyBillboardCount(frontPos); });
    auto behindTape = tapes.custom([&] { return remotePartyBillboardCount(behindPos); });
    game.startNewGame();
    game.teleportTo(map, partyPos, partyYawDegrees);
    test.startTaping();
    engine->_remoteParties = {RemoteParty{frontPos, 0, false}, RemoteParty{behindPos, 0, true}};
    game.tick();
    EXPECT_EQ(frontTape, tape(1));
    EXPECT_EQ(behindTape, tape(0));
}

GAME_TEST(Coop, RemotePartyDrawnOutdoors) {
    // Other players' parties are drawn outdoors.
    checkRemotePartiesDrawn(game, test, tapes, MAP_EMERALD_ISLAND, Vec3f(12552, 800, 193), 90, Vec3f(0, 600, 0));
}

GAME_TEST(Coop, RemotePartyDrawnIndoors) {
    // Other players' parties are drawn indoors.
    checkRemotePartiesDrawn(game, test, tapes, MAP_HIDDEN_TOMB, Vec3f(-111, -25, 1), 180, Vec3f(-40, 0, 0));
}

GAME_TEST(Coop, HostAndClientSeeEachOther) {
    // A client joining a hosting game gets the world, sees the host's party, and its own party shows up on the host's
    // map only while it's on the same map.
    Vec3f guestPos;
    auto guestTape = tapes.custom([&] { return remotePartyBillboardCount(guestPos); });
    game.startNewGame();
    game.teleportTo(MAP_EMERALD_ISLAND, Vec3f(12552, 800, 193), 90);
    guestPos = pParty->pos + Vec3f(0, 600, 0);
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    test.startTaping();

    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
    EXPECT_GT(guest.takeSaveGame().size(), 0);

    CoopPartyPose pose;
    pose.map = MAP_EMERALD_ISLAND;
    pose.pos = guestPos;
    guest.sendLocalPose(pose);
    for (int i = 0; i < 100 && engine->_remoteParties.empty(); i++) { // The guest sends what it queued only when updated.
        guest.update();
        game.tick();
    }
    guest.update();
    ASSERT_TRUE(guest.remoteParties().contains(CoopServer::HOST_PLAYER_ID));
    EXPECT_EQ(guest.remoteParties().at(CoopServer::HOST_PLAYER_ID).pos, pParty->pos);
    EXPECT_EQ(guest.remoteParties().at(CoopServer::HOST_PLAYER_ID).map, MAP_EMERALD_ISLAND);

    pose.map = MAP_HARMONDALE;
    guest.sendLocalPose(pose);
    for (int i = 0; i < 100 && !engine->_remoteParties.empty(); i++) {
        guest.update();
        game.tick();
    }
    EXPECT_EQ(guestTape, tape(0, 1, 0));
}

GAME_TEST(Coop, ClientJoinsFromMainMenu) {
    // A client with a pending join leaves the main menu for the host's world, and the host sees its party there.
    auto mapTape = tapes.map();
    auto messageBoxTape = tapes.messageBoxes();
    game.startNewGame();
    game.teleportTo(MAP_HARMONDALE, Vec3f(-17331, 12547, 465), 90);
    Vec3f hostPos = pParty->pos;
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.teleportTo(MAP_EMERALD_ISLAND, Vec3f(12552, 800, 193), 90);
    game.goToMainMenu();
    test.startTaping();

    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && engine->_currentLoadedMapId != MAP_HARMONDALE; i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    auto serverSeesGuest = [&] { return !server.remoteParties().empty() && server.remoteParties().begin()->second.pos == pParty->pos; };
    for (int i = 0; i < 100 && !serverSeesGuest(); i++) { // The party settles on the ground in its first frames after loading.
        server.update();
        game.tick();
    }

    EXPECT_EQ(mapTape, tape(MAP_EMERALD_ISLAND, MAP_HARMONDALE));
    EXPECT_CONTAINS(messageBoxTape.flatten(), "Joining co-op game...");
    EXPECT_EQ(pParty->pos.xy(), hostPos.xy()); // Height settles by a unit on loading.
    ASSERT_EQ(server.remoteParties().size(), 1);
    EXPECT_EQ(server.remoteParties().begin()->second.map, MAP_HARMONDALE);
    EXPECT_EQ(server.remoteParties().begin()->second.pos, pParty->pos);
}

GAME_TEST(Coop, ClientJoinsWithOwnParty) {
    // A client that brings its own save plays its own characters in the host's world, with its buffs keeping their
    // remaining time on the host's clock.
    auto mapTape = tapes.map();
    game.startNewGame();
    pParty->pCharacters[0].name = "Guestina";
    pParty->SetGold(1234);
    pParty->pCharacters[0].pCharacterBuffs[CHARACTER_BUFF_BLESS].expireTime = pParty->GetPlayingTime() + Duration::fromHours(1);
    Blob guestSave = createSaveData(false, "Guest").second;

    game.restAndHeal(); // Puts the host's clock hours ahead of the guest's.
    game.teleportTo(MAP_HARMONDALE, Vec3f(-17331, 12547, 465), 90);
    pParty->SetGold(5);
    Time hostTime = pParty->GetPlayingTime();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.teleportTo(MAP_EMERALD_ISLAND, Vec3f(12552, 800, 193), 90);
    game.goToMainMenu();
    test.startTaping();

    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", std::move(guestSave));
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && engine->_currentLoadedMapId != MAP_HARMONDALE; i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }

    EXPECT_EQ(mapTape, tape(MAP_EMERALD_ISLAND, MAP_HARMONDALE));
    EXPECT_EQ(pParty->pCharacters[0].name, "Guestina");
    EXPECT_EQ(pParty->GetGold(), 1234);
    EXPECT_EQ(pParty->pCharacters[0].pCharacterBuffs[CHARACTER_BUFF_BLESS].expireTime, hostTime + Duration::fromHours(1));
}

GAME_TEST(Coop, LeavingTheMapNeedsEveryonesAgreement) {
    // Leaving a map in co-op asks the other players first. The host leaves once the guest agrees, and stays with a
    // message saying who refused when the guest doesn't.
    for (bool accept : {false, true}) {
        SCOPED_TRACE(fmt::format("accept={}", accept));
        test.prepareForNextTest();
        auto mapTape = tapes.map();
        auto statusTape = tapes.statusBar();
        game.startNewGame();
        game.teleportTo(MAP_HIDDEN_TOMB, Vec3f(-111, -25, 1), 0); // Just inside the entrance, facing the exit.
        coopSession = CoopSession::host(0, 4);
        MM_AT_SCOPE_EXIT(coopSession.reset());
        CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
        for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
            game.tick();
            guest.update();
        }
        ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
        test.startTaping();

        game.pressKey(PlatformKey::KEY_UP);
        game.tick(10);
        game.releaseKey(PlatformKey::KEY_UP);
        game.pressAndReleaseKey(PlatformKey::KEY_SPACE);
        game.tick();
        game.pressGuiButton("Transition_Yes");
        for (int i = 0; i < 100 && !guest.pendingVote(); i++) { // The guest sends what it queued only when updated.
            game.tick();
            guest.update();
        }
        ASSERT_TRUE(guest.pendingVote());
        EXPECT_EQ(guest.pendingVote()->destination.map(), MAP_ERATHIA);
        EXPECT_EQ(engine->_currentLoadedMapId, MAP_HIDDEN_TOMB);

        guest.answerVote(accept);
        std::optional<CoopVoteDecision> decision;
        for (int i = 0; i < 100 && !decision; i++) {
            guest.update();
            game.tick();
            decision = guest.takeVoteDecision();
        }
        ASSERT_TRUE(decision);
        if (accept) {
            game.skipLoadingScreen();
            EXPECT_EQ(mapTape, tape(MAP_HIDDEN_TOMB, MAP_ERATHIA));
        } else {
            game.tick();
            EXPECT_EQ(mapTape, tape(MAP_HIDDEN_TOMB));
            EXPECT_CONTAINS(statusTape, "Guest doesn't want to travel to Erathia");
        }
        EXPECT_CONTAINS(statusTape, "Asked the others to travel to Erathia");
    }
}

GAME_TEST(Coop, GuestAnswersTravelPrompt) {
    // A guest is asked whether to follow the host's trip. Agreeing takes the guest along, refusing keeps everyone put
    // and tells the host who refused.
    for (bool accept : {false, true}) {
        SCOPED_TRACE(fmt::format("accept={}", accept));
        test.prepareForNextTest();
        auto mapTape = tapes.map();
        auto screenTape = tapes.screen();
        game.startNewGame();
        Blob hostSave = createSaveData(false, "Host").second;
        CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
        game.goToMainMenu();
        coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
        MM_AT_SCOPE_EXIT(coopSession.reset());
        for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
            server.update();
            game.tick();
        }
        ASSERT_EQ(server.remoteParties().size(), 1);
        test.startTaping();

        server.requestVote(COOP_VOTE_TRAVEL, MapDestination(MAP_HARMONDALE, MAP_START_POINT_PARTY));
        for (int i = 0; i < 100 && current_screen_type != SCREEN_CHANGE_LOCATION; i++) {
            server.update();
            game.tick();
        }
        game.pressGuiButton(accept ? "Transition_Yes" : "Transition_No");
        std::optional<CoopVoteDecision> hostDecision;
        for (int i = 0; i < 100 && !hostDecision; i++) {
            game.tick();
            server.update();
            hostDecision = server.takeVoteDecision();
        }
        if (accept)
            game.skipLoadingScreen();

        ASSERT_TRUE(hostDecision);
        EXPECT_EQ(hostDecision->approved, accept);
        EXPECT_CONTAINS(screenTape, SCREEN_CHANGE_LOCATION);
        EXPECT_EQ(screenTape.back(), SCREEN_GAME);
        EXPECT_EQ(mapTape, accept ? tape(MAP_EMERALD_ISLAND, MAP_HARMONDALE) : tape(MAP_EMERALD_ISLAND));
        if (!accept)
            EXPECT_EQ(hostDecision->declinedBy, "Guest");
    }
}

GAME_TEST(Coop, TravelPromptClosesWhenSomeoneElseRefuses) {
    // The travel prompt goes away by itself when another player refuses first, and the status line says who it was.
    auto screenTape = tapes.screen();
    auto statusTape = tapes.statusBar();
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient other("127.0.0.1", server.port(), "Other");
    for (int i = 0; i < 100 && server.remoteParties().size() < 2; i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        other.update();
        other.sendLocalPose(CoopPartyPose());
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 2);
    test.startTaping();

    server.requestVote(COOP_VOTE_TRAVEL, MapDestination(MAP_HARMONDALE, MAP_START_POINT_PARTY));
    for (int i = 0; i < 100 && (current_screen_type != SCREEN_CHANGE_LOCATION || !other.pendingVote()); i++) {
        server.update();
        other.update();
        game.tick();
    }
    ASSERT_EQ(current_screen_type, SCREEN_CHANGE_LOCATION);
    ASSERT_TRUE(other.pendingVote());

    other.answerVote(false);
    for (int i = 0; i < 100 && current_screen_type == SCREEN_CHANGE_LOCATION; i++) {
        other.update();
        server.update();
        game.tick();
    }
    EXPECT_EQ(screenTape.back(), SCREEN_GAME);
    EXPECT_FALSE(coopSession->pendingVote());
    EXPECT_CONTAINS(statusTape, "Other doesn't want to travel to Harmondale");
}

GAME_TEST(Coop, HostKeepsRunningWithoutFocus) {
    // A hosting game whose window lost focus stopped answering the network, so a guest started on the same machine,
    // which takes the focus, couldn't join.
    game.startNewGame();
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    auto deactivate = std::make_unique<PlatformWindowEvent>();
    deactivate->type = EVENT_WINDOW_DEACTIVATE;
    deactivate->window = ::application->window();
    game.postEvent(std::move(deactivate));
    game.tick();

    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    EXPECT_EQ(guest.state(), COOP_CLIENT_JOINED);
}

GAME_TEST(Coop, GuestWaitsForHostToStartGame) {
    // A guest that connects before the host is in a game waits, and joins once the host has a game going.
    auto messageBoxTape = tapes.messageBoxes();
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    bool hostInGame = false;
    CoopServer server(0, 4, [&] { return hostInGame ? Blob::share(hostSave) : Blob(); });
    game.goToMainMenu();
    test.startTaping();

    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && !messageBoxTape.flatten().contains("Waiting for the host to start a game..."); i++) {
        server.update();
        game.tick();
    }
    hostInGame = true;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10); // Retries go by wall clock time.
    while (server.remoteParties().empty() && std::chrono::steady_clock::now() < deadline) {
        server.update();
        game.tick();
    }

    EXPECT_CONTAINS(messageBoxTape.flatten(), "Waiting for the host to start a game...");
    EXPECT_EQ(server.remoteParties().size(), 1);
}

GAME_TEST(Coop, HostSendsMonsters) {
    // A guest gets the monsters on the host's map, where the host has them.
    game.startNewGame();
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);

    std::map<int, CoopActorState> latest;
    bool gotFullUpdate = false;
    for (int i = 0; i < 100 && !(gotFullUpdate && std::ssize(latest) == std::ssize(pActors)); i++) { // Encounters spawn more monsters in the first frames.
        game.tick();
        guest.update();
        for (const CoopActorStates &states : guest.takeActorStates()) {
            EXPECT_EQ(states.map, MAP_EMERALD_ISLAND);
            gotFullUpdate |= std::ssize(states.actors) == states.actorCount && states.actorCount == std::ssize(pActors);
            for (const CoopActorState &state : states.actors)
                latest[state.index] = state;
        }
    }
    ASSERT_TRUE(gotFullUpdate);
    ASSERT_EQ(std::ssize(latest), std::ssize(pActors));
    for (const auto &[index, state] : latest) {
        EXPECT_EQ(state.monster, pActors[index].monsterInfo.id);
        EXPECT_EQ(state.pos, pActors[index].pos);
        EXPECT_EQ(state.aiState, pActors[index].aiState);
        EXPECT_EQ(state.hp, pActors[index].hp);
    }
}

GAME_TEST(Coop, GuestMonstersFollowHost) {
    // A guest's monsters don't act on their own, they stand where the host's game puts them and die when it says so.
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    ASSERT_GT(pActors.size(), 0);
    auto posTape = actorTapes.custom(0, [](const Actor &actor) { return actor.pos; });
    auto stateTape = actorTapes.aiState(0);
    auto movedTape = tapes.custom([positions = std::vector<Vec3f>()]() mutable {
        int moved = 0;
        for (int i = 0; i < std::ssize(pActors); i++)
            moved += i < std::ssize(positions) && positions[i] != pActors[i].pos;
        positions.clear();
        for (const Actor &actor : pActors)
            positions.push_back(actor.pos);
        return moved;
    });
    test.startTaping();

    game.tick(20); // Long enough for the monsters on the beach to start wandering if they were left to themselves.
    Vec3f startPos = pActors[0].pos;
    CoopActorStates states;
    states.map = MAP_EMERALD_ISLAND;
    states.actorCount = pActors.size();
    CoopActorState dead;
    dead.index = 0;
    dead.monster = pActors[0].monsterInfo.id;
    dead.pos = startPos + Vec3f(100, 0, 0);
    dead.yaw = pActors[0].yawAngle;
    dead.sectorId = pActors[0].sectorId;
    dead.aiState = Dead;
    dead.animation = ANIM_Dead;
    states.actors = {dead};
    server.sendActorStates(states);
    for (int i = 0; i < 100 && pActors[0].aiState != Dead; i++) {
        server.update();
        game.tick();
    }

    EXPECT_EQ(posTape, tape(startPos, startPos + Vec3f(100, 0, 0)));
    EXPECT_EQ(stateTape.back(), Dead);
    EXPECT_EQ(movedTape.filter([](int moved) { return moved > 1; }).size(), 0); // Only the monster the host moved ever moves.
}

GAME_TEST(Coop, GuestMatchesHostMonsterList) {
    // Encounters spawn at random, so the guest's monster list can differ from the host's. The guest takes over the
    // host's monsters, their number and their kinds.
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    int guestCount = pActors.size();
    ASSERT_GE(guestCount, 2);
    ASSERT_NE(pActors[1].monsterInfo.id, MONSTER_GOBLIN_A);
    test.startTaping();

    CoopActorState goblin;
    goblin.monster = MONSTER_GOBLIN_A;
    goblin.pos = pActors[1].pos;
    goblin.hp = 10;
    CoopActorStates more;
    more.map = MAP_EMERALD_ISLAND;
    more.actorCount = guestCount + 1;
    more.actors = {goblin, goblin};
    more.actors[0].index = 1;
    more.actors[1].index = guestCount;
    server.sendActorStates(more);
    for (int i = 0; i < 100 && std::ssize(pActors) == guestCount; i++) {
        server.update();
        game.tick();
    }
    ASSERT_EQ(std::ssize(pActors), guestCount + 1);
    EXPECT_EQ(pActors[1].monsterInfo.id, MONSTER_GOBLIN_A);
    EXPECT_EQ(pActors[guestCount].monsterInfo.id, MONSTER_GOBLIN_A);
    EXPECT_EQ(pActors[guestCount].pos, goblin.pos);
    EXPECT_EQ(pActors[guestCount].aiState, Standing);

    CoopActorStates fewer;
    fewer.map = MAP_EMERALD_ISLAND;
    fewer.actorCount = 1;
    server.sendActorStates(fewer);
    for (int i = 0; i < 100 && pActors[1].aiState != Removed; i++) {
        server.update();
        game.tick();
    }
    EXPECT_NE(pActors[0].aiState, Removed);
    for (int i = 1; i < std::ssize(pActors); i++)
        EXPECT_EQ(pActors[i].aiState, Removed) << "Actor " << i;
}

GAME_TEST(Coop, GuestHitsHurtHostMonsters) {
    // Damage a guest's party deals reaches the host's monster, and a monster the guest kills dies on the host too
    // without giving the host's party any experience.
    auto hpTape = actorTapes.hp(0);
    auto stateTape = actorTapes.aiState(0);
    auto expTape = tapes.totalExperience();
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    engine->config->debug.NoActors.setValue(false);
    game.spawnMonster(pParty->pos + Vec3f(0, 300, 0), MONSTER_RAT_A, SPAWN_DUMMY);
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
    test.startTaping();

    guest.sendActorDamage(CoopActorDamage{.map = MAP_EMERALD_ISLAND, .index = 0, .damage = 2});
    for (int i = 0; i < 100 && pActors[0].hp == pActors[0].monsterInfo.hp; i++) { // The guest sends what it queued only when updated.
        guest.update();
        game.tick();
    }
    guest.sendActorDamage(CoopActorDamage{.map = MAP_EMERALD_ISLAND, .index = 0, .damage = 100});
    for (int i = 0; i < 100 && pActors[0].aiState != Dying; i++) {
        guest.update();
        game.tick();
    }

    EXPECT_EQ(hpTape, tape(4, 0)); // A giant rat has 6 hit points, dying sets them to zero.
    EXPECT_EQ(stateTape, tape(Standing, Dying));
    EXPECT_EQ(expTape.size(), 1);
}

GAME_TEST(Coop, GuestHitsAreSentToHost) {
    // A guest's shot at a monster is reported to the host, with the damage it did.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    Character &shooter = pParty->pCharacters[0];
    shooter.inventory.equip(ITEM_SLOT_MAIN_HAND, Item(ITEM_BLASTER));
    shooter.setSkillValue(SKILL_BLASTER, CombinedSkillValue(10, MASTERY_GRANDMASTER));
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    engine->config->debug.NoActors.setValue(false);
    game.spawnMonster(pParty->pos + Vec3f(0, 80, 0), MONSTER_RAT_A, SPAWN_DUMMY); // Right in front of the party.
    auto statusTape = tapes.statusBar();
    test.startTaping();

    game.pressAndReleaseKey(PlatformKey::KEY_A);
    game.tick();
    std::vector<CoopActorDamage> damage;
    for (int i = 0; i < 100 && damage.empty(); i++) {
        server.update();
        damage = server.takeActorDamage();
        game.tick();
    }

    ASSERT_EQ(damage.size(), 1);
    EXPECT_EQ(damage[0].map, MAP_EMERALD_ISLAND);
    EXPECT_EQ(damage[0].index, 0);
    EXPECT_CONTAINS(statusTape, fmt::format("{} inflicts {} points killing Giant Rat", shooter.name, damage[0].damage));
}

GAME_TEST(Coop, MonstersGoAfterGuests) {
    // Monsters go after a guest's party the way they go after the host's. A goblin next to a guest far from the host
    // comes at the guest, the same goblin with nobody around stays put.
    for (bool guestNearby : {false, true}) {
        SCOPED_TRACE(fmt::format("guestNearby={}", guestNearby));
        test.prepareForNextTest();
        engine->config->debug.NoActors.setValue(true);
        game.startNewGame();
        prepareForBattleTest();
        engine->config->debug.NoActors.setValue(false);
        Vec3f guestPos(2386, 9696, 960); // In front of the fire guild's door, far from the host at the bridge.
        game.spawnMonster(guestPos + Vec3f(0, -300, 0), MONSTER_GOBLIN_A);
        auto stateTape = actorTapes.aiState(0);
        coopSession = CoopSession::host(0, 4);
        MM_AT_SCOPE_EXIT(coopSession.reset());
        CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
        for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
            game.tick();
            guest.update();
        }
        ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
        test.startTaping();

        CoopPartyPose pose;
        pose.map = guestNearby ? MAP_EMERALD_ISLAND : MAP_HARMONDALE;
        pose.pos = guestPos;
        for (int i = 0; i < 50; i++) { // The goblin notices the party and walks up to it.
            guest.sendLocalPose(pose);
            guest.update();
            game.tick();
        }

        bool cameAtGuest = stateTape.contains(Pursuing) || stateTape.contains(AttackingMelee);
        EXPECT_EQ(cameAtGuest, guestNearby);
    }
}

GAME_TEST(Coop, MonsterHitsReachGuest) {
    // A monster's attack on a guest's party is sent to the guest, the actor that stands in for the guest on the host
    // takes no damage.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    engine->config->debug.NoActors.setValue(false);
    Vec3f guestPos(2386, 9696, 960); // In front of the fire guild's door, far from the host at the bridge.
    int goblin = game.spawnMonster(guestPos + Vec3f(0, -100, 0), MONSTER_GOBLIN_A)->id;
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
    CoopPartyPose pose;
    pose.map = MAP_EMERALD_ISLAND;
    pose.pos = guestPos;
    auto findStandIn = [] { return std::ranges::find_if(pActors, [](const Actor &actor) { return actor.attributes & ACTOR_COOP_STAND_IN; }); };
    for (int i = 0; i < 100 && findStandIn() == pActors.end(); i++) {
        guest.sendLocalPose(pose);
        guest.update();
        game.tick();
    }
    ASSERT_NE(findStandIn(), pActors.end());
    auto standInHpTape = actorTapes.hp(findStandIn()->id);
    test.startTaping();

    auto fromGoblin = [&](const CoopPartyHit &hit) { return hit.attackerIndex == goblin; };
    std::vector<CoopPartyHit> hits;
    for (int i = 0; i < 300 && !std::ranges::any_of(hits, fromGoblin); i++) { // The goblin walks up and swings.
        guest.sendLocalPose(pose);
        guest.update();
        game.tick();
        for (const CoopPartyHit &hit : guest.takePartyHits())
            hits.push_back(hit);
    }

    ASSERT_TRUE(std::ranges::any_of(hits, fromGoblin));
    EXPECT_EQ(hits[0].map, MAP_EMERALD_ISLAND);
    EXPECT_EQ(standInHpTape.size(), 1);
}

GAME_TEST(Coop, GuestTakesMonsterHits) {
    // A monster attack the host reports hurts the guest's party.
    auto hpTape = tapes.totalHp();
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    engine->config->debug.NoActors.setValue(false);
    int goblin = game.spawnMonster(pParty->pos + Vec3f(0, 100, 0), MONSTER_GOBLIN_A)->id;
    int playerId = server.remoteParties().begin()->first;
    test.startTaping();

    for (int i = 0; i < 50 && hpTape.size() < 2; i++) { // Some swings miss.
        server.sendPartyHit(playerId, CoopPartyHit{MAP_EMERALD_ISLAND, goblin, ABILITY_ATTACK1});
        server.update();
        game.tick();
    }

    EXPECT_LT(hpTape.delta(), 0);
}

GAME_TEST(Coop, WoundedMonsterTurnsToGuest) {
    // A monster a guest wounds from beyond its sight reacts to the guest the way it would to the host's party, it
    // flinches and turns to face the guest. The same monster left alone does neither.
    for (bool wound : {false, true}) {
        SCOPED_TRACE(fmt::format("wound={}", wound));
        test.prepareForNextTest();
        engine->config->debug.NoActors.setValue(true);
        game.startNewGame();
        prepareForBattleTest();
        engine->config->debug.NoActors.setValue(false);
        Vec3f guestPos(12600, 20224, 0); // On the shore, far from the host at the bridge.
        int goblin = game.spawnMonster(guestPos + Vec3f(0, -6000, 0), MONSTER_GOBLIN_A)->id;
        auto stateTape = actorTapes.aiState(goblin);
        coopSession = CoopSession::host(0, 4);
        MM_AT_SCOPE_EXIT(coopSession.reset());
        CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
        CoopPartyPose pose;
        pose.map = MAP_EMERALD_ISLAND;
        pose.pos = guestPos;
        for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
            game.tick();
            guest.update();
        }
        ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
        for (int i = 0; i < 100 && !hasCoopStandIn(); i++) {
            guest.sendLocalPose(pose);
            guest.update();
            game.tick();
        }
        test.startTaping();

        if (wound)
            guest.sendActorDamage(CoopActorDamage{.map = MAP_EMERALD_ISLAND, .index = goblin, .damage = 1});
        guest.update();
        game.tick();

        float yaw = pActors[goblin].yawAngle * 2 * std::numbers::pi_v<float> / 2048; // A full turn is 2048.
        Vec2f toGuest = (guestPos - pActors[goblin].pos).xy();
        bool facesGuest = dot(Vec2f(std::cos(yaw), std::sin(yaw)), toGuest) > toGuest.length() * std::cos(std::numbers::pi_v<float> / 8);
        EXPECT_EQ(stateTape.contains(InPain), wound);
        EXPECT_EQ(facesGuest, wound);
    }
}

GAME_TEST(Coop, CorpseLootGoesToFirstAsker) {
    // The host hands a corpse's loot to the guest who asks for it, not to the host's party, and only once.
    auto hostGoldTape = tapes.gold();
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    engine->config->debug.NoActors.setValue(false);
    Character &shooter = pParty->pCharacters[0];
    shooter.inventory.equip(ITEM_SLOT_MAIN_HAND, Item(ITEM_BLASTER));
    shooter.setSkillValue(SKILL_BLASTER, CombinedSkillValue(10, MASTERY_GRANDMASTER));
    int rat = game.spawnMonster(pParty->pos + Vec3f(0, 80, 0), MONSTER_RAT_A, SPAWN_DUMMY)->id; // Right in front of the party.
    game.pressAndReleaseKey(PlatformKey::KEY_A);
    for (int i = 0; i < 100 && pActors[rat].aiState != Dead; i++) // The rat goes through its death animation.
        game.tick();
    ASSERT_EQ(pActors[rat].aiState, Dead);
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
    test.startTaping();

    std::vector<CoopLoot> loot;
    for (int attempt = 0; attempt < 2; attempt++) {
        guest.sendLootRequest(CoopLootRequest{.map = MAP_EMERALD_ISLAND, .index = rat});
        for (int i = 0; i < 20; i++) { // A fixed wait, the second request is the one that must go unanswered.
            guest.update();
            game.tick();
            for (CoopLoot &found : guest.takeLoot())
                loot.push_back(std::move(found));
        }
    }

    EXPECT_EQ(loot.size(), 1);
    EXPECT_EQ(pActors[rat].aiState, Removed);
    EXPECT_EQ(hostGoldTape.size(), 1);
}

GAME_TEST(Coop, GuestLootComesFromHost) {
    // A guest looting a corpse asks the host, and gets what the host hands out.
    auto goldTape = tapes.gold();
    auto potionTape = tapes.totalItemCount(ITEM_POTION_CURE_WOUNDS);
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    engine->config->debug.NoActors.setValue(false);
    int rat = game.spawnMonster(pParty->pos + Vec3f(0, 80, 0), MONSTER_RAT_A, SPAWN_DUMMY)->id; // Right in front of the party.
    CoopActorStates states;
    states.map = MAP_EMERALD_ISLAND;
    states.actorCount = pActors.size();
    CoopActorState dead;
    dead.index = rat;
    dead.monster = MONSTER_RAT_A;
    dead.pos = pActors[rat].pos;
    dead.aiState = Dead;
    dead.animation = ANIM_Dead;
    states.actors = {dead};
    server.sendActorStates(states);
    for (int i = 0; i < 100 && pActors[rat].aiState != Dead; i++) {
        server.update();
        game.tick();
    }
    ASSERT_EQ(pActors[rat].aiState, Dead);
    test.startTaping();

    game.pressAndReleaseKey(PlatformKey::KEY_SPACE);
    game.tick();
    std::vector<CoopLootRequest> requests;
    for (int i = 0; i < 100 && requests.empty(); i++) {
        server.update();
        requests = server.takeLootRequests();
        game.tick();
    }
    ASSERT_EQ(requests.size(), 1);
    EXPECT_EQ(requests[0].index, rat);

    CoopLoot loot;
    loot.gold = 37;
    loot.foundItem = Item(ITEM_POTION_CURE_WOUNDS);
    server.sendLoot(requests[0].playerId, loot);
    for (int i = 0; i < 100 && potionTape.back() == potionTape.front(); i++) {
        server.update();
        game.tick();
    }

    EXPECT_EQ(goldTape.delta(), 37);
    EXPECT_EQ(potionTape.delta(), 1);
}

GAME_TEST(Coop, QuestProgressIsShared) {
    // A quest bit one guest's game sets is set in the host's game and passed on to the other guests.
    auto bitTape = tapes.questBit(QBIT_TALKED_TO_ROLAND);
    game.startNewGame();
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient talker("127.0.0.1", coopSession->server()->port(), "Talker");
    CoopClient other("127.0.0.1", coopSession->server()->port(), "Other");
    for (int i = 0; i < 100 && (talker.state() == COOP_CLIENT_CONNECTING || other.state() == COOP_CLIENT_CONNECTING); i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        talker.update();
        other.update();
    }
    ASSERT_EQ(talker.state(), COOP_CLIENT_JOINED);
    ASSERT_EQ(other.state(), COOP_CLIENT_JOINED);
    test.startTaping();
    game.tick(); // Tapes the bit before the change.

    talker.sendQuestBit(CoopQuestBit{.bit = QBIT_TALKED_TO_ROLAND, .value = true});
    std::vector<CoopQuestBit> relayed;
    for (int i = 0; i < 100 && relayed.empty(); i++) { // The talker sends what it queued only when updated.
        talker.update();
        game.tick();
        other.update();
        relayed = other.takeQuestBits();
    }
    talker.update();

    EXPECT_EQ(bitTape, tape(false, true));
    ASSERT_EQ(relayed.size(), 1);
    EXPECT_EQ(relayed[0].bit, QBIT_TALKED_TO_ROLAND);
    EXPECT_TRUE(relayed[0].value);
    EXPECT_TRUE(talker.takeQuestBits().empty()); // Not echoed back to the player whose game made the change.
}

GAME_TEST(Coop, GuestQuestProgressReachesHost) {
    // Talking to Roland in a guest's game sets his quest bit in the host's game too.
    game.startNewGame();
    game.teleportTo(MAP_COLONY_ZOD, Vec3f(-10986, 8576, 1728), 180); // On the ledge east of the hanging cage, facing it.
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    auto bitTape = tapes.questBit(QBIT_TALKED_TO_ROLAND);
    test.startTaping();

    game.pointMouseAtDecoration(1); // Roland's cage.
    game.pressAndReleaseButton(BUTTON_LEFT);
    game.tick(3);
    std::vector<CoopQuestBit> changes;
    for (int i = 0; i < 100 && changes.empty(); i++) {
        server.update();
        changes = server.takeQuestBits();
        game.tick();
    }

    EXPECT_EQ(bitTape, tape(false, true));
    ASSERT_EQ(changes.size(), 1);
    EXPECT_EQ(changes[0].bit, QBIT_TALKED_TO_ROLAND);
    EXPECT_TRUE(changes[0].value);
}

GAME_TEST(Coop, QuestRewardsAreShared) {
    // What one guest's game gives out for a quest, the host's party and the other guests get too.
    auto keyTape = tapes.totalItemCount(ITEM_COLONY_ZOD_KEY);
    game.startNewGame();
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient talker("127.0.0.1", coopSession->server()->port(), "Talker");
    CoopClient other("127.0.0.1", coopSession->server()->port(), "Other");
    for (int i = 0; i < 100 && (talker.state() == COOP_CLIENT_CONNECTING || other.state() == COOP_CLIENT_CONNECTING); i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        talker.update();
        other.update();
    }
    ASSERT_EQ(talker.state(), COOP_CLIENT_JOINED);
    ASSERT_EQ(other.state(), COOP_CLIENT_JOINED);
    test.startTaping();
    game.tick(); // Tapes the inventory before the reward.

    CoopQuestRewards rewards;
    rewards.rewards = {QuestReward{VAR_PlayerItemInHands, std::to_underlying(ITEM_COLONY_ZOD_KEY), 0}};
    talker.sendQuestRewards(rewards);
    std::vector<CoopQuestRewards> relayed;
    for (int i = 0; i < 100 && relayed.empty(); i++) { // The talker sends what it queued only when updated.
        talker.update();
        game.tick();
        other.update();
        relayed = other.takeQuestRewards();
    }

    EXPECT_EQ(keyTape, tape(0, 1));
    ASSERT_EQ(relayed.size(), 1);
    ASSERT_EQ(relayed[0].rewards.size(), 1);
    EXPECT_EQ(relayed[0].rewards[0].value, std::to_underlying(ITEM_COLONY_ZOD_KEY));
}

GAME_TEST(Coop, GuestQuestRewardsReachHost) {
    // Roland hands over his key in the same script that marks him as talked to, so the key counts as a quest reward
    // and goes to the host too.
    game.startNewGame();
    game.teleportTo(MAP_COLONY_ZOD, Vec3f(-10986, 8576, 1728), 180); // On the ledge east of the hanging cage, facing it.
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    test.startTaping();

    game.pointMouseAtDecoration(1); // Roland's cage.
    game.pressAndReleaseButton(BUTTON_LEFT);
    game.tick(3);
    std::vector<CoopQuestRewards> rewards;
    for (int i = 0; i < 100 && rewards.empty(); i++) {
        server.update();
        rewards = server.takeQuestRewards();
        game.tick();
    }

    ASSERT_EQ(rewards.size(), 1);
    ASSERT_EQ(rewards[0].rewards.size(), 1);
    EXPECT_EQ(rewards[0].rewards[0].variable, VAR_PlayerItemInHands);
    EXPECT_EQ(rewards[0].rewards[0].value, std::to_underlying(ITEM_COLONY_ZOD_KEY));
}

GAME_TEST(Coop, ChestsAreShared) {
    // A chest one guest emptied a bit is emptied the same way in the host's game and the other guests' games.
    game.startNewGame();
    int chestId = 0;
    while (chestId < std::ssize(vChests) && vChests[chestId].inventory.size() == 0)
        chestId++;
    ASSERT_LT(chestId, std::ssize(vChests));
    auto chestItemsTape = tapes.custom([&] { return vChests[chestId].inventory.size(); });
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient taker("127.0.0.1", coopSession->server()->port(), "Taker");
    CoopClient other("127.0.0.1", coopSession->server()->port(), "Other");
    for (int i = 0; i < 100 && (taker.state() == COOP_CLIENT_CONNECTING || other.state() == COOP_CLIENT_CONNECTING); i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        taker.update();
        other.update();
    }
    ASSERT_EQ(taker.state(), COOP_CLIENT_JOINED);
    ASSERT_EQ(other.state(), COOP_CLIENT_JOINED);
    test.startTaping();
    game.tick(); // Tapes the chest before the change.

    CoopChest taken{.map = MAP_EMERALD_ISLAND, .chestId = chestId, .chest = vChests[chestId]};
    taken.chest.inventory.take(*taken.chest.inventory.entries().begin());
    taker.sendChest(taken);
    std::vector<CoopChest> relayed;
    for (int i = 0; i < 100 && relayed.empty(); i++) { // The taker sends what it queued only when updated.
        taker.update();
        game.tick();
        other.update();
        relayed = other.takeChests();
    }

    int before = chestItemsTape.front();
    EXPECT_EQ(chestItemsTape, tape(before, before - 1));
    ASSERT_EQ(relayed.size(), 1);
    EXPECT_EQ(relayed[0].chestId, chestId);
    EXPECT_EQ(relayed[0].chest.inventory.size(), before - 1);
}

GAME_TEST(Coop, GuestChestChangesReachHost) {
    // Opening a chest and grabbing an item from it in a guest's game sends the chest to the host each time.
    game.startNewGame();
    game.teleportTo(MAP_EMERALD_ISLAND, Vec3f(8600, 5200, 1), 90); // Next to a chest, facing it.
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    auto screenTape = tapes.screen();
    test.startTaping();

    std::vector<CoopChest> chests;
    auto collect = [&] {
        for (int i = 0; i < 100 && chests.empty(); i++) {
            server.update();
            chests = server.takeChests();
            game.tick();
        }
    };
    game.pressAndReleaseKey(PlatformKey::KEY_SPACE);
    game.tick();
    collect();
    ASSERT_EQ(screenTape.back(), SCREEN_CHEST);
    ASSERT_EQ(chests.size(), 1);
    int chestId = pGUIWindow_CurrentChest->chestId();
    EXPECT_EQ(chests[0].chestId, chestId);
    EXPECT_EQ(chests[0].chest.inventory.size(), vChests[chestId].inventory.size());
    int itemsBefore = vChests[chestId].inventory.size();

    chests.clear();
    game.pressAndReleaseKey(PlatformKey::KEY_SPACE); // Grabs an item.
    game.tick();
    collect();
    ASSERT_EQ(chests.size(), 1);
    EXPECT_EQ(chests[0].chest.inventory.size(), itemsBefore - 1);
    EXPECT_EQ(vChests[chestId].inventory.size(), itemsBefore - 1);
}

GAME_TEST(Coop, GroundItemsAreShared) {
    // An item one guest picks up from the ground disappears from the host's game and the other guests' games.
    auto pileTape = tapes.custom([] { return pSpriteObjects[83].uObjectDescID != 0; });
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    game.teleportTo(MAP_TULAREAN_FOREST, Vec3f(-2854, 13472, 1133), 0, -20); // Looking at a pile of 142 gold.
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient taker("127.0.0.1", coopSession->server()->port(), "Taker");
    CoopClient other("127.0.0.1", coopSession->server()->port(), "Other");
    for (int i = 0; i < 100 && (taker.state() == COOP_CLIENT_CONNECTING || other.state() == COOP_CLIENT_CONNECTING); i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        taker.update();
        other.update();
    }
    ASSERT_EQ(taker.state(), COOP_CLIENT_JOINED);
    ASSERT_EQ(other.state(), COOP_CLIENT_JOINED);
    test.startTaping();
    game.tick(); // Tapes the pile before it's taken.

    CoopGroundItemTaken taken;
    taken.map = MAP_TULAREAN_FOREST;
    taken.item = pSpriteObjects[83].containing_item.itemId;
    taken.pos = pSpriteObjects[83].vPosition + Vec3f(5, 0, 0); // Lying items settle a little differently in every game.
    taker.sendGroundItemTaken(taken);
    std::vector<CoopGroundItemTaken> relayed;
    for (int i = 0; i < 100 && relayed.empty(); i++) { // The taker sends what it queued only when updated.
        taker.update();
        game.tick();
        other.update();
        relayed = other.takeGroundItemsTaken();
    }

    EXPECT_EQ(pileTape, tape(true, false));
    ASSERT_EQ(relayed.size(), 1);
    EXPECT_EQ(relayed[0].item, taken.item);
}

GAME_TEST(Coop, GuestGroundItemPickupReachesHost) {
    // Picking up a pile of gold in a guest's game tells the host which item it was and where it lay.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    game.teleportTo(MAP_TULAREAN_FOREST, Vec3f(-2854, 13472, 1133), 0, -20); // Looking at a pile of 142 gold.
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    ItemId pileItem = pSpriteObjects[83].containing_item.itemId;
    auto goldTape = tapes.gold();
    test.startTaping();
    game.tick(); // Space picks items from the billboards the previous frame drew.

    game.pressAndReleaseKey(PlatformKey::KEY_SPACE);
    game.tick();
    std::vector<CoopGroundItemTaken> taken;
    for (int i = 0; i < 100 && taken.empty(); i++) {
        server.update();
        taken = server.takeGroundItemsTaken();
        game.tick();
    }

    EXPECT_EQ(goldTape.delta(), 142);
    ASSERT_EQ(taken.size(), 1);
    EXPECT_EQ(taken[0].map, MAP_TULAREAN_FOREST);
    EXPECT_EQ(taken[0].item, pileItem);
    EXPECT_EQ(pSpriteObjects[83].uObjectDescID, 0);
    EXPECT_EQ(taken[0].pos, pSpriteObjects[83].vPosition); // Taking an item leaves its last position behind.
}

GAME_TEST(Coop, RestNeedsEveryonesAgreement) {
    // Resting in co-op asks the other players first. The host rests once the guest agrees, and doesn't when the guest
    // refuses, with a message saying who refused.
    for (bool accept : {false, true}) {
        SCOPED_TRACE(fmt::format("accept={}", accept));
        test.prepareForNextTest();
        auto timeTape = tapes.time();
        auto statusTape = tapes.statusBar();
        engine->config->debug.NoActors.setValue(true);
        game.startNewGame();
        coopSession = CoopSession::host(0, 4);
        MM_AT_SCOPE_EXIT(coopSession.reset());
        CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
        for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
            game.tick();
            guest.update();
        }
        ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
        test.startTaping();

        game.pressAndReleaseKey(PlatformKey::KEY_R);
        game.tick();
        game.pressGuiButton("Rest_RestAndHeal");
        for (int i = 0; i < 100 && !guest.pendingVote(); i++) { // The guest sends what it queued only when updated.
            game.tick();
            guest.update();
        }
        ASSERT_TRUE(guest.pendingVote());
        EXPECT_EQ(guest.pendingVote()->kind, COOP_VOTE_REST);

        guest.answerVote(accept);
        for (int i = 0; i < 100 && (accept ? current_screen_type == SCREEN_REST : !statusTape.contains("Guest doesn't want to rest")); i++) {
            guest.update();
            game.tick(); // The rest plays out in the rest window, a few game hours a frame.
        }

        EXPECT_CONTAINS(statusTape, "Asked the others to rest");
        if (accept) {
            EXPECT_GE(timeTape.back() - timeTape.front(), Duration::fromHours(8));
        } else {
            EXPECT_LT(timeTape.back() - timeTape.front(), Duration::fromHours(1));
            EXPECT_CONTAINS(statusTape, "Guest doesn't want to rest");
        }
    }
}

GAME_TEST(Coop, GuestRestsWhenGroupAgrees) {
    // A guest asked to rest by the host rests too once it agrees.
    auto timeTape = tapes.time();
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    test.startTaping();

    server.requestVote(COOP_VOTE_REST);
    for (int i = 0; i < 100 && current_screen_type != SCREEN_CHANGE_LOCATION; i++) {
        server.update();
        game.tick();
    }
    game.pressGuiButton("Transition_Yes");
    bool rested = false;
    for (int i = 0; i < 200 && !rested; i++) { // The rest plays out in the rest window, a few game hours a frame.
        server.update();
        game.tick();
        rested = timeTape.back() - timeTape.front() >= Duration::fromHours(8) && current_screen_type == SCREEN_GAME;
    }

    EXPECT_TRUE(rested);
}

GAME_TEST(Coop, NoTurnBasedModeInCoop) {
    // Turn-based mode can't be switched on in a co-op game, the other players' games wouldn't take turns.
    auto turnBasedTape = tapes.turnBasedMode();
    auto statusTape = tapes.statusBar();
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    test.startTaping();

    game.pressAndReleaseKey(PlatformKey::KEY_RETURN);
    game.tick();

    EXPECT_EQ(turnBasedTape, tape(false));
    EXPECT_CONTAINS(statusTape, "Turn-based mode isn't available in co-op");
}

GAME_TEST(Coop, HostSendsItsClock) {
    // Guests get the host's game time.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);

    std::optional<CoopClock> clock;
    for (int i = 0; i < 100 && !clock; i++) {
        game.tick();
        guest.update();
        clock = guest.takeClock();
    }

    ASSERT_TRUE(clock);
    EXPECT_EQ(clock->playingTime, pParty->GetPlayingTime());
}

GAME_TEST(Coop, GuestFollowsHostClock) {
    // A guest's clock follows the host's when the two drift apart, both ways, and is left alone for small differences
    // that network delay explains.
    for (int offsetSeconds : {7200, -7200, 10}) {
        SCOPED_TRACE(fmt::format("offsetSeconds={}", offsetSeconds));
        test.prepareForNextTest();
        auto timeTape = tapes.time();
        engine->config->debug.NoActors.setValue(true);
        game.startNewGame();
        game.restAndHeal(); // Gives the clock room to go back by hours.
        Blob hostSave = createSaveData(false, "Host").second;
        CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
        game.goToMainMenu();
        coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
        MM_AT_SCOPE_EXIT(coopSession.reset());
        for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
            server.update();
            game.tick();
        }
        ASSERT_EQ(server.remoteParties().size(), 1);
        test.startTaping();
        game.tick(); // Tapes the clock before the host's arrives.

        server.sendClock(CoopClock{pParty->GetPlayingTime() + Duration::fromSeconds(offsetSeconds)});
        for (int i = 0; i < 10; i++) { // A fixed wait, the small difference must not change anything.
            server.update();
            game.tick();
        }

        std::vector<Time> times(timeTape.begin(), timeTape.end());
        Duration biggestJump;
        for (size_t i = 1; i < times.size(); i++) {
            Duration jump = times[i] - times[i - 1];
            if (std::abs(jump.ticks()) > std::abs(biggestJump.ticks()))
                biggestJump = jump;
        }
        if (offsetSeconds == 7200) {
            EXPECT_GE(biggestJump, Duration::fromHours(2) - Duration::fromMinutes(1));
        } else if (offsetSeconds == -7200) {
            EXPECT_LE(biggestJump, Duration() - Duration::fromHours(2) + Duration::fromMinutes(1));
        } else {
            EXPECT_LT(biggestJump, Duration::fromMinutes(1));
        }
    }
}

GAME_TEST(Coop, HostMenusDontPauseTheWorld) {
    // The host's world keeps running while the host has the spellbook open and a guest is playing, and still pauses
    // like in single player when nobody has joined.
    for (bool guestJoined : {false, true}) {
        SCOPED_TRACE(fmt::format("guestJoined={}", guestJoined));
        test.prepareForNextTest();
        engine->config->debug.NoActors.setValue(true);
        game.startNewGame();
        prepareForBattleTest();
        engine->config->debug.NoActors.setValue(false);
        int goblin = game.spawnMonster(pParty->pos + Vec3f(0, 1500, 0), MONSTER_GOBLIN_A)->id;
        auto goblinPosTape = actorTapes.custom(goblin, [](const Actor &actor) { return actor.pos; });
        auto screenTape = tapes.screen();
        coopSession = CoopSession::host(0, 4);
        MM_AT_SCOPE_EXIT(coopSession.reset());
        std::optional<CoopClient> guest;
        if (guestJoined) {
            guest.emplace("127.0.0.1", coopSession->server()->port(), "Guest");
            for (int i = 0; i < 100 && guest->state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
                game.tick();
                guest->update();
            }
            ASSERT_EQ(guest->state(), COOP_CLIENT_JOINED);
        }

        game.pressGuiButton("Game_CastSpell");
        game.tick();
        test.startTaping();
        game.tick(20); // The goblin spots the party and comes at it, unless the world is paused.

        EXPECT_EQ(screenTape, tape(SCREEN_SPELL_BOOK));
        EXPECT_EQ(goblinPosTape.size() > 1, guestJoined);
    }
}

GAME_TEST(Coop, GuestCarriesOnAloneWhenHostLeaves) {
    // A guest whose host goes away is told so, and goes on as a single player game, with its own monsters acting again.
    auto statusTape = tapes.statusBar();
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    auto server = std::make_unique<CoopServer>(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server->port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server->remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server->update();
        game.tick();
    }
    ASSERT_EQ(server->remoteParties().size(), 1);
    ASSERT_TRUE(engine->_actorsFollowHost);
    test.startTaping();

    server.reset();
    for (int i = 0; i < 100 && coopSession; i++) // The host's going away arrives over the network.
        game.tick();

    EXPECT_FALSE(coopSession);
    EXPECT_FALSE(engine->_actorsFollowHost);
    EXPECT_CONTAINS(statusTape, "Lost the co-op game: Disconnected from the host. You're playing alone now.");
}

GAME_TEST(Coop, GuestSpellEffectsReachHostMonsters) {
    // A spell effect a guest's party puts on a monster, like paralysis, takes hold of the host's monster too.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    engine->config->debug.NoActors.setValue(false);
    int goblin = game.spawnMonster(pParty->pos + Vec3f(0, 1500, 0), MONSTER_GOBLIN_A, SPAWN_DUMMY)->id;
    auto paralyzedTape = actorTapes.hasBuff(goblin, ACTOR_BUFF_PARALYZED);
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
    test.startTaping();
    game.tick(); // Tapes the goblin before the effect.

    CoopActorBuff paralysis{.map = MAP_EMERALD_ISLAND, .index = goblin, .buff = ACTOR_BUFF_PARALYZED};
    paralysis.value.Apply(pParty->GetPlayingTime() + Duration::fromHours(1), MASTERY_GRANDMASTER, 0, 0, 0);
    guest.sendActorBuff(paralysis);
    for (int i = 0; i < 100 && !pActors[goblin].buffs[ACTOR_BUFF_PARALYZED].Active(); i++) { // The guest sends what it queued only when updated.
        guest.update();
        game.tick();
    }

    EXPECT_EQ(paralyzedTape, tape(false, true));
}

GAME_TEST(Coop, GuestArmorHalvingReachesHost) {
    // A guest's grandmaster axe halving a monster's armor tells the host about it.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest({{CLASS_RANGER_LORD, RACE_GOBLIN}});
    Character &axeman = pParty->pCharacters[0];
    axeman.inventory.equip(ITEM_SLOT_MAIN_HAND, Item(ITEM_BATTLE_AXE));
    axeman.setSkillValue(SKILL_AXE, CombinedSkillValue(60, MASTERY_GRANDMASTER)); // Every grandmaster hit procs on a dummy, which never resists.
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    engine->config->debug.NoActors.setValue(false);
    int titan = game.spawnMonster(pParty->pos + Vec3f(0, 300, 0), MONSTER_TITAN_A, SPAWN_DUMMY)->id;
    test.startTaping();

    std::vector<CoopActorBuff> buffs;
    auto halved = [&] { return std::ranges::any_of(buffs, [&](const CoopActorBuff &buff) { return buff.buff == ACTOR_BUFF_HALVED_ARMOR; }); };
    for (int i = 0; i < 30 && !halved(); i++) {
        game.pressAndReleaseKey(PlatformKey::KEY_A);
        game.tick(5); // The swing lands, and the recovery passes.
        server.update();
        for (const CoopActorBuff &buff : server.takeActorBuffs())
            buffs.push_back(buff);
    }

    ASSERT_TRUE(halved());
    EXPECT_EQ(buffs.back().index, titan);
    EXPECT_EQ(buffs.back().value.expireTime, pActors[titan].buffs[ACTOR_BUFF_HALVED_ARMOR].expireTime);
}

GAME_TEST(Coop, GuestArmageddonHurtsHostMonsters) {
    // Armageddon cast in a guest's game hurts the host's monsters, like any other hit from the guest.
    engine->config->debug.NoActors.setValue(true);
    engine->config->debug.AllMagic.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    engine->config->debug.NoActors.setValue(false);
    int goblin = game.spawnMonster(pParty->pos + Vec3f(0, 1000, 0), MONSTER_GOBLIN_A, SPAWN_DUMMY)->id;
    auto goblinHpTape = actorTapes.hp(goblin);
    test.startTaping();

    game.castSpell(0, SPELL_DARK_ARMAGEDDON);
    std::vector<CoopActorDamage> damage;
    auto hitGoblin = [&] { return std::ranges::any_of(damage, [&](const CoopActorDamage &hit) { return hit.index == goblin; }); };
    for (int i = 0; i < 200 && !hitGoblin(); i++) { // The ground shakes for a while before the blast.
        game.tick();
        server.update();
        for (const CoopActorDamage &hit : server.takeActorDamage())
            damage.push_back(hit);
    }

    ASSERT_TRUE(hitGoblin());
    EXPECT_LT(goblinHpTape.delta(), 0);
}

GAME_TEST(Coop, HostSpellEffectsReachGuests) {
    // A guest learns which spell effects the host's monsters are under, and how much they're shrunk, so that it can draw
    // a paralyzed monster frozen and a shrunk one smaller.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    engine->config->debug.NoActors.setValue(false);
    Actor *goblin = game.spawnMonster(pParty->pos + Vec3f(0, 1500, 0), MONSTER_GOBLIN_A, SPAWN_DUMMY);
    int goblinId = goblin->id;
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);

    pActors[goblinId].buffs[ACTOR_BUFF_PARALYZED].Apply(pParty->GetPlayingTime() + Duration::fromHours(1), MASTERY_GRANDMASTER, 0, 0, 0);
    pActors[goblinId].buffs[ACTOR_BUFF_SHRINK].Apply(pParty->GetPlayingTime() + Duration::fromHours(1), MASTERY_GRANDMASTER, 3, 0, 0);
    std::optional<CoopActorState> latest;
    uint32_t paralyzed = 1u << std::to_underlying(ACTOR_BUFF_PARALYZED);
    for (int i = 0; i < 100 && !(latest && (latest->activeBuffs & paralyzed)); i++) {
        game.tick();
        guest.update();
        for (const CoopActorStates &states : guest.takeActorStates())
            for (const CoopActorState &state : states.actors)
                if (state.index == goblinId)
                    latest = state;
    }

    ASSERT_TRUE(latest);
    EXPECT_TRUE(latest->activeBuffs & paralyzed);
    EXPECT_EQ(latest->shrinkPower, 3);
}

GAME_TEST(Coop, GuestTakesOverHostSpellEffects) {
    // A guest's monster takes on the spell effects the host reports, without sending them back as the guest's own.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    engine->config->debug.NoActors.setValue(false);
    int goblin = game.spawnMonster(pParty->pos + Vec3f(0, 500, 0), MONSTER_GOBLIN_A, SPAWN_DUMMY)->id;
    auto paralyzedTape = actorTapes.hasBuff(goblin, ACTOR_BUFF_PARALYZED);
    test.startTaping();
    game.tick(); // Tapes the goblin before the host's state arrives.

    CoopActorStates states;
    states.map = MAP_EMERALD_ISLAND;
    states.actorCount = pActors.size();
    CoopActorState state;
    state.index = goblin;
    state.monster = MONSTER_GOBLIN_A;
    state.pos = pActors[goblin].pos;
    state.activeBuffs = 1u << std::to_underlying(ACTOR_BUFF_PARALYZED);
    states.actors = {state};
    server.sendActorStates(states);
    std::vector<CoopActorBuff> echoed;
    for (int i = 0; i < 20; i++) { // A fixed wait, nothing may come back.
        server.update();
        game.tick();
        for (const CoopActorBuff &buff : server.takeActorBuffs())
            echoed.push_back(buff);
    }

    EXPECT_EQ(paralyzedTape, tape(false, true));
    EXPECT_TRUE(echoed.empty());
}

GAME_TEST(Coop, DroppedItemsAreShared) {
    // An item one guest drops lands in the host's game and is passed on to the other guests.
    auto swordsTape = tapes.custom([] { return groundItemCount(ITEM_CRUDE_LONGSWORD); });
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient dropper("127.0.0.1", coopSession->server()->port(), "Dropper");
    CoopClient other("127.0.0.1", coopSession->server()->port(), "Other");
    for (int i = 0; i < 100 && (dropper.state() == COOP_CLIENT_CONNECTING || other.state() == COOP_CLIENT_CONNECTING); i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        dropper.update();
        other.update();
    }
    ASSERT_EQ(dropper.state(), COOP_CLIENT_JOINED);
    ASSERT_EQ(other.state(), COOP_CLIENT_JOINED);
    test.startTaping();
    game.tick(); // Tapes the ground before the drop.

    CoopItemDropped dropped;
    dropped.map = MAP_EMERALD_ISLAND;
    dropped.item = Item(ITEM_CRUDE_LONGSWORD);
    dropped.pos = pParty->pos + Vec3f(0, 0, pParty->eyeLevel);
    dropped.yaw = pParty->_viewYaw;
    dropper.sendItemDropped(dropped);
    std::vector<CoopItemDropped> relayed;
    for (int i = 0; i < 100 && relayed.empty(); i++) { // The dropper sends what it queued only when updated.
        dropper.update();
        game.tick();
        other.update();
        relayed = other.takeItemsDropped();
    }

    EXPECT_EQ(swordsTape, tape(0, 1));
    ASSERT_EQ(relayed.size(), 1);
    EXPECT_EQ(relayed[0].item.itemId, ITEM_CRUDE_LONGSWORD);
}

GAME_TEST(Coop, GuestDropReachesHost) {
    // Dropping an item in a guest's game tells the host what was dropped and how it was thrown.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    test.startTaping();

    pParty->setHoldingItem(Item(ITEM_CRUDE_LONGSWORD));
    game.pressAndReleaseButton(BUTTON_LEFT, 240, 170); // A click in the viewport drops the held item.
    game.tick();
    std::vector<CoopItemDropped> dropped;
    for (int i = 0; i < 100 && dropped.empty(); i++) {
        server.update();
        dropped = server.takeItemsDropped();
        game.tick();
    }

    ASSERT_EQ(dropped.size(), 1);
    EXPECT_EQ(dropped[0].map, MAP_EMERALD_ISLAND);
    EXPECT_EQ(dropped[0].item.itemId, ITEM_CRUDE_LONGSWORD);
    EXPECT_EQ(dropped[0].yaw, pParty->_viewYaw);
}

GAME_TEST(Coop, JoinFromCoopMenu) {
    // The co-op screen on the main menu joins a game, without any command line options.
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    MM_AT_SCOPE_EXIT(coopSession.reset());
    test.startTaping();

    game.pressAndReleaseKey(PlatformKey::KEY_P);
    game.tick();
    game.pressGuiButton("CoopMenu_JoinPage");
    game.tick();
    typeText(game, "127.0.0.1"); // The address field is selected first.
    game.pressAndReleaseKey(PlatformKey::KEY_RETURN);
    game.tick(); // Enter finishes the field, the next one takes input from the next frame.
    eraseText(game, 4);
    typeText(game, std::to_string(server.port()));
    game.tick();
    game.pressGuiButton("CoopMenu_Join");
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }

    ASSERT_TRUE(coopSession);
    EXPECT_EQ(coopSession->client()->state(), COOP_CLIENT_JOINED);
    EXPECT_EQ(server.remoteParties().size(), 1);
}

GAME_TEST(Coop, HostFromCoopMenu) {
    // The co-op screen on the main menu starts hosting on the port typed in, and goes back to the main menu.
    game.startNewGame();
    game.goToMainMenu();
    MM_AT_SCOPE_EXIT(coopSession.reset());
    test.startTaping();

    game.pressAndReleaseKey(PlatformKey::KEY_P);
    game.tick();
    game.pressGuiButton("CoopMenu_HostPage");
    game.tick();
    game.pressGuiButton("CoopMenu_Field2"); // The port.
    game.tick();
    eraseText(game, 4);
    typeText(game, "47347");
    game.tick();
    game.pressGuiButton("CoopMenu_Host");
    game.tick();

    ASSERT_TRUE(coopSession);
    ASSERT_TRUE(coopSession->server());
    EXPECT_EQ(coopSession->server()->port(), 47347);
    EXPECT_EQ(GetCurrentMenuID(), MENU_MAIN);
}

GAME_TEST(Coop, GuestStunsAndKnockbackReachHost) {
    // A stun and a knockback from a guest's hit move the host's monster the same way, not only the guest's copy.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    engine->config->debug.NoActors.setValue(false);
    int goblin = game.spawnMonster(pParty->pos + Vec3f(0, 1500, 0), MONSTER_GOBLIN_A, SPAWN_DUMMY)->id;
    auto recoveryTape = actorTapes.custom(goblin, [](const Actor &actor) { return actor.monsterInfo.recoveryTime; });
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
    test.startTaping();
    game.tick(); // Tapes the goblin before the hit.

    guest.sendActorDamage(CoopActorDamage{.map = MAP_EMERALD_ISLAND, .index = goblin, .damage = 1,
                                          .extraRecovery = Duration::fromTicks(20), .knockback = Vec3f(0, 500, 0)});
    for (int i = 0; i < 100 && recoveryTape.size() < 2; i++) { // The guest sends what it queued only when updated.
        guest.update();
        game.tick();
    }

    EXPECT_EQ(recoveryTape.delta(), Duration::fromTicks(20));
}

GAME_TEST(Coop, GuestForceWeaponStunReachesHost) {
    // Hitting a monster with a weapon of force in a guest's game sends the stun and the knockback to the host.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    Item sword(ITEM_CRUDE_LONGSWORD);
    sword.specialEnchantment = ITEM_ENCHANTMENT_OF_FORCE;
    pParty->pCharacters[0].inventory.equip(ITEM_SLOT_MAIN_HAND, sword);
    pParty->pCharacters[0].setSkillValue(SKILL_SWORD, CombinedSkillValue(60, MASTERY_GRANDMASTER)); // So that the swings land.
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    engine->config->debug.NoActors.setValue(false);
    int titan = game.spawnMonster(pParty->pos + Vec3f(0, 300, 0), MONSTER_TITAN_A, SPAWN_DUMMY)->id;
    test.startTaping();

    std::vector<CoopActorDamage> hits;
    for (int i = 0; i < 30 && hits.empty(); i++) {
        game.pressAndReleaseKey(PlatformKey::KEY_A);
        game.tick(5); // The swing lands, and the recovery passes.
        server.update();
        hits = server.takeActorDamage();
    }

    ASSERT_FALSE(hits.empty());
    EXPECT_EQ(hits[0].index, titan);
    EXPECT_GT(hits[0].extraRecovery, Duration());
    EXPECT_NE(hits[0].knockback, Vec3f());
}

GAME_TEST(Coop, GuestDrawsHostShrinkPower) {
    // A monster the host shrank takes on the shrink's power in a guest's game, which decides how small it's drawn.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    engine->config->debug.NoActors.setValue(false);
    int goblin = game.spawnMonster(pParty->pos + Vec3f(0, 500, 0), MONSTER_GOBLIN_A, SPAWN_DUMMY)->id;
    auto powerTape = actorTapes.custom(goblin, [](const Actor &actor) {
        return actor.buffs[ACTOR_BUFF_SHRINK].Active() ? actor.buffs[ACTOR_BUFF_SHRINK].power : 0;
    });
    test.startTaping();
    game.tick(); // Tapes the goblin before the host's state arrives.

    CoopActorStates states;
    states.map = MAP_EMERALD_ISLAND;
    states.actorCount = pActors.size();
    CoopActorState state;
    state.index = goblin;
    state.monster = MONSTER_GOBLIN_A;
    state.pos = pActors[goblin].pos;
    state.activeBuffs = 1u << std::to_underlying(ACTOR_BUFF_SHRINK);
    state.shrinkPower = 3;
    states.actors = {state};
    server.sendActorStates(states);
    std::vector<CoopActorBuff> echoed;
    for (int i = 0; i < 20; i++) { // A fixed wait, nothing may come back.
        server.update();
        game.tick();
        for (const CoopActorBuff &buff : server.takeActorBuffs())
            echoed.push_back(buff);
    }

    EXPECT_EQ(powerTape, tape(0, 3));
    EXPECT_TRUE(echoed.empty());
}

GAME_TEST(Coop, HostSpellsFlyThroughGuests) {
    // A fire bolt the host casts flies through a guest standing in its way and hits the monster behind the guest.
    engine->config->debug.NoActors.setValue(true);
    engine->config->debug.AllMagic.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    engine->config->debug.NoActors.setValue(false);
    int goblin = game.spawnMonster(pParty->pos + Vec3f(0, 1500, 0), MONSTER_GOBLIN_A, SPAWN_DUMMY)->id;
    auto goblinHpTape = actorTapes.hp(goblin);
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
    guest.sendLocalPose(CoopPartyPose{.map = MAP_EMERALD_ISLAND, .pos = pParty->pos + Vec3f(0, 500, 0)});
    for (int i = 0; i < 100 && !hasCoopStandIn(); i++) {
        guest.update();
        game.tick();
    }
    ASSERT_TRUE(hasCoopStandIn());
    test.startTaping();

    game.castQuickSpell(0, SPELL_FIRE_FIRE_BOLT);
    for (int i = 0; i < 100 && !hasSprite(SPRITE_SPELL_FIRE_FIRE_BOLT_IMPACT); i++)
        game.tick();

    EXPECT_LT(goblinHpTape.delta(), 0);
}

GAME_TEST(Coop, HostFireballMissesGuests) {
    // A fireball the host lands next to a guest hits the monster but not the guest, who isn't a monster to the host.
    engine->config->debug.NoActors.setValue(true);
    engine->config->debug.AllMagic.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    engine->config->debug.NoActors.setValue(false);
    int goblin = game.spawnMonster(pParty->pos + Vec3f(0, 1500, 0), MONSTER_GOBLIN_A, SPAWN_DUMMY)->id;
    auto goblinHpTape = actorTapes.hp(goblin);
    auto statusTape = tapes.statusBar();
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
    guest.sendLocalPose(CoopPartyPose{.map = MAP_EMERALD_ISLAND, .pos = pActors[goblin].pos + Vec3f(100, 0, 0)});
    for (int i = 0; i < 100 && !hasCoopStandIn(); i++) {
        guest.update();
        game.tick();
    }
    ASSERT_TRUE(hasCoopStandIn());
    test.startTaping();

    game.castQuickSpell(0, SPELL_FIRE_FIREBALL);
    for (int i = 0; i < 100 && !hasSprite(SPRITE_SPELL_FIRE_FIREBALL_IMPACT); i++)
        game.tick();

    EXPECT_LT(goblinHpTape.delta(), 0);
    EXPECT_FALSE(std::ranges::any_of(statusTape, [](const std::string &status) { return status.contains("Fighter"); })); // What the host's game calls the guest.
}

GAME_TEST(Coop, GuestSpellsAreDrawnOnHost) {
    // A fire bolt a guest casts flies and explodes in the host's game, but only the guest's game deals its damage.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    engine->config->debug.NoActors.setValue(false);
    int goblin = game.spawnMonster(pParty->pos + Vec3f(0, 1500, 0), MONSTER_GOBLIN_A, SPAWN_DUMMY)->id;
    auto goblinHpTape = actorTapes.hp(goblin);
    auto spritesTape = tapes.sprites();
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient caster("127.0.0.1", coopSession->server()->port(), "Caster");
    CoopClient other("127.0.0.1", coopSession->server()->port(), "Other");
    for (int i = 0; i < 100 && (caster.state() == COOP_CLIENT_CONNECTING || other.state() == COOP_CLIENT_CONNECTING); i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        caster.update();
        other.update();
    }
    ASSERT_EQ(caster.state(), COOP_CLIENT_JOINED);
    ASSERT_EQ(other.state(), COOP_CLIENT_JOINED);
    Vec3f casterPos = pParty->pos + Vec3f(0, 500, 0);
    caster.sendLocalPose(CoopPartyPose{.map = MAP_EMERALD_ISLAND, .pos = casterPos});
    for (int i = 0; i < 100 && !hasCoopStandIn(); i++) {
        caster.update();
        game.tick();
    }
    ASSERT_TRUE(hasCoopStandIn());
    test.startTaping();

    CoopSpriteLaunched launched;
    launched.map = MAP_EMERALD_ISLAND;
    launched.sprite.spriteId = SPRITE_SPELL_FIRE_FIRE_BOLT;
    launched.sprite.uSpellID = SPELL_FIRE_FIRE_BOLT;
    launched.sprite.spell_level = 10;
    launched.sprite.spell_skill = MASTERY_MASTER;
    launched.sprite.spell_caster_pid = Pid::character(0);
    launched.sprite.vPosition = casterPos + Vec3f(0, 0, pParty->eyeLevel);
    launched.sprite.vVelocity = Vec3f(0, 1000, 0);
    launched.sprite.uFacing = 512;
    caster.sendSpriteLaunched(launched);
    std::vector<CoopSpriteLaunched> relayed;
    for (int i = 0; i < 100 && !hasSprite(SPRITE_SPELL_FIRE_FIRE_BOLT_IMPACT); i++) { // The caster sends what it queued only when updated.
        caster.update();
        game.tick();
        other.update();
        for (const CoopSpriteLaunched &sprite : other.takeSpritesLaunched())
            relayed.push_back(sprite);
    }

    EXPECT_CONTAINS(spritesTape.flatten(), SPRITE_SPELL_FIRE_FIRE_BOLT);
    EXPECT_CONTAINS(spritesTape.flatten(), SPRITE_SPELL_FIRE_FIRE_BOLT_IMPACT);
    EXPECT_EQ(goblinHpTape.delta(), 0);
    ASSERT_EQ(relayed.size(), 1);
    EXPECT_EQ(relayed[0].sprite.spriteId, SPRITE_SPELL_FIRE_FIRE_BOLT);
}

GAME_TEST(Coop, GuestSpellsReachHost) {
    // A fire bolt cast in a guest's game is sent to the host, so that everyone sees it.
    engine->config->debug.NoActors.setValue(true);
    engine->config->debug.AllMagic.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    test.startTaping();

    game.castQuickSpell(0, SPELL_FIRE_FIRE_BOLT);
    std::vector<CoopSpriteLaunched> launched;
    for (int i = 0; i < 100 && launched.empty(); i++) {
        server.update();
        launched = server.takeSpritesLaunched();
        game.tick();
    }

    ASSERT_EQ(launched.size(), 1);
    EXPECT_EQ(launched[0].map, MAP_EMERALD_ISLAND);
    EXPECT_EQ(launched[0].sprite.spriteId, SPRITE_SPELL_FIRE_FIRE_BOLT);
    EXPECT_EQ(launched[0].sprite.spell_caster_pid, Pid::character(0));
}

GAME_TEST(Coop, HostMonsterProjectilesReachGuests) {
    // An archer in the host's game shooting at a guest sends its arrows to the guests, so that they see them fly.
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    prepareForBattleTest();
    engine->config->debug.NoActors.setValue(false);
    Vec3f guestPos(2386, 9696, 960); // In front of the fire guild's door, far from the host at the bridge.
    int archer = game.spawnMonster(guestPos + Vec3f(0, 1000, 0), MONSTER_ARCHER_A, SPAWN_STATIONARY)->id;
    coopSession = CoopSession::host(0, 4);
    MM_AT_SCOPE_EXIT(coopSession.reset());
    CoopClient guest("127.0.0.1", coopSession->server()->port(), "Guest");
    for (int i = 0; i < 100 && guest.state() == COOP_CLIENT_CONNECTING; i++) { // Loopback, but ENet's handshake takes a few round trips.
        game.tick();
        guest.update();
    }
    ASSERT_EQ(guest.state(), COOP_CLIENT_JOINED);
    test.startTaping();

    auto fromArcher = [&](const CoopSpriteLaunched &launched) { return launched.sprite.spell_caster_pid == Pid(OBJECT_Actor, archer); };
    std::vector<CoopSpriteLaunched> launched;
    for (int i = 0; i < 300 && !std::ranges::any_of(launched, fromArcher); i++) { // The archer spots the guest, aims and shoots.
        guest.sendLocalPose(CoopPartyPose{.map = MAP_EMERALD_ISLAND, .pos = guestPos});
        guest.update();
        game.tick();
        for (const CoopSpriteLaunched &sprite : guest.takeSpritesLaunched())
            launched.push_back(sprite);
    }

    auto arrow = std::ranges::find_if(launched, fromArcher);
    ASSERT_NE(arrow, launched.end());
    EXPECT_EQ(arrow->map, MAP_EMERALD_ISLAND);
    EXPECT_EQ(arrow->sprite.spriteId, SPRITE_PROJECTILE_ARROW);
}

GAME_TEST(Coop, GuestDrawsHostMonsterProjectiles) {
    // An arrow a monster shot in the host's game flies at a guest's party, but only the host's word hurts the party.
    auto hpTape = tapes.totalHp();
    auto spritesTape = tapes.sprites();
    engine->config->debug.NoActors.setValue(true);
    game.startNewGame();
    Blob hostSave = createSaveData(false, "Host").second;
    CoopServer server(0, 4, [&] { return Blob::share(hostSave); });
    game.goToMainMenu();
    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest", Blob());
    MM_AT_SCOPE_EXIT(coopSession.reset());
    for (int i = 0; i < 100 && server.remoteParties().empty(); i++) { // Loopback, but ENet's handshake takes a few round trips.
        server.update();
        game.tick();
    }
    ASSERT_EQ(server.remoteParties().size(), 1);
    engine->config->debug.NoActors.setValue(false);
    Actor *archer = game.spawnMonster(pParty->pos + Vec3f(0, 1000, 0), MONSTER_ARCHER_A, SPAWN_DUMMY);
    test.startTaping();

    CoopSpriteLaunched launched;
    launched.map = MAP_EMERALD_ISLAND;
    launched.sprite.spriteId = SPRITE_PROJECTILE_ARROW;
    launched.sprite.spell_caster_pid = Pid(OBJECT_Actor, archer->id);
    launched.sprite.spellCasterAbility = ABILITY_ATTACK1;
    launched.sprite.vPosition = archer->pos + Vec3f(0, 0, archer->height / 2);
    launched.sprite.vVelocity = Vec3f(0, -1000, 0);
    launched.sprite.uFacing = 1536;
    server.sendSpriteLaunched(launched);
    bool arrived = false;
    for (int i = 0; i < 100 && !(arrived && !hasSprite(SPRITE_PROJECTILE_ARROW)); i++) { // The arrow flies until it hits the party.
        server.update();
        game.tick();
        arrived |= hasSprite(SPRITE_PROJECTILE_ARROW);
    }

    EXPECT_CONTAINS(spritesTape.flatten(), SPRITE_PROJECTILE_ARROW);
    EXPECT_EQ(hpTape.size(), 1);
}

GAME_TEST(Coop, MultiplayerButtonOpensCoopMenu) {
    // The main menu's Multiplayer button asks whether to host or join, and Escape steps back one screen at a time.
    auto choiceTape = tapes.custom([] { return hasGuiButton("CoopMenu_HostPage"); });
    auto joinFormTape = tapes.custom([] { return hasGuiButton("CoopMenu_Join"); });
    auto mainMenuTape = tapes.custom([] { return hasGuiButton("MainMenu_NewGame"); });
    game.goToMainMenu();
    test.startTaping();

    game.pressGuiButton("MainMenu_Coop");
    game.tick();
    game.pressGuiButton("CoopMenu_JoinPage");
    game.tick();
    game.pressAndReleaseKey(PlatformKey::KEY_ESCAPE);
    game.tick();
    game.pressAndReleaseKey(PlatformKey::KEY_ESCAPE);
    game.tick();

    EXPECT_EQ(choiceTape, tape(true, false, true, false));
    EXPECT_EQ(joinFormTape, tape(false, true, false));
    EXPECT_EQ(mainMenuTape, tape(false, true));
    EXPECT_FALSE(coopSession);
}
