# RDRVR: Red Dead Redemption in VR

A native-stereo VR mod for the PC port of **Red Dead Redemption** (2024). The game draws the scene once per eye from
your headset's view, John's body and arms follow you, and the guns are held, aimed, holstered and reloaded with your
hands.

Version 0.5.0 (an early public release). Tested with a Meta Quest 3 on Virtual Desktop; any PC headset with an
OpenXR runtime should work (see [Compatibility](#compatibility)).

> RDRVR is an unofficial fan project. It is not affiliated with or endorsed by Rockstar Games or Take-Two Interactive,
> and it contains no game files. The game sends its crash reports to Rockstar: a crash with the mod installed is
> reported too.

## Features

**VR view**
- True stereo: the scene drawn twice per frame, once per eye, with your headset's own field of view (asymmetric and
  canted displays included) and late-latched head and hand poses.
- Per-eye fixes so both eyes see the same frame: grass, wind, lights, forest, exposure, rain and particles, god rays,
  wetness and blood, cloud shadows.
- Culling, tree billboards and distant trees follow your head, and stay upright.
- FXAA, the game's TAA or DLSS run per eye (DLSS with its own history per eye, falling back to one shared history
  if its setup fails, which `RDRVR.log` says; since 0.5.0 it is usable in the headset: its images were handed to the
  headset a few frames stale, two defects in what the game feeds DLSS are corrected, its jitter's length and units,
  and a hitch no longer drops the per-eye histories); the game's own colour and gamma.
- Optional (new in 0.3.0, off by default, tried briefly in a Quest 3; the General tab, under the anti-aliasing, with
  FXAA only): each eye rendered in your headset's own shape and size, with square pixels, instead of the game's 16:9
  frame stretched over it: fewer pixels drawn for the same sharpness. Restart the game after turning it on: the eye
  images take their size at the start.
- Optional (new in 0.4.0, off by default, tried briefly in a Quest 3; the General tab, "Render resolution", from the
  next start): the game's frame at your headset's height, apart from your monitor's modes and without DSR. Choose
  Automatic (16:9 at the height your headset's OpenXR runtime asked for in its last session, never below your own
  resolution; the game's own size until a first session with that runtime) or a height named for the headsets (and
  Virtual Desktop quality levels) it fits, 1600 to 3264 tall, for example "2208 tall: Quest 3 (3926 x 2208)". In
  Windowed mode your window keeps its size (the frame is scaled into it). The game's Graphics menu shows the size, and
  `graphicsOptions.xml` keeps your own resolution. With "Eyes in the headset's shape" on, each eye is drawn at the size
  your headset asks for, at most this tall. Larger is sharper and costs more GPU time. A size too large for the
  graphics card's memory is greyed out in the menu, and a size whose start ended (a crash) within two minutes is not
  used again until you choose it again; the row's "Now:" line says which size runs, or why not.
- The HUD on a floating panel, or on your wrist like a watch (radar, meters, ammo shown when you look at it).
- Automatic view modes: stereo in gameplay, a cinema screen for loading, menus and videos; cutscenes on the screen or
  in 3D.

**Body**
- A first-person body (full, forearms and hands, or hands only), upright and facing your view, no head in the way.
- Arms that follow the controllers (two-bone IK, stretching past John's reach), the eye height following crouching
  and riding.
- A left-handed mode: a full mirror (the gun in John's left hand, the holsters and reloading mirrored).

**Guns**
- Shots leave the barrel of the gun in your hand; the right trigger alone fires; the gun stays drawn until you put it
  away.
- An optional reticle where the shot will land, from the game's own aim (off by default: the Hands tab), a ring or
  a dot (new in 0.5.0).
- Holsters you grab: hips, back, left shoulder, belt, lower back, chest (rounds); a weapon chosen per holster; each
  holster movable and resizable. They are fixed to your headset's position, so they stay with you as you step,
  crouch and lean (or to John's body: the Holsters tab).
- Reloading by hand: take a round at your chest and bring it to the gun (or squeeze the gun at your chest).
- Every gun's action worked by hand except the magazine pistols' (revolvers flicked open, levers, pumps, bolts,
  breeches, break-actions), with the game's own sounds on your movements; two-handed long guns; dual wielding (a second gun, or a copy of your sidearm);
  perfect accuracy; the guns shown at their holsters; throwing, punching and the lasso by swinging. All on by default,
  each can be switched off in the menu. Off by default: pushing and grabbing loose objects.
- No executions: the trigger fires at close range instead of starting John's third-person execution.
- New in 0.3.0, off by default (each in the menu, not yet tried in a headset): a gun-butt melee, the gun in your
  hand swung into someone landing the game's own melee blow where it meets them (knock-out damage, as a punch does;
  the Gestures tab, "Gun-butt melee"); shooting from right behind cover (the Hands tab, "Shoot past the arm block");
  jumping, and so vaulting and climbing, with a gun raised (the Controls tab, "Jumping lowers the raised gun"); the
  gun hand kept at each long gun's aiming grip in every pose (the Hands tab, "Long guns held by their aiming grip").

**Controls and comfort**
- The controllers drive the game's gamepad (every button remappable), with the game's rumble on the controllers.
- Smooth or snap turning; on a horse, steering by the stick or by your view, the left stick click brakes.
- An in-headset menu (laser and trigger) with every setting, grouped under headings with a help line for the row
  under the laser; the tabs are two rows of buttons, and each tab's page scrolls with your gun hand's stick (the
  laser on the page); adjustments by arrows, one step a press.

## Requirements

- **Red Dead Redemption** for PC (the 2024 port), from Steam or the Rockstar Games Launcher.
- **RedHook v0.8** by K3rhos: https://www.nexusmods.com/reddeadredemption/mods/192. RDRVR's gameplay plugin runs in it;
  RedHook is not included here.
- A PC VR headset and its OpenXR runtime set as the active one (SteamVR, Meta Quest Link, Virtual Desktop, WMR, ...).
- Windows 10 or 11, a DirectX 12 GPU with headroom for drawing the game twice per frame.

## Installation

1. Install RedHook into the game folder (its own instructions). In its `RedHook.ini` set:
   ```ini
   [DirectXHook]
   Disabled=true
   ```
   RDRVR draws and presents the frame itself; RedHook's DirectX hook gets in its way.
2. Download `RDRVR-0.5.0.zip` from the [Releases](../../releases) page, unzip it, and run `Install.cmd`. It finds
   the game through Steam (or asks for the folder; or `Install.cmd -GameDir "<the game folder>"`), backs up anything it
   would replace, and never changes a game file.
3. Start your headset's runtime, then the game.

To uninstall, run `RDRVR_Uninstall.cmd` in the game folder (your settings stay in `%LOCALAPPDATA%\RDRVR`).

## Using it

- **Recentre:** your headset's own recentre, Scroll Lock with the game in front, or both stick clicks held a second.
- **The mod's menu:** hold the menu button, or (with the wrist HUD) turn your palm flat to look at the HUD and hold
  Y, or press F7 on the keyboard. Point with the laser, click with the trigger.
- **The game's buttons:** the controllers act as the game's pad: sticks, A/B/X/Y, grips (LB/RB), triggers (LT/RT).
  Tapping the menu button is Start; with it held, the right stick is the D-pad and A is Back (the map).
- **Guns:** grab a holster with the grip to draw; let go at a holster to put the gun away. Reload by taking a round at
  your chest (grip there) and bringing it to the gun's loading point.
- **Settings:** everything is in the menu's tabs. Your changes are saved in `%LOCALAPPDATA%\RDRVR\RDRVR.user.ini`,
  which wins over the shipped `RDRVR.ini` (every key there is commented).

## Tips

- **Sharpness:** each eye shows the game's own frame (16:9), or, with "Eyes in the headset's shape" on, a frame in
  the headset's own shape. For a sharper image on a high-resolution headset, choose the "Render resolution" (the General
  tab) named for your headset, a taller one, or Automatic, if your GPU allows (new in 0.4.0); "Eyes in the headset's
  shape" (FXAA only) makes it cheaper, and DLSS Quality (usable since 0.5.0; from the next start) renders each eye at
  two thirds of the width and height (about 9 ms of GPU a frame at 3926 x 2208 on an RTX 4070 Ti, against FXAA's 7).
- **Frame generation** layers (frame interpolation in the OpenXR runtime or a layer) make the guns lag behind your
  hands: the generated frames cannot follow the gun. Prefer a lower refresh rate (72 or 80 Hz) with real frames.

## Compatibility

- Bindings: Meta Touch, Touch Pro and Touch Plus, Valve Index, HTC Vive, Vive Cosmos and Focus 3, Windows Mixed
  Reality, HP Reverb G2, Samsung Odyssey, Pico 4 and Neo 3, and the Khronos simple controller. WMR and the Vive wands
  have no A/B buttons: the trackpad's click gives them (upper half B/Y, lower A/X).
- Only Touch controllers on a Quest 3 have been tested on real hardware. If a controller type sits differently in your
  hand, the Hands tab's "Controller fit" moves and turns the hands for that type.
- The game's own builds: the mod was made on one build of `RDR.exe`. On another build it looks for the game code and
  data it uses by their bytes (new in 0.3.0, not yet tried on a real other build). If it finds and checks every one,
  it runs; if not, it stands down (the game plays flat). Either way it writes `RDRVR_build_report.txt` in the game
  folder: if the mod stands down or misbehaves, please attach it, with `RDRVR.log`, to an issue. RedHook, which runs
  the gameplay plugin, has to work on that build too.

## Known issues

- Shooting from right behind cover can be refused by the game's "gun against a wall" check, which looks from John's
  shoulder, not from your gun: "Shoot past the arm block" (the Hands tab, off by default, new in 0.3.0) lifts it.
- Standing or walking with a gun raised, the game takes no jump, so no vault or climb: "Jumping lowers the raised gun"
  (the Controls tab, off by default, new in 0.3.0) lowers the gun for the jump. Vaulting over a fence or a low wall
  has not been checked yet.
- DLSS (in the anti-aliasing choice, from the next start) has been tried in one headset (a Quest 3 over Virtual
  Desktop) at Quality; FXAA stays the default. With DLSS, Virtual Desktop's latency figure counts the GPU's whole
  frame now (it did not before), so it reads higher than with FXAA for the same real latency.
- Rarely, the game freezes or slows to a few frames a second (seen a few times in the developer's simulator tests;
  the cause is not known yet). If it happens, please attach `RDRVR.log` to an issue: it records what the game's
  render threads were doing.
- The holsters' guns cost frame time by how many are shown: the back's long guns (off by default) are the expensive
  part.
- John's gun hand can sit a few centimetres up a long gun (at the receiver, not the stock's wrist) when it is lowered
  or carried: "Long guns held by their aiming grip" (the Hands tab, off by default, new in 0.3.0) holds each long gun
  by its aiming grip in every pose.
- "Render resolution" has been tried (in the developer's simulator) in Windowed mode only (the game's Screen Type):
  use Windowed with it for now. In Fullscreen the game may undo a size smaller than your monitor's at the start (the
  row's "Now:" line then says the game changed its size).
- Changing the resolution in the game's Graphics menu while in the headset stops the headset view (the flat game on
  the cinema screen) until you restart the game.
- Frames wider than 2560 (a 4K monitor, DSR, or "Render resolution") show the HUD panel and the cinema screen filtered
  to 2560 wide (new in 0.4.0): `[XR] UiQuadMaxWidth=0` and `CinemaMaxWidth=0` in `RDRVR.ini` bring back 0.3.0's
  full-size images.
- The default places of the holsters, the loading and foregrip rings and the interaction spot were tuned in the
  headset by one person: if they do not suit you, move them with the menu's arrows (Holsters, Reloading, Gun in hand
  and Hands tabs).

## Building

Needs Visual Studio 2022 (the C++ desktop workload, with its CMake and Ninja), [vcpkg](https://github.com/microsoft/vcpkg)
(`VCPKG_ROOT` set, or at `C:\dev\vcpkg`), and for the gameplay plugin the **RedHook SDK**
(https://github.com/K3rhos/RedHookSDK, MIT), which this repository does not include:

```
extern/redhook-sdk/include/   the SDK's Headers/ plus Resources/RedHook.h
extern/redhook-sdk/lib/       Resources/RedHook.lib
```

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1                 # build\dinput8.dll and build\RDRVR.Gameplay.red
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -RedHookSdk D:\sdk\redhook   # the SDK elsewhere
powershell -ExecutionPolicy Bypass -File tools\package.ps1               # build\dist\RDRVR-<version>.zip
```

Without the SDK only `dinput8.dll` is built. The game's code addresses the mod hooks are in `src/core/anchors.txt`;
`src/core/anchors.inc` is generated from it with `tools/gen_anchors.py` against your own copy of `RDR.exe` (placed at
`research/RDR.exe`, which is never committed), and each one is checked against the running game's bytes at startup:
on another build (or after a game update) the mod looks for each one by its signature (`src/core/anchors_sig.inc`,
from `tools/gen_buildsig.py`) and runs on the found addresses only when every one is found and checked; otherwise it
installs no game hooks.

## How it works (briefly)

- `dinput8.dll` (the core) loads with the game, hooks its D3D12 renderer to draw the scene once per eye, owns the
  OpenXR session, the body, the hands, the holsters and the menu.
- `RDRVR.Gameplay.red` (a RedHook plugin) runs on the game's script thread and calls the game's natives the core asks
  for.

## Licence

MIT (see [LICENSE](LICENSE)). Third-party libraries: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
