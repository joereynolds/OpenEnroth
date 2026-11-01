#pragma once

#include "Engine/PartyEnums.h"
#include "Engine/Evt/EvtEnums.h"
#include "Engine/Objects/Item.h"
#include "Engine/Objects/ItemEnums.h"

#include "Core/Time/Duration.h"

#include "Library/Geometry/Vec.h"
#include "Engine/Objects/ActorEnums.h"

/**
 * Damage that the local party dealt to a monster.
 */
struct ActorHit {
    int actorId = -1;
    int damage = 0;
    Duration extraRecovery; // How much longer the monster takes to recover, a stun.
    Vec3f knockback; // Velocity the hit gave the monster.
};

/**
 * A monster's attack that landed on an actor standing in for another co-op player's party.
 */
struct StandInHit {
    int standInId = -1;
    int attackerId = -1;
    ActorAbility ability = ABILITY_ATTACK1;
};

/**
 * A quest bit the local party's scripts set or cleared.
 */
struct QuestBitChange {
    QuestBit bit = QBIT_INVALID;
    bool value = false;
};

/**
 * Something a script gave one of the local party's characters in the same run that changed a quest bit.
 */
struct QuestReward {
    EvtVariable variable = VAR_FixedGold;
    int value = 0;
    int characterIndex = 0;
};

/**
 * An item the local party picked up from the ground.
 */
struct GroundItemTaken {
    ItemId item = ITEM_NULL;
    Vec3f pos;
};

/**
 * An item the local party dropped or threw.
 */
struct DroppedItem {
    Item item;
    Vec3f pos; // Where the throw started.
    int yaw = 0;
};
