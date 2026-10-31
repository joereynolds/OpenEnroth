#include <string>
#include <utility>
#include <variant>

#include "Testing/Unit/UnitTest.h"

#include "Engine/Coop/CoopProtocol.h"

#include "Utility/Exception.h"

template<class T>
static T roundTrip(T message) {
    CoopMessage decoded = decodeCoopMessage(encodeCoopMessage(std::move(message)));
    EXPECT_TRUE(std::holds_alternative<T>(decoded));
    return std::get<T>(std::move(decoded));
}

UNIT_TEST(CoopProtocol, HelloRoundTrip) {
    // A hello keeps its version and player name through encoding.
    CoopHello hello;
    hello.protocolVersion = 42;
    hello.playerName = "Zoltan";

    CoopHello decoded = roundTrip(hello);
    EXPECT_EQ(decoded.protocolVersion, 42);
    EXPECT_EQ(decoded.playerName, "Zoltan");
}

UNIT_TEST(CoopProtocol, WelcomeRoundTrip) {
    // A welcome keeps its player id and save game bytes, including embedded zeros.
    std::string save("save\0game", 9);
    CoopWelcome welcome;
    welcome.playerId = 3;
    welcome.saveGame = Blob::fromString(save);

    CoopWelcome decoded = roundTrip(std::move(welcome));
    EXPECT_EQ(decoded.playerId, 3);
    EXPECT_EQ(decoded.saveGame.str(), save);
}

UNIT_TEST(CoopProtocol, RejectRoundTrip) {
    // A reject keeps its reason.
    CoopReject reject;
    reject.reason = "Version mismatch";

    EXPECT_EQ(roundTrip(reject).reason, "Version mismatch");
}

UNIT_TEST(CoopProtocol, PartyPoseRoundTrip) {
    // A party pose keeps all of its fields.
    CoopPartyPose pose;
    pose.playerId = 2;
    pose.map = MAP_HARMONDALE;
    pose.pos = Vec3f(-1234.5f, 678.25f, 96.0f);
    pose.yaw = 1536;
    pose.pitch = -64;
    pose.walking = true;

    CoopPartyPose decoded = roundTrip(pose);
    EXPECT_EQ(decoded.playerId, 2);
    EXPECT_EQ(decoded.map, MAP_HARMONDALE);
    EXPECT_EQ(decoded.pos, Vec3f(-1234.5f, 678.25f, 96.0f));
    EXPECT_EQ(decoded.yaw, 1536);
    EXPECT_EQ(decoded.pitch, -64);
    EXPECT_TRUE(decoded.walking);
}

UNIT_TEST(CoopProtocol, PlayerLeftRoundTrip) {
    // A player left message keeps its player id.
    CoopPlayerLeft left;
    left.playerId = 5;

    EXPECT_EQ(roundTrip(left).playerId, 5);
}

UNIT_TEST(CoopProtocol, UnknownTypeThrows) {
    // A message with a type byte past the last known message is rejected.
    EXPECT_THROW((void) decodeCoopMessage(Blob::fromString(std::string(1, '\x7F'))), Exception);
}

UNIT_TEST(CoopProtocol, EmptyMessageThrows) {
    // An empty packet is rejected.
    EXPECT_THROW((void) decodeCoopMessage(Blob()), Exception);
}

UNIT_TEST(CoopProtocol, TruncatedMessageThrows) {
    // A message cut short is rejected.
    CoopHello hello;
    hello.playerName = "Zoltan";
    std::string wire(encodeCoopMessage(hello).str());

    EXPECT_THROW((void) decodeCoopMessage(Blob::fromString(wire.substr(0, wire.size() - 1))), Exception);
}

UNIT_TEST(CoopProtocol, TrailingDataThrows) {
    // A message with extra bytes at the end is rejected.
    CoopPlayerLeft left;
    std::string wire(encodeCoopMessage(left).str());

    EXPECT_THROW((void) decodeCoopMessage(Blob::fromString(wire + "x")), Exception);
}
