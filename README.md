# RDRVR: Red Dead Redemption in VR

A native-stereo VR mod for the PC port of **Red Dead Redemption** (2024). The game draws the scene once per eye from
your headset's view, John's body and arms follow you, and the guns are held, aimed, holstered and reloaded with your
hands.

Version 0.1.0 (an early public release). Tested with a Meta Quest 3 on Virtual Desktop; any PC headset with an
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
- FXAA or the game's TAA run per eye; the game's own colour and gamma.
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
  away; no reticle.
- Body holsters you grab: hips, back, left shoulder, belt, lower back, chest (rounds); a weapon chosen per holster;
  each holster movable and resizable.
- Reloading by hand: take a round at your chest and bring it to the gun (or squeeze the gun at your chest).
- Every gun's action worked by hand (revolvers flicked open, levers, pumps, bolts, breeches, break-actions), with the
  game's own sounds on your movements; two-handed long guns; dual wielding (a second gun, or a copy of your sidearm);
  perfect accuracy; the guns shown at their holsters; throwing, punching and the lasso by swinging. All on by default,
  each can be switched off in the menu. Off by default: pushing and grabbing loose objects.
- No executions: the trigger fires at close range instead of starting John's third-person execution.

**Controls and comfort**
- The controllers drive the game's gamepad (every button remappable), with the game's rumble on the controllers.
- Smooth or snap turning; on a horse, steering by the stick or by your view, the left stick click brakes.
- An in-headset menu (laser and trigger) with every setting; adjustments by arrows, one step a press.

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
2. Download `RDRVR-0.1.0.zip` from the [Releases](../../releases) page, unzip it, and run `Install.cmd`. It finds
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

- **Sharpness:** each eye shows the game's own frame at the game's resolution (16:9). For a sharper image on a
  high-resolution headset, run the game at 3840x2160 (with NVIDIA DSR/DLDSR on a 1440p monitor) if your GPU allows.
- **Frame generation** layers (frame interpolation in the OpenXR runtime or a layer) make the guns lag behind your
  hands: the generated frames cannot follow the gun. Prefer a lower refresh rate (72 or 80 Hz) with real frames.

## Compatibility

- Bindings: Meta Touch, Touch Pro and Touch Plus, Valve Index, HTC Vive, Vive Cosmos and Focus 3, Windows Mixed
  Reality, HP Reverb G2, Samsung Odyssey, Pico 4 and Neo 3, and the Khronos simple controller. WMR and the Vive wands
  have no A/B buttons: the trackpad's click gives them (upper half B/Y, lower A/X).
- Only Touch controllers on a Quest 3 have been tested on real hardware. If a controller type sits differently in your
  hand, the Hands tab's "Controller fit" moves and turns the hands for that type.

## Known issues

- Dual wielding two of the same gun from its own holster can leave the second gun invisible.
- DLSS is not available in VR yet (the mod forces FXAA by default; TAA is the alternative).
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
if the game is updated and they no longer match, the mod installs no game hooks.

## How it works (briefly)

- `dinput8.dll` (the core) loads with the game, hooks its D3D12 renderer to draw the scene once per eye, owns the
  OpenXR session, the body, the hands, the holsters and the menu.
- `RDRVR.Gameplay.red` (a RedHook plugin) runs on the game's script thread and calls the game's natives the core asks
  for.

## Licence

MIT (see [LICENSE](LICENSE)). Third-party libraries: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
