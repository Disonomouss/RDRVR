#pragma once

// Compatibility for pre-customizable-belt profiles. No values are written or migrated.
namespace rdrvr::legacy_holsters {
template<class Has>
bool selected(Has has) {
    const bool old = has("LowerBackOffset") || has("LowerBackRadius") ||
                     has("LowerBackEnabled") || has("LowerBackWeapon");
    const bool current = has("LassoOffset") || has("LassoRadius") ||
                         has("LassoEnabled") || has("LassoWeapon");
    return old && !current;
}

template<class Zone>
void defaults(Zone* zones) {
    // Index 5 remains the drawn pelvis in BodyPoints; index 6 is the new rope bone.
    // Keep the old key so loading, menu adjustments and saves all use the same offset.
    zones[7] = {"LowerBack", "Lower back (knife and lasso)", 5, {0, 3, -1},
                {0.0f, 0.05f, -0.12f}, 0.16f, -1};
    zones[3].label = "Left hip (knife and lasso; off: moved to the lower back)";
    zones[3].slots[1] = 3;
    zones[3].enabled = false;
}
} // namespace rdrvr::legacy_holsters
