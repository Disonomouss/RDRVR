#include "core/steering.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

int checks = 0;
void check(bool ok, const char* why) {
    ++checks;
    if (!ok) {
        std::cerr << "FAIL: " << why << '\n';
        std::exit(1);
    }
}
using Stick = std::array<float, 2>;
Stick turn(Stick s, float deg) {
    const float a = deg * 0.0174532925f;
    return {s[0] * std::cos(a) - s[1] * std::sin(a), s[0] * std::sin(a) + s[1] * std::cos(a)};
}

int main() {
    using rdrvr::steering::stick_yaw_deg;
    // Regression case: DRIVING, no horse mount, head yaw 102.9 degrees.
    // A left input must not acquire right/backward components in that state.
    const auto left = turn({-1.0f, 0.0f}, stick_yaw_deg(true, false, false, 102.9f, 135.9f, 0.0f));
    check(left[0] == -1.0f && left[1] == 0.0f, "carriage left must stay left at a 102.9-degree head angle");

    const std::array<Stick, 9> inputs{{{-1, 0}, {1, 0}, {0, 1}, {0, -1}, {-0.4f, 0.8f},
                                      {0.4f, 0.8f}, {-0.4f, -0.8f}, {0.4f, -0.8f}, {0, 0}}};
    // Includes slowing/reversing diagonals, partial steering, neutral, and
    // both turn directions. Speed and brake buttons do not enter this mapping.
    for (int head = -360; head <= 360; head += 15) {
        for (float view : {-179.0f, 0.0f, 179.0f}) {
            for (float mount : {-170.0f, 0.0f, 170.0f}) {
                for (bool riding : {false, true}) {
                    for (bool stick_steer : {false, true}) {
                        const float yaw = stick_yaw_deg(true, riding, stick_steer, static_cast<float>(head), view, mount);
                        check(yaw == 0, "driver steering is independent of head, camera, mount and horse preference");
                        for (const auto& s : inputs) {
                            const auto actual = turn(s, yaw);
                            check(actual == s, "driving preserves both stick axes, including slowing and neutral");
                        }
                        const float original = riding && !stick_steer
                            ? std::fmod(view + static_cast<float>(head) - mount + 540.0f, 360.0f) - 180.0f
                            : stick_steer ? 0.0f : static_cast<float>(head);
                        check(stick_yaw_deg(false, riding, stick_steer, static_cast<float>(head), view, mount) == original,
                              "all non-driving walking and horse steering results stay identical");
                    }
                }
            }
        }
    }
    // The mapper is stateless: braking, dismounting or changing modes cannot
    // leave behind a driver-specific yaw on the next walking frame.
    check(stick_yaw_deg(true, false, false, 180, 0, 0) == 0, "driving does not reverse after a physical half turn");
    check(stick_yaw_deg(false, false, false, 90, 0, 0) == 90, "dismount restores head-relative walking immediately");
    check(stick_yaw_deg(false, true, true, 90, 120, -150) == 0, "horse stick steering remains native");
    check(stick_yaw_deg(false, true, false, 90, 120, -150) == 0, "horse head steering retains wrap behavior");
    std::cout << "PASS: " << checks << " steering checks; driver axes preserved and non-driving mapping unchanged.\n";
    std::cout << "Input-transform tests only; vehicle physics and headset behavior need live verification.\n";
}
