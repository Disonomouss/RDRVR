#include "core/bootstrap.h"

#include <windows.h>

#include "core/actions.h"
#include "core/anchors.h"
#include "core/audio.h"
#include "core/build_check.h"
#include "core/camera_lever.h"
#include "core/config.h"
#include "core/d3d_hooks.h"
#include "core/diag.h"
#include "core/dlss.h"
#include "core/dual.h"
#include "core/dual_pass.h"
#include "core/exit_guard.h"
#include "core/burst_grab.h"
#include "core/ui_layer.h"
#include "core/vr_mode.h"
#include "core/hands.h"
#include "core/physics.h"
#include "core/pose.h"
#include "core/menu.h"
#include "core/eye_grab.h"
#include "core/eye_shape.h"
#include "core/lum_check.h"
#include "core/taa.h"
#include "core/frame_grab.h"
#include "core/game_hooks.h"
#include "core/gpu_timer.h"
#include "core/log.h"
#include "core/render_settings.h"
#include "core/ring_probe.h"
#include "core/body.h"
#include "core/holster.h"
#include "core/aim.h"
#include "core/reload.h"
#include "core/gestures.h"
#include "core/gun_melee.h"
#include "core/controls.h"
#include "core/test_channel.h"
#include "core/xinput.h"
#include "core/xr.h"
#include "core/zone_rings.h"
#include "core/round_draw.h"
#include "core/render_res.h"

namespace rdrvr::bootstrap {
namespace {

constexpr DWORD kMtlxWaitMs = 120000;

bool loaded(const wchar_t* module) { return GetModuleHandleW(module) != nullptr; }

void log_modules(const char* when) {
    log::info("[boot] %s: MTLX=%d d3d12=%d dxgi=%d sl.interposer=%d RedHook=%d winmm=%d xinput1_4=%d", when,
              loaded(L"MTLX.dll"), loaded(L"d3d12.dll"), loaded(L"dxgi.dll"), loaded(L"sl.interposer.dll"),
              loaded(L"RedHook.dll"), loaded(L"winmm.dll"), loaded(L"xinput1_4.dll"));
}

DWORD WINAPI bootstrap_thread(void*) {
    log_modules("bootstrap thread start");
    // Wait for the MTLX wrapper to finish and unload: the game's real entry point runs only after that,
    // and nothing may be patched while the wrapper (anti-debug, module enumeration) is resident.
    bool saw_mtlx = false;
    ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < kMtlxWaitMs) {
        if (loaded(L"MTLX.dll")) {
            saw_mtlx = true;
        } else if (saw_mtlx || GetTickCount64() - t0 > 2000) {
            break;
        }
        Sleep(1);
    }
    log::info("[boot] MTLX %s after %.0f ms", saw_mtlx ? "unloaded" : "never seen", static_cast<double>(GetTickCount64() - t0));
    log_modules("after MTLX");

    config::load();
    // [Render] RenderResolution: before the game's device init reads its size (about 300 ms from here at best); on
    // another build after anchors::verify() below
    render_res::early_arm();
    // D3D12 first: the game creates its device soon after its real entry point, and DRED must be armed before.
    d3d::install_startup_hooks();
    xinput::install();
    gpu_timer::init();
    ring_probe::init();
    exit_guard::install();
    frame_grab::init();
    eye_grab::init();
    lum_check::init();
    burst_grab::init();
    eye_shape::init();  // before ui_layer::init: its monitor repaint's frame end runs before the UI mirror's
    ui_layer::init();
    vr_mode::init();
    hands::init();
    controls::init();
    pose::init();
    holster::init();
    aim::init();
    reload::init();
    audio::init();
    gestures::init();
    gun_melee::init();
    dual::init();  // after holster::init: its frame-end listener runs after the holsters'
    actions::init();
    physics::init();
    menu::init();  // before the XR frame end, which reads the cinema switch
    zone_rings::init();
    round_draw::init();  // the second copy tap (frame_grab::init sets the first)
    xr::init();
    diag::start_watchdog();
    test_channel::start();

    anchors::verify();  // on another build it writes RDRVR_build_report.txt
    if (!anchors::stand_down() && config::get_bool("Debug", "BuildReport", false)) anchors::build_report();  // the test of it
    if (anchors::stand_down()) {
        log::error("[boot] STAND DOWN: no RDR.exe hooks will be installed this run (D3D12 instruments stay)");
    } else {
        render_res::install();  // another build: the render resolution, if the game has not read its size yet
        game_hooks::install();
        camera_lever::install();
        dual_pass::install();
        eye_shape::install();
        body::install();
        holster::install();
        aim::install();
        dual::install();
        reload::install();
        taa::install();
        dlss::install();
        render_settings::install();
    }
    log_modules("bootstrap done");
    return 0;
}

}  // namespace

void on_process_attach() {
    log::open();
    log::info("[boot] RDRVR core loaded (dinput8 proxy), built %s %s", __DATE__, __TIME__);
    log_modules("DllMain");
    diag::install_crash_handler();
    build_check::log_self_and_game_async();
    if (HANDLE t = CreateThread(nullptr, 0, bootstrap_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

// Process exit: every other thread is already gone, possibly while holding the log lock, so nothing here may take a
// lock (or call into RedHook, see the plugin's DllMain). The last status file stays as the run's final state.
void on_process_detach() {}

}  // namespace rdrvr::bootstrap
