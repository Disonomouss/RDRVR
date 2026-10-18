#include "core/body_facing.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using rdrvr::body_facing::HeadFacing;
using rdrvr::body_facing::Heading;
using rdrvr::body_facing::follows_physical_turn;
using Vec = std::array<double, 3>;
constexpr double rad = 0.017453292519943295;
int checks = 0;

void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

double distance(Vec a, Vec b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

Vec yaw(Vec v, double degrees) {
    const auto a = degrees * rad;
    return {std::cos(a) * v[0] + std::sin(a) * v[2], v[1], -std::sin(a) * v[0] + std::cos(a) * v[2]};
}

int main() {
    // Rotational covariance: the virtual shoulder, elbow hint and hip should
    // occupy the same places relative to the player throughout a physical turn.
    // Controller targets are independent inputs, never adjusted by this helper.
    const Vec left_shoulder{-0.18, 1.4, 0.08}, right_shoulder{0.18, 1.4, 0.08};
    const Vec left_pole{-0.6, -1, 0.3}, right_pole{0.6, -1, 0.3};
    const Vec right_hip{0.22, 0.9, 0.04}, hand{0.28, 1.25, -0.4};
    double legacy_max_error = 0;
    for (float base : {-177.0f, -90.0f, 0.0f, 90.0f, 177.0f}) {
        HeadFacing facing;
        for (int angle = -720; angle <= 720; ++angle) {
            const auto physical = static_cast<float>(angle);
            const Heading h = facing.sample(base, true, true, physical, 1);
            check(h.tracking_deg == base, "physical turns must not move the tracking/camera base");
            for (Vec point : {left_shoulder, right_shoulder, left_pole, right_pole, right_hip}) {
                const Vec expected = yaw(yaw(point, physical), base);
                check(distance(yaw(point, h.drawn_deg), expected) < 1e-5,
                      "body and elbow frame follow physical rotation across wrap and full turns");
            }
            const Vec tracked_hand = yaw(yaw(hand, physical), base);
            const Vec shoulder = yaw(right_shoulder, h.drawn_deg);
            check(std::abs(distance(shoulder, tracked_hand) - distance(right_shoulder, hand)) < 1e-5,
                  "physical turning alone must not change shoulder-to-tracked-hand reach");
            check(distance(yaw(right_hip, h.holster_deg(false, 0)), yaw(right_hip, h.drawn_deg)) < 1e-5,
                  "default holster frame agrees with the drawn body");
            check(distance(yaw(right_hip, h.holster_deg(true, physical)), yaw(right_hip, h.drawn_deg)) < 1e-5,
                  "existing head-follow holsters must not apply physical yaw twice");
            const double old_error = distance(yaw(right_shoulder, base), yaw(yaw(right_shoulder, physical), base));
            if (old_error > legacy_max_error) legacy_max_error = old_error;
        }
    }
    check(legacy_max_error > 0.39, "the same invariant detects the original fixed-shoulder defect");

    HeadFacing facing;
    auto h = facing.sample(177, true, true, 172, 1);
    check(h.drawn_deg == 349 && h.tracking_deg == 177, "near-half-turn regression: shoulders turn, camera heading stays at 177");
    h = facing.sample(-153, true, true, 172, 1);
    check(distance(yaw(right_hip, h.drawn_deg), yaw(yaw(right_hip, 349), 30)) < 1e-5,
          "stick turns apply immediately and exactly once on top of a physical turn");
    h = facing.sample(-153, true, false, 0, 1);
    check(h.drawn_deg == 19, "a missing tracking sample holds the last physical heading");
    h = facing.sample(-153, true, true, std::numeric_limits<float>::quiet_NaN(), 1);
    check(h.drawn_deg == 19, "nonfinite head yaw cannot poison body transforms");
    h = facing.sample(-153, true, false, 172, 2);
    check(h.drawn_deg == -153, "a new recenter generation discards stale physical yaw even without a pose");
    h = facing.sample(-153, true, true, -3, 2);
    check(h.drawn_deg == -156, "post-recenter tracking supplies the new physical yaw");
    h = facing.sample(-153, false, true, 90, 2);
    check(h.drawn_deg == -153, "ineligible modes clear physical heading");
    h = facing.sample(-153, true, false, 0, 2);
    check(h.drawn_deg == -153, "returning without tracking must not resurrect an old physical turn");
    check(h.holster_deg(true, 40) == -113, "an existing holster dead-zone result remains relative to tracking");

    // A carriage driver uses the anchored camera's tracking heading, as on
    // foot. Moving the vehicle or turning the view must not strand the elbows
    // when the wearer physically turns; the tracked hands remain independent.
    for (uint32_t flags : {RDRVR_ACTOR_DRIVING, RDRVR_ACTOR_DRIVING | RDRVR_ACTOR_CROUCHING,
                           RDRVR_ACTOR_DRIVING | RDRVR_ACTOR_VEHICLE}) {
        HeadFacing driver;
        for (float base : {-170.0f, 0.0f, 170.0f}) {
            for (int angle = -180; angle <= 180; angle += 15) {
                const float physical = static_cast<float>(angle);
                const auto carriage = driver.sample(base, follows_physical_turn(true, true, true, flags), true, physical, 3);
                const auto tracked = yaw(yaw(hand, physical), base);
                const auto expected_shoulder = yaw(yaw(right_shoulder, physical), base);
                check(distance(yaw(right_shoulder, carriage.drawn_deg), expected_shoulder) < 1e-5,
                      "carriage shoulders must follow a physical turn around the seated player");
                check(std::abs(distance(yaw(right_shoulder, carriage.drawn_deg), tracked) - distance(right_shoulder, hand)) < 1e-5,
                      "turning on a carriage must preserve shoulder-to-controller reach");
                check(distance(yaw(right_pole, carriage.drawn_deg), yaw(yaw(right_pole, physical), base)) < 1e-5,
                      "carriage elbow hints must turn with the shoulders");
                check(carriage.tracking_deg == base, "carriage body correction must not turn tracking or headset translation");
                check(distance(yaw(right_hip, carriage.holster_deg(true, physical)), yaw(right_hip, carriage.drawn_deg)) < 1e-5,
                      "carriage head-follow holsters must not apply physical yaw twice");
            }
        }
        auto end = driver.sample(20, follows_physical_turn(true, true, true, RDRVR_ACTOR_IN_COVER), true, 90, 3);
        check(end.drawn_deg == 20, "entering cover clears carriage physical yaw");
        end = driver.sample(20, follows_physical_turn(true, true, true, flags), false, 0, 3);
        check(end.drawn_deg == 20, "returning to the carriage without a pose must not restore stale yaw");
        end = driver.sample(20, true, true, 90, 3);
        end = driver.sample(110, true, false, 0, 4);
        check(end.drawn_deg == 110, "manual recenter on a carriage clears the previous physical yaw");
    }

    // Newer upstream supplies a separate vehicle view turn. Include it in
    // the driver's body once, without the holster's riding step adding it again.
    for (float vehicle : {-170.0f, 0.0f, 170.0f}) {
        for (float view_turn : {-90.0f, 0.0f, 90.0f}) {
            HeadFacing driver;
            const auto pose = driver.sample(vehicle + view_turn, true, true, 40, 5, true);
            check(distance(yaw(right_hip, pose.drawn_deg + pose.remaining_ride_turn(view_turn)),
                           yaw(yaw(yaw(right_hip, vehicle), view_turn), 40)) < 1e-5,
                  "driver view turn and physical turn reach holsters exactly once");
            check(pose.tracking_deg == vehicle + view_turn, "driver translation uses the camera tracking frame");
            const auto horse = driver.sample(vehicle, false, true, 40, 5, true);
            check(horse.remaining_ride_turn(view_turn) == view_turn,
                  "ineligible body modes retain the existing riding holster turn");
        }
    }

    for (uint32_t flags = 0; flags < 16; ++flags) {
        const bool follows = flags == 0 || flags == RDRVR_ACTOR_CROUCHING ||
                             flags == RDRVR_ACTOR_DRIVING || flags == (RDRVR_ACTOR_DRIVING | RDRVR_ACTOR_CROUCHING);
        check(follows_physical_turn(true, true, true, flags) == follows,
              "on-foot and carriage modes follow physical turning; horseback and cover retain their behavior");
        check(!follows_physical_turn(false, true, true, flags), "unanchored mode stays unchanged");
        check(!follows_physical_turn(true, false, true, flags), "LockFacing off stays unchanged");
        check(!follows_physical_turn(true, true, false, flags), "missing actor data cannot enable physical following");
    }
    std::cout << "PASS: " << checks << " checks; baseline shoulder error detected: " << legacy_max_error << " m\n";
    std::cout << "Math/policy validation only; no game renderer or headset was exercised.\n";
}
