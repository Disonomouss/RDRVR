#pragma once
// [Gestures] HogtieClearProps (2026-10-20; after optimumbox's finding, fork 0.8.11.8): the game's own "press Y to
// hogtie" never started in VR. Its check (CAN_ACTOR_HOGTIE_TARGET, RDR.exe 140ca9ae0) sweeps a 0.5 m capsule between
// the player and the one on the lasso and refuses on any hit but the two actors and what is attached to them, and the
// mod's free props (held_prop: the guns and models shown at the holsters, with [Holsters] ShowGuns or ShowModels) are
// such hits. While someone is on the player's lasso (on foot) those props are hidden, until the rope has been empty
// for a second; the game's own holstered gun shows meanwhile. 2026-11-04 (the user, 0.8.12 on the other PC: "Hogtie is
// broken again": the release's ini had it off): on in the shipped ini too, and every prop of the mod's hidden, as
// optimumbox's (the round, the off hand's item, the dual wield's copy as a prop: its moved draw stands in). It acts
// only while the rope holds someone, and polls only while such a prop is wanted.
#include <string>

namespace rdrvr::hogtie {
void init();
std::string command(const std::string& line);  // "hogtie [on|off]": the switch for the session, and the state
}  // namespace rdrvr::hogtie
