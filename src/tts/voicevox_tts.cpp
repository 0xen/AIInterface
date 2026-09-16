#include "tts/voicevox_tts.h"

#include <cstring>
#include <filesystem>

#include "voicevox_core.h"

namespace fs = std::filesystem;

namespace aii {

VoicevoxTts::VoicevoxTts(const std::string& core_dir, const std::string& models_dir, uint32_t style_id)
    : style_(style_id) {
  auto fail = [&](const char* what, VoicevoxResultCode r) {
    error_ = std::string(what) + ": " + voicevox_error_result_to_message(r);
    return false;
  };

  std::string ort_dir = core_dir + "/onnxruntime/lib/";
  std::string ort_path = ort_dir + voicevox_get_onnxruntime_lib_recommended_unversioned_filename();
  if (!fs::exists(ort_path)) ort_path = ort_dir + voicevox_get_onnxruntime_lib_recommended_versioned_filename();
  auto ort_opts = voicevox_make_default_load_onnxruntime_options();
  ort_opts.filename = ort_path.c_str();
  const VoicevoxOnnxruntime* ort = nullptr;
  VoicevoxResultCode r = voicevox_onnxruntime_load_once(ort_opts, &ort);
  if (r != VOICEVOX_RESULT_OK) { fail("onnxruntime_load_once", r); return; }

  std::string dict_dir = models_dir + "/dict/open_jtalk_dic_utf_8-1.11";
  OpenJtalkRc* ojt = nullptr;
  r = voicevox_open_jtalk_rc_new(dict_dir.c_str(), &ojt);
  if (r != VOICEVOX_RESULT_OK) { fail("open_jtalk_rc_new", r); return; }

  auto init = voicevox_make_default_initialize_options();
  init.acceleration_mode = VOICEVOX_ACCELERATION_MODE_CPU;
  init.cpu_num_threads = 0;
  VoicevoxSynthesizer* syn = nullptr;
  r = voicevox_synthesizer_new(ort, ojt, init, &syn);
  voicevox_open_jtalk_rc_delete(ojt);
  if (r != VOICEVOX_RESULT_OK) { fail("synthesizer_new", r); return; }

  std::string vvm_path = models_dir + "/models/vvms/0.vvm";
  VoicevoxVoiceModelFile* model = nullptr;
  r = voicevox_voice_model_file_open(vvm_path.c_str(), &model);
  if (r != VOICEVOX_RESULT_OK) { fail("voice_model_file_open", r); voicevox_synthesizer_delete(syn); return; }
  r = voicevox_synthesizer_load_voice_model(syn, model, voicevox_make_default_load_voice_model_options());
  voicevox_voice_model_file_delete(model);
  if (r != VOICEVOX_RESULT_OK) { fail("load_voice_model", r); voicevox_synthesizer_delete(syn); return; }
  synth_ = syn;
}

VoicevoxTts::~VoicevoxTts() {
  if (synth_) voicevox_synthesizer_delete(synth_);
}

bool VoicevoxTts::synthesize(const std::string& text, AudioChunk& out) {
  if (!synth_) return false;
  uintptr_t wav_len = 0;
  uint8_t* wav = nullptr;
  VoicevoxResultCode r =
      voicevox_synthesizer_tts(synth_, text.c_str(), style_, voicevox_make_default_tts_options(), &wav_len, &wav);
  if (r != VOICEVOX_RESULT_OK) {
    error_ = std::string("tts: ") + voicevox_error_result_to_message(r);
    return false;
  }
  // Parse the in-memory RIFF/WAVE buffer (16-bit PCM mono 24 kHz).
  bool okay = false;
  if (wav_len >= 12 && !std::memcmp(wav, "RIFF", 4) && !std::memcmp(wav + 8, "WAVE", 4)) {
    size_t p = 12;
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    while (p + 8 <= wav_len) {
      uint32_t sz;
      std::memcpy(&sz, wav + p + 4, 4);
      if (!std::memcmp(wav + p, "fmt ", 4) && sz >= 16) {
        std::memcpy(&channels, wav + p + 10, 2);
        std::memcpy(&rate, wav + p + 12, 4);
        std::memcpy(&bits, wav + p + 22, 2);
      } else if (!std::memcmp(wav + p, "data", 4)) {
        if (bits == 16 && channels >= 1 && rate > 0) {
          size_t frames = std::min<size_t>(sz, wav_len - p - 8) / (2 * channels);
          out.sample_rate = (int)rate;
          out.samples.resize(frames);
          const int16_t* s = reinterpret_cast<const int16_t*>(wav + p + 8);
          for (size_t i = 0; i < frames; ++i) out.samples[i] = s[i * channels] / 32768.0f;
          okay = !out.samples.empty();
        }
        break;
      }
      p += 8 + sz + (sz & 1);
    }
  }
  voicevox_wav_free(wav);
  if (!okay) error_ = "unexpected WAV format from voicevox";
  return okay;
}

}  // namespace aii
