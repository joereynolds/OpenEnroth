#pragma once

#include "Library/Geometry/Vec.h"

/**
 * Another player's party in a co-op game, as seen on the local map.
 */
struct RemoteParty {
    Vec3f pos; // Feet position, same as `Party::pos`.
    int yaw = 0; // Same units as `Party::_viewYaw`.
    bool walking = false;
};
