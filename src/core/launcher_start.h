#pragma once
// The launcher at the game's start (GOAL-round52-fixes-and-scopes.md item 10; research/run30/launcher.md).
// However the game was started (Steam, the Rockstar Games Launcher's PlayRDR.exe, RDR.exe itself), the bootstrap opens
// RDRVR_Launcher.exe from the game folder's RDRVR_Launcher subfolder (in the folder itself it loaded the game folder's
// winmm.dll, RedHook's proxy, and crashed: r301) and holds the game's main thread at the entry of its device init
// (FUN_14012e420, anchor BootDeviceInit: no window and no D3D12 device yet) until the launcher closes (its Play, its
// Close, its countdown) or the game's own deadline passes; then the ini is read again, so what was saved there applies
// to this start. Nothing of it runs in DllMain; any failure opens the gate and the start is as without it.
// Not shown: [Launcher] ShowAtStart=0, no RDRVR_Launcher\RDRVR_Launcher.exe in the game folder, the simulator deployed
// (RDRVR_xr_runtime.txt: a test cycle; [Debug] LauncherWithSimulator=1 for r65), the launcher's own Play just started
// the game (its note %LOCALAPPDATA%\RDRVR\RDRVR.launcher_started), a -rebootToMenu relaunch, another RDR.exe build, or
// the device init already begun.

namespace rdrvr::launcher_start {

// The bootstrap thread, right after the first config::load(): may open the launcher and wait for it (the game's main
// thread then held at its device init), then reads the ini again. Returns at once when it is not shown.
void run();
// The bootstrap thread, at its end (everything it installs in before the game's device, as on a start without the
// launcher): the game's device init may go on. Idempotent; nothing when run() held nothing.
void release_game();

// The menu (Advanced tab): the settings as read at this start, and their writers (applied at the next start)
bool show_at_start();
void set_show_at_start(bool on);
int start_timeout();  // seconds; 0 = no countdown (the game's 5-minute deadline only)
void set_start_timeout(int seconds);

}  // namespace rdrvr::launcher_start
