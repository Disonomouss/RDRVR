# RDRVR Launcher

A small desktop program for choosing RDRVR's settings before playing, then starting the game. Building it puts
`build\RDRVR_Launcher.exe` next to the mod's other outputs. The harness deploys it into the game folder's
`RDRVR_Launcher` subfolder (2026-10-30, item 10), and the installer ships it from there. The mod then opens it at every
start of the game (below). It must not sit in the game folder itself (r301, r303). There Windows found the game
folder's `winmm.dll` (RedHook's proxy) for it first, RedHook loaded into the launcher, and the launcher crashed within
1 s. The Steam overlay that Steam injects into the game's child processes imports `winmm.dll` before `Main` runs, so
only the move fixes it. After `Main` starts, the launcher loads DLLs by name from System32 only
(`SetDefaultDllDirectories`), but that is not a guard against this crash. It finds the game folder as the folder above
its own.

## The tabs
- **Graphics:** the mod's render settings, as the in-headset menu's Display tab has them:
  - anti-aliasing and DLSS quality;
  - sharpening;
  - the eye shape;
  - motion blur and heat haze;
  - the refresh rate and cutscenes;
  - the render resolution (headset and size, computed as `src/core/render_res.cpp` does);
  - the three presets (`src/core/menu.cpp`);
  - single-pass stereo, a test with no headset-menu row. One switch writes HEADSET-TESTS.md round 34's `[Stereo]` lines.
    - It asks for the folder of converted shaders (`InstancedShaders`: `vs_*.dxil` and `twins.txt`). That folder is made
      from the player's own game files and never shipped.
    - `PrepassInstPrevMirror` and `PrepassInstPrevK` follow the folder's `instprev<K>` lines.
    - Unticked, each key the files turn on is written off, and the folder is cleared: `pso.cpp` makes twins from it
      whenever it is set.

  The game's own graphics options (graphicsOptions.xml) are never touched: DESIGN.md's rule, kept on the user's
  choice (2026-10-26).
- **Mod settings:** every setting of the in-headset menu's tabs, with the menu's labels and values, from
  `menu_schema.json`.
  - A setting that depends on another is greyed out while that one is off, as in the menu.
  - A holster's weapon lists the weapons of its kind; the menu lists only the owned ones that fit, read in gameplay.
    One not owned, or not fitting, draws the automatic one in both.
  - The front hand's hold has a row per long gun. The menu shows the one for the gun in hand.
  - A control that writes several keys together, or needs the headset or the running game, is described and marked
    "Set in the headset menu". Examples: a ring's place, the controller fit, a calibration, the refresh rate.
- **Controls:** a picture of both controllers with what each button does, drawn from the settings the game will use.
  - It follows the [Controls] remap, left-handed, the holster mode, the HUD and the turn mode.
  - It also lists the chords and the main gestures.
  - `--render-controls <file.png>` saves it.

## The settings files
The mod reads the game folder's `RDRVR.ini`, then `%LOCALAPPDATA%\RDRVR\RDRVR.user.ini` over it, once at the game's
start (`src/core/config.cpp`). The launcher:
- **shows** each setting as the game will see it: the user file's value, else the game folder's, else the menu's
  default;
- **writes** only the keys you change into the user file, as the mod's own writer does: every other line and comment
  kept, CRLF, no BOM, through a `.tmp` file and a replace;
- **reads the files again before every save** and whenever its window comes back to the front after they changed.
  What the game wrote meanwhile (a holster's ring, a menu change) is kept, and the launcher's edits go on top, as
  `config::set` does;
- reads each value **as the mod does**:
  - `get_bool`: 1, true, yes or on;
  - `get_int`: strtol, so `010` is 8;
  - choices as written, so `Snap` is not `snap`;
  - the render resolution by `RenderResolution`, as `load_choice` does;
- keeps the file's bytes: UTF-8, or the ANSI code page the mod falls back to for a path, and a BOM;
- **refuses to save while the game runs**, because the mod writes the same file;
- asks for the game folder before saving when it can't find it, because the release's reset tag lives there;
- **handles the release's one-time settings reset** (`[Compat] SettingsReset`) exactly as the mod would at its next
  start. The old file is kept as `RDRVR.user.ini.before-<tag>.bak`, and the new one carries the tag, so the mod does
  not move the launcher's file aside.

## Starting the game
Through Steam (`steam://rungameid/2668510`) when the game is in a Steam library, else the Rockstar Games Launcher's
`PlayRDR.exe` in the game folder. The game folder is found as the installer finds it: the launcher's own folder,
Steam's libraries, then the Rockstar install key. It never passes `-width`/`-height`, because the mod's render
resolution refuses to arm with them. Just before the start it writes `%LOCALAPPDATA%\RDRVR\RDRVR.launcher_started`
(`started <UTC unix seconds> pid <n>`). The mod reads that note, deletes it, and does not open a second launcher
(within 300 s).

## At the game's start
However the game starts (Steam, the Rockstar Games Launcher, `RDR.exe`), the mod (`src/core/launcher_start.cpp`)
opens `RDRVR_Launcher\RDRVR_Launcher.exe --at-start <game pid> <seconds>`, with its own folder as the working
directory. It holds the game at its device init, before its window and its D3D12 device, until this window closes.
Then it reads the ini again, so what was saved here applies to that start. The design and its safety are in
`research/run30/launcher.md`.
- A strip at the top shows the countdown (`[Launcher] StartTimeout`, 30 s). At 0 the window closes and the game goes
  on with what is saved.
- A click, key or wheel stops the countdown. The game then waits for Save && Play, 5 minutes at most.
- Save && Play saves and closes; it never starts a second game. Close (or the X) closes, and the game goes on too.
- "Show this window when the game starts" in the bar is `[Launcher] ShowAtStart`, also in the Mod settings tab and the
  in-headset menu's Advanced tab.
- It sets the mod's "up" event once running; without it (an older launcher in the game folder) the game goes on after
  20 s.
- Saving is allowed while the mod holds the game: it reads the user file again only after this window has closed.
  If the game went on without it (its deadline), the window closes, or keeps unsaved changes with saving refused while
  the game runs.
- Exit codes, which the mod logs: 0 Play, 1 closed, 2 the countdown, 3 the game had gone on, 4 the game was gone, 5 an
  error.
- Not opened: `ShowAtStart=0`, no `RDRVR_Launcher\RDRVR_Launcher.exe` in the game folder, the simulator deployed (`RDRVR_xr_runtime.txt`,
  unless `[Debug] LauncherWithSimulator=1`), the note above, a `-rebootToMenu` relaunch, another RDR.exe build.
- Test arguments (`[Debug] LauncherTestArgs`, `tests/sim/r65_launcher_start.py`): `--test-set Section.Key=Value` (an edit,
  as if made in the window), `--test-press play|close <seconds>`, `--test-hold <seconds>` (as a click after that long), `--test-no-countdown`.

## Building and checking
- `powershell -ExecutionPolicy Bypass -File tools\launcher\build_launcher.ps1` builds it with the C# compiler that
  ships with Windows (.NET Framework 4.8; no SDK or packages) and embeds `menu_schema.json`.
- `RDRVR_Launcher.exe --selftest <scratch dir>` prints PASS / FAIL for each check, on copies in that folder only:
  - the ini writer;
  - the settings reset;
  - the resolution formula;
  - the requirements' parser. It also lists the few requirements it cannot check, which need the running game.
- `RDRVR_Launcher.exe --wheeltest` sends the mouse wheel to every list and number. None may change: the wheel scrolls
  the page. As a control case, a plain NumericUpDown must change.
- `RDRVR_Launcher.exe --screenshot <dir>` renders every tab to PNG without showing the window:
  - `page_<n>.png` is a whole Mod settings page, and `graphics.png` the whole Graphics tab;
  - nothing is saved.

## menu_schema.json
Made from `src/core/menu.cpp` (its `source` field names the commit). When the menu changes, remake it the same way and
rebuild: each item names the section and key the menu writes, the values, the labels, the default (the release ini's
value, else the code's) and whether it applies only at the next start.
