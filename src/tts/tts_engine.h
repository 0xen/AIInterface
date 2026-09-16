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
  // Synthesise one sentence of UTF-8 text.
  virtual bool synthesize(const std::string& text, AudioChunk& out) = 0;
};

}  // namespace aii
