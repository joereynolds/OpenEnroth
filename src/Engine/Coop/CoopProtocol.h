#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "Engine/CoopEvents.h"
#include "Engine/MapEnums.h"
#include "Engine/PartyEnums.h"
#include "Engine/PartyPlacement.h"
#include "Engine/Objects/ActorEnums.h"
#include "Engine/Objects/Chest.h"
#include "Engine/Objects/Item.h"
#include "Engine/Objects/MonsterEnums.h"
#include "Engine/Objects/SpriteObject.h"
#include "Engine/Spells/SpellBuff.h"

#include "Core/Time/Duration.h"
#include "Core/Time/Time.h"

#include "Library/Geometry/Vec.h"

#include "Utility/Memory/Blob.h"

#include "CoopEnums.h"

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

/** Client to host, proposes something the whole group has to agree on. The host puts it to a vote. */
struct CoopVoteRequest {
    CoopVoteKind kind = COOP_VOTE_TRAVEL;
    MapDestination destination; // Where to travel, unused by other kinds of votes.
};

/** Host to clients, asks every player except the proposer whether to go along. */
struct CoopVote {
    int voteId = -1;
    CoopVoteKind kind = COOP_VOTE_TRAVEL;
    std::string proposerName;
    MapDestination destination;
};

/** Client to host, a player's answer to a `CoopVote`. */
struct CoopVoteAnswer {
    int voteId = -1;
    bool accepted = false;
};

/** Host to clients, how a vote ended. Everyone does what was voted on if it was approved. */
struct CoopVoteDecision {
    int voteId = -1; // -1 for a request that was turned down without a vote.
    CoopVoteKind kind = COOP_VOTE_TRAVEL;
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
    uint32_t activeBuffs = 0; // Bit `1 << buff` for every active `ActorBuff`, guests draw paralyzed monsters frozen.
    int shrinkPower = 0; // Power of `ACTOR_BUFF_SHRINK`, the only effect whose strength changes how a monster looks.
};

/** Host to clients, the state of the monsters on the host's map. */
struct CoopActorStates {
    MapId map = MAP_INVALID;
    int actorCount = 0; // Size of the host's actor list. Encounters spawn at random, so a client's list can differ.
    std::vector<CoopActorState> actors; // Only the monsters that changed, unless this is a periodic full update.
};

/** Client to host, the client's party hurt a monster on the client's map. */
struct CoopActorDamage {
    int playerId = -1; // Filled in by the host, clients leave it at -1.
    MapId map = MAP_INVALID;
    int index = -1; // Index in the map's actor list.
    int damage = 0; // Already reduced by the monster's resistances.
    Duration extraRecovery; // How much longer the monster takes to recover, a stun.
    Vec3f knockback; // Velocity the hit gave the monster.
};

/** Host to a client, a monster on the host's map attacked the client's party. */
struct CoopPartyHit {
    MapId map = MAP_INVALID;
    int attackerIndex = -1; // Index in the map's actor list.
    ActorAbility ability = ABILITY_ATTACK1;
};

/** Client to host, the client's party wants to loot a corpse. */
struct CoopLootRequest {
    int playerId = -1; // Filled in by the host, clients leave it at -1.
    MapId map = MAP_INVALID;
    int index = -1; // Index of the dead monster in the map's actor list.
};

/** Host to a client, what the client's party found on a corpse. */
struct CoopLoot {
    int gold = 0;
    std::optional<Item> foundItem;
    std::vector<Item> extraItems;
};

/** A quest bit changed in one player's game. Clients send their own, the host relays them to everyone else. */
struct CoopQuestBit {
    int playerId = -1; // Filled in by the host, clients leave it at -1.
    QuestBit bit = QBIT_INVALID;
    bool value = false;
};

/** What one player's game gave out for a quest, everyone gets it. Relayed like `CoopQuestBit`. */
struct CoopQuestRewards {
    int playerId = -1; // Filled in by the host, clients leave it at -1.
    std::vector<QuestReward> rewards;
};

/** A chest one player's game changed, everyone takes over its contents. Relayed like `CoopQuestBit`. */
struct CoopChest {
    int playerId = -1; // Filled in by the host, clients leave it at -1.
    MapId map = MAP_INVALID;
    int chestId = -1;
    Chest chest; // Position is left out, every game works it out on its own.
};

/** An item one player picked up from the ground, it's gone for everyone. Relayed like `CoopQuestBit`. */
struct CoopGroundItemTaken {
    int playerId = -1; // Filled in by the host, clients leave it at -1.
    MapId map = MAP_INVALID;
    ItemId item = ITEM_NULL;
    Vec3f pos; // Ground items have different indices in different games, they are matched by kind and place.
};

/** Host to clients, the host's game time. Clients follow it, the host's clock is the world's. */
struct CoopClock {
    Time playingTime;
};

/** Client to host, the client's party put a spell effect on a monster, like paralysis or halved armor. */
struct CoopActorBuff {
    int playerId = -1; // Filled in by the host, clients leave it at -1.
    MapId map = MAP_INVALID;
    int index = -1; // Index in the map's actor list.
    ActorBuff buff = ACTOR_BUFF_CHARM;
    SpellBuff value;
};

/** An item one player dropped, every game throws it the same way. Relayed like `CoopQuestBit`. */
struct CoopItemDropped {
    int playerId = -1; // Filled in by the host, clients leave it at -1.
    MapId map = MAP_INVALID;
    Item item;
    Vec3f pos; // Where the throw started.
    int yaw = 0;
};

/**
 * A projectile or spell effect that one player's party, or a monster in the host's game, launched. Relayed like
 * `CoopQuestBit`, other games only draw it.
 */
struct CoopSpriteLaunched {
    int playerId = -1; // Filled in by the host, clients leave it at -1.
    MapId map = MAP_INVALID;
    SpriteObject sprite;
};

/**
 * Host to client, accepts a `CoopHello` into the lobby of a game that hasn't started yet. A `CoopWelcome` follows once
 * the host starts it.
 */
struct CoopLobbyJoined {
    int playerId = -1;
};

/** One player waiting in the lobby. */
struct CoopLobbyPlayer {
    int playerId = -1;
    std::string name;
    bool ready = false; // Has a party to start with.

    friend bool operator==(const CoopLobbyPlayer &l, const CoopLobbyPlayer &r) = default;
};

/** Host to clients, everyone in the lobby with the host first. Sent whenever someone joins, leaves or gets ready. */
struct CoopLobbyStatus {
    std::vector<CoopLobbyPlayer> players;
};

/** Client to host, whether the client's party is ready for the game to start. */
struct CoopLobbyReady {
    bool ready = false;
};

using CoopMessage = std::variant<CoopHello, CoopWelcome, CoopReject, CoopPartyPose, CoopPlayerLeft,
                                 CoopVoteRequest, CoopVote, CoopVoteAnswer, CoopVoteDecision,
                                 CoopActorStates, CoopActorDamage, CoopPartyHit, CoopLootRequest, CoopLoot,
                                 CoopQuestBit, CoopQuestRewards, CoopChest, CoopGroundItemTaken, CoopClock,
                                 CoopActorBuff, CoopItemDropped, CoopSpriteLaunched, CoopLobbyJoined, CoopLobbyStatus,
                                 CoopLobbyReady>;

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
