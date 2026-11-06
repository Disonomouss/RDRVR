# RDRVR: Red Dead Redemption in VR

A native-stereo VR mod for the PC port of **Red Dead Redemption** (2024). The game draws the scene once per eye from
your headset's view, John's body and arms follow you, and the guns are held, aimed, holstered and reloaded with your
hands.

Version 0.8.13 (an early public release). Tested with a Meta Quest 3 on Virtual Desktop; any PC headset with an
OpenXR runtime should work (see [Compatibility](#compatibility)).

> RDRVR is an unofficial fan project. It is not affiliated with or endorsed by Rockstar Games or Take-Two Interactive,
> and it contains no game files. The game sends its crash reports to Rockstar: a crash with the mod installed is
> reported too.

## Features

**VR view**
- True stereo: the scene drawn twice per frame, once per eye, with your headset's own field of view (asymmetric and
  canted displays included) and late-latched head and hand poses.
- Single pass (the default since 0.8.12; the Display tab, "Rendering"): both eyes drawn from one walk of the scene,
  about a millisecond a frame faster than drawing each eye on its own, with shaders converted from your own game files
  at the first start. Two-pass stays a switch.
- Per-eye fixes so both eyes see the same frame: grass, wind, lights, forest, exposure, rain and particles, god rays,
  wetness and blood, cloud shadows.
- Culling, tree billboards and distant trees follow your head, and stay upright.
- Faster in 0.8.0, both on by default and used in a Quest 3 over Virtual Desktop at 90 Hz ("performance is smooth"),
  the shared shadows switchable in the Advanced tab, the batches with `[Render] PlaybackBatch` in the ini (since 0.8.9):
  - the game's render thread hands its recorded work to its GPU thread in batches instead of one command at a time:
    about 2.5-3 ms less CPU a frame in the developer's tests (about a fifth of the stereo frame), the image unchanged;
  - the shadows (the sun's, the lamps' and the spot lights') drawn once a frame for both eyes instead of once per eye:
    about 0.9 ms less CPU a frame; the shadows' edges can move by a texel.
- New in 0.8.4 (tried in the developer's simulator only, not yet in a headset): one-click Quality, Balanced and
  Performance presets in the Display tab (they set the eye shape, DLSS's quality, a cap on Automatic's resolution and,
  for Performance, a 72 Hz refresh rate); a headset refresh rate row where your VR runtime lets the mod choose it (the
  row says when it does not); and a note suggesting a lighter preset when the headset keeps missing frames. The mod's
  own work costs less on slower processors (about 1 ms a frame less on a simulated 4- or 8-core CPU, nothing measurable
  on a fast one; the image unchanged).
- FXAA, the game's TAA or DLSS run per eye (DLSS with its own history per eye, falling back to one shared history
  if its setup fails, which `RDRVR.log` says; since 0.5.0 it is usable in the headset: its images were handed to the
  headset a few frames stale, two defects in what the game feeds DLSS are corrected, its jitter's length and units,
  and a hitch no longer drops the per-eye histories); the game's own colour and gamma.
- Optional (new in 0.3.0, off by default, tried briefly in a Quest 3; the Display tab, under the anti-aliasing, with
  FXAA): each eye rendered in your headset's own shape and size, with square pixels, instead of the game's 16:9
  frame stretched over it: fewer pixels drawn for the same sharpness. Restart the game after turning it on: the eye
  images take their size at the start. The same with DLSS (new in 0.7.0; off by default; since 0.8.0 tried in a
  Quest 3 over Virtual Desktop, where it looked as sharp and took about a quarter less GPU time at 3926 x 2208 with
  DLSS Quality): the eye-shaped image upscaled by DLSS into an eye-shaped output. Choose DLSS in the Display tab's
  Anti-aliasing, then tick "Eyes in the headset's shape with DLSS" under it (new in 0.8.0), from the next start. It
  applies only while DLSS runs per eye; if DLSS falls back to one shared history, the eyes get the game's frame
  (`RDRVR.log`'s `[eye]` lines say which).
- Optional (new in 0.4.0, off by default, tried briefly in a Quest 3; the Display tab, "Render resolution: headset"
  and "Render resolution: size", from the next start): the game's frame at your headset's height, apart from your
  monitor's modes and without DSR. Choose your headset, then its size: 100%, 150% or 200% of its pixels (new in 0.6.0,
  tried in the developer's simulator only; 15 headsets from the Valve Index to the Galaxy XR (new in 0.8.4), by their
  screens' heights; for example a Quest 3 at 3926 x 2208, 4808 x 2704 or 5550 x 3122). Or choose
  Automatic: the size your OpenXR runtime (SteamVR, Pimax Play, Virtual Desktop...) asks for, for its lenses, at those
  percentages; it is learned in a first session with that runtime (the game's own size until then), never below your
  own resolution. Keep the runtime's own resolution setting at its default (SteamVR's 100%): it multiplies with the
  mod's. A size of your own can be written in `%LOCALAPPDATA%\RDRVR\RDRVR.user.ini`, where the menu saves its choice
  (`[Render] RenderResolution`, a height or WxH, 16:9; the menu then shows it as Custom). In
  Windowed mode your window keeps its size (the frame is scaled into it). The game's Graphics menu shows the size, and
  `graphicsOptions.xml` keeps your own resolution. With "Eyes in the headset's shape" on, each eye is drawn at the size
  your headset asks for, at most this tall. Larger is sharper and costs more GPU time. A size too large for the
  graphics card's memory is greyed out in the menu; the row's "Now:" line says which size runs, or why not.
- New in 0.8.5, on by default (used in a Quest 3 over Virtual Desktop): motion blur held off (the Display tab, "Motion
  blur" off; it smears the view as you turn your head). The game's settings file is not changed, and its Graphics
  menu still shows and saves your own setting. A switch for the desert's heat haze (the Display tab, "Heat haze", on
  as in the game; some find it uncomfortable in VR): off removes the shimmer.
- Fixed in 0.8.5 (tried in the developer's simulator only): coming back from Undead Nightmare restarts the game, and
  that start ran at the game's own size instead of the "Render resolution" one, so the view was blurry. The size is
  now put back; `RDRVR.log` records each start's command line.
- The HUD on a floating panel, or on your wrist like a watch (the default: the radar, its meters and the ammo shown
  when you look at it; since 0.8.12 the rest of that corner, subtitles too, stays on the panel).
- Looking through a rifle's scope (since 0.8.12; the Guns tab): bring the eyepiece to either eye and that eye sees the
  scope's magnified picture.
- Automatic view modes: stereo in gameplay, a cinema screen for loading, menus and videos; cutscenes on the screen or
  in 3D.
- New in 0.8.7, on by default (used in a Quest 3: skinning a deer stayed first person): a script's short scenes that
  showed John in third person (a mission's quick shots, looting and skinning, a chest, a bed, a door) stay at John's
  eyes; the game's own camera for the scene still runs behind it, so the scene plays on as made. The full cutscenes
  are unchanged (the screen or 3D; a cutscene's very first frame may still show from John's eyes). The Display tab,
  "Short scenes in first person". Not yet tried: looting, a mission's quick shots, a chest, a bed, a door.
- New in 0.8.8, on by default (used in a Quest 3: "spatial audio is good"): the game's sound is heard from your
  head. It placed every sound for the way John's body faced, so turning your head moved nothing and you could not tell
  where things came from; now a voice comes to the front as you face it. And on headphones (any stereo device) the
  game dropped what it pans behind you to a third of its level, so people behind you sounded far away; that is now
  folded into your left and right ears at full level (the game's own slight quieting of sounds behind you is kept;
  a 5.1 or 7.1 device keeps the game's mix). In the ini, `[XR]` `AudioFollowsHead` and `AudioRearFold` (0 = as before).
- Fixed in 0.8.8 (tried in the developer's simulator only, not yet in a headset): the gun in your hand could smear or
  trail behind your hand, mostly in fights. With DLSS or the game's TAA, its motion vectors (what carries the image
  from frame to frame) were the game's own move of its gun, its aim and its recoil, not your hand's. They are now taken
  from where the gun was drawn the frame before. In the ini, `[Hands] GunMotionVectors` (0 = as before).

**Body**
- A first-person body (full, forearms and hands, or hands only), upright and facing your view, no head in the way.
- Arms that follow the controllers (two-bone IK, stretching past John's reach), the eye height following crouching
  and riding.
- A left-handed mode: a full mirror (the gun in John's left hand, the holsters and reloading mirrored).
- Moving in your room (since 0.8.12, each off by default; the Comfort tab): room-scale walking (John walks under you as
  you step), a physical crouch, and John turning when you turn your body.
- John's shadow matches the body you see (since 0.8.12: his belt's and his hands' shadows fell where the game's own
  John would stand).
- New in 0.8.5, off by default (used in a Quest 3: "looks really good"; the Comfort tab, Torso: "Still"): a still body,
  the torso, hips and belt held still under your view, only the arms and legs moving. The belt and the holster on it
  stay with the body (they swung with the legs). Optional, in `RDRVR.ini` only (`[Body] StillFeet=1`, tried in the
  developer's simulator only): the feet kept where the game plants them, by a leg solve.
- New in 0.8.5, on by default (used in a Quest 3): drawing a gun no longer lowers the view (the game's armed stance
  lowered the view by about 10 cm; `[Body] ArmedStance=game` in `RDRVR.ini` turns this off, no menu row). Since the same release, tried in the developer's simulator only: the view also keeps its
  height for a few seconds after putting a gun away, and in the fist-fight stance (they lowered it by 10 and 30 cm).

**Guns**
- New in 0.8.1: every gun's aiming hold is built in (learned for all 21 guns in the developer's Quest 3 over Virtual
  Desktop; for the guns tried in the developer's simulator, the game's own aim there matches it within a fraction of
  a millimetre), and both "held by their aiming grip" settings are on by default (the Advanced tab since 0.8.9). A gun now sits in your hand the way the game aims it from its
  first draw, instead of only after you have aimed it once, and a sidearm moves with your hand at once instead of
  lagging when the game runs slowly. The holsters, the foregrip rings and the loading points are placed anew (in the
  same headset), and the holster rings, the hand dots and the weapon points are hidden by default (the Holsters tab
  shows them again). A place, size or switch you set yourself is kept: only the ones you never changed take the new
  defaults. The new defaults on a fresh install are tried in the developer's simulator only.
- Fixed in 0.8.1 (from a player's log; the fixes tried in the developer's simulator): after a cutscene, the gun the
  game puts in your hands by itself went to the hand that drew last, and taking another gun then put it inside the
  first; such a gun now goes to your gun hand. Putting away the gun in hand while a second gun is out in the other
  hand makes the second your gun in hand (before, every holster refused that hand).
- Fixed in 0.8.5 (used in a Quest 3): the holsters keep their places after riding and after cutscenes (they were
  placed again during the dismount, or from the cutscene's camera, once across the road); a fast swing of an empty hand
  on horseback no longer draws your last gun; the holstered guns no longer take a nearby person's gun of the same model
  onto your holster.
- Shots leave the barrel of the gun in your hand; the right trigger alone fires; the gun stays drawn until you put it
  away. Since 0.8.12 you put it away by letting go of the grip that holds it ("Hold the grip to keep the gun", the
  Holsters tab, on): guns, the knife, the torch, the lasso and the throwables stay in your hand while you hold the
  grip and go back to their holsters when you let go, anywhere, hidden on the way (no holstering animation).
- An optional reticle where the shot will land (off by default: the Guns tab), a ring or a dot (new in 0.5.0). Since
  0.8.3 it shows whenever a gun is in hand, in Dead Eye too, found by a line test along the barrel each game tick, and it
  turns red only while you aim and the game's target actor is under it (it could stay red before). Tried in the
  developer's simulator, not yet in a headset; turning red on a person not yet tried.
  Since 0.8.5 (used in a Quest 3) it follows the barrel every frame with your hand's tremor smoothed (it moved in steps
  a tick behind the hand), and with two guns each has its own reticle (the second one never turns red); fixed later in 0.8.5 (tried in the developer's
  simulator only): the second gun's reticle on that gun's own line of fire (it was off its aim).
- Holsters you grab: both hips (sidearms), both sides of the back (long guns), the knife, the satchel (throwables), the
  lasso, the chest (rounds); a weapon chosen per holster; each
  holster movable and resizable. They follow John's body by default since 0.8.9 (or your headset, so they stay with
  you as you step, crouch and lean: the Holsters tab, "They follow").
- Reloading by hand: take rounds at your chest (up to 8 a grab since 0.8.12) and bring them to the gun's loading point.
- Every gun's action worked by hand (revolvers cocked and opened by the stick, levers, pumps, bolts, breeches,
  break-actions, and since 0.8.12 the pistols' magazines and slides and the Blunderbuss's ramrod), with the game's own
  sounds on your movements; two-handed long guns, carried by the foregrip in the front hand (since 0.8.12); dual
  wielding (a second gun, or a copy of your sidearm; since 0.8.12 also the knife, the torch or a throwable beside a gun);
  perfect accuracy (your shots only: other people's keep the game's spread); the guns shown at their holsters;
  throwing and punching by swinging, the lasso by the left trigger (aim) and the right (throw) in either hand (since
  0.8.12). All on by default, each can be switched off in the menu. Off by default: pushing and grabbing loose objects.
- The part of a gun you work next glows on the gun itself (since 0.8.12; the Holsters tab, "Interaction points"): the
  hammer, the lever, the bolt, the slide, the loading gate, the foregrip.
- The holsters, the dots at your hands and the guns' points (the foregrip ring, the loading point, the action hints)
  each shown or hidden by its own switch (new in 0.6.0, tried in the developer's simulator; the Holsters tab).
- New in 0.7.0, off by default (each in the Holsters tab, tried in the developer's simulator only): the holsters turn
  with your headset (with an adjustable dead zone, 0° by default so they follow every look; at 30 to 45° a glance
  aside leaves them), or by your head and hands together; steady when you lean forward or bend over; steady holster
  edges (no buzzing at a ring's edge); each ring shown only while
  a hand is near it; and placing the holsters by hand (grip a ring and move it). An experimental holster model at the
  hips (`[Holsters] ShowModels`, ini only) still looks like a strap.
- New in 0.8.5, off by default (used in a Quest 3; the Holsters tab, "Any weapon in the gun holsters"; since 0.8.9 the
  gun holsters only): the hips' and the backs' holsters take any weapon you own. Put one away at a gun holster and it is
  drawn from there again. The knife's, the satchel's and the lasso's holsters keep their own items, the chest the rounds.
- New in 0.8.5 (used in a Quest 3; the Holsters tab, under "Show the guns at their holsters"): each shown gun's angle
  and place at its holster, by arrows. Fixed in 0.8.5 (tried in the developer's simulator, from a player's
  description): turning toward a holster could show a second copy of its gun beside it, facing another way (the game's
  own holstered gun drawn apart from the mod's).
- The holsters placed anew for 0.8.5 (in the developer's headset); a place or size you set yourself is kept.
- New in 0.8.9 (used in the developer's Quest 3; the Holsters tab, "Customize John's belt", on by default with the
  developer's layout): John's own holster, knife sheath, satchel, lasso and rifle sling moved and turned where you put
  them, by arrows or by hand ("Placing by hand: Belt pieces and guns"), each drawn whole where you put it; a mirrored
  copy of his holster at the left hip for the secondary sidearm. Untick it for the game's own belt (your places are
  kept); "Reset the belt to the game's positions" puts every piece back. It replaces 0.8.8's belt of props, which is
  gone.
- New in 0.8.9 (used in the developer's Quest 3): each holster's ring sits on its own piece of the belt (the right hip
  on the holster, the left hip on the mirrored one, both backs on the sling, the knife on the sheath, the satchel on
  the satchel, the lasso on the lasso; the ammo ring where you put it). "Rings stay on their holsters" (on) keeps a
  ring and its piece together: moving one moves the other the same way. "Reset the rings to their defaults" puts every
  ring back. Fixed (the same release): the left hip's and the left back's rings swayed with John's idle (mirrored
  through his root bone, which drifts against the body) and moved with the right hip's ring.
- New in 0.8.9 (used in the developer's Quest 3): a holstered gun turned and moved by hand to sit in its holster
  ("Placing by hand: Belt pieces and guns", grip the gun), as its Gun's angle and place arrows do.
- New in 0.8.9 (tried in the developer's simulator only): a Lasso holster on John's lasso, in place of the lower back's
  knife-and-lasso holster; the knife holster holds the knife only.
- The foregrip ring held still on the gun (new in 0.6.0, tried in the developer's simulator, not yet in a headset): it
  followed the game's arm sway, so the off hand could grab the air beside the gun, worst on the Sawed-off (which now
  has a grip of its own).
- No executions: the trigger fires at close range instead of starting John's third-person execution.
- Changed in 0.8.7 (used in a Quest 3: "head aim and trigger work well"): the lasso is aimed where you look and thrown
  by the lasso hand's trigger (the game's aim held 0.3 s first, so it locks on). Other choices on the Gestures tab,
  under "Lasso by hand": thrown by a swing toward the aim (overhand, sidearm or straight; still hard to time) or
  either, aimed by the lasso hand (held still once the swing starts) or by the other controller (these three tried in
  the developer's simulator only).
- New in 0.7.0, on by default (tried in the developer's simulator, not yet in a headset): the lasso held in your
  hands (the coil in the off hand, the loop in the gun hand; it floated in front of them before); with dual wielding,
  a second gun drawn from a back holster is that holster's gun (a sidearm in hand made every back holster give the
  same gun); each long gun's front hand in the game's aiming hold, its fingers too (the Guns tab, "The front
  hand's hold": aiming, lowered, or Automatic, the way 0.6.0 chose by the game's stance); a long gun held lowered no
  longer flips its front hand between holds (with Automatic).
- New in 0.7.0, off by default (tried in the developer's simulator only): throwing by letting go of the grip (the
  Gestures tab, under "Throw by hand": grip to hold the throwable, the trigger to light or ready it, swing and let
  go; tried with dynamite and the throwing knife, the fire bottle and the tomahawk not yet); sidearms held by their aiming grip (the Advanced tab, "Sidearms held by their aiming grip", on by default since
  0.8.1: for a revolver that lags the hand).
- A whistle for your horse (new in 0.6.0, used in a headset; on by default since 0.7.0): your hand at your mouth and
  its trigger (the Gestures tab, "Whistle for the horse"; that trigger does nothing else there; the gun hand whistles
  only with no gun in it). New in 0.7.0 (tried in the developer's simulator only): the off hand whistles with a gun
  raised in the other: the game will not whistle while aiming, so the aim is let go for under a second first, and no
  shot fires meanwhile.
- New in 0.6.0, off by default (tried in the developer's simulator, not yet in a headset): the game's weapon wheel
  instead of the holsters, a grip held opening it in front of your hand, the hand moved toward a weapon's picture and
  the grip let go to take it (the Holsters tab, "The weapon wheel").
- New in 0.8.5, on by default (used in a Quest 3, in a fist fight): with your fists out, both hands raised before your
  face hold the game's block for as long as you hold them there (the Gestures tab, "Guard with both fists"). Fixed
  later in 0.8.5 (tried in the developer's simulator): when the game had put a gun away itself, the guard could draw
  that gun; it selects the fists first now. Changed in 0.8.6 (tried in the developer's simulator, not yet in a
  headset): the guard needs both triggers held too, so raising your fists alone no longer takes the fighting stance
  (which alarmed people nearby); the right trigger does not punch while guarding. `GuardTriggers=0` under `[Gestures]`
  in your ini brings back the hands alone.
- New in 0.3.0, off by default (each in the menu, most not yet tried in a headset): a gun-butt melee, the gun in your
  hand swung into someone landing the game's own melee blow where it meets them (knock-out damage, as a punch does;
  the Gestures tab, "Gun-butt melee"; tried in a headset with 0.5.0, where it hardly ever hit: it needed 3 m/s at the
  moment of contact. Since 0.6.0 a hit is judged by your swing's fastest moment, from 1.8 m/s (the "Hit speed" under
  it), the gun still moving into them where it meets them; the new rule tried in the developer's simulator only); shooting from right behind cover (the Guns tab, "Shoot past the arm block");
  jumping, and so vaulting and climbing, with a gun raised (the Controls tab, "Jumping lowers the raised gun"); the
  gun hand kept at each long gun's aiming grip in every pose (the Advanced tab, "Long guns held by their aiming grip").

**Controls and comfort**
- The controllers drive the game's gamepad (every button remappable), with the game's rumble on the controllers.
- A desktop launcher (since 0.8.12) opens as the game starts: every setting before you put the headset on, then Save &
  Play, or Close to cancel the start.
- Smooth or snap turning; on a horse, steering by the stick or by your view, the left stick click brakes. New in
  0.7.0, on by default (used in a headset since 0.8.0): with stick steering the right stick turns the view on
  horseback too, while the left stick steers the horse (the Comfort tab, under "Steer with the stick"). Since 0.8.0
  the holsters turn with your view as you do (before, they kept facing the horse); recentring faces them back.
  New in 0.8.5, on by default (used in a Quest 3): driving a wagon or a coach, the view's heading follows it as it
  follows a horse (`[Horse] VehicleHeading=0` in `RDRVR.ini` turns it off, no menu row).
- The game's own menus by the controllers (new in 0.7.0, on by default, tried in the developer's simulator only, not
  yet in a headset; `[Controls] MenuControls` in `RDRVR.ini`, no menu row): in the satchel and shops the triggers
  change tabs; in the satchel, the pause menu and shops the left stick moves one item a push. Fixed in 0.8.2: the pause
  menu's map keeps the game's own controls (the left stick moves the map, LT and RT zoom); since 0.7.0 it did not
  respond to them (tried in the developer's simulator).
- Shops in first person (new in 0.7.0, on by default; tried at a shop in the developer's simulator, not yet in a
  headset; `[Body] KeepAnchorCamera` in `RDRVR.ini`, no menu row): talking to a shopkeeper no longer takes the view to
  third person. Fixed in 0.8.4: only a shop's camera is taken back now; any other script's camera (a mission's
  scene) is left to it until it lets go, then first person returns. Before, the mod took the camera back from a
  mission's scene that took it once, so the scene could stop before it began, with no control of John (seen in two
  players' logs; 0.8.1's fix covered only a script that kept taking it). The fix is tried in the developer's simulator
  with a stand-in script camera and at a shop, not yet in that mission. If one looks wrong, set `KeepAnchorCamera=0`
  and please report it.
  Fixed in 0.8.5 (tried in the developer's simulator only, where it did not happen): leaving a shop could leave the
  view in third person; for 10 seconds after a shop's menu closes, first person is taken back too. `RDRVR.log` notes
  the shop's menu opening and closing.
- An in-headset menu (laser and trigger) with every setting, grouped under headings with a help line for the row
  under the laser; the tabs are a row of buttons, and each tab's page scrolls with your gun hand's stick (the
  laser on the page); adjustments by arrows, one step a press. Cleaned up in 0.8.9 (used in the developer's Quest 3):
  nine tabs instead of thirteen, each setting where you would look for it, shorter plain tips, "(restart)" on the rows
  that need one, and the switches whose off side only brought back a fixed problem moved to the ini (see the
  [Menu guide](#menu-guide)).
- New in 0.8.9: this release starts every player from its defaults once (the developer's own settings, without the
  picture settings that depend on your graphics card and headset, the weapon choices or the buttons). Your old
  settings are kept as `RDRVR.user.ini.before-0.8.9.bak`. 0.8.12 does so again (below).
- Fixed in 0.8.9 (tried in the developer's simulator, with a stand-in for the game's restart): going back to the
  game's main menu (the game restarts itself) crashed; the render size was not handed to the restarted game. A size
  the mod once marked as risky after an early quit is no longer blocked: every render resolution can be chosen.
- Fixed in 0.8.9 (used in the developer's Quest 3): on horseback, John's body and holsters stay under you at a gallop
  (they slid about 45 cm back; `[Horse] BodyWithCamera`, on).
- Fixed in 0.8.10 (tried in the developer's simulator): in fist fights, other men's hats popped in and out of view
  (a hat near John's head was taken for his own and hidden); John's own hat could also show briefly after a mounted gun.
- Fixed in 0.8.10 (used in the developer's Quest 3): on a mounted gun (the Gatling, the Browning, the cannon) John and
  the gun spun around with no way to stop. The game's own camera and aim now take over while you use one.
- New in 0.8.10, experimental and off (tried in the developer's simulator only, which cannot put John behind a mounted
  gun): **Mounted guns in first person** (the Guns tab) keeps first person on a mounted gun and turns the gun toward
  where your gun hand points (it learns the gun's barrel from your first shot). While it is on, the right stick no
  longer turns the gun: if the gun does not follow your hand, turn it off.
- New in 0.8.10 (tried in the developer's simulator only): **World scale** (the Comfort tab), how big the world looks.
- Fixed in 0.8.11 (used in the developer's Quest 3): a gun drawn outside and then taken indoors shimmered when held
  still and jittered when moved (one of its draws took your hand's place from the frame before; `[Hands] StaleGunFix`,
  on).
- New in 0.8.11 (tried in the developer's simulator): **Fists on both triggers** (the Gestures tab, on). With nothing in
  your hands, hold both triggers for the fighting stance and swing a fist to punch while holding them (whatever "Melee
  by swing" says); raise both fists before your face to block. A trigger alone does nothing. If something in the game
  waits for a single trigger with nothing in your hands and does not respond, turn this off.
- New in 0.8.12: this release starts every player from its defaults once (the developer's own settings of 2026-11-04,
  without the display settings and the holsters' weapon choices; the controls included). Your old settings are kept
  as `RDRVR.user.ini.before-0.8.12.bak` (see [Installation](#installation)).
- New in 0.8.12, the picture and performance:
  - **Single pass**, now the default (used in the developer's Quest 3; the Display tab, "Rendering", restart): both
    eyes drawn from one walk of the scene, about a millisecond a frame faster. The shaders it needs are converted from
    your own game files by the shader converter the installer puts in `RDRVR_Converter\`, in the background at the
    first start (about 20 seconds; until a shader is ready, what uses it is drawn as two passes), and kept in
    `%LOCALAPPDATA%\RDRVR\vi_cache` (made again after a game update). "Two-pass" draws each eye on its own, as before.
  - Fixed (tried in the developer's simulator): the frame rate could fall to a few frames a second and stay there.
    The mod's checks before it reads the game's memory (about 20 a frame) came to take about 10 ms each once the game
    had written to enough of its memory; they now ask a much cheaper question first.
  - Fixed (used in the developer's Quest 3): John's shadow matches the body you see. The shadows of his belt pieces and
    of what he held fell where the game's own John would stand and face, and the left hand's shadow was open around
    what it held.
  - **Sharpening** (tried in the developer's simulator; the Display tab, 0% = off, the default): AMD CAS sharpening of
    each eye in the headset, restoring detail DLSS and TAA soften.
  - The wrist HUD takes only the radar and the ammo counter (tried in the developer's simulator): the rest of that
    corner of the screen, a long subtitle too, stays on the floating screen (`[XR] WristHudShape=rect`: the whole
    corner, as before).
  - The mod's menu also shows in the game's window (tried in the developer's simulator), so a recording of the window
    has it.
- New in 0.8.12, **the launcher** (used in the developer's Quest 3): `RDRVR_Launcher\RDRVR_Launcher.exe` in the game
  folder opens on the desktop whenever the game starts (from Steam, the Rockstar Games Launcher or `RDR.exe`), and the
  game waits for it. It has the Display tab's picture settings, every other menu setting and a picture of what each
  controller button does, so you can set things before you put the headset on. Save & Play saves and lets the game
  start; Close (or the window's X) cancels the start. Untouched, the game goes on with the saved settings after 30
  seconds (a click stops the countdown; the game then waits up to 5 minutes). Run with the game closed, its Save &
  Play starts the game. The Advanced tab, "Show the launcher when the game starts" (on) and "Goes on by itself after".
- New in 0.8.12, moving in your room (the Comfort tab; each off unless named):
  - **Room-scale walking** (used in the developer's Quest 3): step in your room and John walks under you, so your view
    moves only with your head; **Small steps at once** puts him under you at once for a step up to 20 cm. "Body moves
    with the headset" moved there from the Body tab.
  - **Physical crouch** (tried in the developer's simulator): crouch for real and John crouches; "Set my standing
    height" sets the height it is measured from.
  - **John turns when you turn** (tried in the developer's simulator): turn your body in your room and John turns with
    you; a look aside does not.
  - **Recentre on getting on** (used in the developer's Quest 3; on): getting on a horse, onto a wagon or into a
    vehicle turns your view to face the way it faces.
- New in 0.8.12, holding and the holsters (the Holsters tab; all on):
  - **Hold the grip to keep the gun** (used in the developer's Quest 3): grip at a holster to draw and keep holding; let
    go anywhere and the gun goes back to its holster. With your other hand on a long gun's foregrip, letting go leaves
    the gun carried by that hand. The knife, the torch, the lasso and the throwables are held the same way
    (`[Holsters] HoldItems`); a throwable let go during a throw flies. The game's RB (cover) is now the gun hand's
    stick flicked up, as the grip holds the gun.
  - Put away unseen (used in the developer's Quest 3): a gun or an item you let go is hidden on its way to its holster.
    John takes no step and no holstering animation shows, nor the flames of the fire bottle and the torch, nor the
    lasso's rope.
  - **Interaction points** (used in the developer's Quest 3; under "Shown"): with "Glowing parts", the default, the
    part of the gun you work next (the hammer, the lever, the bolt, the slide, the loading gate, the foregrip) glows on
    the gun itself, hidden where your hand covers it. Or soft highlights over the places, the rings as before, or both.
  - The weapon wheel (used in the developer's Quest 3; with "Weapons by: The weapon wheel"): the wheel hand's stick
    picks another gun of the highlighted kind, as the game's D-pad does, and the world nearly stands still while the
    wheel is open.
- New in 0.8.12, dual wielding and the free hand (the Guns tab; all on unless named):
  - **Carry a long gun by its foregrip** (used in the developer's Quest 3): holding a long gun two-handed, let go with
    the gun hand and the front hand keeps it (it does not fire), the gun hand free to draw another weapon; grip near the
    gun's grip to take it back.
  - **Items in the free hand** (used in the developer's Quest 3): with a gun in one hand, the other draws the knife, the
    torch or a throwable while the gun keeps aiming, shooting and reloading. The knife drawn so is the game's own knife
    in that hand. In Undead Nightmare, "The free hand's torch lit" (experimental) lights that torch.
  - A second gun beside the knife, a throwable or the torch (beside the knife used in the developer's Quest 3, beside
    the torch tried in the developer's simulator; "A gun beside the torch").
  - **Each hand reloads the other gun** (used in the developer's Quest 3): with two guns out, either hand takes rounds
    at the chest for the gun in the other hand.
  - **Open the second gun by its stick** (tried in the developer's simulator): a revolver as the second gun opens by its
    own hand's stick, held down and clicked.
  - The copy fires at its own gun's rate (used in the developer's Quest 3; `[Hands] CopyOwnRate`, on): with "Your
    other sidearm's model", the copy fires at the rate of the gun it shows, and each gun waits on its own shots only.
  - **Guns touch** (used in the developer's Quest 3): your two guns, your hands and your arms don't pass through each
    other; the other gun swept back across a single action's hammer cocks it.
  - **Gun kick** (tried in the developer's simulator; 0% = off, the default): each shot kicks the gun in your hand for
    a moment. Looks only.
- New in 0.8.12, guns and reloading (all on unless named):
  - **Look through a rifle's scope** (used in the developer's Quest 3; the Guns tab): bring the Rolling Block's or the
    Carcano's eyepiece (the Buffalo's sights) to either eye and that eye sees through it, in a round picture, at the
    game's zoom (about 7.75x, the default) or a realistic 4x, your other eye open or dark.
  - The lever (used in the developer's Quest 3): hold the right stick down and move the gun down to open the lever, up
    to shut it; the gun stays put meanwhile. It is the only way now: the choice listed for the next release in 0.8.11's
    notes is gone. The Volcanic's lever is worked by a flick of the right stick down.
  - Revolvers cocked by a flick (used in the developer's Quest 3; the Reloading tab, "Revolvers"): after each shot a
    flick of the right stick down cocks the Cattleman's, the Schofield's and the LeMat's hammer, John's thumb drawing it
    back; hold the stick down and click it to open the gun. Your other hand's palm swept back across the hammer fans it.
  - The bolt and the pump while carried (used in the developer's Quest 3): a bolt gun carried by its foregrip is worked
    by your free gun hand (by its trigger), the Pump-action by a jerk of the carrying hand. A bolt in hand is worked by
    your other hand's trigger.
  - The Blunderbuss's ramrod (used in the developer's Quest 3): after the round is down the barrel, take the ramrod from
    under the barrel with your other hand, push it in and pull it out.
  - The pistols (used in the developer's Quest 3): the Semi-Automatic Pistol's and the High Power's magazines (the
    Mauser's stripper clip) dropped by a flick of the right stick down and pushed in by your other hand, and the slide
    (the Mauser's bolt) racked by hand.
  - Rounds (used in the developer's Quest 3): each gun's own round, at its real size and lit by the world; up to 8 taken
    at once; from the chest, from near the gun or palm up ("Rounds come from"). Each gun's places (its foregrip, rings,
    magazine and the spot where its action is worked) can be set for each hand (the Guns tab, "This gun").
  - The Double-barrel and the Sawed-off open by the right stick held down and clicked (used in the developer's Quest 3;
    a plain flick no longer opens them).
  - John's hand closes on a bolt's knob and on a pistol's slide (used in the developer's Quest 3).
  - **Steady gun far out** (the Advanced tab) and **Hand smoothing** (the Guns tab) (used in the developer's Quest 3):
    the gun no longer trembles far from the map's middle; your hands and the aim are steadied while they are still.
  - **The front hand turns the gun** (tried in the developer's simulator; the Guns tab, under "Two-handed long guns",
    on): untick it to have John's front hand hold the gun without steering it.
  - **Physical rifle stock** (tried in the developer's simulator; the Guns tab, off): for a stock that holds both
    controllers. Your front hand's trigger at the foregrip takes hold of the gun too, only your gun hand aims it, and
    **Calibrate the stock** sets every long gun straight along the stock.
  - **Your empty hand's trigger takes a round** (tried in the developer's simulator; the Reloading tab, off): at the
    chest, as the grip does.
- New in 0.8.12, Dead Eye (used in the developer's Quest 3; the Guns tab, under "Dead Eye"): a mark painted with the
  button lands where your barrel points; the game's X is drawn on each marked target in the headset; the level you use
  (up to what the story has unlocked) and its sepia colour can be chosen.
- New in 0.8.12, gestures (used in the developer's Quest 3):
  - The lasso by the triggers in either hand (the Gestures tab, "Thrown by: Lock on", the default): hold the left
    trigger to lock on to the one you look at, then the right trigger (or a throwing swing) throws it.
  - Dynamite or a fire bottle lit and let go slowly drops from your hand, still lit, instead of flying off.
  - The hat tip (on): touch your hat's brim with your right hand and pull its trigger to greet.
  - The flames of the fire bottle and the torch follow the item drawn in your hand.
  - Undead Nightmare: its weapons work with the holsters, the grip, the free hand and dual wielding.
- New in 0.8.12 (used in the developer's Quest 3): a **Gunfights** tab while Ped Gunfight Overhaul 1.1.5 is installed
  (see the [Menu guide](#menu-guide)).
- 0.8.12.1, a hotfix (your 0.8.12 settings are kept; no reset):
  - Fixed (used in the developer's Quest 3): the hogtie. In 0.8.12 the game's "hogtie" press failed on someone on your
    lasso: the mod's props (the guns shown at your holsters, the shells, the item in your other hand) were in the way of
    the game's check. They are now hidden while someone is on your lasso on foot, and show again a second after the
    rope is empty (`[Gestures] HogtieClearProps`, on).
  - Fixed (used in the developer's Quest 3): the front hand's grip on a long gun. The Pump-action's and the Carcano's
    could change to a lower one with a different hand pose mid-session (the hand borrowed another gun's hold, or one
    still being learned). Every long gun's own hold now ships with the mod and is used from the first grab, and the
    Pump-action's and the Carcano's foregrip places are set anew on them (and the Pump-action's foregrip ring for the
    left hand moved 1 cm). A foregrip place you set yourself on those two in 0.8.12 was set on the borrowed hold: set
    it again if the hand sits wrong. The gun hand's saved holds are no longer rewritten by a fraction of a millimetre at
    each start.
  - New (tried in the developer's simulator): **What goes to the wrist** (the Display tab, under "HUD on the left
    wrist"): the radar and ammo count only (the default), or the whole corner of the screen as before 0.8.12.
- 0.8.12.2, a hotfix (your settings are kept; no reset):
  - Fixed (used in the developer's Quest 3): a long gun carried by its foregrip. Letting go of the gun's grip while
    swinging it two-handed (for a few seconds or more) left the gun a few centimetres off the carrying hand, the hand
    holding air beside it, for as long as it was carried. The carried gun now stays where it was in your hand.

- New in 0.8.13 (your settings are kept; no reset):
  - Used in the developer's Quest 3: **Snap turns in third person** (the Comfort tab, off by default) with its own
    angle: with First person off, a push turns the view one step at once while the game's camera catches up
    underneath. **The reticle in third person** ("Reticle where the shot lands", the Guns tab): where the game's own
    aim lands, the game's flat crosshair hidden meanwhile. **Bullet trails** can be turned off (the Guns tab).
  - Used in the developer's Quest 3: lever guns worked four ways (the Reloading tab, "Lever guns"): the stick held and
    the gun down and up (as in 0.8.12, the default), a roll of the gun hand, a flick of the right stick, or a flick of
    the gun. Fanning's zone size and speed, and the zone shown as a ring ("Show the fanning zone", off); a bigger zone
    no longer makes the next sweep harder to arm.
  - Used in the developer's Quest 3: the gun-butt melee finds townspeople, its "Hit strength" (the Gestures tab) can knock
    them down, and the second gun and the copy swing on their own. The copy's shots do the damage of the gun it shows
    ("The copy hits as the gun it shows", on). Left-handed: John always holds the gun in his left hand and the holsters
    stay where they are.
  - Used in the developer's Quest 3: a duster's cloth is drawn with the body you see (`[Body] ClothFollows`, on), and a
    world object you kneel at (a chest) no longer turns with you (`[Body] WorldPropsStay`, on). The game's own gun at
    the right hip is usually hidden while the hip shows the mod's, after a load too (see [Known issues](#known-issues)).
  - Ini only, off: `[Body] ClothPins=1` draws the duster's cloth on the body you see, still swaying with the game's idle
    (seen to work in the developer's Quest 3; not yet a menu row).
  - The radar cut-out's arrows (the Display tab, under "What goes to the wrist") and a button that saves a picture of
    the HUD, for a piece of the radar's ring left on the floating screen.

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
2. Download `RDRVR-0.8.13.zip` from the [Releases](../../releases) page, unzip it, and run `Install.cmd`. It finds
   the game through Steam (or asks for the folder; or `Install.cmd -GameDir "<the game folder>"`), backs up anything it
   would replace, and never changes a game file. Its window stays open until you press a key (since 0.8.0): if it
   stops on an error, the text says why, and `%TEMP%\RDRVR_install.log` has everything it printed (attach that file to
   an issue if you need help). It changes nothing if a file it would replace is in use (the game, its crash
   reporter or a virus scan): close it and run the installer again. Besides the mod's files in the game folder it puts
   the shader converter for single pass in `RDRVR_Converter\` and the launcher in `RDRVR_Launcher\`.
3. Start your headset's runtime, then the game. The launcher opens on the desktop as the game starts: Save & Play, or
   leave it and the game goes on with your saved settings after 30 seconds (Close cancels the start). The first start
   converts the single-pass shaders in the background (about 20 seconds).

To uninstall, run `RDRVR_Uninstall.cmd` in the game folder (your settings stay in `%LOCALAPPDATA%\RDRVR`).

**Upgrading (from any earlier version):** run the new `Install.cmd` over the old install; there is no need to
uninstall first. It replaces the earlier version's files, and the uninstaller still puts back what was in the game
folder before RDRVR. If you installed another build of RDRVR (someone else's fork), uninstall that first with its own
uninstaller: its extra files would stay. 0.8.12 resets every player's settings to its defaults once, as 0.8.9 did. On
the first start (or the first save in the launcher) your old settings file is renamed to
`%LOCALAPPDATA%\RDRVR\RDRVR.user.ini.before-0.8.12.bak` (or `.1.bak` and so on if that name is taken; nothing is
deleted) and a fresh one is begun. The new defaults are the developer's own settings of 2026-11-04, without the display
settings (the anti-aliasing, DLSS, the render resolution, its size and headset, and the eye shape keep the shipped
`RDRVR.ini`'s values: FXAA and the game's own resolution) and without the holsters' weapon choices (each holster on
Automatic). The controls are included: the right controller's B presses the game's X, the left X presses its B, and
the grips press nothing (they only grab; the gun hand's stick flicked up is the game's RB, cover). Set your display
settings again in the Display tab or the launcher. To bring any other old value back, copy its line from the `.bak`
file into `RDRVR.user.ini`, or set it again in the menu.

## Using it

- **Recentre:** your headset's own recentre, Scroll Lock with the game in front, or both stick clicks held a second.
- **The mod's menu:** hold the menu button, or (with the wrist HUD) turn your palm flat to look at the HUD and hold
  Y, or press F7 on the keyboard. Point with the laser, click with the trigger.
- **The game's buttons:** the controllers act as the game's pad: sticks, A/B/X/Y, triggers (LT/RT). By default
  (since 0.8.12) the right controller's B presses the game's X and the left X its B, and the grips press nothing: they
  grab (holsters, rounds, the gun's parts) and hold what you draw. The gun hand's stick flicked up presses RB (into and
  out of cover; riding, only in Dead Eye). The Controls tab's Buttons change any of this.
  Tapping the menu button is Start; with it held, the right stick is the D-pad and A is Back (John's satchel: his
  items). With the wrist HUD, "A double tap of Y at the wrist opens the satchel" (new in 0.7.0, on by default; the
  Display tab, under "HUD on the left wrist") lets two quick presses of Y open it; a single press still reaches the
  game as Y, about a third of a second late.
- **Guns:** grip at a holster to draw and keep holding the grip: the gun stays in your hand while you hold it, and
  goes back to its holster when you let go, anywhere (with your other hand on a long gun's foregrip, that hand keeps
  it). Reload by taking rounds at your chest (grip there with the other hand) and bringing them to the gun's loading
  point; the part of the gun to work next glows. After a shot, work the action: a flick of the right stick down cocks
  a single-action revolver, the stick held down and clicked opens a revolver or a break-action, the stick held down
  with the gun moved down and up works a lever, and your other hand works a bolt, a pump or a pistol's slide.
- **Settings:** everything is in the menu's tabs, and in the launcher on the desktop. Your changes are saved in
  `%LOCALAPPDATA%\RDRVR\RDRVR.user.ini`, which wins over the shipped `RDRVR.ini` (every key there is commented).
  The [Menu guide](#menu-guide) below lists every tab and setting.

## Menu guide

To open the mod's menu, hold the menu button. If your controller has no menu button, hold both B buttons. You can also turn your palm down to look at the wrist HUD and hold Y, or press F7 on the keyboard. Point the laser and click with the trigger. The tabs are the row of buttons at the top, the gun hand's stick scrolls the page, the arrows change a value by one step per press, and the help line at the bottom explains the row under the laser. Each change is saved to `%LOCALAPPDATA%\RDRVR\RDRVR.user.ini` right away, except rows marked "this session". Rows marked "(restart)" in the menu need a game restart; rows marked "(experimental)" are not yet confirmed in a headset.

Since 0.8.9 the menu has nine tabs (it had thirteen); since 0.8.12 a tenth, Gunfights, shows while Ped Gunfight Overhaul is installed. A few switches whose off side only brings back a known problem (test aids, the rendering fixes, the stereo passes, the batched hand-off, the rigid belt pieces, the body kept with the view on horseback, the gun-butt melee's dry run) are no longer in the menu; their ini keys still work. The launcher on the desktop (since 0.8.12) shows the same settings with the menu's names, before the game starts. The defaults below are a fresh 0.8.12 install's (see [Installation](#installation)).

### Display

The picture, single pass, the resolution the game renders at for your headset, performance, the HUD, cutscenes and sound.

| Setting | What it does | Default |
|---|---|---|
| **Anti-aliasing** | Off, FXAA, Native TAA or DLSS (restart). DLSS turns on or off after a restart; the others switch at once. | FXAA |
| **Rendering** (restart; new in 0.8.12) | **Single pass (faster)**: both eyes drawn from one walk of the scene, about a millisecond a frame faster; the shaders it needs are converted from your own game files in the background at the first start (until one is ready, what uses it is drawn as two passes). **Two-pass**: each eye drawn on its own. The "Now:" line under it says which runs and how many shaders are ready, or why it is two-pass (for example a missing converter: install RDRVR again). | Single pass (faster) |
| ↳ **DLSS quality** (restart; with DLSS) | How many pixels DLSS renders before it upscales: DLAA (sharpest, most demanding), Dynamic, Ultra performance, Performance, Balanced, Quality. | DLAA (native resolution) |
| **Sharpening** (new in 0.8.12) | AMD CAS sharpening of each eye's image in the headset, 5% per press; 0% turns it off. Restores the detail DLSS and TAA soften. | 0% |
| ↳ **Eyes in the headset's shape** (with FXAA) | Draws each eye in your headset's own shape with square pixels, so fewer pixels are drawn for the same sharpness (takes full effect after a restart). | Off |
| ↳ **Eyes in the headset's shape with DLSS** (restart; with DLSS; new in 0.8.0) | The same with DLSS: each eye rendered smaller in your headset's shape and upscaled by DLSS into an eye-shaped image. | Off |
| **Motion blur** (new in 0.8.5; was "No motion blur" before 0.8.9) | The game's motion blur, which smears the view as you turn your head. On: the game's own graphics setting decides. The game's settings file is not changed. | Off |
| **Heat haze** (new in 0.8.5) | The desert's heat shimmer. | Off |
| **Render resolution: headset** (restart) | The game's frame size: your headset's screen height, Automatic (the size your VR runtime asks for), or the game's own size. | The game's own (its Graphics menu) |
| ↳ **Render resolution: size** (restart) | 100%, 150% or 200% of the chosen headset's pixels, still 16:9. | 100% once a headset or Automatic is chosen |
| **Now: ...** | Shows the size the game is running at now, or why it uses its own size. | – |
| **Preset: Quality / Balanced / Performance** (new in 0.8.4) | One click sets the eye shape, DLSS's quality, Automatic's cap, the eye scale (`[XR] EyeScale`), the shared shadows and the refresh rate. Quality: DLSS at DLAA, Automatic at your runtime's full size. Balanced: DLSS Quality, Automatic at most 2880 pixels tall. Performance: DLSS Performance (without DLSS, the eyes at 0.85 of the size), at most 2208 tall, 72 Hz where your headset offers it. The eye shape, DLSS and the resolution apply after a restart, the refresh rate at once. A note suggests Balanced or Performance when gameplay keeps missing frames. | None chosen |
| **Headset refresh rate** (new in 0.8.4) | The runtime's, or one of your headset's refresh rates, where your VR runtime lets the mod choose (otherwise the row says to set it in your headset's or runtime's own settings). A frame that misses the rate drops the runtime to half the rate; 72 or 80 holds steadier on a slower PC. | The runtime's |
| **HUD on the left wrist** | Puts the radar and the ammo counter on the back of your left hand (the right when left-handed) like a watch: turn the palm down and look at it to see them. Prompts, subtitles and the rest of that corner of the screen stay on the floating screen (since 0.8.12; with "The whole corner" below, all of that corner goes to the wrist). | On |
| ↳ **What goes to the wrist: The radar and ammo count / The whole corner** (new in 0.8.12.1) | The radar and the ammo counter only, anything else in that corner (a subtitle) staying on the floating screen; or the screen's whole lower-left corner as a rectangle. | The radar and ammo count |
| ↳ ↳ **Radar cut-out: left / right, up / down, size** (new in 0.8.13; with The radar and ammo count) | Moves or resizes the part of the screen that goes to the wrist as the radar, 0.2% (the size 2%) per press: if a piece of the radar's ring stays on the floating screen, move it toward that piece or make it larger. A button below saves a picture of the HUD to send with an issue. | 0, 0, 100% |
| ↳ **A double tap of Y at the wrist opens the satchel** (new in 0.7.0) | With the wrist HUD in view, two quick presses of Y open John's satchel and B closes it. A single press still reaches the game as Y, about a third of a second late. | On |
| **Cutscenes: On the cinema screen / In 3D** | Cutscenes on a floating 16:9 screen, or in 3D around you from the cutscene camera with your head's turning added (you can switch mid-cutscene). | In 3D |
| ↳ **3D separation** | The cutscenes' 3D depth as a fraction of your eye distance (1.00 = your own; lower is flatter), 0.05 per press. | 1.00 |
| **Short scenes in first person** (new in 0.8.7) | A script's short scenes that show John in third person (a mission's quick shots, looting and skinning, chests, beds, doors) stay at John's eyes. Not the full cutscenes. Needs First person. | On |
| **Sound follows your head** (new in 0.8.8; in the menu since 0.8.9) | Sounds are placed around your head as you turn it. Off: around the way John faces. | On |
| **Sounds behind you at full volume** (new in 0.8.8; in the menu since 0.8.9) | On stereo headphones the game plays sounds behind you about 10 dB quieter, so people behind you seem far away; on, they are as loud as in front. 5.1 and 7.1 devices keep the game's mix. | On |

With Automatic, leave your runtime's own resolution setting at its default (100% in SteamVR), because it multiplies with the size you choose here. A "Restart the game to apply" note appears whenever your choice differs from what is running.

### Comfort

How you see and move: the first-person view, walking and crouching in your room, stick turning and riding.

| Setting | What it does | Default |
|---|---|---|
| **Recentre** | Puts the view, John's facing, the cinema screen and the HUD back in front of you. | – (button) |
| **First person** | The view from John's eyes, moved by your head. Off gives the game's own camera turned by your head. | On |
| **Eye height** | Raises or lowers your view, 1 cm per press (-0.20 to +0.30 m). | +0.09 m |
| **World scale** (new in 0.8.10) | How big the world looks: above 1 bigger (you feel smaller), below 1 smaller, 0.05 per press (0.50 to 2.00). Your eyes, head movement and hands are scaled; the game's world, aiming and holsters are not. | 1.00 |
| **Room-scale walking** (new in 0.8.12) | Step in your room and John walks under you, so your view moves only with your head: he turns to face your step, walks (never runs) and is put right under you at the end; your own stick always wins. Where he cannot follow (a wall, a fence, someone in the way) he stops. On foot only. Mind ledges: a step toward one walks him off it. | Off |
| **Body moves with the headset** (in the Body tab before 0.8.12) | John's body moves with you when you lean or step in your room, so it stays under your view; with room-scale walking, until he walks after you. Off: it stays where John stands. | Off |
| ↳ **Small steps at once** (new in 0.8.12) | A step up to 20 cm puts John right under you at once, without turning or walking, where nothing is in the way; a longer step, or one with a wall between, he walks. | Off |
| ↳ **Step before he walks** | How far you can move from John before he walks after you; inside it your view simply follows your head. 5 cm per press (0.05 to 0.40 m). | 0.05 m |
| ↳ **His walking pace** | How hard his walk pushes the stick at most, 5% per press (40 to 60%; 45% is the game's walk). Higher catches up sooner. | 60% of the stick |
| ↳ **Fade out when he is blocked** | Your view fades to black as you step past where John stopped, instead of passing through the wall. Off: the view is held 65 cm from him. | Off |
| **John turns when you turn** (new in 0.8.12) | Turn your body in your room and John turns with you, smoothly; your view does not move with it. A look aside does not turn him: only a turn past the angle below, held for the time below. Not while riding, in cover, in a menu or the wheel, or with a long gun held in both hands. | Off |
| ↳ **Turn past** | How far your head may turn from John's body before he turns after you, 5° per press (20 to 90°). | 20° |
| ↳ **Hold the turn** | How long your head must stay turned past that angle before he follows, 50 ms per press (0 to 1000 ms). | 0 ms |
| ↳ **His turn speed** | How fast he turns at most, 30°/s per press (60 to 360°/s); he slows as he comes round to face you. | 360°/s |
| **Physical crouch** (new in 0.8.12) | Crouch for real and John crouches; stand up and he stands. A look at the ground or a bend is not a crouch. Not while riding, in cover or in a menu. The left stick click still crouches too, and the recentre's two stick clicks no longer crouch John on foot. Sitting counts as a crouch: seated, stand John up (L3), then press "Set my standing height" sitting. | Off |
| ↳ **Crouch depth** | How far your head must drop to crouch John, as a share of your standing height, 5% per press (15 to 50%). You stand him up again a little above it. | 15% |
| ↳ **Crouch hold** | How long your head must stay past the depth before John crouches (or back up before he stands), 25 ms per press (50 to 600 ms); a quick, deep duck crouches him sooner. | 150 ms |
| ↳ **Set my standing height** | Press, then stand straight and look ahead for 3 seconds: your crouch is measured from there. A recentre while standing sets it too. The line under it shows your standing eye height. | – (button) |
| **Smooth turning / Snap turning** | How the right stick turns you. | Smooth turning |
| ↳ **Turn speed** | Smooth turning speed with the stick pushed all the way, 10°/s per press (30 to 360). | 90°/s |
| ↳ **Snap angle** | The size of each snap turn, 5° per press (10 to 90). | 30° |
| **Snap turns in third person** (new in 0.8.13) | With First person off, a push of the right stick turns the view one step at once while the game's camera turns round underneath; a push while one is still settling adds the next step. Where the camera cannot swing as far (by a wall or in a doorway), the view is handed back to it in a short blink. Not while aiming or on a mounted gun; with it on, a push past a third of the way never turns the third person camera smoothly. | Off |
| ↳ **Third person snap angle** (new in 0.8.13) | The size of each third person snap, 5° per press (10 to 90), apart from the first person snap angle. | 30° |
| **Riding: Steer with the stick / Steer with the head** | With the stick, the view faces the horse and the left stick steers while you look around freely; with the head, the left stick rides where you look. | Steer with the stick |
| ↳ **The right stick turns the view while riding** (new in 0.7.0) | The right stick turns your view on horseback while the left stick still steers; mounting or recentring faces the horse again. | On |
| **Recentre on getting on** (new in 0.8.12) | Getting on a horse, onto a wagon or into a vehicle recentres your view (as holding both stick clicks): you face the way it faces. | On |
| **View from the saddle** | A steady view above the saddle, so the gallop's bounce stays out of it. | On |
| ↳ **Keep its height on climbs** | Stops the saddle view sinking toward the saddle on a steep climb. | On |
| ↳ **Saddle smoothing** | How much the saddle view's height is smoothed, 0.05 s per press (0 to 0.60 s). | 0.15 s |
| **Left stick click brakes** | On a horse or wagon, hold the left stick click to brake (pull the stick back too for a hard stop); on foot it still crouches. | On |

### Body

How much of John is drawn, his torso, and how his arms and hands fit your controllers.

| Setting | What it does | Default |
|---|---|---|
| **Shown: Whole body / Forearms and hands / Hands only** | How much of John is drawn in first person (only what you see changes, not gameplay). | Whole body |
| **Forearms and hands when crouching, in cover or riding** | Draws only the forearms and hands at those times, when the body gets in the way of the view. | Off |
| **Hide head and hat** | Hides John's head and hat in first person; cutscenes still show them. | On |
| **Torso: Upper body upright / Still / Remove lean only / Game animation** | Holds the torso upright over the hips, holds it still under your view (new in 0.8.5: only the arms and legs move), removes only its lean, or draws it as animated. | Still |
| ↳ **Torso pitch** | Tilts the upright torso, 1 degree per press; positive leans it forward. | 0° |
| **Body back** | How far behind your view the body is drawn, so looking down shows your chest from outside. | 0.04 m |
| **Keep the torso over the hips** | Stops running from pushing the torso forward into view. | On |
| **Lock body facing** | The body, legs included, faces where you face, so walking sideways doesn't turn a shoulder into view. | On |
| **Arms stretch to reach the controllers** | When your controller is beyond John's reach, his arm lengthens so the hand stays on it. | On |
| ↳ **Longest arm** | The longest an arm may stretch, as a multiple of John's arm length (1.0 to 1.6). | 1.60× |
| **Wrist offset** | How far back from the controller's grip John's wrist sits, 5 mm per press. | 0.065 m |
| **Hand pitch / Hand yaw / Hand roll** | Tilts, turns and rolls John's hand against the controller, 2° per press (mirrored for the left hand). | -68° / 0° / 0° |
| **Controller fit: Offset / Pitch / Yaw / Roll** | Moves and turns John's hands against your controller type. Saved for that controller type only, and shown once the mod has seen your controller. | 0 (no change) |
| **Interaction spot** | Moves the point your hands grab with (the dot Show the hand dots draws) away from the wrist, 5 mm per press. | right -0.030, up +0.005, forward +0.085 m |

"Body moves with the headset" is in the Comfort tab since 0.8.12, under "Walking in your room".

### Holsters

The body holsters: how you draw and hold, what they follow, placing them by hand, what is shown, each holster's weapon, John's belt, and each holster's own rows.

| Setting | What it does | Default |
|---|---|---|
| **Body holsters** | Reach to a holster and grip to draw; let go of the weapon to put it away (anywhere with "Hold the grip to keep the gun", else at its holster). A short buzz marks a holster. | On |
| ↳ **Weapons by: The holsters / The weapon wheel** | Draw by reaching to the holsters, or hold a grip to open the game's weapon wheel, move toward a weapon and let go (the holsters then draw nothing; the chest still gives rounds). | The holsters |
| ↳ ↳ **The wheel's hand: its stick is the D-pad** (with the weapon wheel; new in 0.8.12) | While a grip holds the wheel open, push that hand's stick left or right to pick another gun of the highlighted kind (the game's D-pad); the stick neither walks nor turns meanwhile. | On |
| ↳ ↳ **The world stands still while the wheel is open** (new in 0.8.12) | While a grip holds the wheel open, the world nearly stops (the game's own time warp, as Dead Eye's) until you let go. Not while Dead Eye is on or a hand holds an object. | On |
| ↳ ↳ ↳ **The world's speed meanwhile** | How fast the world runs while the wheel is open, 0.01 per press (0.01 to 0.08×): 0.02 is nearly still, 0.08 slow motion. | 0.02× |
| ↳ **Putting a gun away selects the fists** | Holstering also switches to unarmed, so your fists come up instead of drawing the gun again (since 0.8.11 with both triggers held: "Fists on both triggers"). | On |
| ↳ **Hold the grip to keep the gun** (new in 0.8.12) | Grip at a holster to draw and keep holding: the gun stays in your hand only while you hold the grip. Let go anywhere and it goes back to its holster, hidden on its way; with your other hand on a long gun's foregrip, letting go leaves the gun carried by that hand ("Carry a long gun by its foregrip", the Guns tab). A second gun too, and the knife, the torch, the lasso and the throwables (`[Holsters] HoldItems`). With a gun in hand your gun hand's grip is not the game's RB (cover): flick that hand's stick up instead. Not with the weapon wheel. | On |
| ↳ **Draw into the hand that grabs the holster** | The gun goes into whichever hand grabbed it, and that hand's trigger fires. | On |
| ↳ **They follow: John's body / Your headset** | Whether the holsters stay with John's body or move with you (riding and cover always use the body; on horseback they also turn with the right stick's turn of the view). | John's body |
| ↳ ↳ **Turn with the headset** (new in 0.7.0) | The holsters turn around you as you turn your head; off, they face the way John faces. | Off |
| ↳ ↳ ↳ **Turn by your head and hands** (new in 0.7.0) | While both hands aim in front of you, the holsters turn halfway toward your hands, so looking aside swings them less. | Off |
| ↳ ↳ ↳ **Turn dead zone** (new in 0.7.0) | How far you can look aside before the holsters follow, 0 to 90°; 30 to 45 lets you glance aside. | 45° |
| ↳ ↳ **Steady when you lean** (new in 0.7.0) | Leaning or bending over leaves the holsters at your hips; crouching and stepping still move them. | On |
| **Placing by hand: Off / Rings / Belt pieces and guns** (this session; one choice since 0.8.9) | Rings: close the menu, grip a holster's ring and drag it. Belt pieces and guns (new in 0.8.8; the guns since 0.8.9): grip a piece of John's belt or a holstered gun, then move and turn it. Let go to leave it there (saved). Nothing is drawn meanwhile: set Off when done. The rings show while you place them, whatever Interaction points says. | Off |
| **Rings stay on their holsters** (new in 0.8.9) | Each holster's ring and its piece of John's belt keep their places to each other: moving one, by hand or by the arrows, moves the other the same way (a piece's turn leaves its ring). Off: each moved on its own. Turning it on or off moves nothing. The ammo ring is always on its own. | On |
| **Interaction points: Rings / Highlights / Both / Glowing parts** (new in 0.8.12) | How the places your hands work are shown. Rings: the rings below, as their switches say. Highlights: instead of the rings, a soft glow over the place itself, only while using it would do something now (a holster with a gun to draw or room for one, the loading gate with room in the gun, a hammer that can be cocked), never over your sights. Both: the rings and the highlights. Glowing parts: the gun's parts (the hammer, the lever, the bolt, the slide, the loading gate, the foregrip) glow on the gun itself, as if their own texture glowed, hidden where your hand covers them; the holsters have no glow. | Glowing parts |
| ↳ **The glow's colour** (with Glowing parts) | Red, green and blue, each 0 to 1, 0.05 per press. | Red (1.00, 0.00, 0.00) |
| ↳ **The glow's strength** (with Glowing parts) | How bright the glowing parts are against their own texture, 0 to 8, 0.1 per press. | 8.0 |
| **Show the holsters** | Shows each holster as a ring: white, green with a hand in it, amber while gripped. Drawn only with Interaction points on Rings or Both (and while placing by hand). | On (drawn with Rings or Both) |
| ↳ **Only near a hand** (new in 0.7.0) | Each ring appears faintly only when a hand comes near it. | On |
| **Show the hand dots** | A dot at each hand's grab point, green inside a holster or on the foregrip. | Off |
| **Show the weapon points** | Shows the foregrip ring, where a round goes in, and the hints for the gun's action, with Interaction points on Rings or Both. | On (drawn with Rings or Both) |
| **Show the guns at their holsters** | Shows each holstered gun on your body; hidden while it is in a hand. | On |
| ↳ **The long guns on the back too** | Also shows the long guns on your back. | Off |
| **Choose the weapon in each holster** | Each holster draws the weapon you pick in its own rows below, and the game's weapon wheel follows; Automatic draws the game's weapon. | On |
| ↳ **Any weapon in the gun holsters** (new in 0.8.5; the gun holsters only since 0.8.9) | The hips' and the backs' holsters take any weapon you own; one put away at a gun holster is drawn from there again. The knife's, the satchel's and the lasso's keep their own items, the chest the rounds. | Off |
| **Customize John's belt** (new in 0.8.9) | Move and turn John's own holster, knife sheath, satchel, lasso and rifle sling (each holster's Place / Angle rows, or Placing by hand), and add **A mirrored holster at the left hip** for the secondary sidearm. Off: John's belt exactly as the game has it (with Rings stay on their holsters, the rings move back with the pieces); your places are kept. **Reset the belt to the game's positions** returns every piece to the game's place, and **Reset the rings to their defaults** every ring to its default place on its piece (each on a second click). | On (the developer's layout, the mirrored holster on) |
| **Each holster** (since 0.8.9 in this order): **Right hip (primary sidearm), Left hip (secondary sidearm), Right back (primary long gun), Left back (secondary long gun), Knife, Satchel (throwables), Lasso, Ammo** | Turns each holster on or off (the ammo holster is always on). The Lasso holster (new in 0.8.9) sits on John's lasso and replaced the lower back's knife-and-lasso holster; the knife holster holds the knife only. | On |
| ↳ **Weapon** | The weapon this holster draws. On Automatic, the secondary sidearm and long gun holsters take a different owned gun when you have one. A weapon you no longer own shows "(not owned: automatic)". The lists fill in once you are in gameplay. | Automatic |
| ↳ **Ring / Size arrows** | Move each holster's ring right / up / forward, 5 mm per press, and change its size; saved at once. Set Interaction points to Rings or Both and turn on Show the holsters to see them while you move them. | Placed for the release |
| ↳ **Place / Angle arrows** (with Customize John's belt on) | Move and turn that holster's piece of John's belt: the holster, the mirrored left holster, the rifle sling (both backs), the sheath, the satchel, the lasso. | Placed for the release |
| ↳ **Gun's angle / Gun's place** (new in 0.8.5; with Show the guns on) | Turns the holstered gun about its grip (tilt, turn, lean; 2.5° per press) and moves it (5 mm per press), so it sits in the holster. The left holsters mirror the right ones until you set them. | Placed for the release |

### Guns

The gun in your hand: the reticle, the scope, how it fires, the free hand, Dead Eye, dual wielding, two-handed long guns, and the gun you are holding now.

| Setting | What it does | Default |
|---|---|---|
| **Bullet trails** (new in 0.8.13) | The streaks bullets leave in the air, everyone's shots. Off: no trails. | On |
| **Reticle where the shot lands** | With a gun in hand (lowered, raised or in Dead Eye), shows a reticle where the bullet will hit; it turns red while you aim at a person or animal (that part not yet tried). Since 0.8.13 also in third person (First person off), where the game's own aim lands, with the game's flat crosshair hidden meanwhile. | Off |
| ↳ **Ring and dot / Dot only** | The reticle's look. | Dot only |
| **Look through a rifle's scope** (new in 0.8.12) | Bring a scoped rifle's eyepiece (the Rolling Block, the Carcano; the Buffalo by its sights) to either eye as you aim: that eye sees through the scope, magnified, in a round picture at the eyepiece, fading in and out as it comes and goes. One hand is enough. | On |
| ↳ **Magnification: Realistic (4x) / The game's zoom** | A period scope's about 4x, or the game's own sniper view, about 7.75x. | The game's zoom |
| ↳ **The other eye: Open, as with a real scope / Dark while scoped** | Your other eye while you look through the scope. | Open, as with a real scope |
| **Keep the gun drawn** | A drawn gun stays in your hand until you put it away, instead of being holstered 3 seconds after your last shot. | On |
| **Executions up close** (was "No executions up close" before 0.8.9) | Pulling the trigger close to someone can start the game's third-person execution, pistol whip or butt strike. Off: it just fires. | Off |
| **Shoot past the arm block** | Lets you fire when the game thinks John's shoulder-to-gun line hits a wall; a friendly in the way still blocks the shot. | On |
| **Perfect accuracy** | Every shot goes exactly along the barrel, with no random spread. Other characters keep the game's spread. | On |
| **Shotgun pellets spread** | Pellets spread in the game's cone; off puts them all on one line. | On |
| **Gun kick** (new in 0.8.12) | Each shot kicks the gun in your hand muzzle up and back for a moment, most on revolvers, rifles and shotguns; a long gun held with both hands slides back instead. The second gun and the copy kick on their own shots. Looks only: your shots still go where the barrel points. 25% per press (0 to 200%); 0% is off. | 0% |
| **Hand smoothing** (new in 0.8.12) | Steadies your hands and the aim while they are still (a gentle filter that lets go as they move). Off: the controllers exactly as tracked. | On |
| **Guns touch** (new in 0.8.12) | Your two guns (dual wield, the copy, a carried long gun) and your hands and arms don't pass through each other: a gun that touches the other gun, your other hand or an arm is drawn held against it, with a buzz. With the revolvers cocked by a flick, the other gun swept back across a single action's hammer cocks it. Looks only; a hand holding a round, a magazine, the foregrip or a part is never pushed off it. | On |
| **Items in the free hand** (new in 0.8.12) | With a gun in one hand, grip at the knife's sheath or the satchel with the other hand to draw the knife, the torch or a throwable into it: the gun stays in hand and keeps aiming, shooting and reloading. A fast stab or slash hits as the game's knife; a fast swing of the torch knocks them and sets them alight; a slow move does neither. Let go of a throwable during a throw and it flies with your hand's speed; let go slowly, or of the knife or the torch, and it goes back where it came from. | On |
| **The free hand's torch lit (experimental)** (new in 0.8.12) | Undead Nightmare: the torch drawn into your free hand burns with the game's own flame and light. It needs the torch to be one of your weapons the game has ready; otherwise it stays unlit. Needs Items in the free hand. | On |
| **Mounted guns in first person (experimental)** (new in 0.8.10, not yet tried on a real mounted gun) | The Gatling, the Browning and the cannon: first person kept, John's hands on the gun, and the gun turned toward where your gun hand points (it learns the gun's barrel from your first shot). While on, the right stick no longer turns the gun: if the gun does not follow your hand, turn it off. Off: the game's own camera and aim while you use one. | Off |
| **Dead Eye level: The game's / 1: slow motion / 2: marks as you sweep / 3: marks by the button** (new in 0.8.12) | Which Dead Eye you use, up to the level the story has unlocked (the row says which): slow motion only, the marks painted as you sweep over targets, or the marks painted with the button. A change applies at the next Dead Eye. | The game's |
| **Dead Eye's sepia colour** (new in 0.8.12) | Dead Eye's sepia screen colour. Off: the colours stay as they are; the slow motion and the rest stay. | On |
| **Dead Eye's X on each mark** (new in 0.8.12) | The game's X on each target you mark in Dead Eye, drawn on the target in the headset (it showed nowhere before). While marks show, a small part of the floating HUD is left out. Turned on while the game runs, it applies after a restart. | On |
| ↳ **Its size** | How big each X looks, 0.5° per press (0.5 to 15°). | 3.0° |
| **Dead Eye marks where the barrel points** (new in 0.8.12) | A mark painted with the button lands on whoever your barrel points at. Off: the game picks the target from its flat camera's view, which in the headset often is not the one you aim at. Turned on after the game started with it off, it applies after a restart. | On |
| **Dual wield** | With a gun in one hand, grip another gun holster with the free hand to take a second gun with its own trigger and ammo. | On |
| ↳ **A gun beside the torch** (new in 0.8.12) | Undead Nightmare: with the torch in one hand, grip a gun holster with the free hand to take a second gun beside it, with its own trigger, reticle and ammo. It is reloaded by pressing it to your chest, as "The gun hand squeezed at the chest" (the Reloading tab) says, so not with its default, "Does nothing". Put the torch away and the second gun becomes the gun in hand. The knife and a throwable take a second gun the same way (`[Hands] DualWieldBesideItems`, on). | On |
| ↳ **The knife beside a gun is the game's knife** (new in 0.8.12) | Your other hand draws the knife while a gun is in hand: the knife becomes the game's own weapon in that hand (its stab hits as the game's knife) and the gun the second gun in your gun hand. Off: the knife as a copy beside the gun. | On |
| **Each hand reloads the other gun** (new in 0.8.12) | With a second gun or the copy out, grip at the chest with either hand to take rounds for the gun in your other hand, then bring them to that gun's loading ring. The hand holding rounds does not fire its own gun until they are in and its trigger is let go. The chest squeeze is off for the two guns while this is on. | On |
| **Open the second gun by its stick** (new in 0.8.12) | With each gun's action by hand on and a revolver as the second gun or the copy: hold that hand's stick down and click it to open the gun, and flick that hand (a roll or a turn) to shut it. That click never crouches or brakes. While such a gun is out, a quick flick of that stick back and a quick click do nothing: hold them a moment longer (the two waits below) to walk back, crouch or brake. | On |
| ↳ **The walk back waits** | On foot, how long that stick pulled back waits before you walk back, 25 ms per press (0 to 600 ms). Riding, there is no wait. | 250 ms |
| ↳ **The click held in for** | How long that stick's click must be held in before it crouches you (or brakes on a horse), 25 ms per press (0 to 600 ms). | 200 ms |
| **The same sidearm in both hands** | With a sidearm in hand, grip a hip holster with the free hand to take a copy of it; let go at a hip to put it back. | On |
| ↳ **The copy drawn as a prop** | Draws the copy as a model of the gun, so you see it in the free hand. | On |
| ↳ **Your other sidearm's model** | If you own a different sidearm, the copy is that gun (the one that hip's holster shows), and its shots leave its muzzle. Since 0.8.12 it also fires at that gun's own rate (`[Hands] CopyOwnRate`, on). | On |
| ↳ ↳ **A second of the gun at its own holster** | Gripping the holster the gun was drawn from gives a second of that same gun; the other hip still gives your other sidearm. | On |
| ↳ ↳ **The copy hits as the gun it shows** (new in 0.8.13) | The copy's shots do the damage of the gun it shows (your other sidearm's), not the gun in hand's. Turned on while the game runs, it works from the next start. | On |
| **Two-handed long guns** | Grip a long gun's foregrip with your other hand to aim it with both hands. | On |
| ↳ **The front hand snaps onto the gun** | While you hold the foregrip, John's front hand sits in the game's own grip instead of following your controller. | On |
| ↳ **The front hand turns the gun** (new in 0.8.12) | On: the gun points from your gun hand toward your front hand. Off: John's front hand is drawn holding the gun, but only your gun hand moves it. | On |
| ↳ **Physical rifle stock** (new in 0.8.12) | For a stock that holds both controllers: your front hand's trigger at the foregrip takes hold of the gun as its grip does (the trigger still aims), and only your gun hand aims the gun. | Off |
| ↳ ↳ **Calibrate the stock** | Press (the menu closes), then within 5 seconds hold any long gun on your stock with both hands, aiming down the sights: every long gun is set straight along the stock with its top up. | – (button) |
| ↳ ↳ **Clear the stock calibration** | The long guns back to the game's own angle in your gun hand. | – (button) |
| ↳ **Carry a long gun by its foregrip** (new in 0.8.12) | Holding a long gun with both hands, let go of the gun hand's grip away from the holsters: the front hand keeps the gun by its foregrip (carried, it does not fire) and the gun hand is free to draw another weapon. Grip near the gun's grip with the empty hand to take it back; let go with the front hand to put it back in its holster. | On |
| **This gun: Right hand / Left hand; The foregrip / The foregrip ring / The loading ring / The magazine / The action's spot (bolt, breech, slide, hammer)** (each hand and every gun since 0.8.12) | Draw a gun first: the heading names it. Right hand / Left hand picks which hand's places the arrows set (the left uses the right's until you set it). The foregrip (where John's front hand sits) and the foregrip ring (where your front hand takes hold) on long guns, the loading ring (where a round goes in) on every gun, the magazine's well on the magazine pistols, and the action's spot (the bolt's handle, the breech, the slide, the fanned hammer or the ramrod), 5 mm per press, the rings' sizes too. Until you change a value it uses every gun's (the Reloading tab). | Every gun's (most guns have their own, placed for the release) |
| ↳ **Use every gun's** | Clears this gun's own value for this hand so it uses the shared one again (the left hand: the right hand's). | – (button) |
| **The front hand's hold: Automatic / Lowered / Aiming** (new in 0.7.0) | Long guns only: which of the game's holds John's front hand uses on this gun. Until the game has shown a hold once this session, its label says so. | Aiming |

### Reloading

Reloading by hand, where the rounds come from, the round in your hand, every gun's rings, working each gun's action, and the empty gun.

| Setting | What it does | Default |
|---|---|---|
| **Reload by hand** | Grip at the chest with your free hand to take rounds (a buzz), then bring them to the open gun to load it (not in Dead Eye). | On |
| ↳ **Your empty hand's trigger takes a round** (new in 0.8.12; not yet tried in a headset) | At the chest, your empty hand's trigger takes a round as its grip does; keep it pulled while you bring the round to the gun. The trigger still aims. | Off |
| ↳ **Rounds taken at once** (new in 0.8.12) | How many rounds a grip takes, as many as the gun has room for, 1 to 8; all of them drawn in your hand. | 8 |
| ↳ **The rounds held go in: One at a time / All at once** (new in 0.8.12) | Held at the gun's loading ring, the rounds go in one after another, or all as they reach it. Let go at the gun and the rest go in. | One at a time |
| ↳ **Rounds come from: Your chest / Grip near the gun / Palm up** (new in 0.8.12) | Your chest: grip at your chest. Grip near the gun: also grip with your empty hand within a hand's width of the gun's loading ring, and the rounds appear in your hand. Palm up: also turn your empty hand palm up, away from the gun, and grip. Gripping a part of the gun (a bolt, the barrels, the foregrip) still works that part. | Your chest |
| **Show the rounds at the chest** | While the gun can take rounds, a row of them (up to six) is drawn at your chest. | On |
| **The gun hand squeezed at the chest:** | Does nothing, Loads one round, or Reloads fully. | Does nothing |
| **The game's automatic reloads** | Turns the game's own reloads back on alongside the hand reload. | Off |
| **The game's reload button** | Turns the game's reload button back on alongside the hand reload. | Off |
| **Rounds seen in the hand: Off / The mod's rounds / The game's shotgun shells too** (was "A round seen in the hand" before 0.8.12) | Draws the rounds you took between your fingers (and the row at the chest). The game's shotgun shells too: a shotgun's shells are the game's own model, lit by the game. | The mod's rounds |
| **Each gun's own round (the new models)** (new in 0.8.12) | Each gun's round at its real size, textured and lit as the game's own props. Off: the old simple rounds. | On |
| **The round in your: Left hand / Right hand**, then **Offset / Direction / Turn / Tilt / Brightness** (each hand its own since 0.8.12) | Where the round sits in that hand's fingers (2 mm per press, up to 20 cm), which way it points (Along the fingers, Out of the hand, or Custom by its turn and tilt, 5° per press, each all the way round), and how bright the drawn rounds are (0.25 per press, the chest's too). The right hand uses the left's until you set it. | Custom: left turn 0°, tilt −50°; right turn −175°, tilt −65°; brightness 0.50 |
| **Every gun's rings: The foregrip / The foregrip ring / The loading ring (Offset / Size arrows)** | The shared foregrip, foregrip ring and loading ring for every gun without its own (the Guns tab, This gun), 5 mm per press. | Foregrip ring 0.110 m, loading ring 0.050 m |
| ↳ **A round goes in at a touch** | The round loads when it touches the loading ring; off, your wrist has to be inside the ring. | On |
| **Work each gun's action by hand** | After a shot, work the bolt, lever or pump yourself before the trigger fires again, and open and shut the guns to load them, as the rows below say. | On |
| ↳ **Revolvers drawn open** | An open revolver is drawn open (tipped, swung out or gate open). | On |
| ↳ **Revolvers: Opened by a flick of the stick / Cocked by a flick, opened by holding it and clicking** (new in 0.8.12) | Opened by a flick: a flick of the right stick down opens a revolver. Cocked by a flick: after each shot a flick of the right stick down cocks the Cattleman's, the Schofield's and the LeMat's hammer (the Double-action needs none), and the stick held down and clicked opens a revolver. Either way a flick of the gun shuts it. A revolver in your other hand is worked by that hand's stick (on foot). | Cocked by a flick, opened by holding it and clicking |
| ↳ ↳ **John's thumb cocks the hammer** | On the stick's flick, John's thumb reaches up to the hammer and draws it back, then returns to the grip. | On |
| ↳ ↳ **Fan the hammer with your other hand** | Sweep your other hand's palm back across the hammer to cock it; keep the trigger pulled and each sweep fires. Since 0.8.13 the next sweep counts once your hand is 2.4 cm out of the zone, whatever its size (it was a third of the size again, so a big zone got harder). | On |
| ↳ ↳ ↳ **Fanning zone size** (new in 0.8.13) | How near the hammer the sweep must pass, 1 cm per press (2 to 30 cm). | 8 cm |
| ↳ ↳ ↳ **Fanning speed** (new in 0.8.13) | How fast the sweep must be to cock the hammer, 0.1 m/s per press (0.2 to 6.0 m/s); lower is more sensitive. | 1.0 m/s |
| ↳ ↳ ↳ **Show the fanning zone** (new in 0.8.13) | A ring round the hammer as big as the zone: bright while a sweep would cock it, lit while your hand is in it, faint while your hand must leave it before the next sweep counts, plain while the hammer is back. | Off |
| ↳ **Lever guns: the lever drawn as you work it** | The lever drawn opening and closing as you work it, with the game's lever sounds. | On |
| ↳ **Lever guns: Stick held, gun down and up / Roll the gun / Flick the stick / Flick the gun** (new in 0.8.13; 0.8.12 had the first only) | Stick held: hold the right stick down, move the gun down to open the lever and up to shut it; the gun stays put meanwhile. Roll the gun: after a shot, roll your gun hand a little left or right to open the lever and back to shut it. Flick the stick: after a shot, a flick of the right stick down works the lever. Flick the gun: after a shot, flick the gun down and back up (with the lever drawn, the flick down opens it and the flick up shuts it). | Stick held, gun down and up |
| ↳ ↳ **Roll to open** (new in 0.8.13; with Roll the gun) | How far the gun hand rolls, either way, to open the lever, 1° per press (8 to 60°). Rolling back about half way shuts it. | 20° |
| ↳ **Volcanic: its lever by a flick of the right stick** (new in 0.8.12) | After each shot, a flick of the right stick down works the Volcanic's lever. | On |
| ↳ **Pump-action: the fore-end in your front hand** | After a shot, pull the fore-end back with your front hand and push it forward. | On |
| ↳ **Bolt actions: the bolt in your other hand** | On the Bolt Action and Carcano: lift, pull back, push forward and turn down the bolt by hand. | On |
| ↳ **Bolt actions: the bolt by your gun hand when carried by the foregrip** (new in 0.8.12) | With a bolt gun carried by its foregrip, your free gun hand works the bolt. A shot not yet cycled goes with the gun and stays so until you work the bolt; grip near the gun's grip to take it back. The carried gun never fires. | On |
| ↳ **Blunderbuss: the ramrod at your chest** (new in 0.8.12) | After the round is down the barrel, take the ramrod with your other hand, bring it to the muzzle, push it in and pull it out: loaded. Off: your bare hand at the muzzle makes two ram strokes. | On |
| ↳ ↳ **Take the ramrod: From the gun / At your chest** | Where your other hand takes it: where it lies under the Blunderbuss's barrel, or at your chest. | From the gun |
| ↳ ↳ **Where it is taken on the gun**; **The ramrod in your: Left hand / Right hand** (The ramrod in the hand / Turn / Tilt) | The ramrod's grab point on the gun (5 mm per press), and where it sits in each hand (5 mm or 5° per press). | Placed for the release |
| ↳ **Bolt actions: the bolt by the off hand's trigger** (new in 0.8.12) | Work the bolt of the gun in your hand with your other hand's trigger (pull it at the bolt and make the strokes); that trigger does not aim while it is at the bolt. Off: by its grip. | On |
| ↳ **The carried bolt by the trigger** (new in 0.8.12; with the carried bolt) | Work the carried gun's bolt with your gun hand's trigger; your grip then always takes the gun back, even beside the bolt. Off: by the grip. | On |
| ↳ **Pump-action: pumped by a jerk when carried by the foregrip** (new in 0.8.12) | With the Pump-action carried by its foregrip, snap that hand back along the barrel, then forward: a one-handed pump. The carried gun never fires. | On |
| ↳ **Single-shot rifles opened by hand** | Open the Springfield's and Rolling Block's breech with your other hand (the Buffalo's with a flick), load one round, then shut it. | On |
| ↳ **Semi-Auto Shotgun: the bolt racked by hand** | After loading from empty, pull the bolt handle back with your other hand. | On |
| ↳ **Pistols: magazines (the Mauser: a stripper clip)** (new in 0.8.12) | Semi-Automatic Pistol and High Power: flick the right stick down to drop the magazine (a round stays in the chamber if the gun wasn't empty; its rounds go back to your ammo), take a new one with your other hand (as "Rounds come from" says) and push it up into the bottom of the grip. Mauser: pull its bolt back by the ears with your other hand, push a stripper clip down into the top, then pull the bolt and let go (or flick the stick down) to close it. No single rounds go into these three. | On |
| ↳ ↳ **Left hand / Right hand**: **Magazine in the hand / Turn / Tilt**, **Use every pistol's** | Where the magazine (the Mauser's clip) sits in each hand (2 mm or 5° per press); with a pistol in hand, that pistol's own. | Placed for the release |
| ↳ **Pistols: the slide racked by hand** (new in 0.8.12) | Semi-Automatic Pistol (its toggle), High Power and Mauser (its bolt): grip the slide with your other hand, pull it back and let go to chamber a round. Racking with a round already chambered throws it out (it is lost). After the last shot the slide stays back: put a new magazine in, then rack it. With nothing chambered the trigger does nothing. | On |
| ↳ **Break actions opened by the stick** | Hold the right stick down and click it to break the Double-barrel or Sawed-off open (since 0.8.12; `[Reload] BreakOpenClick=0` in the ini brings back the plain flick down); flick the gun up, or swing the barrels up by hand, to shut it. | On |
| ↳ ↳ **John's hand on the barrels as they swing up** | Draws John's hand on the open barrels while you hold them. | On |
| ↳ **John's hand on the part your other hand grips** | Draws John's hand on the bolt, breech or pistol slide you are working (since 0.8.12 closed on a bolt's knob and on the slide). | On |
| ↳ **A click when fired before the action is worked** | A trigger pull on a gun that is open or not yet worked plays the empty click. | On |
| **An empty gun: Clicks / Buzzes / Clicks and buzzes / Does nothing** | What an empty gun's trigger does: a click from the gun hand's side, a controller buzz, both, or nothing. | Clicks |
| ↳ **Click volume** | The click's loudness from 0 to 1, 0.05 per press. The game's volume settings don't affect it; the game's slider in the Windows volume mixer does. | 0.16 |

### Gestures

Actions you do with your own hand movements.

| Setting | What it does | Default |
|---|---|---|
| **Throw by hand** | Dynamite, fire bottles, throwing knives and tomahawks fly with your hand's speed and direction: pull the trigger and swing. | On |
| ↳ **Throw strength** | How hard throws fly, as a multiple of your hand's speed, 0.1 per press (0.5× to 4.0×). | 2.0× |
| ↳ **Throw by letting go of the grip** (new in 0.7.0) | Hold the grip to hold the throwable, pull the trigger to light or ready it, then swing and let go of the grip to throw. A lit one held too long goes off in your hand; since 0.8.12 dynamite or a fire bottle lit and let go slowly drops from your hand, still lit (`[Gestures] DropLitSlow`). | On |
| **Melee by swing** | A fast swing of either hand punches, stabs with the knife or strikes with the torch. With "Fists on both triggers" on (0.8.11), the fists punch only while both triggers are held, whatever this says. | On |
| **Lasso by hand** | With the lasso in hand, it is thrown as chosen below; a yank back pulls on the rope. | On |
| ↳ **Thrown by: Trigger / Swing / Either / Lock on** (Lock on new in 0.8.12) | Trigger: the lasso hand's trigger. Swing: a fast swing toward the aim. Either. Lock on: hold the left trigger (the game's aim) to lock on to the one you look at and spin the lasso, then the right trigger or a throwing swing throws it, whichever hand holds the lasso (left-handed: mirrored). Let go of the left trigger without either and nothing is thrown; keep it held to keep them on the rope. | Lock on |
| ↳ **Aimed by: Head / Lasso hand / Other hand** (new in 0.8.7) | Head (where you look), Lasso hand, or Other hand (where your other controller points). Lock on always aims by your head. | Head |
| ↳ **Swing speed** | How fast a swing must be to punch or throw the lasso, 0.25 m/s per press (1.0 to 8.0 m/s). | 1.50 m/s |
| **Gun-butt melee** | Swing the gun in your hand into someone to land the game's knock-out hit (with the usual crime and anger). Since 0.8.13 it also finds townspeople, and the second gun and the copy swing on their own. | On |
| ↳ **Hit speed** | How fast the gun's swing must be to hit, 0.25 m/s per press (1.0 to 6.0 m/s). | 1.00 m/s |
| ↳ **Hit strength** (new in 0.8.13) | How much of a person's knock-out a hit takes, 5 per press (5 to 200): 100 knocks them down in one hit, and a hard swing doubles it. The game's own is 10. | 50 |
| **Whistle for the horse** | Put a hand at your mouth and pull its trigger to whistle (the gun hand whistles only when it holds no gun). | On |
| **Fists on both triggers** (new in 0.8.11) | With nothing in your hands, hold both triggers for the fighting stance and swing a fist to punch while holding them; raise both fists before your face to block. A trigger alone does nothing. Off: each trigger as the game has it. | On |
| **Guard with both fists** (new in 0.8.5) | With your fists out, raise both hands before your face and hold both triggers to block. Fists raised without the triggers stay lowered for the game. Not riding, not in the game's menus. | On |
| **Tip the hat to greet (experimental)** (new in 0.8.5) | Your right hand at your hat's brim (since 0.8.12: 13 cm above your eyes, 8 cm forward) and its trigger press the game's greet button. Seen to work in the developer's headset in 0.8.12, though the menu still marks it experimental. | On |
| **Hands push loose objects (experimental)** | An empty hand pushes bottles, crates and chairs with the game's physics (never people). | Off |
| **Grab loose objects (experimental)** | Grip with an empty hand to hold a loose prop; let go to throw it at your hand's speed. | Off |

### Controls

Handedness, how the triggers aim and fire, and button mapping.

| Setting | What it does | Default |
|---|---|---|
| **Left-handed** | Swaps the controllers' roles: the left trigger fires and the right stick moves. Since 0.8.13 John always holds the gun in his left hand then (the row "The gun in John's left hand" is gone: off crossed his arms), and the holsters stay where they are. | Off |
| **The trigger alone fires** | The right trigger alone fires; the mod holds the aim trigger for you. | On |
| **Aim while the gun is raised** | While the gun is raised in front of you, the aim is held, so every pull fires at once. | On |
| **Sprinting lowers the raised gun** | Pressing sprint lowers a raised gun so John can sprint with it. | On |
| **Jumping lowers the raised gun** | The jump button first lowers the gun (up to 250 ms), so John can jump, vault or climb with a gun in hand. | On |
| **Buttons: Right A, Right B, Left X, Left Y, Left stick click, Right stick click, Left grip, Right grip, Left trigger, Right trigger** | Picks the game button each VR button presses ("none": nothing). A trigger set to a button presses it past halfway; a button set to LT or RT pulls it fully. | Since 0.8.12: Right B presses X, Left X presses B, both grips none (they only grab); the others the button of the same name (triggers LT / RT) |
| ↳ **Reset to the game's scheme** | Puts every VR button back on the game button of the same name (the grips on LB / RB). | – (button) |
| **The menu: ...** | How to open this menu. A tap of the menu button pauses. Held with the right stick it is the D-pad, and held with A it opens the satchel. | – |

The gun hand's stick flicked up presses the game's RB for a moment (into and out of cover; riding, only in Dead Eye), since 0.8.12, as the grip now holds the gun (`[Controls] FlickUpRB`, no menu row).

### Advanced

Live performance and view-mode readouts (frame rate, missed frames, CPU and GPU time), fixes you can turn off to compare or if one misbehaves for you, and the launcher. Leave the fixes on for normal play.

| Setting | What it does | Default |
|---|---|---|
| **Long guns held by their aiming grip** | Each long gun is held the way the game holds it when aiming, in every pose; off, the game's own hold of each moment. | On |
| **Sidearms held by their aiming grip** (new in 0.7.0) | Each revolver or pistol is held the way the game holds it when aimed, so it moves with your hand at once instead of lagging when the game runs slowly. | On |
| **Arms follow the controllers** | John's drawn arms reach to your controllers and his hands turn with them; off, the game's own arm animation. | On |
| **Shadows drawn once for both eyes** | The shadows drawn once a frame for both eyes instead of once per eye: about 0.9 ms faster; a shadow's edge can shift by a pixel. | On |
| **Steady gun far out** (new in 0.8.12) | The gun in your hand doesn't tremble far from the map's middle (the game's positions there step by half a millimetre): it is held where it was while your hand and head stay still. | On |
| **Still clothes** (new in 0.8.12) | John's clothes don't flutter in the wind (seen from inside the body it reads as a buzz). | On |
| **Arm motion vectors (experimental)** (new in 0.8.9) | Tells DLSS and TAA how John's arms moved, which may cut the ghosting beside fast-moving arms. | On |
| **Show the launcher when the game starts (next start)** (new in 0.8.12) | `RDRVR_Launcher.exe` opens on the desktop each time the game starts, and the game waits for its Save & Play (its Close cancels the start): the settings saved there apply to that start. Off: the game starts at once. | On |
| ↳ **Goes on by itself after** | If nobody presses Save & Play (you started the game from the headset), the game goes on after this many seconds with the saved settings, 5 s per press (0 to 300 s). A click in the launcher stops the countdown; 0: no countdown (the game still goes on after 5 minutes). Applies at the next start. | 30 s |

### Gunfights

Shown only while Ped Gunfight Overhaul (a separate RedHook mod by another author: `PedGunfightOverhaul.red` next to `RDR.exe`) is installed (new in 0.8.12). The page sets that mod's own settings. With its version 1.1.5 running, a change applies within a second, and Save keeps it for the next game. Another version, or one not running: the page changes only its ini (Save, then restart the game or press its own Reload key). With Ped Gunfight Overhaul installed, a lasso catch kills the person caught: in the developer's tests that is its doing, not RDRVR's.

| Setting | What it does | Default |
|---|---|---|
| **NPC accuracy (%)** | How well NPCs aim: 100 is the game's own, 50 half as well, 200 twice as well. Hold an arrow to change it faster. | Its ini's |
| **NPC health** | How much NPCs can take: 25 is the game's own, 50 twice as much; 0: they cannot be hurt. | Its ini's |
| **Cripple at (%)** | NPCs shot may limp, kneel or crawl only once their health is at or below this share. 100 is the game's own (any time); 0: never. | Its ini's |
| **Headshots always kill** | An NPC shot in the head dies, whatever their health. | Its ini's |
| **Your health** | How much John can take: 100 is the game's own (35 its hardcore), 200 twice as much; 0: he cannot be hurt. | Its ini's |
| **Its keyboard controls** | Ped Gunfight Overhaul's own menu on the keyboard's arrow keys (and its Numpad 5 save, Numpad 0 reload). | Its ini's |
| **Save / Reload** | Save writes these settings into `PedGunfightOverhaul.ini`, as its Numpad 5 does; Reload takes the saved ones back, as its Numpad 0 does. | – (buttons) |

### Where to find...

- **Recentre:** Comfort, "Recentre". You can also use the headset's own recentre, Scroll Lock, or hold both stick clicks for a second.
- **Single pass or two-pass:** Display, "Rendering" (restart).
- **Render resolution:** Display, "Render resolution: headset" and "size" (restart).
- **DLSS:** Display, "Anti-aliasing" then "DLSS quality" (restart).
- **Sharpening:** Display, "Sharpening".
- **Eye shape:** Display, "Eyes in the headset's shape" (FXAA), or "Eyes in the headset's shape with DLSS".
- **Cutscenes in 3D:** Display, "Cutscenes".
- **Room-scale walking, the physical crouch, John turning with you:** Comfort, "Room-scale walking", "Physical crouch" and "John turns when you turn".
- **Snap or smooth turning:** Comfort, "Smooth turning / Snap turning".
- **Holding the grip to keep a gun:** Holsters, "Hold the grip to keep the gun".
- **The glow on the gun's parts, or the rings:** Holsters, "Interaction points".
- **Holster placement:** Holsters, "Placing by hand", or each holster's Ring / Size arrows.
- **The weapon in a holster:** Holsters, that holster's "Weapon".
- **Scopes:** Guns, "Look through a rifle's scope".
- **Dead Eye:** Guns, "Dead Eye level" and the rows under it.
- **Carrying a long gun by its foregrip:** Guns, "Carry a long gun by its foregrip".
- **Horse whistle:** Gestures, "Whistle for the horse".
- **Satchel:** hold the menu button and press A. With the wrist HUD you can also turn on Display, "A double tap of Y at the wrist opens the satchel".
- **Left-handed mode:** Controls, "Left-handed".
- **Button mapping:** Controls, "Buttons".
- **Reticle:** Guns, "Reticle where the shot lands".
- **Weapon wheel instead of holsters:** Holsters, "Weapons by: The weapon wheel".
- **The launcher at the game's start:** Advanced, "Show the launcher when the game starts".

## Tips

- **Sharpness:** each eye shows the game's own frame (16:9), or, with "Eyes in the headset's shape" on, a frame in
  the headset's own shape. For a sharper image on a high-resolution headset, choose the "Render resolution" (the Display
  tab): your headset or Automatic, at 100%, 150% or 200%, if your GPU allows (new in 0.4.0, the sizes in 0.6.0); "Eyes in the headset's
  shape" (with FXAA, or "... with DLSS", new in 0.8.0) makes it cheaper, and DLSS Quality (usable since 0.5.0; from the next start) renders each eye at
  two thirds of the width and height (about 9 ms of GPU a frame at 3926 x 2208 on an RTX 4070 Ti, against FXAA's 7).
  "Sharpening" (the Display tab, new in 0.8.12) brings back some of the detail DLSS and TAA soften.
- **Frame generation** layers (frame interpolation in the OpenXR runtime or a layer) make the guns lag behind your
  hands: the generated frames cannot follow the gun. Prefer a lower refresh rate (72 or 80 Hz) with real frames.

## Compatibility

- Bindings: Meta Touch, Touch Pro and Touch Plus, Valve Index, HTC Vive, Vive Cosmos and Focus 3, Windows Mixed
  Reality, HP Reverb G2, Samsung Odyssey, Pico 4 and Neo 3, and the Khronos simple controller. WMR and the Vive wands
  have no A/B buttons: the trackpad's click gives them (upper half B/Y, lower A/X).
- Only Touch controllers on a Quest 3 have been tested on real hardware. If a controller type sits differently in your
  hand, the Body tab's "Controller fit" moves and turns the hands for that type.
- The game's own builds: the mod was made on one build of `RDR.exe`. On another build it looks for the game code and
  data it uses by their bytes (new in 0.3.0, not yet tried on a real other build). If it finds and checks every one,
  it runs; if not, it stands down (the game plays flat). Either way it writes `RDRVR_build_report.txt` in the game
  folder: if the mod stands down or misbehaves, please attach it, with `RDRVR.log`, to an issue. Since 0.8.1 two of them are optional. One: in a player's build
  the mod found every one but the playback thread's dispatch (used only by the batched hand-off, `[Render] PlaybackBatch` in the ini), and
  it stood down there. Such a build should now run with the batched hand-off off (`RDRVR.log` and the report say so;
  tried by forcing it on the developer's build, not yet on that player's), and the report shows the bytes near a
  missing one so a later release can find it. The other (0.8.2) tells the map screen apart: a build without it keeps
  the game-menu controls on the map, as before 0.8.2. A game executable
  without the Steam version's start-up stage (the same game build otherwise) should no longer start flat (new in 0.6.0,
  not yet tried on such an executable; the developer forced the path on the Steam version): the mod notices and hooks
  at once, and `RDRVR.log` says if its hooks came too late. RedHook, which runs
  the gameplay plugin, has to work on that build too.

## Known issues

- Shooting from right behind cover can be refused by the game's "gun against a wall" check, which looks from John's
  shoulder, not from your gun: "Shoot past the arm block" (the Guns tab, on by default, new in 0.3.0) lifts it; off,
  the game's check applies.
- Standing or walking with a gun raised, the game takes no jump, so no vault or climb: "Jumping lowers the raised gun"
  (the Controls tab, on by default, new in 0.3.0) lowers the gun for the jump; off, the game's rule applies. Vaulting
  over a fence or a low wall has not been checked yet.
- DLSS (in the anti-aliasing choice, from the next start) has been tried in one headset (a Quest 3 over Virtual
  Desktop) at Quality; FXAA stays the default. With DLSS, Virtual Desktop's latency figure counts the GPU's whole
  frame now (it did not before), so it reads higher than with FXAA for the same real latency.
- Rarely, the game froze or slowed to a few frames a second (seen a few times in the developer's simulator tests).
  A slowdown that never recovered was found and fixed in 0.8.12 (the mod's checks before it reads the game's memory);
  a freeze's cause is not known yet. If either happens, please attach `RDRVR.log` to an issue: it records what the
  game's render threads were doing, and its `[xr] timing` lines count those checks.
- The holsters' guns cost frame time by how many are shown: the back's long guns (off by default) are the expensive
  part.
- With "Long guns held by their aiming grip" off (the Advanced tab; on by default since 0.8.1), John's gun hand can sit
  a few centimetres up a long gun (at the receiver, not the stock's wrist) when it is lowered or carried.
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
- A revolver at the right hip can sometimes show a second copy of itself that trails slightly behind (the game's own
  holstered gun; the Cattleman's turned, the Schofield's near the chest). Being looked into.
- A dual-wield copy of the Cattleman can sit turned in the hand (its barrel pointing sideways), seen after walking out
  of a building and back in with it; putting it back and taking it again does not always help. Being looked into;
  `RDRVR.log`'s `[wield] the copy turned` lines say when it happened.
- Foregrip positions can shift slightly between sessions, and a grab can sometimes land in a different hand pose
  (being looked into).
- Short bursts of stutter (single frames of about 90 ms for 10 to 20 seconds, the GPU not the limit) were seen in the
  developer's headset in 0.8.5 as in earlier versions; the cause is not known yet. `RDRVR.log`'s `[xr] timing` lines
  show them: please attach it to an issue with what you were doing.
- The default places of the holsters, the belt pieces, each gun's foregrip, loading ring and action spot, the rounds,
  magazines and ramrod in the hand, and the interaction spot were tuned in the headset by one person: if they do not
  suit you, move them with the menu's arrows (Holsters, Reloading, Guns and Body tabs).

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
