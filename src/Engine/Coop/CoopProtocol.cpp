#include "CoopProtocol.h"

#include <cassert>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "Library/Binary/BinarySerialization.h"

#include "Utility/Exception.h"
#include "Utility/Streams/BlobOutputStream.h"
#include "Utility/Streams/MemoryInputStream.h"

static void serializeInt(int value, OutputStream *dst) {
    serialize(static_cast<int32_t>(value), dst);
}

static int deserializeInt(InputStream &src) {
    return fromStream<int32_t>(src);
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
}

static void deserializeBody(InputStream &src, CoopReject *dst) {
    deserialize(src, &dst->reason);
}

static void serializeBody(const CoopPartyPose &src, OutputStream *dst) {
    serializeInt(src.playerId, dst);
    serialize(std::to_underlying(src.map), dst);
    serialize(src.pos.x, dst);
    serialize(src.pos.y, dst);
    serialize(src.pos.z, dst);
    serializeInt(src.yaw, dst);
    serializeInt(src.pitch, dst);
    serialize(static_cast<uint8_t>(src.walking), dst);
}

static void deserializeBody(InputStream &src, CoopPartyPose *dst) {
    dst->playerId = deserializeInt(src);
    dst->map = static_cast<MapId>(fromStream<std::underlying_type_t<MapId>>(src));
    deserialize(src, &dst->pos.x);
    deserialize(src, &dst->pos.y);
    deserialize(src, &dst->pos.z);
    dst->yaw = deserializeInt(src);
    dst->pitch = deserializeInt(src);
    dst->walking = fromStream<uint8_t>(src) != 0;
}

static void serializeBody(const CoopPlayerLeft &src, OutputStream *dst) {
    serializeInt(src.playerId, dst);
}

static void deserializeBody(InputStream &src, CoopPlayerLeft *dst) {
    dst->playerId = deserializeInt(src);
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
