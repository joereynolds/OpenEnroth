#pragma once

#include <string>
#include <variant>
#include <vector>

#include "Engine/MapEnums.h"
#include "Engine/PartyPlacement.h"
#include "Engine/Objects/ActorEnums.h"
#include "Engine/Objects/MonsterEnums.h"

#include "Core/Time/Duration.h"

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
    bool retryLater = false; // The host can't take players right now, but may be able to later.
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

/** What a client needs to show one monster the way the host's game has it. */
struct CoopActorState {
    int index = -1; // Index in the map's actor list.
    MonsterId monster = MONSTER_INVALID;
    Vec3f pos;
    int yaw = 0;
    int sectorId = 0;
    int hp = 0;
    AIState aiState = Standing;
    ActorAnimation animation = ANIM_Standing;
    Duration actionTime;
    Duration actionLength;
};

/** Host to clients, the state of the monsters on the host's map. */
struct CoopActorStates {
    MapId map = MAP_INVALID;
    int actorCount = 0; // Size of the host's actor list. Encounters spawn at random, so a client's list can differ.
    std::vector<CoopActorState> actors; // Only the monsters that changed, unless this is a periodic full update.
};

/** Client to host, the client's party hurt a monster on the client's map. */
struct CoopActorDamage {
    MapId map = MAP_INVALID;
    int index = -1; // Index in the map's actor list.
    int damage = 0; // Already reduced by the monster's resistances.
};

/** Host to a client, a monster on the host's map attacked the client's party. */
struct CoopPartyHit {
    MapId map = MAP_INVALID;
    int attackerIndex = -1; // Index in the map's actor list.
    ActorAbility ability = ABILITY_ATTACK1;
};

using CoopMessage = std::variant<CoopHello, CoopWelcome, CoopReject, CoopPartyPose, CoopPlayerLeft,
                                 CoopTravelRequest, CoopTravelVote, CoopTravelAnswer, CoopTravelDecision,
                                 CoopActorStates, CoopActorDamage, CoopPartyHit>;

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
