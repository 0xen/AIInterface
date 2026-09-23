#pragma once
// M18.4. Choosing an audio endpoint by name instead of taking the Windows
// default.
//
// The app opens the default capture and playback devices, and on the machine
// it is developed on the default capture device is a headset microphone that
// returns digital silence (found while tuning barge-in). Tuning
// barge-in means capturing through the webcam microphone that actually hears
// the room, and measuring the loudspeaker configuration means *playing* through
// the loudspeakers while the default output stays the headset -- neither of
// which should require re-plumbing Windows sound settings between runs. So:
// `AII_MIC` and `AII_SPEAKER` name a device by a substring of its name, the
// way the M18.0 probe's `--in`/`--out` did, and `MicIn::open()` /
// `AudioOut::start()` take the id this finds.
//
// The lookup opens a miniaudio context of its own, copies the id out and closes
// it again. An `ma_device_id` is a value (on WASAPI, the endpoint id string),
// so the copy stays valid after the context that produced it is gone; the
// probe relied on the same thing.
#include <string>
#include <vector>

#include "miniaudio.h"

namespace aii {

struct AudioDeviceName {
  std::string name;
  bool is_default = false;
};

// The first device of that kind whose name contains `needle` (case-sensitive,
// as the probe was). False when nothing matches or the context cannot be
// opened; `*name` is then untouched.
bool find_audio_device(bool playback, const std::string& needle, ma_device_id* id,
                       std::string* name);

// Every device of that kind, for the log line that says what *was* there when
// a name did not match.
std::vector<AudioDeviceName> list_audio_devices(bool playback);

}  // namespace aii
