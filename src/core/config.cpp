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

const char* const kSystemPrompt =
    "You are a voice assistant running on the user's Windows PC. The user speaks English (primary) and Japanese. "
    "Always reply in the language the user just used; if they mix languages, use the dominant one. "
    "Your reply is read aloud by a text-to-speech engine, so write plain spoken prose: short sentences, "
    "one to three sentences unless the user asks for detail, no markdown, no lists, no headings, no emoji, no URLs. "
    "Write Japanese in normal Japanese script, never romaji. "
    "If something must be shown rather than spoken (code, a table, a long quote), put it inside a ``` fence and say "
    "briefly that it is shown on screen; text inside fences is displayed but not spoken.";

Config Config::from_env() {
  Config c;
  c.backend = env_or("AII_BACKEND", "code");
  c.effort = env_or("AII_EFFORT", "low");
  c.model_override = env_or("AII_MODEL", "");
  c.stt_lang = env_or("AII_STT_LANG", "auto");
  c.claude_exe = env_or("AII_CLAUDE_EXE", env_or("USERPROFILE", "C:\\Users\\Default") + "\\.local\\bin\\claude.exe");
  c.api_key = env_or("ANTHROPIC_API_KEY", "");
  c.kokoro_sid = std::atoi(env_or("AII_KOKORO_SID", "3").c_str());
  c.vv_style = (unsigned)std::atoi(env_or("AII_VOICEVOX_STYLE", "2").c_str());
  c.early_words = std::atoi(env_or("AII_EARLY_WORDS", "12").c_str());

  c.models_dir = AII_MODELS_DIR;
  c.stt_dir = c.models_dir + "/sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-560ms-int8-2026-06-11";
  c.kokoro_dir = c.models_dir + "/kokoro-multi-lang-v1_0";
  c.vv_models_dir = c.models_dir + "/voicevox";
  c.vv_core_dir = AII_VV_CORE_DIR;
  return c;
}

}  // namespace aii
