#pragma once
// Runtime configuration shared by every executable: environment variables and
// the model locations baked in by CMake.
//
// The system prompt used to be a `kSystemPrompt` literal here. It is now a
// seeded, composed prompt tree — see `core/prompt_store.h` and `aii::system_prompt()`.
#include <string>

#include "core/language.h"
#include "core/tool_policy.h"

namespace aii {

// Environment variable or a default (empty values count as unset).
std::string env_or(const char* name, const std::string& def);

struct Config {
  std::string backend;         // AII_BACKEND: "code" (Claude Code CLI) or "api"
  std::string effort;          // AII_EFFORT
  // M3.11. Which model the *conversational* instance runs on, as the string
  // that goes on `claude --model` — a CLI alias ("opus", "sonnet", "haiku"),
  // or **empty**, which means the flag is not passed at all and the CLI picks.
  // Empty is the default and has to stay reachable.
  //
  // `AII_MODEL` seeds it; the avatar reads `settings.json` over that before
  // the session is built (main.cpp), through the table in
  // `core/model_choice.h` — which is also what keeps a stale or mistyped
  // stored value from reaching the command line, since a model string the CLI
  // rejects means a child that starts and then fails every turn.
  //
  // Like `tools` it cannot be pushed down as a level: `--model` is fixed when
  // the child starts, so a change reaches Claude at the next launch and the
  // settings surface says so rather than appearing to do nothing.
  //
  // **Workers never read this.** `WorkerPool::spawn` sets no model at all and
  // keeps the CLI's default whatever is picked here.
  std::string model_override;
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
  // M3.15. AII_HANDOFF_AT: how full the context window may get before the
  // session hands over to a fresh one, as a **fraction of that model's own
  // window** — the number `UsageStats::ctx` reports and the footer draws.
  // Default 0.40, which is the user's own ("say, for example, around 40%").
  // **0 or less is off**, the spelling `listen_timeout` already uses; a value
  // above 1 is read as a percentage, so a hand-edited `40` means the same
  // thing. See `core/handoff_policy.h` for the whole of the rule, including
  // why 40% of a `[1m]` model is deliberately five times the tokens 40% of a
  // 200k one is.
  //
  // It sits in Config beside `listen_timeout` for the same reason and with the
  // same ownership: main.cpp reads `settings.json` over it before the session
  // is built, and unlike `tools` and `model_override` it is not fixed at the
  // child's launch — nothing about it reaches the command line, so it could be
  // pushed down as a level later if a control is ever drawn for it.
  float handoff_threshold = 0.40f;
  // M3.8. Which tool groups the *conversational* instance gets. The default is
  // the table's (core/tool_policy.h); main.cpp reads settings.json over it
  // before the session is built, and `build_llm` turns it into `--tools`.
  //
  // It sits in Config for the same reason `listen_timeout` does — the session
  // is constructed from Config — but unlike that one it cannot be pushed down
  // as a level afterwards: `--allowedTools` is fixed when the child process
  // starts, so a change here reaches Claude at the next launch and not before.
  // The settings surface says so in as many words rather than appearing to do
  // nothing, which is the failure M1f.2 spent a paragraph avoiding.
  //
  // **Workers never read this.** `WorkerPool::spawn` passes "default" and
  // keeps every built-in tool whatever is ticked here.
  ToolPolicy tools;

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
