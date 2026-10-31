#include "Testing/Game/GameTest.h"

#include "Engine/Engine.h"
#include "Engine/Coop/CoopClient.h"
#include "Engine/Coop/CoopServer.h"
#include "Engine/Coop/CoopSession.h"
#include "Engine/MapEnums.h"
#include "Engine/Party.h"
#include "Engine/RemoteParty.h"
#include "Engine/Graphics/Renderer/Renderer.h"

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

    coopSession.reset();
}
