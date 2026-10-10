# RDRVR: Red Dead Redemption in VR

A native-stereo VR mod for the PC port of **Red Dead Redemption** (2024). The game draws the scene once per eye from
your headset's view, John's body and arms follow you, and the guns are held, aimed, holstered and reloaded with your
hands.

Version 0.8.0 (an early public release). Tested with a Meta Quest 3 on Virtual Desktop; any PC headset with an
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
- Faster in 0.8.0, both on by default and used in a Quest 3 over Virtual Desktop at 90 Hz ("performance is smooth"),
  each switchable in the General tab under "Performance":
  - the game's render thread hands its recorded work to its GPU thread in batches instead of one command at a time:
    about 2.5-3 ms less CPU a frame in the developer's tests (about a fifth of the stereo frame), the image unchanged;
  - the shadows (the sun's, the lamps' and the spot lights') drawn once a frame for both eyes instead of once per eye:
    about 0.9 ms less CPU a frame; the shadows' edges can move by a texel.
- FXAA, the game's TAA or DLSS run per eye (DLSS with its own history per eye, falling back to one shared history
  if its setup fails, which `RDRVR.log` says; since 0.5.0 it is usable in the headset: its images were handed to the
  headset a few frames stale, two defects in what the game feeds DLSS are corrected, its jitter's length and units,
  and a hitch no longer drops the per-eye histories); the game's own colour and gamma.
- Optional (new in 0.3.0, off by default, tried briefly in a Quest 3; the General tab, under the anti-aliasing, with
  FXAA): each eye rendered in your headset's own shape and size, with square pixels, instead of the game's 16:9
  frame stretched over it: fewer pixels drawn for the same sharpness. Restart the game after turning it on: the eye
  images take their size at the start. The same with DLSS (new in 0.7.0; off by default; since 0.8.0 tried in a
  Quest 3 over Virtual Desktop, where it looked as sharp and took about a quarter less GPU time at 3926 x 2208 with
  DLSS Quality): the eye-shaped image upscaled by DLSS into an eye-shaped output. Choose DLSS in the General tab's
  Anti-aliasing, then tick "Eyes in the headset's shape with DLSS" under it (new in 0.8.0), from the next start. It
  applies only while DLSS runs per eye; if DLSS falls back to one shared history, the eyes get the game's frame
  (`RDRVR.log`'s `[eye]` lines say which).
- Optional (new in 0.4.0, off by default, tried briefly in a Quest 3; the General tab, "Render resolution: headset"
  and "Render resolution: size", from the next start): the game's frame at your headset's height, apart from your
  monitor's modes and without DSR. Choose your headset, then its size: 100%, 150% or 200% of its pixels (new in 0.6.0,
  tried in the developer's simulator only; 14 headsets from the Valve Index to the Pimax Dream Air, by their screens'
  heights; for example a Quest 3 at 3926 x 2208, 4808 x 2704 or 5550 x 3122). Or choose
  Automatic: the size your OpenXR runtime (SteamVR, Pimax Play, Virtual Desktop...) asks for, for its lenses, at those
  percentages; it is learned in a first session with that runtime (the game's own size until then), never below your
  own resolution. Keep the runtime's own resolution setting at its default (SteamVR's 100%): it multiplies with the
  mod's. A size of your own can be written in `%LOCALAPPDATA%\RDRVR\RDRVR.user.ini`, where the menu saves its choice
  (`[Render] RenderResolution`, a height or WxH, 16:9; the menu then shows it as Custom). In
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
  perfect accuracy (your shots only: other people's keep the game's spread); the guns shown at their holsters;
  throwing, punching and the lasso by swinging. All on by default, each can be switched off in the menu. Off by
  default: pushing and grabbing loose objects.
- The holsters, the dots at your hands and the guns' points (the foregrip ring, the loading point, the action hints)
  each shown or hidden by its own switch (new in 0.6.0, tried in the developer's simulator; the Holsters tab).
- New in 0.7.0, off by default (each in the Holsters tab, tried in the developer's simulator only): the holsters turn
  with your headset (with an adjustable dead zone, 0° by default so they follow every look; at 30 to 45° a glance
  aside leaves them), or by your head and hands together; steady when you lean forward or bend over; steady holster
  edges (no buzzing at a ring's edge); each ring shown only while
  a hand is near it; and placing the holsters by hand (grip a ring and move it). An experimental holster model at the
  hips (`[Holsters] ShowModels`, ini only) still looks like a strap.
- The foregrip ring held still on the gun (new in 0.6.0, tried in the developer's simulator, not yet in a headset): it
  followed the game's arm sway, so the off hand could grab the air beside the gun, worst on the Sawed-off (which now
  has a grip of its own).
- No executions: the trigger fires at close range instead of starting John's third-person execution.
- New in 0.7.0, on by default (tried in the developer's simulator, not yet in a headset): the lasso held in your
  hands (the coil in the off hand, the loop in the gun hand; it floated in front of them before); with dual wielding,
  a second gun drawn from a back holster is that holster's gun (a sidearm in hand made every back holster give the
  same gun); each long gun's front hand in the game's aiming hold, its fingers too (the Gun in hand tab, "The front
  hand's pose": aiming, lowered, or Automatic, the way 0.6.0 chose by the game's stance); a long gun held lowered no
  longer flips its front hand between holds (with Automatic).
- New in 0.7.0, off by default (tried in the developer's simulator only): throwing by letting go of the grip (the
  Gestures tab, under "Throw by hand": grip to hold the throwable, the trigger to light or ready it, swing and let
  go; tried with dynamite and the throwing knife, the fire bottle and the tomahawk not yet); sidearms held by their aiming grip (the Hands tab, "Sidearms held by their aiming grip": a test for a revolver
  that lags the hand; please report whether it helps).
- A whistle for your horse (new in 0.6.0, used in a headset; on by default since 0.7.0): your hand at your mouth and
  its trigger (the Gestures tab, "Whistle for the horse"; that trigger does nothing else there; the gun hand whistles
  only with no gun in it). New in 0.7.0 (tried in the developer's simulator only): the off hand whistles with a gun
  raised in the other: the game will not whistle while aiming, so the aim is let go for under a second first, and no
  shot fires meanwhile.
- New in 0.6.0, off by default (tried in the developer's simulator, not yet in a headset): the game's weapon wheel
  instead of the holsters, a grip held opening it in front of your hand, the hand moved toward a weapon's picture and
  the grip let go to take it (the Holsters tab, "The weapon wheel").
- New in 0.3.0, off by default (each in the menu, most not yet tried in a headset): a gun-butt melee, the gun in your
  hand swung into someone landing the game's own melee blow where it meets them (knock-out damage, as a punch does;
  the Gestures tab, "Gun-butt melee"; tried in a headset with 0.5.0, where it hardly ever hit: it needed 3 m/s at the
  moment of contact. Since 0.6.0 a hit is judged by your swing's fastest moment, from 1.8 m/s (the "Hit speed" under
  it), the gun still moving into them where it meets them; the new rule tried in the developer's simulator only); shooting from right behind cover (the Hands tab, "Shoot past the arm block");
  jumping, and so vaulting and climbing, with a gun raised (the Controls tab, "Jumping lowers the raised gun"); the
  gun hand kept at each long gun's aiming grip in every pose (the Hands tab, "Long guns held by their aiming grip").

**Controls and comfort**
- The controllers drive the game's gamepad (every button remappable), with the game's rumble on the controllers.
- Smooth or snap turning; on a horse, steering by the stick or by your view, the left stick click brakes. New in
  0.7.0, on by default (used in a headset since 0.8.0): with stick steering the right stick turns the view on
  horseback too, while the left stick steers the horse (the Comfort tab, under "Steer with the stick"). Since 0.8.0
  the holsters turn with your view as you do (before, they kept facing the horse); recentring faces them back.
- The game's own menus by the controllers (new in 0.7.0, on by default, tried in the developer's simulator only, not
  yet in a headset; `[Controls] MenuControls` in `RDRVR.ini`, no menu row): in the satchel and shops the triggers
  change tabs; in the satchel, the pause menu and shops the left stick moves one item a push.
- Shops in first person (new in 0.7.0, on by default; tried at a shop in the developer's simulator, not yet in a
  headset; `[Body] KeepAnchorCamera` in `RDRVR.ini`, no menu row): talking to a shopkeeper no longer takes the view to
  third person. Other scripted cameras outside cutscenes are kept first person too; this is not yet tried in
  missions. If one looks wrong, set `KeepAnchorCamera=0` and please report it.
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
2. Download `RDRVR-0.8.0.zip` from the [Releases](../../releases) page, unzip it, and run `Install.cmd`. It finds
   the game through Steam (or asks for the folder; or `Install.cmd -GameDir "<the game folder>"`), backs up anything it
   would replace, and never changes a game file. Its window stays open until you press a key (since 0.8.0): if it
   stops on an error, the text says why, and `%TEMP%\RDRVR_install.log` has everything it printed (attach that file to
   an issue if you need help). It changes nothing if a file it would replace is in use (the game, its crash
   reporter or a virus scan): close it and run the installer again.
3. Start your headset's runtime, then the game.

To uninstall, run `RDRVR_Uninstall.cmd` in the game folder (your settings stay in `%LOCALAPPDATA%\RDRVR`).

## Using it

- **Recentre:** your headset's own recentre, Scroll Lock with the game in front, or both stick clicks held a second.
- **The mod's menu:** hold the menu button, or (with the wrist HUD) turn your palm flat to look at the HUD and hold
  Y, or press F7 on the keyboard. Point with the laser, click with the trigger.
- **The game's buttons:** the controllers act as the game's pad: sticks, A/B/X/Y, grips (LB/RB), triggers (LT/RT).
  Tapping the menu button is Start; with it held, the right stick is the D-pad and A is Back (John's satchel: his
  items). With the wrist HUD, "A double tap of Y at the wrist opens the satchel" (new in 0.7.0, off by default, tried
  in the developer's simulator only; the General tab, under "HUD on the left wrist") lets two quick presses of Y open
  it; a single press still reaches the game as Y, about a third of a second late.
- **Guns:** grab a holster with the grip to draw; let go at a holster to put the gun away. Reload by taking a round at
  your chest (grip there) and bringing it to the gun's loading point.
- **Settings:** everything is in the menu's tabs. Your changes are saved in `%LOCALAPPDATA%\RDRVR\RDRVR.user.ini`,
  which wins over the shipped `RDRVR.ini` (every key there is commented).
  The [Menu guide](#menu-guide) below lists every tab and setting.

## Menu guide

To open the mod's menu, hold the menu button. If your controller has no menu button, hold both B buttons. You can also turn your palm down to look at the wrist HUD and hold Y, or press F7 on the keyboard. Point the laser and click with the trigger. The tabs are the two rows of buttons at the top, the gun hand's stick scrolls the page, the arrows change a value by one step per press, and the help line at the bottom explains the row under the laser. Each change is saved to `%LOCALAPPDATA%\RDRVR\RDRVR.user.ini` right away, except rows marked "this session". Rows marked "from the next start" need a game restart.

### General

Where the HUD goes, the anti-aliasing, the resolution the game renders at for your headset, and two performance
options.

| Setting | What it does | Default |
|---|---|---|
| **HUD on the left wrist** | Puts the radar, meters and ammo counter on the back of your off hand like a watch: turn the palm down and look at it to see them (prompts stay on the floating screen). | On |
| ↳ **A double tap of Y at the wrist opens the satchel** (new in 0.7.0) | With the wrist HUD in view, two quick presses of Y open John's satchel and B closes it. A single press still reaches the game as Y, about a third of a second late. | Off |
| **Anti-aliasing** | Off, FXAA, Native TAA or DLSS (DLAA). DLSS turns on or off from the next start; the others switch at once. | FXAA |
| ↳ **DLSS quality** | How many pixels DLSS renders before it upscales: DLAA (sharpest, most demanding), Dynamic, Ultra performance, Performance, Balanced, Quality; from the next start. | DLAA (native resolution) |
| ↳ **Eyes in the headset's shape** | Draws each eye in your headset's own shape with square pixels, so fewer pixels are drawn for the same sharpness (FXAA only, takes full effect from the next start). | Off |
| ↳ **Eyes in the headset's shape with DLSS** (new in 0.8.0) | The same with DLSS: each eye rendered smaller in your headset's shape and upscaled by DLSS into an eye-shaped image; it also turns on Eyes in the headset's shape. From the next start. | Off |
| **Render resolution: headset** | The game's frame size from the next start: your headset's screen height, Automatic (the size your VR runtime asks for), or the game's own size. Sizes your graphics card's memory can't handle are greyed out. | The game's own (its Graphics menu) |
| ↳ **Render resolution: size** | 100%, 150% or 200% of the chosen headset's pixels, still 16:9; from the next start. | 100% once a headset or Automatic is chosen |
| **Now: ...** | Shows the size the game is running at now, or why it uses its own size. | – |

With Automatic, leave your runtime's own resolution setting at its default (100% in SteamVR), because it multiplies with the size you choose here. A "Restart the game to apply" note appears whenever your choice differs from what is running.

Under the **Performance** heading (new in 0.8.0):

| Setting | What it does | Default |
|---|---|---|
| **Shadows drawn once for both eyes** | The sun's, the lamps' and the spot lights' shadows drawn once a frame, fitted for both eyes, instead of once per eye: about 0.9 ms less CPU a frame. The shadows' edges can move by a texel. | On |
| **Batched hand-off to the GPU thread** | The game's render thread hands its recorded work to its GPU thread in batches instead of one command at a time: about 2.5-3 ms less CPU a frame, nothing drawn differently. | On |

### Comfort

How you see and move: the first-person view, how much of John's body is drawn, stick turning, and riding.

| Setting | What it does | Default |
|---|---|---|
| **Recentre** | Puts the view, the body's heading, the cinema screen and the HUD back in front of you. | – (button) |
| **First person** | The view at John's eyes, moved by your head. Off gives the game's own camera turned by your head. | On |
| **Eye height** | Raises or lowers your eye height, 1 cm per press (-0.20 to +0.30 m). | +0.09 m |
| **Shown: Whole body / Forearms and hands / Hands only** | How much of John is drawn in first person (only what you see changes, not gameplay). | Forearms and hands |
| **Forearms and hands when crouching, in cover or riding** | Draws only the forearms and hands at those times, when the body gets in the way of the view. | On |
| **Hide head and hat** | Hides John's head and hat in first person; cutscenes still show them. | On |
| **Torso: Upper body upright / Remove lean only / Game animation** | Holds the drawn torso upright over the hips, removes only its forward/back lean, or draws it exactly as animated. | Upper body upright |
| ↳ **Torso pitch** | Tilts the upright torso, 1 degree per press; positive leans it forward. | +2° |
| **Body back** | How far behind the camera the body is drawn, so looking down shows your chest from outside. | 0.04 m |
| **Keep the torso over the hips** | Stops running from pushing the torso forward into view. | On |
| **Lock body facing** | The body, legs included, faces where the camera faces, so walking sideways doesn't turn a shoulder into view. | On |
| **Body moves with the headset** | The body follows your head when you lean; off, it stays where John stands. | Off |
| **Smooth turning / Snap turning** | How the right stick turns you. | Smooth turning |
| ↳ **Turn speed** | Smooth turning speed with the stick pushed all the way, 10°/s per press (30 to 240). | 90°/s |
| ↳ **Snap angle** | The size of each snap turn, 5° per press (10 to 90). | 30° |
| **Riding: Steer with the stick / Steer with the head** | With the stick, the view faces the horse and the left stick steers while you look around freely; with the head, the left stick goes where you look. | Steer with the stick |
| ↳ **The right stick turns the view while riding** (new in 0.7.0) | The right stick turns your view on horseback while the left stick still steers; mounting or recentring faces the horse again. | On |
| **View from the saddle** | Puts the camera at a smoothed rider's eye height above the horse, so the gallop's bounce stays out of the view. | On |
| ↳ **Keep its height on climbs** | Stops the saddle view sinking toward the saddle on a steep gallop. | On |
| ↳ **Saddle smoothing** | How much the saddle view's height is smoothed, 0.05 s per press (0 to 0.60 s). | 0.15 s |
| **Left stick click brakes** | On a horse or wagon, hold the left stick click to brake (pull the stick back too for a hard stop); on foot it still crouches. | On |

### Hands

Fits John's hands and arms to your controllers and sets how the gun in your hand is held and aimed, plus dual wielding.

| Setting | What it does | Default |
|---|---|---|
| **Interaction spot** | Moves the point your hands grab with (the white dot on the holster rings) away from the wrist, 5 mm per press. | right -0.030, up +0.005, forward +0.085 m |
| **Controller fit: Offset / Pitch / Yaw / Roll** | Moves and turns John's hands against your controller type. Saved for that controller type only, and shown once the mod has seen your controller. | 0 (no change) |
| **Arms follow the controllers** | John's drawn arms bend so each hand reaches your controller and turns with it. | On |
| ↳ **Arms stretch to reach the controllers** | When your controller is beyond John's reach, his arm lengthens so the hand stays on it. | On |
| ↳ **Longest arm** | The longest an arm may stretch, as a multiple of John's arm length (1.0 to 1.6). | 1.60× |
| **Wrist offset** | How far back from the controller's grip John's wrist sits, 5 mm per press. | 0.065 m |
| **Hand pitch / Hand yaw / Hand roll** | Tilts, turns and rolls John's hand against the controller, 2° per press (mirrored for the left hand). | -68° / 0° / 0° |
| **Reticle where the shot lands** | While aiming, shows a reticle where the bullet will hit; it turns red over a person or animal. | Off |
| ↳ **Ring and dot / Dot only** | The reticle's look. | Ring and dot |
| **Keep the gun drawn** | A drawn gun stays in your hand until you put it away, instead of being holstered 3 seconds after your last shot. | On |
| **No executions up close** | Pulling the trigger close to someone fires the gun, so no third-person execution, pistol whip or butt strike starts. | On |
| **Shoot past the arm block** | Lets you fire when the game thinks John's shoulder-to-gun line hits a wall; a friendly in the way still blocks the shot. | Off |
| **Long guns held by their aiming grip** | Each long gun is held the way the game holds it when aiming, in every pose (learned per gun as you aim). | Off |
| **Sidearms held by their aiming grip** (new in 0.7.0) | Each revolver or pistol is held the way the game holds it when aimed, so it moves with your hand at once instead of lagging when the game runs slowly. | Off |
| **Dual wield** | With a gun in one hand, grip another holster with the free hand to take a second gun with its own trigger, barrel and ammo. | On |
| **The same sidearm in both hands** | With a sidearm in hand, grip a hip holster with the free hand to take a copy of it; let go at a hip to put it back. | On |
| ↳ **The copy drawn as a prop** | Draws the copy as a model of the gun, so you see it in the free hand. | On |
| ↳ **Your other sidearm's model** | If you own a different sidearm, the copy shows that gun instead, and its shots leave that gun's muzzle. Since 0.8.0 it is the gun that hip's holster shows, and the holster is empty while you hold it. | On |
| ↳ ↳ **A second of the gun at its own holster** | Needs Your other sidearm's model on. Gripping the holster the gun was drawn from gives a second of that same gun; the other hip still gives your other sidearm. | On |

### Holsters

The body holsters: how you draw, what they follow, what is shown, and where each one sits.

| Setting | What it does | Default |
|---|---|---|
| **Body holsters** | Reach to a holster and grip to draw; grip and let go there with the gun in hand to put it away (a short buzz marks a holster). | On |
| ↳ **Putting a gun away selects the fists** | Holstering also switches to unarmed, so the left trigger gives the fist-fight stance instead of drawing again. | On |
| ↳ **Draw into the hand that grabs the holster** | The gun goes into whichever hand grabbed it, and that hand's trigger fires. | On |
| ↳ **They follow: John's body / Your headset** | Whether the holsters stay with John's body or move with you (riding and cover always use the body; on horseback, since 0.8.0, they also turn with the right stick's turn of the view). | Your headset |
| ↳ **Turn with the headset** (new in 0.7.0) | The holsters turn around you as you turn your head; off, they face the way John faces. | Off |
| ↳ ↳ **Turn by your head and hands** (new in 0.7.0) | With Turn with the headset on: while both hands aim in front of you, the holsters turn halfway between your head and hands, so looking aside swings them less. | Off |
| ↳ ↳ **Turn dead zone** (new in 0.7.0) | With Turn with the headset on: how far you can look aside before the holsters follow, 0 to 90°; 30 to 45 lets you glance aside without moving them. | 0° |
| ↳ **Steady when you lean** (new in 0.7.0) | Leaning or bending over leaves the holsters at your hips; crouching and stepping still move them. | Off |
| **Place the holsters by hand** (new in 0.7.0, this session) | Close the menu, grip a holster's ring and drag it to a new spot; let go to leave it there (the new place is saved). Untick it in the menu when you're done. | Off |
| **Steady holster edges** (new in 0.7.0) | A hand counts as in a holster until it is clearly out, so there's no buzzing at a ring's edge and no flipping between the two left-hip holsters. | Off |
| **Weapons by: The holsters / The weapon wheel** | Draw by reaching to the holsters, or hold a grip to open the game's weapon wheel, move toward a weapon and let go. | The holsters |
| **Show the holsters** | Shows each holster as a ring: white, green with a hand in it, amber while gripped. | On |
| ↳ **Only near a hand** (new in 0.7.0) | Each ring appears faintly only when a hand comes near it. | Off |
| **Show the hand dots** | A dot at each hand's grab point, green inside a holster or on the foregrip. | On |
| **Show the weapon points** | Shows the foregrip ring, where a round goes in, and the hints for the gun's action. | On |
| **Show the guns at their holsters** | Shows each holstered gun on your body; hidden while it is in a hand. | On |
| ↳ **The long guns on the back too** | Also shows the long guns on your back and left shoulder. | Off |
| **Each holster: Right hip, Back, Belt, Left hip (second sidearm), Back left shoulder, Lower back** | Turns each holster on or off. | On |
| **Left hip (knife and lasso; off: moved to the lower back)** | The old left-hip knife-and-lasso holster; the knife and lasso now sit at the lower back. | Off |
| ↳ **Offset / Size arrows** (each holster and the chest) | Move each holster right / up / forward, 5 mm per press, and change its ring's size; saved at once. The chest spot, where you take a round to reload, is always on. | Placed for the release |

### Gun in hand

Tunes the gun you are holding right now. Draw a gun first: the heading names it, and until you change a value it uses every gun's settings from the Reloading tab.

| Setting | What it does | Default |
|---|---|---|
| **The foregrip: Offset** | Moves where John's front hand sits on this gun, 5 mm per press. | Every gun's (the Sawed-off and Pump-action have their own) |
| **The foregrip ring: Offset / Size** | Moves and sizes the ring where your real front hand takes hold of this gun. | Every gun's (the Bolt Action, Sawed-off and Pump-action have their own) |
| **The loading ring: Offset / Size** | Moves and sizes the ring where a hand-held round goes into this gun. | Every gun's (the Bolt Action has its own) |
| ↳ **Use every gun's** | Clears this gun's own values so it uses the shared ones again. | – (button) |
| **The front hand's pose: Automatic** (new in 0.7.0) | Long guns only: John's front hand uses whichever hold the game picks for its stance. | Off |
| **The lowered hold** (new in 0.7.0) | Always uses the game's lowered-gun hold on this gun. Until the game has shown it once this session, the label says so and the other hold is used. | Off |
| **The aiming hold** (new in 0.7.0) | Always uses the game's aiming hold on this gun. | On |

### Weapons

Picks which weapon each body holster draws, instead of the game's weapon for that slot.

| Setting | What it does | Default |
|---|---|---|
| **Choose the weapon in each holster** | Each holster draws the weapon you pick below, and the game's weapon wheel follows; Automatic draws the game's weapon. | On |
| ↳ **List every weapon you own for every holster** | Lists every owned weapon in every holster's choices, not only the ones that fit (a rifle still sits on the back). | Off |
| **Right hip, Back, Belt, Left hip (knife and lasso), Left hip (second sidearm), Back left shoulder, Lower back** | The weapon each holster draws. On Automatic, the second sidearm and second long gun holsters take a different owned gun when you have one. A weapon you no longer own shows "(not owned: automatic)". | Automatic |

The lists fill in once you are in gameplay.

### Reloading

Reloading by hand from the chest, the round in your hand, two-handed long guns, and every gun's rings.

| Setting | What it does | Default |
|---|---|---|
| **Reload by hand** | Grip at the chest with your free hand to take a round, then bring it to the gun to load it, one at a time (not in Dead Eye). | On |
| **Show the rounds at the chest** | While the gun can take rounds, a row of them is drawn at your chest. | On |
| **The gun hand squeezed at the chest holster:** | Does nothing, Loads one round, or Reloads fully. | Reloads fully |
| **The game's automatic reloads** | Turns the game's own reloads back on alongside the hand reload. | Off |
| **The game's reload button** | Turns the game's reload button back on alongside the hand reload. | Off |
| **A round seen in the hand** | Draws a matching cartridge in your hand when you take one from the chest. | On |
| ↳ **Offset / Direction / Turn / Tilt / Brightness** | Where the drawn round sits in your fingers, which way it points, and how bright it is (the chest rounds too). | Along the fingers, brightness 1.00 |
| **Two-handed long guns** | Grip a long gun's foregrip with your other hand to aim it with both hands. | On |
| ↳ **The front hand snaps onto the gun** | While you hold the foregrip, John's front hand sits in the game's own grip instead of following your controller. | On |
| **Every gun's rings: Offset / Size arrows** | The shared foregrip, foregrip ring and loading ring for every gun without its own (the Gun in hand tab), 5 mm per press. | Foregrip ring 0.110 m, loading ring 0.080 m |
| **A round goes in at a touch** | The round loads when it touches the loading ring; off, your wrist has to be inside the ring. | On |

### Actions

Which guns you work by hand instead of letting the game cycle them.

| Setting | What it does | Default |
|---|---|---|
| **Work each gun's action by hand** | Master switch: flick the right stick down to open a revolver and flick the gun to close it; after a shot, work the bolt, lever or pump yourself before the trigger fires again. | On |
| ↳ **Revolvers drawn open** | An open revolver is drawn open (tipped, swung out or gate open), with the game's reload sounds on your movements. | On |
| ↳ **Lever guns: the lever by your flick** | Flick the gun down to open the lever and up to close it and chamber the round. | On |
| ↳ **Pump-action: the fore-end in your front hand** | After a shot, pull the fore-end back with your front hand and push it forward. | On |
| ↳ **Bolt actions: the bolt in your other hand** | On the Bolt Action and Carcano: lift, pull back, push forward and turn down the bolt by hand. | On |
| ↳ **Single-shot rifles opened by hand** | Open the Springfield's and Rolling Block's breech with your other hand (the Buffalo's with a flick), load one round, then shut it. | On |
| ↳ **Semi-Auto Shotgun: the bolt racked by hand** | After loading from empty, pull the bolt handle back with your other hand; between shots it cycles itself. | On |
| ↳ **Break actions opened by the stick** | Flick the right stick down to break the Double-barrel or Sawed-off open; flick the gun up, or swing the barrels up by hand, to shut it. | On |
| ↳ **John's hand on the barrels as they swing up** | Draws John's hand on the open barrels while you hold them. | On |
| ↳ **John's hand on the part your other hand grips** | Draws John's hand on the bolt or breech you are working. | On |
| ↳ **A click when fired before the action is worked** | A trigger pull on a gun that is open or not yet worked plays the empty click. | On |

### Sounds

What the trigger does on an empty gun.

| Setting | What it does | Default |
|---|---|---|
| **Clicks / Buzzes / Clicks and buzzes / Does nothing** | A hammer click from the gun hand's side, a controller buzz, both, or nothing. | Clicks and buzzes |
| ↳ **Click volume** | The click's loudness from 0 to 1, 0.05 per press. The game's volume settings don't affect it, but the game's slider in the Windows volume mixer does. | 0.16 |

### Gestures

Actions you do with your own hand movements.

| Setting | What it does | Default |
|---|---|---|
| **Hands push loose objects** | An empty hand pushes bottles, crates and chairs with the game's physics (never people). | Off |
| **Grab loose objects** | Grip with an empty hand to hold a loose prop; let go to throw it at your hand's speed. | Off |
| **Throw by hand** | Dynamite, fire bottles, throwing knives and tomahawks fly with your hand's speed and direction: pull the trigger and swing. | On |
| ↳ **Throw strength** | How hard throws fly, as a multiple of your hand's speed (0.5× to 4.0×). | 2.0× |
| ↳ **Throw by letting go of the grip** (new in 0.7.0) | Hold the grip to hold the throwable, pull the trigger to light or ready it, then swing and let go of the grip to throw. A lit one held too long goes off in your hand. | Off |
| **Melee by swing** | A fast swing of either hand punches, stabs with the knife or strikes with the torch. | On |
| **Lasso by hand** | With the lasso in hand, swing forward to throw it and yank back to pull. | On |
| ↳ **Swing speed** | How fast a swing must be to count (1.0 to 8.0 m/s). | 2.50 m/s |
| **Whistle for the horse** | Put a hand at your mouth and pull its trigger to whistle (the gun hand whistles only when it holds no gun). With a gun raised in the other hand, the aim drops for under a second and no shot fires; it comes back on its own, or at once with a trigger pull. | On |
| **Gun-butt melee** | Swing the gun in your hand into someone to land the game's knock-out hit (with the usual crime and anger). | Off |
| ↳ **Dry run** | For testing: hits are only written to the log, never made. | Off |
| ↳ **Hit speed** | How fast the gun's swing must peak to hit (1.0 to 6.0 m/s). | 1.80 m/s |

### Controls

Handedness, how the triggers aim and fire, shot accuracy, and button mapping.

| Setting | What it does | Default |
|---|---|---|
| **Left-handed** | Swaps the controllers' roles: the left trigger fires and the right stick moves. | Off |
| ↳ **The gun in John's left hand** | Each of John's arms follows its own controller and the gun is drawn in his left hand; off, the arms cross. | Off |
| **The trigger alone fires** | In first person, the right trigger alone fires; the mod holds the aim trigger for you. | On |
| **Aim while the gun is raised** | While the gun hand is raised, the aim is held, so every pull fires at once. | On |
| **Sprinting lowers the raised gun** | Pressing sprint drops the held aim so John can sprint with a gun in hand. | On |
| **Jumping lowers the raised gun** | The jump button first drops the aim (up to 250 ms), so John can jump, vault or climb with a gun in hand. | Off |
| **Perfect accuracy** | Every shot goes exactly along the barrel, with no random spread. Other characters keep the game's spread. | On |
| **Shotgun pellets spread** | Pellets spread in the game's cone; off puts them all on one line. | On |
| **Buttons: Right A, Right B, Left X, Left Y, stick clicks, grips, triggers** | Picks the game button each VR button presses. | The button of the same name (grips LB / RB, triggers LT / RT) |
| ↳ **Reset to the game's scheme** | Puts every VR button back on the game button of the same name. | – (button) |
| **The menu: ...** (corrected in 0.7.0: A opens the satchel, not the map) | How to open this menu. A tap of the menu button pauses. Held with the right stick it is the D-pad, and held with A it opens the satchel. | – |

### Screen

How cutscenes are shown in the headset.

| Setting | What it does | Default |
|---|---|---|
| **On the cinema screen / In 3D** | Cutscenes on a floating 16:9 screen, or in 3D around you from the cutscene camera with your head's turning added (you can switch mid-cutscene). | On the cinema screen |
| ↳ **3D separation** | The cutscenes' 3D depth as a fraction of your eye distance (1.00 = your own; lower is flatter). | 1.00 |

### Debug

Live performance and view-mode readouts (frame rate, missed frames, CPU and GPU time, scene render time), and switches for testing that turn the mod's rendering fixes off for comparison. These are for troubleshooting: leave them as they are for normal play. "Automatic view modes" and "HUD on its own quad" should stay on, and "Hidden parts" should stay on Filtered.

### Where to find...

- **Recentre:** Comfort, "Recentre". You can also use the headset's own recentre, Scroll Lock, or hold both stick clicks for a second.
- **Render resolution:** General, "Render resolution: headset" and "size" (from the next start).
- **DLSS:** General, "Anti-aliasing" then "DLSS quality" (from the next start).
- **Eye shape:** General, "Eyes in the headset's shape" (FXAA), or "Eyes in the headset's shape with DLSS" (DLSS, new in 0.8.0).
- **Snap or smooth turning:** Comfort, "Turning on the right stick".
- **Holster placement:** Holsters, "Place the holsters by hand", or each holster's Offset / Size arrows.
- **Horse whistle:** Gestures, "Whistle for the horse".
- **Satchel:** hold the menu button and press A. With the wrist HUD you can also turn on General, "A double tap of Y at the wrist opens the satchel".
- **Left-handed mode:** Controls, "Left-handed".
- **Button mapping:** Controls, "Buttons".
- **Reticle:** Hands, "Reticle where the shot lands".
- **Weapon wheel instead of holsters:** Holsters, "Weapons by: The weapon wheel".

## Tips

- **Sharpness:** each eye shows the game's own frame (16:9), or, with "Eyes in the headset's shape" on, a frame in
  the headset's own shape. For a sharper image on a high-resolution headset, choose the "Render resolution" (the General
  tab): your headset or Automatic, at 100%, 150% or 200%, if your GPU allows (new in 0.4.0, the sizes in 0.6.0); "Eyes in the headset's
  shape" (with FXAA, or "... with DLSS", new in 0.8.0) makes it cheaper, and DLSS Quality (usable since 0.5.0; from the next start) renders each eye at
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
  folder: if the mod stands down or misbehaves, please attach it, with `RDRVR.log`, to an issue. A game executable
  without the Steam version's start-up stage (the same game build otherwise) should no longer start flat (new in 0.6.0,
  not yet tried on such an executable; the developer forced the path on the Steam version): the mod notices and hooks
  at once, and `RDRVR.log` says if its hooks came too late. RedHook, which runs
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
- The front hand on a long gun held two-handed can move on the gun during a shot, most on the Double-barrel (seen
  in the developer's simulator, back on the grip within 3 seconds). `RDRVR.log` has a `[two] a shot two-handed`
  line after each such shot (since 0.8.0 measured as your eyes see it): please attach it to an issue if your hand
  ends up in the gun or in the air.
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
