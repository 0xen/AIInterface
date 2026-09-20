#pragma once
// Common interface for local speech synthesis engines. Output stays in memory.
#include <string>
#include <vector>

namespace aii {

struct AudioChunk {
  std::vector<float> samples;  // mono, [-1, 1]
  int sample_rate = 0;
};

class TtsEngine {
 public:
  virtual ~TtsEngine() = default;
  virtual bool ok() const = 0;
  virtual int sample_rate() const = 0;
  virtual const char* name() const = 0;
  // Synthesise one sentence of UTF-8 text in the engine's configured voice.
  virtual bool synthesize(const std::string& text, AudioChunk& out) = 0;
  // M13.2. The same, in one of the engine's other voices. `native_voice` is the
  // engine's own id -- a Kokoro speaker id, a VOICEVOX style -- and a negative
  // one means "the configured voice", so the default below is the honest
  // behaviour for an engine that has only one.
  //
  // Deliberately a parameter and not a `set_voice()`: no engine state is
  // mutated, so this stays safe the moment anything but the speech worker calls
  // it, and two voices cannot race over which one is current.
  virtual bool synthesize_as(const std::string& text, int native_voice, AudioChunk& out) {
    (void)native_voice;
    return synthesize(text, out);
  }
};

}  // namespace aii
