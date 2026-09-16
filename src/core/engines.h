#pragma once
// Builds the engines every front-end needs from a Config: the Claude backend,
// the recogniser and the two synthesis voices. Each builder reports one
// timing line per engine through `log` and an error message on failure.
#include <functional>
#include <memory>
#include <string>

#include "core/config.h"
#include "llm/llm_client.h"
#include "stt/recognizer.h"
#include "tts/kokoro_tts.h"
#include "tts/voicevox_tts.h"

namespace aii {

using LogFn = std::function<void(const std::string&)>;

struct Engines {
  std::unique_ptr<LlmClient> llm;
  std::unique_ptr<Recognizer> stt;
  std::unique_ptr<KokoroTts> kokoro;
  std::unique_ptr<VoicevoxTts> voicevox;
};

// Claude Code child process or the Messages API client, per cfg.backend.
bool build_llm(const Config& cfg, Engines& out, const LogFn& log, std::string* error);
// Recogniser + Kokoro + VOICEVOX (all CPU). Several seconds.
bool build_speech(const Config& cfg, Engines& out, const LogFn& log, std::string* error);

}  // namespace aii
