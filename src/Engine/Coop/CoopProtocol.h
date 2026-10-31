#pragma once

#include <string>
#include <variant>

#include "Engine/MapEnums.h"
#include "Engine/PartyPlacement.h"

#include "Library/Geometry/Vec.h"

#include "Utility/Memory/Blob.h"

/** Bumped on every change to the messages below, peers with different versions refuse to talk. */
constexpr int COOP_PROTOCOL_VERSION = 1;

constexpr int COOP_DEFAULT_PORT = 7347;

constexpr int COOP_MAX_CLIENTS = 3; // Four parties in total, counting the host's.

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
    bool walking = false;

    friend bool operator==(const CoopPartyPose &l, const CoopPartyPose &r) = default;
};

/** Host to clients, a player has disconnected. */
struct CoopPlayerLeft {
    int playerId = -1;
};

/** Client to host, asks for the group to travel to another map. The host puts it to a vote. */
struct CoopTravelRequest {
    MapDestination destination;
};

/** Host to clients, asks every player except the proposer whether to travel. */
struct CoopTravelVote {
    int voteId = -1;
    std::string proposerName;
    MapDestination destination;
};

/** Client to host, a player's answer to a `CoopTravelVote`. */
struct CoopTravelAnswer {
    int voteId = -1;
    bool accepted = false;
};

/** Host to clients, how a vote ended. Everyone travels to the destination if it was approved. */
struct CoopTravelDecision {
    int voteId = -1; // -1 for a request that was turned down without a vote.
    bool approved = false;
    std::string declinedBy; // Name of the player who said no, empty if approved or turned down without a vote.
    MapDestination destination;
};

using CoopMessage = std::variant<CoopHello, CoopWelcome, CoopReject, CoopPartyPose, CoopPlayerLeft,
                                 CoopTravelRequest, CoopTravelVote, CoopTravelAnswer, CoopTravelDecision>;

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
