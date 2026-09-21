#pragma once
// Mono float sample-rate conversion, used when a synthesis engine hands back a
// chunk at a rate the output device was not opened at.
//
// M21.2 (review finding 31): `speech_queue` used to warn about the mismatch and
// push the chunk anyway, which plays it at the wrong speed and pitch. Latent
// today -- Kokoro and VOICEVOX are both 24 kHz -- but it is a silent trap for
// the next engine, and the warning went to the status line where a listener
// hearing a chipmunk would not think to look.
//
// Resampling rather than refusing: refusing would drop a sentence the user is
// waiting to hear, and this runs on the speech thread where a few milliseconds
// of linear interpolation cost nothing next to the synthesis that produced the
// chunk. miniaudio's own resampler is used because miniaudio is already the
// audio layer here and its linear mode is what it would apply internally
// anyway.
#include <cstddef>
#include <vector>

namespace aii {

// Converts `in` from in_rate to out_rate into `out` (replaced, not appended).
// Returns false if either rate is not positive or the conversion fails; `out`
// is then left empty. Equal rates copy.
bool resample_mono(const std::vector<float>& in, int in_rate, int out_rate,
                   std::vector<float>& out);

}  // namespace aii
