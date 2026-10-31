#pragma once

#include "Engine/PartyEnums.h"
#include "Engine/Objects/ActorEnums.h"

/**
 * Damage that the local party dealt to a monster.
 */
struct ActorHit {
    int actorId = -1;
    int damage = 0;
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
