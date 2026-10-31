#include "Testing/Game/GameTest.h"

#include "Engine/Engine.h"
#include "Engine/Coop/CoopClient.h"
#include "Engine/Coop/CoopServer.h"
#include "Engine/Coop/CoopSession.h"
#include "Engine/MapEnums.h"
#include "Engine/Party.h"
#include "Engine/RemoteParty.h"
#include "Engine/SaveLoad.h"
#include "Engine/Graphics/Renderer/Renderer.h"

#include "Utility/ScopeGuard.h"

#include "GameTestCommon.h"

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

    coopSession = CoopSession::join("127.0.0.1", server.port(), "Guest");
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
