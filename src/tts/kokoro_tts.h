#pragma once
// English synthesis: Kokoro-82M through the sherpa-onnx offline TTS C API.
#include <string>

#include "tts/tts_engine.h"

struct SherpaOnnxOfflineTts;

namespace aii {

class KokoroTts final : public TtsEngine {
 public:
  KokoroTts(const std::string& model_dir, int speaker_id, float speed, int num_threads);
  ~KokoroTts() override;

  bool ok() const override { return tts_ != nullptr; }
  int sample_rate() const override { return rate_; }
  const char* name() const override { return "kokoro"; }
  bool synthesize(const std::string& text, AudioChunk& out) override;

 private:
  const SherpaOnnxOfflineTts* tts_ = nullptr;
  int rate_ = 0;
  int sid_ = 0;
  float speed_ = 1.0f;
  std::string model_, voices_, tokens_, data_dir_, lexicon_;
};

}  // namespace aii
