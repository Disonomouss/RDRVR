#pragma once
// Test-only game calls for the off-hand research's probes (research/run33/probes_brief.md, "The test-only commands";
// research/run33/drafts/game.md sections 3 and 5). Nothing runs until a "probe" command is sent on the test channel: the
// first one sets the core's tick hook slot 2 (api::set_tick_hook), and the game calls run there, on the game thread
// inside the plugin's script tick (api::in_script_tick), as gun_melee's ActorHit call and offhand's throw do.
//
//   probe snap              FUN_140d19670(G): the game's instant holster snap (G state 0, the item at its holster)
//   probe fuse on|off       FUN_140d1bde0(G, 1|0): the fuse of the item in hand lit or put out
//   probe fuseinfo [w]      read only (the channel's thread): weapon info +0x32c (the fuse, s) and the projectile
//                           definition's +0x20 (its life) for weapons 24, 25, 37, or w
//
// The addresses are used only on the analysed RDR.exe (anchors::exe_matches(), not relocated(), not stand_down()), and
// each function's first 16 bytes are compared with research/RDR.exe's before it is called.

#include <string>

namespace rdrvr::probe {

std::string command(const std::string& line);

}  // namespace rdrvr::probe
