#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace rdrvr::holster_draw_diag {

// Axes X/Y/Z as world vectors, then position. This reconstructs the submitted
// root transform, not the visible mesh's barrel or the result of GPU skinning.
using Pose = std::array<float, 12>;
inline Pose world_root(const float* record, const float* set, const float* offset, bool record_matrix) {
    Pose p{};
    for (int row = 0; row < 3; ++row) {
        if (!set) {
            for (int axis = 0; axis < 3; ++axis) p[axis * 3 + row] = record[axis * 4 + row];
            p[9 + row] = record[12 + row];
        } else if (record_matrix) {
            // Actual root is record * local set root, not the unchanged record.
            for (int axis = 0; axis < 3; ++axis)
                for (int k = 0; k < 3; ++k) p[axis * 3 + row] += record[k * 4 + row] * set[k * 4 + axis];
            p[9 + row] = record[12 + row];
            for (int k = 0; k < 3; ++k) p[9 + row] += record[k * 4 + row] * set[k * 4 + 3];
        } else {
            for (int axis = 0; axis < 3; ++axis) p[axis * 3 + row] = set[row * 4 + axis];
            p[9 + row] = set[row * 4 + 3] + offset[row];
        }
    }
    return p;
}

// Independent axis errors also expose reflections; a trace-only rotation angle
// could misleadingly classify a mirrored or scaled root as an ordinary rotation.
inline float max_axis_error_deg(const Pose& actual, const Pose& target) {
    float worst = 0;
    for (int axis = 0; axis < 3; ++axis) {
        float aa = 0, bb = 0, ab = 0;
        for (int row = 0; row < 3; ++row) {
            const float a = actual[axis * 3 + row], b = target[axis * 3 + row];
            aa += a * a; bb += b * b; ab += a * b;
        }
        if (!std::isfinite(aa) || !std::isfinite(bb) || !std::isfinite(ab) || aa < 1e-12f || bb < 1e-12f) return -1;
        const float c = std::clamp(ab / (std::sqrt(aa) * std::sqrt(bb)), -1.0f, 1.0f);
        worst = std::max(worst, std::acos(c) * 57.2957795f);
    }
    return worst;
}

}  // namespace rdrvr::holster_draw_diag
