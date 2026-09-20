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

// M13.2. The styles the loaded model actually carries, read from VOICEVOX's own
// metas rather than from a table here: the model file is the authority, and it
// is versioned with the voices it holds.
//
// Parsed by hand rather than with the JSON library, because the shape needed is
// exactly `"id":<number>` inside the `styles` arrays and nothing else -- pulling
// nlohmann into the synthesis engine to read one integer per style would be a
// dependency bought for a substring search.
bool VoicevoxTts::has_style(uint32_t style) const {
  if (!synth_) return false;
  char* metas = voicevox_synthesizer_create_metas_json(synth_);
  if (!metas) return false;
  const std::string json(metas);
  voicevox_json_free(metas);

  // `styles` is the per-character array; every entry in it has an `id`. A
  // character's own `speaker_uuid` has no numeric id, so scanning for `"id":`
  // inside the styles arrays alone is what keeps this from matching something
  // that is not a style.
  const std::string want = std::to_string(style);
  size_t at = 0;
  while ((at = json.find("\"styles\"", at)) != std::string::npos) {
    const size_t arr_end = json.find(']', at);
    if (arr_end == std::string::npos) break;
    size_t id_at = at;
    while ((id_at = json.find("\"id\"", id_at)) != std::string::npos && id_at < arr_end) {
      size_t p = json.find(':', id_at);
      if (p == std::string::npos) break;
      ++p;
      while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
      size_t q = p;
      while (q < json.size() && json[q] >= '0' && json[q] <= '9') ++q;
      if (q > p && json.compare(p, q - p, want) == 0) return true;
      id_at = q;
    }
    at = arr_end;
  }
  return false;
}

bool VoicevoxTts::synthesize(const std::string& text, AudioChunk& out) {
  return synthesize_as(text, -1, out);
}

bool VoicevoxTts::synthesize_as(const std::string& text, int native_voice, AudioChunk& out) {
  if (!synth_) return false;
  const uint32_t style = native_voice < 0 ? style_ : static_cast<uint32_t>(native_voice);
  uintptr_t wav_len = 0;
  uint8_t* wav = nullptr;
  VoicevoxResultCode r =
      voicevox_synthesizer_tts(synth_, text.c_str(), style, voicevox_make_default_tts_options(), &wav_len, &wav);
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
