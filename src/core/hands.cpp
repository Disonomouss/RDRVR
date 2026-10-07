#include "core/hands.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <vector>

#include "core/config.h"
#include "core/log.h"

namespace rdrvr::hands {
namespace {

std::mutex g_mutex;
Hand g_hand[2];  // synthetic
Hand g_real[2];
bool g_synthetic = false;

}  // namespace

void init() {
    g_synthetic = config::get_bool("Debug", "SyntheticHands", false);
    if (g_synthetic) log::warn("[hands] SYNTHETIC hand source on (test only): poses and buttons come from the command channel");
}

bool synthetic() { return g_synthetic; }

Hand get(int hand) {
    std::lock_guard lock(g_mutex);
    return g_synthetic ? g_hand[hand & 1] : g_real[hand & 1];
}

Hand get_real(int hand) {
    std::lock_guard lock(g_mutex);
    return g_real[hand & 1];
}

void set_real(int hand, const Hand& h) {
    std::lock_guard lock(g_mutex);
    uint64_t n = g_real[hand & 1].updates + 1;
    g_real[hand & 1] = h;
    g_real[hand & 1].updates = n;
}

std::string command(const std::string& line) {
    if (!g_synthetic) return "ERROR [Debug] SyntheticHands is off";
    std::istringstream in(line);
    std::string word, side;
    in >> word >> side;  // "hand", "l" | "r"
    int h = side == "l" ? 0 : side == "r" ? 1 : -1;
    if (h < 0) return "ERROR hand l|r x y z qx qy qz qw trigger grip buttons sx sy";
    std::vector<float> v;
    float f;
    while (in >> f) v.push_back(f);
    if (v.size() < 7) return "ERROR need at least the pose (x y z qx qy qz qw)";
    std::lock_guard lock(g_mutex);
    Hand& d = g_hand[h];
    for (int k = 0; k < 3; ++k) d.pos[k] = v[k];
    for (int k = 0; k < 4; ++k) d.rot[k] = v[3 + k];
    d.trigger = v.size() > 7 ? v[7] : 0;
    d.grip = v.size() > 8 ? v[8] : 0;
    d.buttons = v.size() > 9 ? static_cast<uint32_t>(v[9]) : 0;
    d.stick[0] = v.size() > 10 ? v[10] : 0;
    d.stick[1] = v.size() > 11 ? v[11] : 0;
    d.valid = true;
    d.connected = true;
    ++d.updates;
    char out[200];
    std::snprintf(out, sizeof(out), "hand %s: pos (%.3f %.3f %.3f) trigger %.2f grip %.2f buttons %u stick (%.2f %.2f), update %llu",
                  h ? "right" : "left", d.pos[0], d.pos[1], d.pos[2], d.trigger, d.grip, d.buttons, d.stick[0], d.stick[1],
                  static_cast<unsigned long long>(d.updates));
    return out;
}

}  // namespace rdrvr::hands
