#pragma once

#include <cmath>

namespace rdrvr::holster_frame {

// The corrected pelvis follows the drawn body; the skeleton root deliberately
// does not receive BodyFollowsHead/torso displacement. Reflecting around that
// root would send a mirrored holster away by twice the body's lateral shift.
// right is the same unit, horizontal axis used to place the holster offsets.
inline void mirror_body_point(float point[3], const float right[3], const float root[3],
                              const float pelvis[3], bool pelvis_ok) {
    const bool have_pelvis = pelvis_ok && std::isfinite(pelvis[0]) && std::isfinite(pelvis[1]) && std::isfinite(pelvis[2]);
    const float* center = have_pelvis ? pelvis : root;
    const float d = (point[0] - center[0]) * right[0] + (point[2] - center[2]) * right[2];
    for (int k = 0; k < 3; ++k) point[k] -= 2.0f * d * right[k];
}

}  // namespace rdrvr::holster_frame
