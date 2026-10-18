#pragma once

#include <cmath>
#include <cstdint>

#include "common/rdrvr_api.h"

namespace rdrvr::body_facing {

// Tracking stays in the scripted camera's frame. Only the drawn torso, shoulder
// positions and elbow hints use the physical turn. Never feed drawn_deg back to
// the camera, locomotion or controller-to-world transforms.
struct Heading {
    float tracking_deg = 0;
    float drawn_deg = 0;
    bool ride_turn_applied = false;  // the driver's view turn already moved these bones

    float remaining_ride_turn(float turn) const { return ride_turn_applied ? 0.0f : turn; }

    float holster_deg(bool turn_with_head, float zone_yaw) const {
        // The optional holster filter already measures yaw from the tracking
        // frame; adding it to drawn_deg would apply the physical turn twice.
        return turn_with_head ? tracking_deg + zone_yaw : drawn_deg;
    }
};

inline bool follows_physical_turn(bool anchored, bool lock_facing, bool actor_valid, uint32_t flags) {
    // A carriage driver shares the on-foot tracking frame. Horseback riding
    // has a separate mount/view-turn frame; cover retains its original pose.
    return anchored && lock_facing && actor_valid &&
           !(flags & (RDRVR_ACTOR_MOUNTED | RDRVR_ACTOR_IN_COVER));
}

// Owned by before_scene, like its other frame state. A missing head sample holds
// the last physical yaw, while a recenter or leaving this mode clears it. This
// observes recenter generations; it does not request a recenter or move the view.
class HeadFacing {
public:
    Heading sample(float tracking_deg, bool enabled, bool head_valid, float head_yaw, uint32_t generation, bool ride_turn_applied = false) {
        if (!enabled || generation != generation_) yaw_ = 0;
        generation_ = generation;
        if (enabled && head_valid && std::isfinite(head_yaw)) yaw_ = std::remainder(head_yaw, 360.0f);
        return {tracking_deg, tracking_deg + yaw_, enabled && ride_turn_applied};
    }

private:
    float yaw_ = 0;
    uint32_t generation_ = 0;
};

}  // namespace rdrvr::body_facing
