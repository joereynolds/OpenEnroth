#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
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

#include "GUI/GUIWindow.h"

#include "Library/Platform/Application/PlatformApplication.h"
#include "Library/Platform/Interface/PlatformEvents.h"

#include "Utility/ScopeGuard.h"

#include "GameTestCommon.h"

static bool hasCoopStandIn() {
    return std::ranges::any_of(pActors, [](const Actor &actor) { return actor.attributes & ACTOR_COOP_STAND_IN; });
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

        guest.answerTravel(accept);
        std::optional<CoopTravelDecision> decision;
        for (int i = 0; i < 100 && !decision; i++) {
            guest.update();
            game.tick();
            decision = guest.takeTravelDecision();
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

        server.requestTravel(MapDestination(MAP_HARMONDALE, MAP_START_POINT_PARTY));
        for (int i = 0; i < 100 && current_screen_type != SCREEN_CHANGE_LOCATION; i++) {
            server.update();
            game.tick();
        }
        game.pressGuiButton(accept ? "Transition_Yes" : "Transition_No");
        std::optional<CoopTravelDecision> hostDecision;
        for (int i = 0; i < 100 && !hostDecision; i++) {
            game.tick();
            server.update();
            hostDecision = server.takeTravelDecision();
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

    server.requestTravel(MapDestination(MAP_HARMONDALE, MAP_START_POINT_PARTY));
    for (int i = 0; i < 100 && (current_screen_type != SCREEN_CHANGE_LOCATION || !other.pendingVote()); i++) {
        server.update();
        other.update();
        game.tick();
    }
    ASSERT_EQ(current_screen_type, SCREEN_CHANGE_LOCATION);
    ASSERT_TRUE(other.pendingVote());

    other.answerTravel(false);
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
