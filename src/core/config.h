#pragma once
// Runtime configuration shared by every executable: environment variables and
// the model locations baked in by CMake.
//
// The system prompt used to be a `kSystemPrompt` literal here. It is now a
// seeded, composed prompt tree — see `core/prompt_store.h` and `aii::system_prompt()`.
#include <string>

#include "core/language.h"

namespace aii {

// Environment variable or a default (empty values count as unset).
std::string env_or(const char* name, const std::string& def);

struct Config {
  std::string backend;         // AII_BACKEND: "code" (Claude Code CLI) or "api"
  std::string effort;          // AII_EFFORT
  std::string model_override;  // AII_MODEL (empty = backend default)
  std::string stt_lang;        // AII_STT_LANG: auto | en | ja (an explicit override)
  // M8.3. Which languages are on. The avatar overwrites this from
  // settings.json before it builds anything; `AII_LANGS` ("en", "ja", "en,ja")
  // is how voiceloop and the test harnesses reach the same switch. It decides
  // three things: the recogniser's option, whether VOICEVOX is built at all,
  // and whether Claude is told to stay in one language. `stt_lang` still wins
  // over it when it is set to something other than the default, so the
  // existing override is not quietly taken away.
  LanguageSelection langs;
  std::string claude_exe;      // AII_CLAUDE_EXE
  std::string api_key;         // ANTHROPIC_API_KEY (api backend only)
  int kokoro_sid = 3;          // AII_KOKORO_SID
  unsigned vv_style = 2;       // AII_VOICEVOX_STYLE
  int early_words = 12;        // AII_EARLY_WORDS (0 = full sentences only)
  // AII_ENDPOINT_SILENCE: how long a pause has to last, in seconds, before
  // the utterance is treated as finished and sent. Raise it if you are being
  // cut off while thinking mid-sentence; lower it for snappier turn-taking.
  float endpoint_silence = 1.0f;
  // M1f.1. AII_LISTEN_TIMEOUT: how long the *latched* microphone may go
  // without hearing a voice before it closes itself, in seconds. **0 (or any
  // value <= 0) means never**, which is the behaviour that predates M1f and
  // the one the user asked to keep available. Default 60.
  //
  // This is the single place the value lives today, and it is the seam M1f.2
  // writes into: the settings surface reads `settings.json`, clamps against
  // the user-facing floor it owns, and pushes the number down through
  // `VoiceSession::set_listen_timeout()` every frame the way mute and the
  // language selection are pushed. Nothing below this line should ever spell
  // "60" again.
  //
  // The floor enforced *here* is deliberately low (kListenTimeoutFloorSec in
  // voice_session.cpp, 1 s) and is not the user-facing one. It exists only so
  // that a harness can ask for a five-second timeout and measure the
  // mechanism without sitting through a real minute; the floor a person is
  // allowed to type belongs in M1f.2's control, where a refusal can be shown.
  float listen_timeout = 60.0f;
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
