#pragma once
// Runtime configuration shared by every executable: environment variables and
// the model locations baked in by CMake.
//
// The system prompt used to be a `kSystemPrompt` literal here. It is now a
// seeded, composed prompt tree — see `core/prompt_store.h` and `aii::system_prompt()`.
#include <string>

namespace aii {

// Environment variable or a default (empty values count as unset).
std::string env_or(const char* name, const std::string& def);

struct Config {
  std::string backend;         // AII_BACKEND: "code" (Claude Code CLI) or "api"
  std::string effort;          // AII_EFFORT
  std::string model_override;  // AII_MODEL (empty = backend default)
  std::string stt_lang;        // AII_STT_LANG: auto | en | ja
  std::string claude_exe;      // AII_CLAUDE_EXE
  std::string api_key;         // ANTHROPIC_API_KEY (api backend only)
  int kokoro_sid = 3;          // AII_KOKORO_SID
  unsigned vv_style = 2;       // AII_VOICEVOX_STYLE
  int early_words = 12;        // AII_EARLY_WORDS (0 = full sentences only)
  // AII_ENDPOINT_SILENCE: how long a pause has to last, in seconds, before
  // the utterance is treated as finished and sent. Raise it if you are being
  // cut off while thinking mid-sentence; lower it for snappier turn-taking.
  float endpoint_silence = 1.0f;
  bool worker_bypass = true;   // AII_WORKER_BYPASS: workers skip permission prompts
                               // (nothing in this app can answer one, so a worker
                               // that asks would hang). Set to 0 to make them ask
                               // and stall instead of acting unattended.

  std::string models_dir;      // AII_MODELS_DIR compile definition
  std::string stt_dir;
  std::string kokoro_dir;
  std::string vv_models_dir;
  std::string vv_core_dir;     // AII_VV_CORE_DIR compile definition

  static Config from_env();
};

}  // namespace aii
