#include "core/engines.h"

#include <chrono>
#include <cstdio>

#include "core/prompt_store.h"
#include "core/tool_policy.h"
#include "llm/claude_client.h"
#include "llm/claude_code_client.h"

namespace aii {
namespace {

using clk = std::chrono::steady_clock;

std::string fmt_secs(clk::time_point since) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f s", std::chrono::duration<double>(clk::now() - since).count());
  return buf;
}

void say(const LogFn& log, const std::string& s) {
  if (log) log(s);
}

}  // namespace

bool build_llm(const Config& cfg, Engines& out, const LogFn& log, std::string* error) {
  auto t = clk::now();
  if (cfg.backend == "api") {
    if (cfg.api_key.empty()) {
      if (error) *error = "AII_BACKEND=api needs ANTHROPIC_API_KEY";
      return false;
    }
    const std::string model = cfg.model_override.empty() ? "claude-opus-5" : cfg.model_override;
    out.llm = std::make_unique<ApiLlmClient>(cfg.api_key, model, cfg.effort, system_prompt());
    say(log, "api backend  model=" + model);
    return true;
  }
  ClaudeCodeClient::Options o;
  o.exe = cfg.claude_exe;
  o.system_prompt = system_prompt();
  o.model = cfg.model_override;
  o.effort = cfg.effort;
  // M3.7, and M3.8 which put it behind a control. What the instance the user
  // talks to can do for itself, named rather than filtered: `--tools` with an
  // explicit list is the CLI's own allowlist, so nothing is opted out of — it
  // is opted in to, one group at a time, and the groups are a table in
  // `core/tool_policy.h`. `ClaudeCodeClient::start` turns this list into the
  // permission flags that must travel with it; see the comment there for why
  // it cannot be set without them.
  //
  // An empty list is a legal and reachable state — every toggle off — and
  // means `--tools ""`, which is what this app ran on before M3.7.
  const std::string tools = tool_list(cfg.tools);
  o.tools = tools;
  // The conversational instance runs on this app's prompts and nothing the CLI
  // found for itself (M3.5). Workers deliberately keep everything — see
  // WorkerPool::spawn, which does not set this. With tools on this also keeps
  // the user's MCP servers out, which `--tools` by itself does not do.
  o.suppress_cli_context = true;
  auto cc = std::make_unique<ClaudeCodeClient>(o);
  std::string err;
  if (!cc->start(&err)) {
    if (error) *error = "claude code backend failed: " + err;
    return false;
  }
  say(log, "claude code launched  " + fmt_secs(t) + "  (" +
               (cfg.model_override.empty() ? "default model" : cfg.model_override) + ", tools: " +
               (tools.empty() ? std::string("none") : tools) + ")");
  out.llm = std::move(cc);
  return true;
}

std::string stt_language(const Config& cfg) {
  // The hand override first, the derived value otherwise. See Config::stt_lang.
  return cfg.stt_lang.empty() ? std::string(stt_language_for(cfg.langs)) : cfg.stt_lang;
}

bool build_stt(const Config& cfg, Engines& out, const LogFn& log, std::string* error) {
  auto t = clk::now();
  out.stt = std::make_unique<Recognizer>(cfg.stt_dir, 8, cfg.endpoint_silence);
  if (!out.stt->ok()) {
    if (error) *error = "recogniser failed to load from " + cfg.stt_dir;
    return false;
  }
  const std::string lang = stt_language(cfg);
  out.stt->set_language(lang);
  say(log, "recogniser ready      " + fmt_secs(t) + "  (language " + lang + ")");
  return true;
}

bool build_kokoro(const Config& cfg, Engines& out, const LogFn& log, std::string* error) {
  auto t = clk::now();
  out.kokoro = std::make_unique<KokoroTts>(cfg.kokoro_dir, cfg.kokoro_sid, 1.0f, 4);
  if (!out.kokoro->ok()) {
    if (error) *error = "kokoro failed to load from " + cfg.kokoro_dir;
    return false;
  }
  say(log, "kokoro ready          " + fmt_secs(t) + "  (" + std::to_string(out.kokoro->sample_rate()) +
               " Hz, sid " + std::to_string(cfg.kokoro_sid) + ")");
  return true;
}

bool build_voicevox(const Config& cfg, Engines& out, const LogFn& log, std::string* error) {
  auto t = clk::now();
  out.voicevox = std::make_unique<VoicevoxTts>(cfg.vv_core_dir, cfg.vv_models_dir, cfg.vv_style);
  if (!out.voicevox->ok()) {
    if (error) *error = "voicevox failed: " + out.voicevox->last_error();
    return false;
  }
  say(log, "voicevox ready        " + fmt_secs(t) + "  (style " + std::to_string(cfg.vv_style) + ")");
  return true;
}

bool build_speech(const Config& cfg, Engines& out, const LogFn& log, std::string* error) {
  if (!build_stt(cfg, out, log, error) || !build_kokoro(cfg, out, log, error)) return false;
  // M8.3. Kokoro is unconditional: it is the voice for every Latin run the
  // script splitter emits, which a Japanese reply has too (names, code words),
  // and it is SpeechQueue's fallback when the other engine is missing. So only
  // VOICEVOX is skippable, and skipping it saves the second it takes to load.
  // `out.voicevox` stays null, which SpeechQueue already handles — and which
  // VoiceSession fills in later if the user switches Japanese back on.
  if (!cfg.langs.japanese) {
    say(log, "japanese voice        skipped (Japanese is off in settings)");
    return true;
  }
  return build_voicevox(cfg, out, log, error);
}

}  // namespace aii
