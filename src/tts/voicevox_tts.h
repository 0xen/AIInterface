#pragma once
// Japanese synthesis: VOICEVOX Core C API (CPU).
#include <cstdint>
#include <mutex>
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
  bool synthesize_as(const std::string& text, int native_voice, AudioChunk& out) override;
  // Whether a style id is in the loaded model. VOICEVOX loads exactly one
  // `.vvm`, so a style outside it is not merely a different character -- it is
  // a call that returns rc=6 and reaches the user as silence. Checked when the
  // voice list loads rather than at synthesis, where it is already too late.
  bool has_style(uint32_t style) const;
  // M21.2 (review finding 30). `error_` is written by whichever thread is
  // synthesising -- the speech queue's -- and read by whoever reports the
  // failure, which is the UI thread. Two threads on a std::string with no
  // synchronisation is a data race whatever the strings are, so both sides go
  // through this lock; it is taken once per failed synthesis and once per
  // report, never in a loop and never anywhere near the audio callback.
  std::string last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
  }

 private:
  void set_error(std::string what) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    error_ = std::move(what);
  }

  VoicevoxSynthesizer* synth_ = nullptr;
  uint32_t style_ = 3;
  mutable std::mutex error_mutex_;
  std::string error_;
};

}  // namespace aii
