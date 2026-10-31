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
    // A reject keeps its reason and whether to retry.
    CoopReject reject;
    reject.reason = "Version mismatch";
    reject.retryLater = true;

    CoopReject decoded = roundTrip(reject);
    EXPECT_EQ(decoded.reason, "Version mismatch");
    EXPECT_TRUE(decoded.retryLater);
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

UNIT_TEST(CoopProtocol, TravelRequestToStartPointRoundTrip) {
    // A travel request to a map's start point keeps the map and the start point.
    CoopVoteRequest request;
    request.kind = COOP_VOTE_TRAVEL;
    request.destination = MapDestination(MAP_HARMONDALE, MAP_START_POINT_NORTH);

    CoopVoteRequest decoded = roundTrip(request);
    EXPECT_EQ(decoded.destination.map(), MAP_HARMONDALE);
    ASSERT_TRUE(std::holds_alternative<MapStartPoint>(decoded.destination.arrival()));
    EXPECT_EQ(std::get<MapStartPoint>(decoded.destination.arrival()), MAP_START_POINT_NORTH);
}

UNIT_TEST(CoopProtocol, RestVoteRoundTrip) {
    // A rest vote keeps its kind.
    CoopVote vote;
    vote.voteId = 2;
    vote.kind = COOP_VOTE_REST;
    vote.proposerName = "Sleepy";

    CoopVote decoded = roundTrip(vote);
    EXPECT_EQ(decoded.kind, COOP_VOTE_REST);
    EXPECT_EQ(decoded.proposerName, "Sleepy");
    EXPECT_EQ(decoded.destination.map(), MAP_INVALID);
}

UNIT_TEST(CoopProtocol, TravelVoteToPlacementRoundTrip) {
    // A travel vote to an exact placement keeps the vote id, the proposer and the whole placement.
    CoopVote vote;
    vote.voteId = 7;
    vote.proposerName = "Zoltan";
    vote.destination = MapDestination(MAP_ERATHIA, PartyPlacement(Vec3f(1, -2, 3.5f), 1024, -32, 100));

    CoopVote decoded = roundTrip(vote);
    EXPECT_EQ(decoded.voteId, 7);
    EXPECT_EQ(decoded.proposerName, "Zoltan");
    EXPECT_EQ(decoded.destination.map(), MAP_ERATHIA);
    ASSERT_TRUE(std::holds_alternative<PartyPlacement>(decoded.destination.arrival()));
    const PartyPlacement &placement = std::get<PartyPlacement>(decoded.destination.arrival());
    EXPECT_EQ(placement.pos, Vec3f(1, -2, 3.5f));
    EXPECT_EQ(placement.yaw, 1024);
    EXPECT_EQ(placement.pitch, -32);
    EXPECT_EQ(placement.zSpeed, 100);
}

UNIT_TEST(CoopProtocol, TravelAnswerRoundTrip) {
    // A travel answer keeps its vote id and verdict.
    CoopVoteAnswer answer;
    answer.voteId = 3;
    answer.accepted = true;

    CoopVoteAnswer decoded = roundTrip(answer);
    EXPECT_EQ(decoded.voteId, 3);
    EXPECT_TRUE(decoded.accepted);
}

UNIT_TEST(CoopProtocol, TravelDecisionRoundTrip) {
    // A travel decision keeps its outcome, who declined, and a destination that stays on the current map.
    CoopVoteDecision decision;
    decision.voteId = 4;
    decision.declinedBy = "Gnomish";

    CoopVoteDecision decoded = roundTrip(decision);
    EXPECT_EQ(decoded.voteId, 4);
    EXPECT_FALSE(decoded.approved);
    EXPECT_EQ(decoded.declinedBy, "Gnomish");
    EXPECT_EQ(decoded.destination.map(), MAP_INVALID);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(decoded.destination.arrival()));
}

UNIT_TEST(CoopProtocol, TravelToStartPointWithoutMapThrows) {
    // A destination that names a start point but no map is rejected, the engine can't resolve it.
    std::string wire(encodeCoopMessage(CoopVoteRequest{COOP_VOTE_TRAVEL, MapDestination(MAP_HARMONDALE, MAP_START_POINT_PARTY)}).str());
    wire[2] = 0; // The map id follows the message type and the vote kind, it's little endian and fits in its first byte.

    EXPECT_THROW((void) decodeCoopMessage(Blob::fromString(wire)), Exception);
}

UNIT_TEST(CoopProtocol, ActorStatesRoundTrip) {
    // Actor states keep the map, the actor count and every field of every actor.
    CoopActorStates states;
    states.map = MAP_TULAREAN_FOREST;
    states.actorCount = 42;
    CoopActorState actor;
    actor.index = 17;
    actor.monster = MONSTER_GOBLIN_B;
    actor.pos = Vec3f(100.5f, -200, 300);
    actor.yaw = 1500;
    actor.sectorId = 3;
    actor.hp = -5;
    actor.aiState = Pursuing;
    actor.animation = ANIM_Walking;
    actor.actionTime = Duration::fromTicks(12);
    actor.actionLength = Duration::fromTicks(128);
    states.actors = {actor, CoopActorState()};

    CoopActorStates decoded = roundTrip(states);
    EXPECT_EQ(decoded.map, MAP_TULAREAN_FOREST);
    EXPECT_EQ(decoded.actorCount, 42);
    ASSERT_EQ(decoded.actors.size(), 2);
    const CoopActorState &first = decoded.actors[0];
    EXPECT_EQ(first.index, 17);
    EXPECT_EQ(first.monster, MONSTER_GOBLIN_B);
    EXPECT_EQ(first.pos, Vec3f(100.5f, -200, 300));
    EXPECT_EQ(first.yaw, 1500);
    EXPECT_EQ(first.sectorId, 3);
    EXPECT_EQ(first.hp, -5);
    EXPECT_EQ(first.aiState, Pursuing);
    EXPECT_EQ(first.animation, ANIM_Walking);
    EXPECT_EQ(first.actionTime, Duration::fromTicks(12));
    EXPECT_EQ(first.actionLength, Duration::fromTicks(128));
}

UNIT_TEST(CoopProtocol, ActorDamageRoundTrip) {
    // Actor damage keeps the player, the map, the actor and the amount.
    CoopActorDamage damage;
    damage.playerId = 2;
    damage.map = MAP_DEYJA;
    damage.index = 9;
    damage.damage = 123;

    CoopActorDamage decoded = roundTrip(damage);
    EXPECT_EQ(decoded.playerId, 2);
    EXPECT_EQ(decoded.map, MAP_DEYJA);
    EXPECT_EQ(decoded.index, 9);
    EXPECT_EQ(decoded.damage, 123);
}

UNIT_TEST(CoopProtocol, PartyHitRoundTrip) {
    // A party hit keeps the map, the attacker and its ability.
    CoopPartyHit hit;
    hit.map = MAP_BARROW_DOWNS;
    hit.attackerIndex = 11;
    hit.ability = ABILITY_SPELL2;

    CoopPartyHit decoded = roundTrip(hit);
    EXPECT_EQ(decoded.map, MAP_BARROW_DOWNS);
    EXPECT_EQ(decoded.attackerIndex, 11);
    EXPECT_EQ(decoded.ability, ABILITY_SPELL2);
}

UNIT_TEST(CoopProtocol, LootRequestRoundTrip) {
    // A loot request keeps the player, the map and the corpse.
    CoopLootRequest request;
    request.playerId = 3;
    request.map = MAP_HARMONDALE;
    request.index = 21;

    CoopLootRequest decoded = roundTrip(request);
    EXPECT_EQ(decoded.playerId, 3);
    EXPECT_EQ(decoded.map, MAP_HARMONDALE);
    EXPECT_EQ(decoded.index, 21);
}

UNIT_TEST(CoopProtocol, LootRoundTrip) {
    // Loot keeps its gold and its items, enchantments included.
    CoopLoot loot;
    loot.gold = 57;
    loot.foundItem = Item(ITEM_KEEN_LONGSWORD);
    loot.foundItem->standardEnchantment = ATTRIBUTE_MIGHT;
    loot.foundItem->standardEnchantmentStrength = 5;
    loot.extraItems = {Item(ITEM_POTION_CURE_WOUNDS)};

    CoopLoot decoded = roundTrip(loot);
    EXPECT_EQ(decoded.gold, 57);
    ASSERT_TRUE(decoded.foundItem);
    EXPECT_EQ(decoded.foundItem->itemId, ITEM_KEEN_LONGSWORD);
    EXPECT_EQ(decoded.foundItem->standardEnchantment, ATTRIBUTE_MIGHT);
    EXPECT_EQ(decoded.foundItem->standardEnchantmentStrength, 5);
    ASSERT_EQ(decoded.extraItems.size(), 1);
    EXPECT_EQ(decoded.extraItems[0].itemId, ITEM_POTION_CURE_WOUNDS);
}

UNIT_TEST(CoopProtocol, QuestBitRoundTrip) {
    // A quest bit change keeps the player, the bit and its new value.
    CoopQuestBit change;
    change.playerId = 1;
    change.bit = QBIT_ESCAPED_EMERALD_ISLE;
    change.value = true;

    CoopQuestBit decoded = roundTrip(change);
    EXPECT_EQ(decoded.playerId, 1);
    EXPECT_EQ(decoded.bit, QBIT_ESCAPED_EMERALD_ISLE);
    EXPECT_TRUE(decoded.value);
}

UNIT_TEST(CoopProtocol, QuestRewardsRoundTrip) {
    // Quest rewards keep the player and every reward with its character.
    CoopQuestRewards rewards;
    rewards.playerId = 2;
    rewards.rewards = {QuestReward{VAR_FixedGold, 1500, 0}, QuestReward{VAR_Experience, 7500, 3}};

    CoopQuestRewards decoded = roundTrip(rewards);
    EXPECT_EQ(decoded.playerId, 2);
    ASSERT_EQ(decoded.rewards.size(), 2);
    EXPECT_EQ(decoded.rewards[0].variable, VAR_FixedGold);
    EXPECT_EQ(decoded.rewards[0].value, 1500);
    EXPECT_EQ(decoded.rewards[0].characterIndex, 0);
    EXPECT_EQ(decoded.rewards[1].variable, VAR_Experience);
    EXPECT_EQ(decoded.rewards[1].value, 7500);
    EXPECT_EQ(decoded.rewards[1].characterIndex, 3);
}

UNIT_TEST(CoopProtocol, GroundItemTakenRoundTrip) {
    // A taken ground item keeps the player, the map, the item and where it lay.
    CoopGroundItemTaken taken;
    taken.playerId = 1;
    taken.map = MAP_EMERALD_ISLAND;
    taken.item = ITEM_POTION_CURE_WOUNDS;
    taken.pos = Vec3f(10, -20, 30.5f);

    CoopGroundItemTaken decoded = roundTrip(taken);
    EXPECT_EQ(decoded.playerId, 1);
    EXPECT_EQ(decoded.map, MAP_EMERALD_ISLAND);
    EXPECT_EQ(decoded.item, ITEM_POTION_CURE_WOUNDS);
    EXPECT_EQ(decoded.pos, Vec3f(10, -20, 30.5f));
}

UNIT_TEST(CoopProtocol, ClockRoundTrip) {
    // A clock keeps the time to the tick.
    CoopClock clock;
    clock.playingTime = Time::fromTicks(123456789);

    EXPECT_EQ(roundTrip(clock).playingTime, Time::fromTicks(123456789));
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
