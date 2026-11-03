#pragma once

#include "Utility/Memory/Blob.h"

/**
 * Puts a guest's own adventurers into the host's world for a co-op game.
 *
 * The guest brings its characters, gold, food, party buffs and the item on its cursor. Everything else, including
 * the party's position, quest bits and time, comes from the host. Absolute times in the guest's party are shifted
 * from the guest's clock to the host's, so buffs and conditions keep their remaining durations. The guest's
 * hirelings stay behind, they are tied to the NPCs of the guest's own world.
 *
 * @param hostSave                      Host's game, as received when joining.
 * @param guestSave                     One of the guest's own saves.
 * @return                              Save to load on the guest's side.
 * @throws Exception                    If either save can't be read.
 */
Blob makeCoopGuestSave(const Blob &hostSave, const Blob &guestSave);
