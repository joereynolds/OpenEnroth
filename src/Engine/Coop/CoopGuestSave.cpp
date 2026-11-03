#include "CoopGuestSave.h"

#include "Engine/Party.h"
#include "Engine/SaveLoad.h"
#include "Engine/Objects/CharacterEnumFunctions.h"
#include "Engine/Snapshots/CompositeSnapshots.h"

#include "Library/Snapshots/SnapshotSerialization.h"

static void shiftTime(Time *time, Duration shift) {
    if (*time) // An unset time means "never", and stays unset.
        *time += shift;
}

static void shiftTimes(Character *character, Duration shift) {
    for (Condition condition : allConditions()) {
        Time time = character->conditions.get(condition);
        shiftTime(&time, shift);
        if (time)
            character->conditions.set(condition, time);
    }
    for (SpellBuff &buff : character->pCharacterBuffs)
        shiftTime(&buff.expireTime, shift);
    for (std::optional<LloydBeacon> &beacon : character->vBeacons)
        if (beacon)
            shiftTime(&beacon->uBeaconTime, shift);
}

Blob makeCoopGuestSave(const Blob &hostSave, const Blob &guestSave) {
    SaveGame host;
    SaveGame guest;
    deserialize(hostSave, &host, tags::via<SaveGame_MM7>);
    deserialize(guestSave, &guest, tags::via<SaveGame_MM7>);

    Party &world = host.party;
    const Party &own = guest.party;
    Duration shift = world.playing_time - own.playing_time;

    world.pCharacters = own.pCharacters;
    for (Character &character : world.pCharacters)
        shiftTimes(&character, shift);

    world.pPartyBuffs = own.pPartyBuffs;
    for (SpellBuff &buff : world.pPartyBuffs)
        shiftTime(&buff.expireTime, shift);

    world.uNumGold = own.uNumGold;
    world.uNumGoldInBank = own.uNumGoldInBank;
    world.uNumFoodRations = own.uNumFoodRations;
    world.pPickedItem = own.pPickedItem;
    world.uNumDeaths = own.uNumDeaths;
    world.uNumPrisonTerms = own.uNumPrisonTerms;
    world.uNumBountiesCollected = own.uNumBountiesCollected;
    world.pArcomageWins = own.pArcomageWins;
    world.uNumArcomageWins = own.uNumArcomageWins;
    world.uNumArcomageLoses = own.uNumArcomageLoses;
    world.uNumArenaWins = own.uNumArenaWins;

    world.pHirelings = {};
    world.pHirelingsSacrifice = {};
    world.pHireling1Name.clear();
    world.pHireling2Name.clear();
    world.hirelingScrollPosition = 0;

    Blob result;
    serialize(host, &result, tags::via<SaveGame_MM7>);
    return result;
}
