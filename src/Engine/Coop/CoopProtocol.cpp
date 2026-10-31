#include "CoopProtocol.h"

#include <cassert>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "Engine/Snapshots/EntitySnapshots.h"

#include "Library/Binary/BinarySerialization.h"
#include "Library/Snapshots/SnapshotSerialization.h"

#include "Utility/Exception.h"
#include "Utility/Streams/BlobOutputStream.h"
#include "Utility/Streams/MemoryInputStream.h"

static void serializeInt(int value, OutputStream *dst) {
    serialize(static_cast<int32_t>(value), dst);
}

static int deserializeInt(InputStream &src) {
    return fromStream<int32_t>(src);
}

static void serializeBool(bool value, OutputStream *dst) {
    serialize(static_cast<uint8_t>(value), dst);
}

static bool deserializeBool(InputStream &src) {
    return fromStream<uint8_t>(src) != 0;
}

static void serializeBlob(const Blob &value, OutputStream *dst) {
    assert(value.size() <= UINT32_MAX);
    serialize(static_cast<uint32_t>(value.size()), dst); // Same layout as an std::string, so that deserializeBlob can read it as one.
    serialize(std::span(static_cast<const char *>(value.data()), value.size()), dst);
}

static Blob deserializeBlob(InputStream &src) {
    return Blob::fromString(fromStream<std::string>(src));
}

static void serializeBody(const CoopHello &src, OutputStream *dst) {
    serializeInt(src.protocolVersion, dst);
    serialize(src.playerName, dst);
}

static void deserializeBody(InputStream &src, CoopHello *dst) {
    dst->protocolVersion = deserializeInt(src);
    deserialize(src, &dst->playerName);
}

static void serializeBody(const CoopWelcome &src, OutputStream *dst) {
    serializeInt(src.playerId, dst);
    serializeBlob(src.saveGame, dst);
}

static void deserializeBody(InputStream &src, CoopWelcome *dst) {
    dst->playerId = deserializeInt(src);
    dst->saveGame = deserializeBlob(src);
}

static void serializeBody(const CoopReject &src, OutputStream *dst) {
    serialize(src.reason, dst);
    serializeBool(src.retryLater, dst);
}

static void deserializeBody(InputStream &src, CoopReject *dst) {
    deserialize(src, &dst->reason);
    dst->retryLater = deserializeBool(src);
}

static void serializeBody(const CoopPartyPose &src, OutputStream *dst) {
    serializeInt(src.playerId, dst);
    serialize(std::to_underlying(src.map), dst);
    serialize(src.pos.x, dst);
    serialize(src.pos.y, dst);
    serialize(src.pos.z, dst);
    serializeInt(src.yaw, dst);
    serializeInt(src.pitch, dst);
    serializeBool(src.walking, dst);
}

static void deserializeBody(InputStream &src, CoopPartyPose *dst) {
    dst->playerId = deserializeInt(src);
    dst->map = static_cast<MapId>(fromStream<std::underlying_type_t<MapId>>(src));
    deserialize(src, &dst->pos.x);
    deserialize(src, &dst->pos.y);
    deserialize(src, &dst->pos.z);
    dst->yaw = deserializeInt(src);
    dst->pitch = deserializeInt(src);
    dst->walking = deserializeBool(src);
}

static void serializeBody(const CoopPlayerLeft &src, OutputStream *dst) {
    serializeInt(src.playerId, dst);
}

static void deserializeBody(InputStream &src, CoopPlayerLeft *dst) {
    dst->playerId = deserializeInt(src);
}

static void serializeDestination(const MapDestination &src, OutputStream *dst) {
    serialize(std::to_underlying(src.map()), dst);
    serialize(static_cast<uint8_t>(src.arrival().index()), dst);
    if (const MapStartPoint *startPoint = std::get_if<MapStartPoint>(&src.arrival())) {
        serialize(std::to_underlying(*startPoint), dst);
    } else if (const PartyPlacement *placement = std::get_if<PartyPlacement>(&src.arrival())) {
        serialize(placement->pos.x, dst);
        serialize(placement->pos.y, dst);
        serialize(placement->pos.z, dst);
        serializeInt(placement->yaw, dst);
        serializeInt(placement->pitch, dst);
        serializeInt(placement->zSpeed, dst);
    }
}

static MapDestination deserializeDestination(InputStream &src) {
    MapId map = static_cast<MapId>(fromStream<std::underlying_type_t<MapId>>(src));
    uint8_t arrival = fromStream<uint8_t>(src);
    switch (arrival) {
    case 0:
        if (map != MAP_INVALID)
            throw Exception("Coop travel destination to map {} doesn't say where to arrive", std::to_underlying(map));
        return MapDestination();
    case 1: {
        MapStartPoint startPoint = static_cast<MapStartPoint>(fromStream<std::underlying_type_t<MapStartPoint>>(src));
        if (map == MAP_INVALID)
            throw Exception("Coop travel destination has a start point but no map");
        return MapDestination(map, startPoint);
    }
    case 2: {
        PartyPlacement placement;
        deserialize(src, &placement.pos.x);
        deserialize(src, &placement.pos.y);
        deserialize(src, &placement.pos.z);
        placement.yaw = deserializeInt(src);
        placement.pitch = deserializeInt(src);
        placement.zSpeed = deserializeInt(src);
        return MapDestination(map, placement);
    }
    default:
        throw Exception("Unknown coop travel arrival type {}", arrival);
    }
}

static void serializeBody(const CoopTravelRequest &src, OutputStream *dst) {
    serializeDestination(src.destination, dst);
}

static void deserializeBody(InputStream &src, CoopTravelRequest *dst) {
    dst->destination = deserializeDestination(src);
}

static void serializeBody(const CoopTravelVote &src, OutputStream *dst) {
    serializeInt(src.voteId, dst);
    serialize(src.proposerName, dst);
    serializeDestination(src.destination, dst);
}

static void deserializeBody(InputStream &src, CoopTravelVote *dst) {
    dst->voteId = deserializeInt(src);
    deserialize(src, &dst->proposerName);
    dst->destination = deserializeDestination(src);
}

static void serializeBody(const CoopTravelAnswer &src, OutputStream *dst) {
    serializeInt(src.voteId, dst);
    serializeBool(src.accepted, dst);
}

static void deserializeBody(InputStream &src, CoopTravelAnswer *dst) {
    dst->voteId = deserializeInt(src);
    dst->accepted = deserializeBool(src);
}

static void serializeBody(const CoopTravelDecision &src, OutputStream *dst) {
    serializeInt(src.voteId, dst);
    serializeBool(src.approved, dst);
    serialize(src.declinedBy, dst);
    serializeDestination(src.destination, dst);
}

static void deserializeBody(InputStream &src, CoopTravelDecision *dst) {
    dst->voteId = deserializeInt(src);
    dst->approved = deserializeBool(src);
    deserialize(src, &dst->declinedBy);
    dst->destination = deserializeDestination(src);
}

static void serializeDuration(Duration value, OutputStream *dst) {
    serialize(static_cast<int64_t>(value.ticks()), dst);
}

static Duration deserializeDuration(InputStream &src) {
    return Duration::fromTicks(fromStream<int64_t>(src));
}

static void serializeBody(const CoopActorStates &src, OutputStream *dst) {
    serialize(std::to_underlying(src.map), dst);
    serializeInt(src.actorCount, dst);
    assert(src.actors.size() <= UINT32_MAX);
    serialize(static_cast<uint32_t>(src.actors.size()), dst);
    for (const CoopActorState &actor : src.actors) {
        serializeInt(actor.index, dst);
        serialize(std::to_underlying(actor.monster), dst);
        serialize(actor.pos.x, dst);
        serialize(actor.pos.y, dst);
        serialize(actor.pos.z, dst);
        serializeInt(actor.yaw, dst);
        serializeInt(actor.sectorId, dst);
        serializeInt(actor.hp, dst);
        serialize(std::to_underlying(actor.aiState), dst);
        serialize(std::to_underlying(actor.animation), dst);
        serializeDuration(actor.actionTime, dst);
        serializeDuration(actor.actionLength, dst);
    }
}

static void deserializeBody(InputStream &src, CoopActorStates *dst) {
    dst->map = static_cast<MapId>(fromStream<std::underlying_type_t<MapId>>(src));
    dst->actorCount = deserializeInt(src);
    uint32_t count = fromStream<uint32_t>(src);
    if (count > src.size() - src.position())
        throw Exception("Coop actor states claim {} actors, but only {} bytes are left", count, src.size() - src.position());
    dst->actors.resize(count);
    for (CoopActorState &actor : dst->actors) {
        actor.index = deserializeInt(src);
        actor.monster = static_cast<MonsterId>(fromStream<std::underlying_type_t<MonsterId>>(src));
        deserialize(src, &actor.pos.x);
        deserialize(src, &actor.pos.y);
        deserialize(src, &actor.pos.z);
        actor.yaw = deserializeInt(src);
        actor.sectorId = deserializeInt(src);
        actor.hp = deserializeInt(src);
        actor.aiState = static_cast<AIState>(fromStream<std::underlying_type_t<AIState>>(src));
        actor.animation = static_cast<ActorAnimation>(fromStream<std::underlying_type_t<ActorAnimation>>(src));
        actor.actionTime = deserializeDuration(src);
        actor.actionLength = deserializeDuration(src);
    }
}

static void serializeBody(const CoopActorDamage &src, OutputStream *dst) {
    serializeInt(src.playerId, dst);
    serialize(std::to_underlying(src.map), dst);
    serializeInt(src.index, dst);
    serializeInt(src.damage, dst);
}

static void deserializeBody(InputStream &src, CoopActorDamage *dst) {
    dst->playerId = deserializeInt(src);
    dst->map = static_cast<MapId>(fromStream<std::underlying_type_t<MapId>>(src));
    dst->index = deserializeInt(src);
    dst->damage = deserializeInt(src);
}

static void serializeBody(const CoopPartyHit &src, OutputStream *dst) {
    serialize(std::to_underlying(src.map), dst);
    serializeInt(src.attackerIndex, dst);
    serializeInt(std::to_underlying(src.ability), dst);
}

static void deserializeBody(InputStream &src, CoopPartyHit *dst) {
    dst->map = static_cast<MapId>(fromStream<std::underlying_type_t<MapId>>(src));
    dst->attackerIndex = deserializeInt(src);
    dst->ability = static_cast<ActorAbility>(deserializeInt(src));
}

static void serializeItem(const Item &item, OutputStream *dst) {
    Item_MM7 snapshotItem;
    snapshot(item, &snapshotItem, tags::context(ITEM_SLOT_INVALID));
    serialize(snapshotItem, dst);
}

static Item deserializeItem(InputStream &src) {
    Item_MM7 snapshotItem;
    deserialize(src, &snapshotItem);
    Item result;
    reconstruct(snapshotItem, &result);
    return result;
}

static void serializeBody(const CoopLootRequest &src, OutputStream *dst) {
    serializeInt(src.playerId, dst);
    serialize(std::to_underlying(src.map), dst);
    serializeInt(src.index, dst);
}

static void deserializeBody(InputStream &src, CoopLootRequest *dst) {
    dst->playerId = deserializeInt(src);
    dst->map = static_cast<MapId>(fromStream<std::underlying_type_t<MapId>>(src));
    dst->index = deserializeInt(src);
}

static void serializeBody(const CoopLoot &src, OutputStream *dst) {
    serializeInt(src.gold, dst);
    serializeBool(src.foundItem.has_value(), dst);
    if (src.foundItem)
        serializeItem(*src.foundItem, dst);
    serializeInt(src.extraItems.size(), dst);
    for (const Item &item : src.extraItems)
        serializeItem(item, dst);
}

static void deserializeBody(InputStream &src, CoopLoot *dst) {
    dst->gold = deserializeInt(src);
    if (deserializeBool(src))
        dst->foundItem = deserializeItem(src);
    int count = deserializeInt(src);
    if (count < 0 || count > 4)
        throw Exception("Coop loot has {} extra items, a corpse can't carry that many", count);
    for (int i = 0; i < count; i++)
        dst->extraItems.push_back(deserializeItem(src));
}

static void serializeBody(const CoopQuestBit &src, OutputStream *dst) {
    serializeInt(src.playerId, dst);
    serialize(std::to_underlying(src.bit), dst);
    serializeBool(src.value, dst);
}

static void deserializeBody(InputStream &src, CoopQuestBit *dst) {
    dst->playerId = deserializeInt(src);
    dst->bit = static_cast<QuestBit>(fromStream<std::underlying_type_t<QuestBit>>(src));
    dst->value = deserializeBool(src);
}

static void serializeBody(const CoopQuestRewards &src, OutputStream *dst) {
    serializeInt(src.playerId, dst);
    serializeInt(src.rewards.size(), dst);
    for (const QuestReward &reward : src.rewards) {
        serializeInt(std::to_underlying(reward.variable), dst);
        serializeInt(reward.value, dst);
        serializeInt(reward.characterIndex, dst);
    }
}

static void deserializeBody(InputStream &src, CoopQuestRewards *dst) {
    dst->playerId = deserializeInt(src);
    int count = deserializeInt(src);
    if (count < 0 || count > static_cast<int>(src.size() - src.position()))
        throw Exception("Coop quest rewards claim {} rewards, but only {} bytes are left", count, src.size() - src.position());
    dst->rewards.resize(count);
    for (QuestReward &reward : dst->rewards) {
        reward.variable = static_cast<EvtVariable>(deserializeInt(src));
        reward.value = deserializeInt(src);
        reward.characterIndex = deserializeInt(src);
    }
}

static void serializeBody(const CoopChest &src, OutputStream *dst) {
    serializeInt(src.playerId, dst);
    serialize(std::to_underlying(src.map), dst);
    serializeInt(src.chestId, dst);
    Chest_MM7 snapshotChest;
    snapshot(src.chest, &snapshotChest);
    serialize(snapshotChest, dst);
}

static void deserializeBody(InputStream &src, CoopChest *dst) {
    dst->playerId = deserializeInt(src);
    dst->map = static_cast<MapId>(fromStream<std::underlying_type_t<MapId>>(src));
    dst->chestId = deserializeInt(src);
    Chest_MM7 snapshotChest;
    deserialize(src, &snapshotChest);
    reconstruct(snapshotChest, &dst->chest, tags::context(dst->chestId));
}

template<size_t I = 0>
static CoopMessage deserializeAlternative(size_t index, InputStream &src) {
    if constexpr (I == std::variant_size_v<CoopMessage>) {
        throw Exception("Unknown coop message type {}", index);
    } else {
        if (index != I)
            return deserializeAlternative<I + 1>(index, src);

        std::variant_alternative_t<I, CoopMessage> result;
        deserializeBody(src, &result);
        return result;
    }
}

Blob encodeCoopMessage(const CoopMessage &message) {
    Blob result;
    BlobOutputStream stream(&result);
    serialize(static_cast<uint8_t>(message.index()), &stream); // The order of CoopMessage alternatives is part of the wire format.
    std::visit([&](const auto &body) { serializeBody(body, &stream); }, message);
    stream.close();
    return result;
}

CoopMessage decodeCoopMessage(const Blob &data) {
    MemoryInputStream stream(data.data(), data.size());
    CoopMessage result = deserializeAlternative(fromStream<uint8_t>(stream), stream);
    if (stream.position() != stream.size())
        throw Exception("Coop message has {} bytes of trailing data", stream.size() - stream.position());
    return result;
}
