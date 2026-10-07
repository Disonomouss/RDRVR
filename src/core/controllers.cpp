#include "core/controllers.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/hands.h"
#include "core/log.h"
#include "core/xinput.h"

namespace rdrvr::controllers {
namespace {

std::mutex g_mutex;  // sync runs on whichever thread owns the frame loop (the idle loop or the presenting thread)
XrInstance g_inst = XR_NULL_HANDLE;
XrSession g_session = XR_NULL_HANDLE;
XrActionSet g_set = XR_NULL_HANDLE;
XrPath g_hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
// Action names are what the simulator matches its emulated inputs by ("trigger", "grip", "thumbstick", "primary",
// "secondary", "menu"); real runtimes only go by the bindings.
XrAction g_aim = XR_NULL_HANDLE, g_grip_pose = XR_NULL_HANDLE, g_trigger = XR_NULL_HANDLE, g_squeeze = XR_NULL_HANDLE,
         g_stick = XR_NULL_HANDLE, g_stick_click = XR_NULL_HANDLE, g_primary = XR_NULL_HANDLE, g_secondary = XR_NULL_HANDLE,
         g_menu = XR_NULL_HANDLE, g_haptic = XR_NULL_HANDLE, g_pad = XR_NULL_HANDLE, g_pad_click = XR_NULL_HANDLE;
XrSpace g_aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE}, g_grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
bool g_ready = false;
// The controller types (their interaction profiles): the fit's key and the trackpad buttons go by them
enum Kind { kTouch, kIndex, kVive, kWMR, kSimple, kPico4, kPicoNeo3, kHP, kCosmos, kFocus3, kTouchPro, kTouchPlus, kOdyssey, kKinds };
const char* const kKindProfile[kKinds] = {
    "/interaction_profiles/oculus/touch_controller",       "/interaction_profiles/valve/index_controller",
    "/interaction_profiles/htc/vive_controller",           "/interaction_profiles/microsoft/motion_controller",
    "/interaction_profiles/khr/simple_controller",         "/interaction_profiles/bytedance/pico4_controller",
    "/interaction_profiles/bytedance/pico_neo3_controller", "/interaction_profiles/hp/mixed_reality_controller",
    "/interaction_profiles/htc/vive_cosmos_controller",    "/interaction_profiles/htc/vive_focus3_controller",
    "/interaction_profiles/facebook/touch_controller_pro", "/interaction_profiles/meta/touch_plus_controller",
    "/interaction_profiles/samsung/odyssey_controller"};
const char* const kKindName[kKinds] = {"Touch",  "Index",     "Vive",       "WMR",      "Simple",    "Pico4",   "PicoNeo3",
                                       "HPReverb", "ViveCosmos", "ViveFocus3", "TouchPro", "TouchPlus", "Odyssey"};
int g_kind[2] = {-1, -1};  // under g_mutex (read without it by fit_name: an int)
struct Fit {
    float off[3] = {0, 0, 0};
    float ang[3] = {0, 0, 0};  // degrees: pitch (about x), yaw (y), roll (z)
};
Fit g_fit[kKinds];  // under g_mutex
bool g_pad_buttons = true;  // [Controls] TrackpadButtons
std::vector<std::string> g_exts;  // the profiles' extensions xr.cpp enabled (before create)
XrVersion g_api = XR_MAKE_VERSION(1, 0, 0);
std::atomic<bool> g_any_tracked{false};
std::atomic<bool> g_profile_dirty{false};
char g_profile[2][96] = {"none", "none"};
std::atomic<uint64_t> g_syncs{0}, g_not_focused{0}, g_pulses{0};
std::atomic<uint64_t> g_relocates{0};
float g_relocate_max_mm = 0.0f, g_relocate_last_mm = 0.0f;  // the latch's move of a hand from the sync's pose (under g_mutex)
// haptics (under g_mutex)
bool g_haptics = true;
float g_haptic_gain = 1.0f;
float g_last_amp[2] = {0, 0};
double g_last_pulse_ms[2] = {0, 0};
// a pulse asked for by the mod (a holster under the hand): amplitude and length, taken by the next sync
std::atomic<float> g_req_amp[2] = {0.0f, 0.0f};
std::atomic<int> g_req_ms[2] = {0, 0};

XrPath path(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_inst, s, &p);
    return p;
}

bool has_ext(const char* e) {
    for (const std::string& x : g_exts)
        if (x == e) return true;
    return false;
}

void qmul(const float a[4], const float b[4], float o[4]) {  // x y z w
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
void qrot(const float q[4], const float v[3], float o[3]) {
    const float p[4] = {v[0], v[1], v[2], 0.0f}, c[4] = {-q[0], -q[1], -q[2], q[3]};
    float t[4], r[4];
    qmul(q, p, t);
    qmul(t, c, r);
    o[0] = r[0], o[1] = r[1], o[2] = r[2];
}
// The hand's fit (its type's, the left mirrored) applied to a located pose: p' = p + R off, R' = R Q(yaw pitch roll)
void apply_fit(int h, float pos[3], float rot[4]) {
    const int k = g_kind[h];
    if (k < 0) return;
    const Fit& f = g_fit[k];
    if (!f.off[0] && !f.off[1] && !f.off[2] && !f.ang[0] && !f.ang[1] && !f.ang[2]) return;
    const float sx = h == 0 ? -1.0f : 1.0f;
    const float off[3] = {sx * f.off[0], f.off[1], -f.off[2]};  // forward is -z in OpenXR's controller axes
    float d[3];
    qrot(rot, off, d);
    for (int i = 0; i < 3; ++i) pos[i] += d[i];
    constexpr float kRad = 0.0174532925f;
    const float p = 0.5f * f.ang[0] * kRad, y = 0.5f * sx * f.ang[1] * kRad, r = 0.5f * sx * f.ang[2] * kRad;
    const float qy[4] = {0, std::sin(y), 0, std::cos(y)}, qx[4] = {std::sin(p), 0, 0, std::cos(p)}, qz[4] = {0, 0, std::sin(r), std::cos(r)};
    float a[4], q[4], o[4];
    qmul(qy, qx, a);
    qmul(a, qz, q);
    qmul(rot, q, o);
    std::memcpy(rot, o, sizeof(o));
}

bool make_action(const char* name, const char* loc, XrActionType type, XrAction* out) {
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
    std::snprintf(ai.actionName, sizeof(ai.actionName), "%s", name);
    std::snprintf(ai.localizedActionName, sizeof(ai.localizedActionName), "%s", loc);
    ai.actionType = type;
    ai.countSubactionPaths = 2;
    ai.subactionPaths = g_hand_path;
    XrResult r = xrCreateAction(g_set, &ai, out);
    if (XR_FAILED(r)) log::warn("[controllers] xrCreateAction(%s) -> %d", name, static_cast<int>(r));
    return XR_SUCCEEDED(r);
}

struct Bind {
    XrAction* action;
    std::string path;
};

void suggest(const char* profile, const std::vector<Bind>& binds) {
    std::vector<XrActionSuggestedBinding> b;
    for (const Bind& x : binds) b.push_back({*x.action, path(x.path.c_str())});
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = path(profile);
    s.countSuggestedBindings = static_cast<uint32_t>(b.size());
    s.suggestedBindings = b.data();
    XrResult r = xrSuggestInteractionProfileBindings(g_inst, &s);
    log::info("[controllers] bindings for %s (%zu) -> %d", profile, b.size(), static_cast<int>(r));
}

void suggest_all() {
    // Both hands of each input, where the profile has it.
    auto both = [](std::vector<Bind>& v, XrAction* a, const char* input) {
        for (const char* hand : {"left", "right"}) v.push_back({a, std::string("/user/hand/") + hand + input});
    };
    {  // Meta Quest Touch (also Rift S / Rift); the right menu button is the system's
        std::vector<Bind> v;
        both(v, &g_aim, "/input/aim/pose");
        both(v, &g_grip_pose, "/input/grip/pose");
        both(v, &g_trigger, "/input/trigger/value");
        both(v, &g_squeeze, "/input/squeeze/value");
        both(v, &g_stick, "/input/thumbstick");
        both(v, &g_stick_click, "/input/thumbstick/click");
        both(v, &g_haptic, "/output/haptic");
        v.push_back({&g_primary, "/user/hand/left/input/x/click"});
        v.push_back({&g_secondary, "/user/hand/left/input/y/click"});
        v.push_back({&g_primary, "/user/hand/right/input/a/click"});
        v.push_back({&g_secondary, "/user/hand/right/input/b/click"});
        v.push_back({&g_menu, "/user/hand/left/input/menu/click"});
        suggest("/interaction_profiles/oculus/touch_controller", v);
    }
    {  // Valve Index (no menu button an application may bind)
        std::vector<Bind> v;
        both(v, &g_aim, "/input/aim/pose");
        both(v, &g_grip_pose, "/input/grip/pose");
        both(v, &g_trigger, "/input/trigger/value");
        both(v, &g_squeeze, "/input/squeeze/value");
        both(v, &g_stick, "/input/thumbstick");
        both(v, &g_stick_click, "/input/thumbstick/click");
        both(v, &g_primary, "/input/a/click");
        both(v, &g_secondary, "/input/b/click");
        both(v, &g_haptic, "/output/haptic");
        suggest("/interaction_profiles/valve/index_controller", v);
    }
    {  // HTC Vive wands: the trackpad as the stick
        std::vector<Bind> v;
        both(v, &g_aim, "/input/aim/pose");
        both(v, &g_grip_pose, "/input/grip/pose");
        both(v, &g_trigger, "/input/trigger/value");
        both(v, &g_squeeze, "/input/squeeze/click");
        both(v, &g_stick, "/input/trackpad");
        both(v, &g_pad, "/input/trackpad");
        both(v, &g_pad_click, "/input/trackpad/click");  // A / B at its ends, the stick click in the middle (sync)
        both(v, &g_menu, "/input/menu/click");
        both(v, &g_haptic, "/output/haptic");
        suggest("/interaction_profiles/htc/vive_controller", v);
    }
    {  // Windows Mixed Reality
        std::vector<Bind> v;
        both(v, &g_aim, "/input/aim/pose");
        both(v, &g_grip_pose, "/input/grip/pose");
        both(v, &g_trigger, "/input/trigger/value");
        both(v, &g_squeeze, "/input/squeeze/click");
        both(v, &g_stick, "/input/thumbstick");
        both(v, &g_stick_click, "/input/thumbstick/click");
        both(v, &g_menu, "/input/menu/click");
        both(v, &g_haptic, "/output/haptic");
        both(v, &g_pad, "/input/trackpad");  // A / B from the trackpad's click (sync)
        both(v, &g_pad_click, "/input/trackpad/click");
        suggest("/interaction_profiles/microsoft/motion_controller", v);
        if (has_ext("XR_EXT_samsung_odyssey_controller") || g_api >= XR_MAKE_VERSION(1, 1, 0))
            suggest("/interaction_profiles/samsung/odyssey_controller", v);  // the same inputs
    }
    // 2026-10-07: the controllers whose profiles come with an extension (or OpenXR 1.1): X / Y and the menu on the
    // left, A / B on the right, the trigger, the grip, the stick and its click, the haptics
    auto face = [&](std::vector<Bind>& v, const char* squeeze, bool left_menu, bool right_menu) {
        both(v, &g_aim, "/input/aim/pose");
        both(v, &g_grip_pose, "/input/grip/pose");
        both(v, &g_trigger, "/input/trigger/value");
        both(v, &g_squeeze, squeeze);
        both(v, &g_stick, "/input/thumbstick");
        both(v, &g_stick_click, "/input/thumbstick/click");
        both(v, &g_haptic, "/output/haptic");
        v.push_back({&g_primary, "/user/hand/left/input/x/click"});
        v.push_back({&g_secondary, "/user/hand/left/input/y/click"});
        v.push_back({&g_primary, "/user/hand/right/input/a/click"});
        v.push_back({&g_secondary, "/user/hand/right/input/b/click"});
        if (left_menu) v.push_back({&g_menu, "/user/hand/left/input/menu/click"});
        if (right_menu) v.push_back({&g_menu, "/user/hand/right/input/menu/click"});
    };
    const bool v11 = g_api >= XR_MAKE_VERSION(1, 1, 0);
    struct Extra {
        const char* profile;
        const char* ext;
        const char* squeeze;
        bool left_menu, right_menu;
    };
    static const Extra kExtra[] = {
        {"/interaction_profiles/bytedance/pico4_controller", "XR_BD_controller_interaction", "/input/squeeze/value", true, false},
        {"/interaction_profiles/bytedance/pico_neo3_controller", "XR_BD_controller_interaction", "/input/squeeze/value", true, true},
        {"/interaction_profiles/hp/mixed_reality_controller", "XR_EXT_hp_mixed_reality_controller", "/input/squeeze/value", true, true},
        {"/interaction_profiles/htc/vive_cosmos_controller", "XR_HTC_vive_cosmos_controller_interaction", "/input/squeeze/click", true, false},
        {"/interaction_profiles/htc/vive_focus3_controller", "XR_HTC_vive_focus3_controller_interaction", "/input/squeeze/click", true, false},
        {"/interaction_profiles/facebook/touch_controller_pro", "XR_FB_touch_controller_pro", "/input/squeeze/value", true, false},
        {"/interaction_profiles/meta/touch_plus_controller", "XR_META_touch_controller_plus", "/input/squeeze/value", true, false},
    };
    for (const Extra& e : kExtra) {
        if (!has_ext(e.ext) && !v11) continue;  // the profile needs its extension on a 1.0 runtime
        std::vector<Bind> v;
        face(v, e.squeeze, e.left_menu, e.right_menu);
        suggest(e.profile, v);
    }
    {  // Khronos simple controller (any runtime's fallback)
        std::vector<Bind> v;
        both(v, &g_aim, "/input/aim/pose");
        both(v, &g_grip_pose, "/input/grip/pose");
        both(v, &g_trigger, "/input/select/click");
        both(v, &g_menu, "/input/menu/click");
        both(v, &g_haptic, "/output/haptic");
        suggest("/interaction_profiles/khr/simple_controller", v);
    }
}

XrSpace make_space(XrAction a, int hand) {
    XrActionSpaceCreateInfo si{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    si.action = a;
    si.subactionPath = g_hand_path[hand];
    si.poseInActionSpace.orientation.w = 1.0f;
    XrSpace s = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateActionSpace(g_session, &si, &s))) return XR_NULL_HANDLE;
    return s;
}

float get_float(XrAction a, int hand, bool* active) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = g_hand_path[hand];
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(g_session, &gi, &st)) || !st.isActive) return 0;
    if (active) *active = true;
    return st.currentState;
}

bool get_bool(XrAction a, int hand, bool* active) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = g_hand_path[hand];
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(g_session, &gi, &st)) || !st.isActive) return false;
    if (active) *active = true;
    return st.currentState == XR_TRUE;
}

XrVector2f get_vec2(XrAction a, int hand, bool* active) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = g_hand_path[hand];
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_FAILED(xrGetActionStateVector2f(g_session, &gi, &st)) || !st.isActive) return {0, 0};
    if (active) *active = true;
    return st.currentState;
}

bool locate(XrSpace s, XrSpace base, XrTime t, float pos[3], float rot[4]) {
    if (s == XR_NULL_HANDLE) return false;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(s, base, t, &loc))) return false;
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((loc.locationFlags & kValid) != kValid) return false;
    pos[0] = loc.pose.position.x;
    pos[1] = loc.pose.position.y;
    pos[2] = loc.pose.position.z;
    rot[0] = loc.pose.orientation.x;
    rot[1] = loc.pose.orientation.y;
    rot[2] = loc.pose.orientation.z;
    rot[3] = loc.pose.orientation.w;
    return true;
}

// The game's two motors onto the two hands: re-issued every 50 ms (100 ms pulses) while on, stopped when they stop.
void haptics(const bool tracked[2]) {
    if (!g_haptics) return;
    uint32_t r = xinput::rumble();
    float amp[2] = {static_cast<float>(r >> 16) / 65535.0f, static_cast<float>(r & 0xFFFF) / 65535.0f};
    double now = log::now_ms();
    for (int h = 0; h < 2; ++h) {
        float a = amp[h] * g_haptic_gain;
        if (a > 1) a = 1;
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = g_haptic;
        hi.subactionPath = g_hand_path[h];
        const int req_ms = g_req_ms[h].exchange(0);
        if (req_ms > 0 && tracked[h] && a <= 0.01f) {
            XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
            v.amplitude = g_req_amp[h].load();
            v.duration = static_cast<XrDuration>(req_ms) * 1000000;
            v.frequency = XR_FREQUENCY_UNSPECIFIED;
            xrApplyHapticFeedback(g_session, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
            g_pulses.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (a > 0.01f && tracked[h]) {
            if (std::fabs(a - g_last_amp[h]) > 0.02f || now - g_last_pulse_ms[h] >= 50) {
                XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
                v.amplitude = a;
                v.duration = 100000000;  // 100 ms, renewed while the game holds the motor on
                v.frequency = XR_FREQUENCY_UNSPECIFIED;
                xrApplyHapticFeedback(g_session, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
                g_last_pulse_ms[h] = now;
                g_last_amp[h] = a;
                g_pulses.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (g_last_amp[h] > 0) {
            xrStopHapticFeedback(g_session, &hi);
            g_last_amp[h] = 0;
        }
    }
}

void read_profiles() {
    for (int h = 0; h < 2; ++h) {
        XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
        std::snprintf(g_profile[h], sizeof(g_profile[h]), "none");
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(g_session, g_hand_path[h], &ps)) && ps.interactionProfile != XR_NULL_PATH) {
            char buf[XR_MAX_PATH_LENGTH];
            uint32_t n = 0;
            if (XR_SUCCEEDED(xrPathToString(g_inst, ps.interactionProfile, sizeof(buf), &n, buf)))
                std::snprintf(g_profile[h], sizeof(g_profile[h]), "%s", buf);
        }
    }
    for (int h = 0; h < 2; ++h) {
        g_kind[h] = -1;
        for (int k = 0; k < kKinds; ++k)
            if (std::strcmp(g_profile[h], kKindProfile[k]) == 0) g_kind[h] = k;
    }
    log::info("[controllers] interaction profile: left %s, right %s", g_profile[0], g_profile[1]);
}

}  // namespace

bool create(XrInstance inst, XrSession session) {
    if (!config::get_bool("Controls", "Controllers", true)) {
        log::info("[controllers] off ([Controls] Controllers=0)");
        return false;
    }
    std::lock_guard lock(g_mutex);
    g_inst = inst;
    g_session = session;
    g_haptics = config::get_bool("Input", "Haptics", true);
    g_haptic_gain = config::get_float("Input", "HapticStrength", 1.0f);
    g_pad_buttons = config::get_bool("Controls", "TrackpadButtons", true);
    for (int k = 0; k < kKinds; ++k) {  // [Controls] GripFit<Type>
        const std::string v = config::get_string("Controls", (std::string("GripFit") + kKindName[k]).c_str(), "");
        Fit f;
        if (!v.empty() && sscanf_s(v.c_str(), "%f %f %f %f %f %f", &f.off[0], &f.off[1], &f.off[2], &f.ang[0], &f.ang[1], &f.ang[2]) == 6) {
            g_fit[k] = f;
            log::info("[controllers] fit for %s: (%.3f %.3f %.3f) m, (%.0f %.0f %.0f) deg", kKindName[k], f.off[0], f.off[1], f.off[2], f.ang[0],
                      f.ang[1], f.ang[2]);
        }
    }
    g_hand_path[0] = path("/user/hand/left");
    g_hand_path[1] = path("/user/hand/right");
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(si.actionSetName, sizeof(si.actionSetName), "gameplay");
    std::snprintf(si.localizedActionSetName, sizeof(si.localizedActionSetName), "Gameplay");
    XrResult r = xrCreateActionSet(inst, &si, &g_set);
    if (XR_FAILED(r)) {
        log::warn("[controllers] xrCreateActionSet -> %d", static_cast<int>(r));
        return false;
    }
    bool ok = make_action("aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT, &g_aim) &&
              make_action("grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT, &g_grip_pose) &&
              make_action("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, &g_trigger) &&
              make_action("grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, &g_squeeze) &&
              make_action("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, &g_stick) &&
              make_action("thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_stick_click) &&
              make_action("primary_button", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_primary) &&
              make_action("secondary_button", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_secondary) &&
              make_action("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_menu) &&
              make_action("trackpad", "Trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT, &g_pad) &&
              make_action("trackpad_click", "Trackpad click", XR_ACTION_TYPE_BOOLEAN_INPUT, &g_pad_click) &&
              make_action("haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, &g_haptic);
    if (!ok) return false;
    suggest_all();
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &g_set;
    r = xrAttachSessionActionSets(session, &ai);
    log::info("[controllers] xrAttachSessionActionSets -> %d", static_cast<int>(r));
    if (XR_FAILED(r)) return false;
    for (int h = 0; h < 2; ++h) {
        g_aim_space[h] = make_space(g_aim, h);
        g_grip_space[h] = make_space(g_grip_pose, h);
    }
    g_ready = true;
    g_profile_dirty = true;
    log::info("[controllers] actions ready (haptics %s, strength %.2f)", g_haptics ? "on" : "off", g_haptic_gain);
    return true;
}

void on_profile_changed() { g_profile_dirty = true; }

void sync(XrSpace base, XrTime time) {
    std::lock_guard lock(g_mutex);
    if (!g_ready || base == XR_NULL_HANDLE) return;
    if (g_profile_dirty.exchange(false)) read_profiles();
    XrActiveActionSet as{g_set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    XrResult r = xrSyncActions(g_session, &si);
    g_syncs.fetch_add(1, std::memory_order_relaxed);
    bool tracked[2] = {false, false};
    if (r == XR_SESSION_NOT_FOCUSED || XR_FAILED(r)) {
        // not focused (the system menu is up): no input, the hands drop out
        g_not_focused.fetch_add(1, std::memory_order_relaxed);
        for (int h = 0; h < 2; ++h) hands::set_real(h, hands::Hand{});
        g_any_tracked = false;
        haptics(tracked);
        return;
    }
    for (int h = 0; h < 2; ++h) {
        hands::Hand d;
        bool active = false;
        d.trigger = get_float(g_trigger, h, &active);
        d.grip = get_float(g_squeeze, h, &active);
        XrVector2f s = get_vec2(g_stick, h, &active);
        d.stick[0] = s.x;
        d.stick[1] = s.y;
        if (get_bool(g_primary, h, &active)) d.buttons |= hands::kA;
        if (get_bool(g_secondary, h, &active)) d.buttons |= hands::kB;
        if (get_bool(g_menu, h, &active)) d.buttons |= hands::kMenu;
        if (get_bool(g_stick_click, h, &active)) d.buttons |= hands::kStick;
        {  // the trackpad's click (WMR, the Vive wands): its upper half B / Y, the lower A / X; the Vive's middle, the stick click
            const int k = g_kind[h];
            if (k == kVive || k == kWMR || k == kOdyssey) {
                const XrVector2f pv = get_vec2(g_pad, h, &active);
                if (get_bool(g_pad_click, h, &active)) {
                    if (!g_pad_buttons || (k == kVive && std::fabs(pv.y) < 0.5f)) {
                        if (k == kVive) d.buttons |= hands::kStick;
                    } else {
                        d.buttons |= pv.y > 0.0f ? hands::kB : hands::kA;
                        if (k == kVive) d.stick[0] = d.stick[1] = 0.0f;  // the pad pressed for a button is not a walk
                    }
                }
            }
        }
        d.valid = locate(g_aim_space[h], base, time, d.pos, d.rot);
        d.grip_valid = locate(g_grip_space[h], base, time, d.grip_pos, d.grip_rot);
        if (d.valid) apply_fit(h, d.pos, d.rot);
        if (d.grip_valid) apply_fit(h, d.grip_pos, d.grip_rot);
        d.connected = active || d.valid;
        tracked[h] = d.valid;
        hands::set_real(h, d);
    }
    g_any_tracked = tracked[0] || tracked[1];
    haptics(tracked);
}

void relocate(XrSpace base, XrTime time) {
    std::lock_guard lock(g_mutex);
    if (!g_ready || base == XR_NULL_HANDLE || !g_any_tracked.load(std::memory_order_relaxed)) return;
    float moved = 0.0f;
    for (int h = 0; h < 2; ++h) {
        hands::Hand d = hands::get_real(h);
        if (!d.valid && !d.grip_valid) continue;
        float p[3], q[4], gp[3], gq[4];
        const bool v = locate(g_aim_space[h], base, time, p, q), gv = locate(g_grip_space[h], base, time, gp, gq);
        if (v) apply_fit(h, p, q);
        if (gv) apply_fit(h, gp, gq);
        if (v && d.valid) {
            const float dx = p[0] - d.pos[0], dy = p[1] - d.pos[1], dz = p[2] - d.pos[2];
            moved = std::fmax(moved, std::sqrt(dx * dx + dy * dy + dz * dz));
            std::memcpy(d.pos, p, sizeof(p));
            std::memcpy(d.rot, q, sizeof(q));
        }
        if (gv && d.grip_valid) {
            std::memcpy(d.grip_pos, gp, sizeof(gp));
            std::memcpy(d.grip_rot, gq, sizeof(gq));
        }
        if ((v && d.valid) || (gv && d.grip_valid)) hands::set_real(h, d);
    }
    g_relocate_last_mm = moved * 1000.0f;
    g_relocate_max_mm = std::fmax(g_relocate_max_mm, g_relocate_last_mm);
    g_relocates.fetch_add(1, std::memory_order_relaxed);
}

bool active() { return g_any_tracked.load(std::memory_order_relaxed); }

void note_extension(const char* name) { g_exts.push_back(name); }
void note_api(XrVersion v) { g_api = v; }

const char* fit_name(int h) {
    const int k = h == 0 || h == 1 ? g_kind[h] : -1;
    return k >= 0 ? kKindName[k] : nullptr;
}
bool fit(int h, float off[3], float ang[3]) {
    std::lock_guard lock(g_mutex);
    const int k = h == 0 || h == 1 ? g_kind[h] : -1;
    if (k < 0) return false;
    std::memcpy(off, g_fit[k].off, sizeof(float) * 3);
    std::memcpy(ang, g_fit[k].ang, sizeof(float) * 3);
    return true;
}
void set_fit(int h, const float off[3], const float ang[3], bool save) {
    int k;
    {
        std::lock_guard lock(g_mutex);
        k = h == 0 || h == 1 ? g_kind[h] : -1;
        if (k < 0) return;
        for (int i = 0; i < 3; ++i) {
            g_fit[k].off[i] = off[i] < -0.1f ? -0.1f : off[i] > 0.1f ? 0.1f : off[i];
            g_fit[k].ang[i] = ang[i] < -45.0f ? -45.0f : ang[i] > 45.0f ? 45.0f : ang[i];
        }
    }
    char b[96];
    std::snprintf(b, sizeof(b), "%.3f %.3f %.3f %.0f %.0f %.0f", off[0], off[1], off[2], ang[0], ang[1], ang[2]);
    if (save) config::set("Controls", (std::string("GripFit") + kKindName[k]).c_str(), b);
    log::info("[controllers] fit for %s: %s%s", kKindName[k], b, save ? " (saved)" : "");
}

void pulse(int h, float amplitude, int ms) {
    if (h < 0 || h > 1 || ms <= 0) return;
    g_req_amp[h] = amplitude < 0 ? 0 : amplitude > 1 ? 1 : amplitude;
    g_req_ms[h] = ms;
}

void status_text(char* out, size_t len) {
    if (!g_ready) {
        std::snprintf(out, len, "controllers: not created (no session yet, or [Controls] Controllers=0)");
        return;
    }
    std::string s;
    char b[320];
    std::snprintf(b, sizeof(b),
                  "profile L %s / R %s, syncs %llu (not focused %llu), late latched %llu (moved the hand %.1f mm, at most %.1f), haptic pulses "
                  "%llu, rumble %#010x%s | types %s / %s, trackpad buttons %d",
                  g_profile[0], g_profile[1], static_cast<unsigned long long>(g_syncs.load()), static_cast<unsigned long long>(g_not_focused.load()),
                  static_cast<unsigned long long>(g_relocates.load()), g_relocate_last_mm, g_relocate_max_mm,
                  static_cast<unsigned long long>(g_pulses.load()), xinput::rumble(), hands::synthetic() ? ", SYNTHETIC hands win" : "",
                  g_kind[0] >= 0 ? kKindName[g_kind[0]] : "none", g_kind[1] >= 0 ? kKindName[g_kind[1]] : "none", g_pad_buttons ? 1 : 0);
    s += b;
    for (int h = 0; h < 2; ++h) {
        hands::Hand d = hands::get_real(h);
        std::snprintf(b, sizeof(b), " | %s: %s%s pos (%.3f %.3f %.3f) trigger %.2f grip %.2f buttons %u stick (%.2f %.2f)", h ? "right" : "left",
                      d.connected ? "on" : "off", d.valid ? " tracked" : "", d.pos[0], d.pos[1], d.pos[2], d.trigger, d.grip, d.buttons,
                      d.stick[0], d.stick[1]);
        s += b;
    }
    std::snprintf(out, len, "%s", s.c_str());
}

}  // namespace rdrvr::controllers
