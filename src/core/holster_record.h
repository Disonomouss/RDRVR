#pragma once

namespace rdrvr::holster_record {

// The record has current and previous 4x4 transforms, with axes as world vectors
// at +0/+4/+8 and position at +12. Premultiply both by the same world correction.
// Local bone matrices stay local: record-only and record*bone consumers then
// share the correction instead of only the latter receiving it.
inline void apply_delta(float* record, const float* rotation, const float* translation) {
    for (int half = 0; half < 2; ++half) {
        float* m = record + half * 16;
        for (int axis = 0; axis < 4; ++axis) {
            const float v[3] = {m[axis * 4], m[axis * 4 + 1], m[axis * 4 + 2]};
            for (int row = 0; row < 3; ++row)
                m[axis * 4 + row] = rotation[row * 3] * v[0] + rotation[row * 3 + 1] * v[1] +
                                    rotation[row * 3 + 2] * v[2] + (axis == 3 ? translation[row] : 0.0f);
        }
    }
}

}  // namespace rdrvr::holster_record
