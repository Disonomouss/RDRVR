#pragma once
// [Render] PlaybackBatch: the game's D3D12 playback thread takes the render thread's recorded packets in batches instead
// of one at a time (the single-pass study, research\sps\measure.md). The recorder counts every packet with a locked
// increment of the chunk's count and rewrites the chunk's used size beside it; the playback loop decremented that count
// and re-read the chunk's data pointer per packet, so the cache line moved between the two cores on every packet (the
// stack sampler: 36-41% of the render thread's pass time stalled there, 59% of the playback's). The loop head is
// replaced (playback_head.asm): it claims all the packets counted so far with one locked subtract, once at least K are
// counted or the recorder stops adding, and keeps the data pointer; the packets themselves are replayed as before.
//   256 (the default): batches. 0: installed, one packet at a time (the original's semantics). -1: not installed.

#include <cstddef>

namespace rdrvr::playback {

void install();                    // after the anchors are checked
bool set_batch(int k);             // the test channel's "playbatch <K>"; false if not installed
bool installed();
int batch();                       // the K in use (-1: not installed)
void status(char* out, size_t len);

}  // namespace rdrvr::playback
