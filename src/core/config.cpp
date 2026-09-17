#include "core/config.h"

#include <cstdlib>

namespace aii {

std::string env_or(const char* name, const std::string& def) {
  char* v = nullptr;
  size_t len = 0;
  if (_dupenv_s(&v, &len, name) == 0 && v) {
    std::string s(v);
    free(v);
    if (!s.empty()) return s;
  }
  return def;
}

Config Config::from_env() {
  Config c;
  c.backend = env_or("AII_BACKEND", "code");
  c.effort = env_or("AII_EFFORT", "low");
  c.model_override = env_or("AII_MODEL", "");
  c.langs = language_selection_from_spec(env_or("AII_LANGS", "en,ja"));
  // Empty by default now, and that is the change: the recogniser's language is
  // *derived* from the enabled languages (both -> auto, one -> that one), so
  // the ordinary case has one source of truth. AII_STT_LANG is kept as a
  // deliberate override for someone pinning the recogniser by hand, and it
  // still wins when it is set.
  c.stt_lang = env_or("AII_STT_LANG", "");
  c.claude_exe = env_or("AII_CLAUDE_EXE", env_or("USERPROFILE", "C:\\Users\\Default") + "\\.local\\bin\\claude.exe");
  c.api_key = env_or("ANTHROPIC_API_KEY", "");
  c.kokoro_sid = std::atoi(env_or("AII_KOKORO_SID", "3").c_str());
  c.vv_style = (unsigned)std::atoi(env_or("AII_VOICEVOX_STYLE", "2").c_str());
  c.early_words = std::atoi(env_or("AII_EARLY_WORDS", "12").c_str());
  c.endpoint_silence = (float)std::atof(env_or("AII_ENDPOINT_SILENCE", "1.0").c_str());
  if (c.endpoint_silence < 0.2f) c.endpoint_silence = 0.2f;
  c.worker_bypass = env_or("AII_WORKER_BYPASS", "1") != "0";

  c.models_dir = AII_MODELS_DIR;
  c.stt_dir = c.models_dir + "/sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-560ms-int8-2026-06-11";
  c.kokoro_dir = c.models_dir + "/kokoro-multi-lang-v1_0";
  c.vv_models_dir = c.models_dir + "/voicevox";
  c.vv_core_dir = AII_VV_CORE_DIR;
  return c;
}

}  // namespace aii
