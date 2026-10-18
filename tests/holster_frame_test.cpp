#include "core/holster_frame.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using Vec = std::array<float, 3>;
static int checks = 0;
static void expect(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void near(const Vec& a, const Vec& b, const char* message) {
    for (int k = 0; k < 3; ++k) expect(std::fabs(a[k] - b[k]) < 0.003f, message);
}
static Vec placed(const Vec& center, float yaw, const Vec& local) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {center[0] + c * local[0] - s * local[2], center[1] + local[1], center[2] - s * local[0] - c * local[2]};
}
static Vec mirror(Vec p, const Vec& right, const Vec& root, const Vec& pelvis, bool valid = true) {
    rdrvr::holster_frame::mirror_body_point(p.data(), right.data(), root.data(), pelvis.data(), valid);
    return p;
}
// v0.8.11's inline reflection, for differential checks alongside the independent
// body-local expectations below. Preserve its result for every valid pose.
static Vec upstream_mirror(Vec p, const Vec& right, const Vec& root, const Vec& pelvis, bool valid = true) {
    const Vec& mid = valid ? pelvis : root;
    const float d = (p[0] - mid[0]) * right[0] + (p[2] - mid[2]) * right[2];
    for (int k = 0; k < 3; ++k) p[k] -= 2.0f * d * right[k];
    return p;
}
int main() {
    const Vec zero{}, right0{1, 0, 0}, shifted{0.5f, 1, 0}, hip{0.7f, 0.9f, 0.1f};
    const Vec expected{0.3f, 0.9f, 0.1f};
    near(mirror(hip, right0, zero, shifted), expected, "physical step: left hip stays with pelvis");
    expect(std::fabs(mirror(hip, right0, zero, shifted, false)[0] - expected[0]) > 0.99f,
           "regression fixture exposes original one-metre mirror error");

    // Define the desired zones in body-local coordinates, independently of the
    // world-space reflection: mirror local X, retain height and fore/aft distance.
    const Vec bones[] = {{0.2f, -0.12f, 0.05f}, {0.15f, 0.5f, -0.18f}}; // hip, shoulder
    const Vec offsets[] = {{-0.04f, 0.08f, 0.06f}, {0.04f, 0.03f, -0.02f}};
    const Vec roots[] = {{0, 0, 0}, {2100, 30, -3700}};
    const Vec shifts[] = {{0, 0, 0}, {0.5f, 0, 0}, {-0.4f, 0.2f, 0.7f}, {1.2f, -0.1f, -1.1f}};
    for (const auto& root : roots) for (const auto& shift : shifts) {
        const Vec pelvis{root[0] + shift[0], root[1] + 1 + shift[1], root[2] + shift[2]};
        for (int degrees = -180; degrees <= 180; degrees += 15) {
            const float yaw = static_cast<float>(degrees) * 0.0174532925f;
            const Vec right{std::cos(yaw), 0, -std::sin(yaw)};
            for (int zone = 0; zone < 2; ++zone) for (int twin = 0; twin < 2; ++twin) for (int lefty = 0; lefty < 2; ++lefty) {
                Vec local = bones[zone], base = placed(pelvis, yaw, local);
                near(mirror(base, right, root, pelvis), upstream_mirror(base, right, root, pelvis),
                     "valid pelvis retains upstream reflection");
                near(mirror(base, right, root, pelvis, false), upstream_mirror(base, right, root, pelvis, false),
                     "missing pelvis retains upstream root fallback");
                if (twin) { base = mirror(base, right, root, pelvis); local[0] = -local[0]; }
                const Vec offset = placed(zero, yaw, offsets[zone]);
                Vec position{};
                for (int k = 0; k < 3; ++k) { position[k] = base[k] + offset[k]; local[k] += offsets[zone][k]; }
                if (lefty) { position = mirror(position, right, root, pelvis); local[0] = -local[0]; }
                near(position, placed(pelvis, yaw, local), "body-relative position through turns, steps and handedness");
                near(mirror(mirror(position, right, root, pelvis), right, root, pelvis), position, "mirror twice restores position");
                expect(position[1] == base[1] + offset[1], "mirror preserves height");
                // Downstream riding/view adjustment is a common rigid transform.
                const Vec camera{root[0] + 0.3f, root[1] + 1.8f, root[2] - 0.2f};
                const auto turn = [&](const Vec& p) {
                    return placed(camera, 0.63f, Vec{p[0] - camera[0], p[1] - camera[1], -(p[2] - camera[2])});
                };
                near(turn(position), turn(placed(pelvis, yaw, local)), "riding view rotation keeps the corrected placement");
            }
            near(mirror(placed(root, yaw, bones[0]), right, root, root),
                 mirror(placed(root, yaw, bones[0]), right, root, root, false), "unshifted baseline unchanged");
        }
    }
    const Vec fallback = mirror(hip, right0, zero, shifted, false);
    for (int axis = 0; axis < 3; ++axis) {
        for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            Vec invalid = shifted; invalid[axis] = bad;
            near(mirror(hip, right0, zero, invalid), fallback, "invalid pelvis uses old root fallback");
        }
    }
    std::printf("PASS: %d holster frame checks\n", checks);
}
