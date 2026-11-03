#pragma once
// Run 5 item 1b: a round seen in the hand that holds it ([Reload] RoundInHand). The game has no live revolver or rifle
// round (research\run4\rounds.md), so the mod draws its own: a lathe mesh made in code (a rim, the brass case, the lead
// bullet; a shotgun shell's brass head and hull), drawn into each eye image in the eye blit's own command list
// (xr_blit), at the drawn hand of the frame, with the frame's eye cameras and projections (camera_lever's scene pass).
// It is hidden where the game's scene is nearer: each eye's scene depth is copied inside the game's own CopyResource
// into "Depth Resolve" (the frame grabber's tap) into a buffer the mod owns, and the pixel shader compares the game's
// distance with the round's. No game resource changes state; nothing is written to the game.
//
// 2026-10-27 (the user: "Create new models for the bullets. Make them match the game style and resolution. Use the
// games shotgun shells."; research/run27/rounds.md): [Reload] RoundModels = 1 (the default) draws each gun's own round
// at its real calibre (kind_of: the .44-40 of the revolvers, the Carbine and the Winchester; the Henry's .44 rimfire;
// the Evans' .44 Evans Long; the .38 ACP, the .45 ACP and the 7.63 Mauser of the automatic pistols; the Volcanic's
// caseless Rocket Ball; a 7x57-class rifle round; the .45-70 of the Springfield, the Buffalo and the Rolling Block;
// the game's shotgun shell's own size; the Blunderbuss's paper cartridge), 12 sides (the shell 10) and 140-216
// triangles each, near the game's own density, textured from one small atlas made in code at boot (about 16 texels a
// cm: brass, copper, lead, the hull, with tarnish, wear and grime; nothing of the game's), lit in the world (a key from
// above and the body's front, a sky and ground ambient: the highlight no longer turns with the head) and shaded by
// the game's own eye image round it (a ring of taps: the hand's shade, dusk, indoors), its silhouette and the fingers'
// edge softened. RoundModels = 0: the old lathe (three rounds, 24 sides, a light in the head), for the perf and A/B.

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace rdrvr::body {
struct BodyPoints;
}

namespace rdrvr::round_draw {

void init();  // bootstrap: the settings, the meshes and the atlas made, the depth tap
bool enabled();
void set_enabled(bool on);  // also written to the user ini
int mode();             // [Reload] RoundInHand: 0 off, 1 the mod's round, 2 also the game's own shells for shotguns
void set_mode(int m, bool save = true);  // save: also written to the user ini ("round mode <m> session": not)
// [Reload] RoundModels (2026-10-27): true the new rounds (each gun's calibre, the atlas, the world light and the
// scene's shade), false the old lathe
bool models();
void set_models(bool new_models, bool save);  // save: also written to the user ini
// Where the round sits in the hand (run 6 item 5; [Reload] RoundOffset, RoundAxis, RoundBrightness; the Reloading tab):
// moved from the pinch by `offset` (m, the hand's wrist axes), pointing along the fingers (RoundAxis 1 0 0), out of the
// hand (auto: from the wrist through the pinch) or along a custom axis (the wrist's axes), drawn `bright` times as bright.
struct Place {
    float offset[3];
    int axis_mode;  // 0 along the fingers, 1 out of the hand, 2 custom
    float axis[3];
    float bright;
};
// hand (2026-10-31, R13): John's 0 left (the keys above), 1 right (its own once set: RoundOffsetRight, RoundAxisRight)
Place place(int hand = 0);
void set_place(const Place& p, bool save, int hand = 0);  // save: also written to the user ini
// 2026-10-29 (round 38: the magazines "don't seem to move with the ammo adjustments"): the held magazine's (the Mauser's
// clip's) own place in the hand, apart from the round's: [Reload] MagazineOffset (right, up, forward in the hand's axes, as
// RoundOffset; m), MagazineTurn and MagazineTilt (degrees about the hand's up and its right, about the held end)
struct MagPlace {
    float offset[3];
    float turn, tilt;
};
// (2026-11-01, GOAL-offhand-changes C38; the user: each pistol's magazine set separately, and for each hand) weapon 1-3 (the
// magazine pistols): that pistol's own for the hand once set ([Weapon.<gun>] HeldMagazineOffset, -Turn, -Tilt; the left's
// ...Left), else the hand's above; -1 the hand's (own: that pistol's own is set)
MagPlace mag_place(int hand = 0, int weapon = -1, bool* own = nullptr);  // (hand: as place's; MagazineOffsetRight, ...TurnRight, ...TiltRight)
void set_mag_place(const MagPlace& p, bool save, int hand = 0, int weapon = -1);  // save: also written to the user ini
void clear_mag_place(int hand, int weapon);  // that pistol's own for the hand forgotten (its keys emptied): the hand's again

// 2026-10-27: the round of each gun (its real calibre; -1 none: no gun, or one that takes no rounds by hand)
enum Kind {
    kK4440 = 0,   // .44-40 WCF: the Cattleman, the Schofield, the Double-Action, the LeMat, the Carbine, the Winchester
    kKHenry,      // .44 Henry rimfire (a copper case): the Henry
    kKEvans,      // .44 Evans Long: the Evans
    kK38Acp,      // .38 ACP: the Semi-Automatic pistol
    kK45Acp,      // .45 ACP class: the High Power
    kKMauser,     // 7.63 x 25 Mauser (a bottleneck): the Mauser
    kKVolcanic,   // .41 Rocket Ball (no case: the powder in the bullet's base): the Volcanic
    kKRifle,      // 7 x 57 / 6.5 Carcano class (rimless, a spitzer): the Bolt Action, the Carcano, the Explosive rifle
    kKBig,        // .45-70 Government (rimmed, straight): the Springfield (a trapdoor), the Buffalo, the Rolling Block
    kKShell,      // the shotgun shell, the game's own size (47.4 x 19.6 mm): the four shotguns
    kKPaper,      // a paper cartridge (a muzzle loader): the Blunderbuss
    kKinds
};
int kind_of(int weapon);
const char* kind_name(int kind);  // "44-40", "henry", ... (the test commands' names)

// The render thread, at each eye's scene pass (camera_lever::scene_once): that pass's eye camera (rows right, up,
// back, position) and its tangents (l < 0 < r, d < 0 < u).
void note_pass(int eye, const float* cam, float l, float r, float u, float d);
// The holsters (each update): John's hand holding a round taken at the chest (0 left, 1 right; -1 none) and the gun in
// hand (its weapon id: the round's kind, kind_of; with RoundModels 0 the old family, audio::click_family). count: the
// rounds that hand holds ([Reload] RoundsPerGrab; up to 8 drawn): side by side across the pinch, the first where one
// round is drawn. fwd: the body's forward (world, level; the new rounds' light), nullptr: kept as it was.
void set_held(int john_hand, int weapon, int count = 1, const float* fwd = nullptr);
// John's hand holding a round now (set_held within 250 ms), -1 none: body.cpp gives it the pinch's fingers with a long
// gun (cycle r171). Any thread.
int held_hand();

// The eye images the blit has just drawn the swapchain images from (xr_blit's staging textures, in
// PIXEL_SHADER_RESOURCE): the new rounds read a ring of them round each round for the scene's light there (2026-10-27).
// cw x ch: the content of each (texels; the image's iw x ih pixels map onto it); gamma: the blit's; fade: [XR]
// RoomScale's fade toward black (the rounds fade with the image).
struct EyeImages {
    ID3D12Resource* tex[2];
    DXGI_FORMAT fmt;
    uint32_t cw, ch;
    float gamma;
    float fade;
    bool skip[2];  // (r55 item 11) no rounds drawn into that eye (the scope's eye, black or showing the scope's view)
};
// The presenting thread, in xr_blit::blit's list after both eye images are drawn (the list's allocator slot `slot`,
// whose previous use the GPU has passed): the round into each eye's swapchain image dst[e] (RENDER_TARGET, its RTV
// rtv[e]). Changes the list's state (root signature, heaps, pipeline, targets): nothing may be drawn after it without
// setting its own. [XR] EyeShape: vw x vh, the eye image at the swapchain image's origin (0: all of w x h), and dw x dh,
// the scene depth's content behind it (0: the whole depth target). eyes: nullptr: no scene light probe (the rounds lit
// as in the open).
void record(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* const dst[2], const D3D12_CPU_DESCRIPTOR_HANDLE rtv[2],
            uint32_t w, uint32_t h, DXGI_FORMAT fmt, int slot, uint32_t vw = 0, uint32_t vh = 0, uint32_t dw = 0, uint32_t dh = 0,
            const EyeImages* eyes = nullptr);

// The presenting thread, after xr_blit closed (ok: and submitted) the list record() was given in `slot` (the review of
// 2026-10-27: the atlas's copy counts as done only then).
void list_submitted(int slot, bool ok);

// [Reload] ShowAmmo (off; run 6 item 6b): while a round could go in now (the holsters say), a row of `n` rounds of the
// gun's (`weapon`, as set_held's) standing at the chest's ammo zone `at`, side by side along the body's right (world:
// the zone, the body's right, up and forward), drawn and hidden by the scene as the round in hand. weapon < 0 or n 0:
// none.
bool show_ammo();
void set_show_ammo(bool on, bool save = true);
void set_row(int weapon, int n, const float at[3], const float rgt[3], const float up[3], const float fwd[3]);
// (2026-11-01, the off-hand build step 7, R12: the magazine and clip row) while a magazine could go in now (the holsters
// say: PistolMagazines' take), `n` (up to 2) of the gun's magazines (the High Power's, the Semi-Auto's) or the Mauser's
// clip standing at the chest's zone `at`, side by side along the body's right, each of `rounds`. weapon < 0 or n 0: none
void set_mag_row(int weapon, int n, int rounds, const float at[3], const float rgt[3], const float up[3], const float fwd[3]);
// The round's drawn pose for John's `hand` from the body's points: 12 floats, the axes X, Y (base to tip), Z, then the
// position (world). False when that hand's points are not valid.
bool pose(const body::BodyPoints& bp, int hand, float* out);
// 2026-10-27 ([Reload] RoundInHand = 2, the game's shells everywhere a shell is drawn): the poses of the shells drawn
// now, in the order record() draws them (the handful's from the pinch out, then the row's from its left), 12 floats
// each as pose()'s, at most `max`; *n_hand: how many are the hand's (the rest are the row's, placed by the body). Only
// a shotgun's (kind kKShell): 0 for any other round. The holsters' thread, after set_held and set_row.
int shell_poses(const body::BodyPoints& bp, float* out, int max, int* n_hand);

// 2026-10-28 ([Reload] PistolMagazines, GOAL-weapons-handling.md item 3; research/run28/magazines.md): our own magazines
// and stripper clip, made here as the rounds are, to the game's own magazines' measures (chamfered profiles carried in
// rings, about the guns' triangle and texel density, each one's steel painted at boot from its gun's own decoded
// diffuse values: nothing of the game's read or shipped), lit by the world and shaded by the scene round them, drawn
// whatever RoundInHand says (they are the option's): the High Power's, the Semi-Automatic Pistol's (its dark base
// knob) and the Mauser's stripper clip of ten. Each is drawn with its gun's rounds in it (a magazine: the top round in
// its lips; the clip: the rounds it holds). The model's frame: x right, y up (the floorplate to the lips; the clip's
// foot to its top), z back (the rounds point along -z), its foot at 0.
// The magazine's (the clip's) length (m): its top's distance from its foot; 0 for any other gun.
float mag_length(int weapon);
// John's hand holding a magazine now (set_held_mag within 250 ms), -1 none: body.cpp draws it with the gun hand's grip,
// mirrored ([Hands] CopyGrip's). Any thread.
int held_mag_hand();
// The holsters (each update): John's hand holding a magazine (a clip) for that gun, with `rounds` in it; -1 none. Drawn
// while called within 250 ms. Any thread.
void set_held_mag(int john_hand, int weapon, int rounds);
// The held magazine's pose for John's `hand` (12 floats: the axes X, Y, Z, then the foot; world): out of the hand along
// the wrist -> drawn pinch line, held by its base at the fingertips (its held end 1 cm short of the farther drawn tip),
// the body out past the fingers: a magazine's foot held, its lips out, its top foot + Y * mag_length; the Mauser's clip
// (weapon 3) held by its top, its foot out (pushed down into the gun's top). False when that hand's points are not
// valid. ("round showmag <hp|sa|clip> ...": one standing at a world place, the pictures' test aid.)
bool mag_pose(const body::BodyPoints& bp, int hand, int weapon, float* out);
// actions.cpp: a magazine (a clip) let go at `pose` (12 floats as mag_pose's, world) with `rounds` in it: it falls and
// is gone 0.6 s later (three at most at once). Any thread.
void drop_mag(const float pose[12], int weapon, int rounds);
// actions.cpp, each frame while the Mauser's clip sits in its guide: its pose (its foot in the guide) and the rounds
// still on it. Drawn while called within 250 ms. Any thread.
void seat_clip(const float pose[12], int rounds);

std::string command(const std::string& line);  // round [on|off] [offset x y z] [axis x y z] [grab <path.bmp>] ...
void status_text(char* out, size_t len);

}  // namespace rdrvr::round_draw
