#include "Testing/Game/GameTest.h"

#include "Engine/Engine.h"
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
