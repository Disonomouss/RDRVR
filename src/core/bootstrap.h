#pragma once
// Startup sequence. DllMain only opens the log and starts the bootstrap thread. That thread waits until
// Rockstar's MTLX wrapper has unloaded (it is resident while the hidden import table is resolved, i.e.
// while we are loaded) and the game's own code runs, then verifies the anchors and installs hooks.

namespace rdrvr::bootstrap {

void on_process_attach();   // from DllMain: no waiting, no loading, no hooks
void on_process_detach();

}  // namespace rdrvr::bootstrap
