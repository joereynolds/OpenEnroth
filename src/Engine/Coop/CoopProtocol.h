#pragma once

#include <string>
#include <variant>

#include "Engine/MapEnums.h"

#include "Library/Geometry/Vec.h"

#include "Utility/Memory/Blob.h"

/** Bumped on every change to the messages below, peers with different versions refuse to talk. */
constexpr int COOP_PROTOCOL_VERSION = 1;

/** Client to host, the first message on a new connection. */
struct CoopHello {
    int protocolVersion = COOP_PROTOCOL_VERSION;
    std::string playerName;
};

/** Host to client, accepts a `CoopHello` and hands over the world to start from. */
struct CoopWelcome {
    int playerId = -1;
    Blob saveGame; // The host's game, serialized the same way as a save file.
};

/** Host to client, refuses a `CoopHello`. The host disconnects right after sending it. */
struct CoopReject {
    std::string reason;
};

/** Where a party is and where it's looking. Clients send their own, the host relays them to everyone else. */
struct CoopPartyPose {
    int playerId = -1; // Filled in by the host, clients leave it at -1.
    MapId map = MAP_INVALID;
    Vec3f pos;
    int yaw = 0;
    int pitch = 0;
};

/** Host to clients, a player has disconnected. */
struct CoopPlayerLeft {
    int playerId = -1;
};

using CoopMessage = std::variant<CoopHello, CoopWelcome, CoopReject, CoopPartyPose, CoopPlayerLeft>;

/**
 * @param message                       Message to encode.
 * @return                              Wire representation of the message.
 */
Blob encodeCoopMessage(const CoopMessage &message);

/**
 * @param data                          Wire representation of a message, as received from a peer.
 * @return                              Decoded message.
 * @throws Exception                    If the data is not a valid message.
 */
CoopMessage decodeCoopMessage(const Blob &data);
