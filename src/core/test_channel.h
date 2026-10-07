#pragma once
// The harness's command channel (DESIGN R0). tools\harness.ps1 writes RDRVR_cmd.txt ("<seq> <command> [args]");
// a worker thread runs each new seq once and writes RDRVR_status.json (temp file + rename, so a reader never sees
// a partial file) with the result and the live counters. A command file that exists at startup is adopted, not run.

namespace rdrvr::test_channel {

void start();

}  // namespace rdrvr::test_channel
