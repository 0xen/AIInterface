#pragma once
// Japanese synthesis: VOICEVOX Core C API (CPU).
#include <cstdint>
#include <string>

#include "tts/tts_engine.h"

struct VoicevoxSynthesizer;

namespace aii {

class VoicevoxTts final : public TtsEngine {
 public:
  // core_dir: contains onnxruntime/lib; models_dir: contains dict/ and models/vvms/
  VoicevoxTts(const std::string& core_dir, const std::string& models_dir, uint32_t style_id);
  ~VoicevoxTts() override;

  bool ok() const override { return synth_ != nullptr; }
  int sample_rate() const override { return 24000; }
  const char* name() const override { return "voicevox"; }
  bool synthesize(const std::string& text, AudioChunk& out) override;
  std::string last_error() const { return error_; }

 private:
  VoicevoxSynthesizer* synth_ = nullptr;
  uint32_t style_ = 3;
  std::string error_;
};

}  // namespace aii
