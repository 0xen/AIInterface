#include "core/engines.h"

#include <chrono>
#include <cstdio>

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
    out.llm = std::make_unique<ApiLlmClient>(cfg.api_key, model, cfg.effort, kSystemPrompt);
    say(log, "api backend  model=" + model);
    return true;
  }
  ClaudeCodeClient::Options o;
  o.exe = cfg.claude_exe;
  o.system_prompt = kSystemPrompt;
  o.model = cfg.model_override;
  o.effort = cfg.effort;
  o.tools = false;
  auto cc = std::make_unique<ClaudeCodeClient>(o);
  std::string err;
  if (!cc->start(&err)) {
    if (error) *error = "claude code backend failed: " + err;
    return false;
  }
  say(log, "claude code launched  " + fmt_secs(t) + "  (" +
               (cfg.model_override.empty() ? "default model" : cfg.model_override) + ")");
  out.llm = std::move(cc);
  return true;
}

bool build_speech(const Config& cfg, Engines& out, const LogFn& log, std::string* error) {
  auto t0 = clk::now();
  out.stt = std::make_unique<Recognizer>(cfg.stt_dir, 8);
  if (!out.stt->ok()) {
    if (error) *error = "recogniser failed to load from " + cfg.stt_dir;
    return false;
  }
  out.stt->set_language(cfg.stt_lang);
  say(log, "recogniser ready      " + fmt_secs(t0));

  auto t1 = clk::now();
  out.kokoro = std::make_unique<KokoroTts>(cfg.kokoro_dir, cfg.kokoro_sid, 1.0f, 4);
  if (!out.kokoro->ok()) {
    if (error) *error = "kokoro failed to load from " + cfg.kokoro_dir;
    return false;
  }
  say(log, "kokoro ready          " + fmt_secs(t1) + "  (" + std::to_string(out.kokoro->sample_rate()) +
               " Hz, sid " + std::to_string(cfg.kokoro_sid) + ")");

  auto t2 = clk::now();
  out.voicevox = std::make_unique<VoicevoxTts>(cfg.vv_core_dir, cfg.vv_models_dir, cfg.vv_style);
  if (!out.voicevox->ok()) {
    if (error) *error = "voicevox failed: " + out.voicevox->last_error();
    return false;
  }
  say(log, "voicevox ready        " + fmt_secs(t2) + "  (style " + std::to_string(cfg.vv_style) + ")");
  return true;
}

}  // namespace aii
