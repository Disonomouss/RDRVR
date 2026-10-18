#pragma once

#include <cmath>

namespace rdrvr::steering {

inline float stick_yaw_deg(bool driving, bool riding, bool stick_steer, float head_yaw, float view_heading, float mount_heading) {
    // Wagons have a DRIVING flag; their mount field may hold a vehicle or be zero. Keep their native steering
    // and braking axes: applying the walking head turn can reverse left/right.
    if (driving) return 0.0f;
    if (riding && !stick_steer)
        return std::fmod(view_heading + head_yaw - mount_heading + 540.0f, 360.0f) - 180.0f;
    return stick_steer ? 0.0f : head_yaw;
}

}  // namespace rdrvr::steering
