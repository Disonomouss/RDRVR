#pragma once
// Logs what is running: our own module (path, size, sha256, link time) and RDR.exe on disk (sha256),
// computed on a background thread so startup is not delayed.

namespace rdrvr::build_check {

void log_self_and_game_async();

}  // namespace rdrvr::build_check
