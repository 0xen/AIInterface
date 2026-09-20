#include "core/config.h"

#include <cstdio>
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
  // M1f.1. Parsed, not clamped, here: a negative or zero value is "never" and
  // is a legal answer rather than a mistake to be repaired. The lower clamp on
  // the positive side lives in VoiceSession, next to the constant that names
  // it. An unparseable string reads as 0 through atof and therefore disables
  // the timeout, which is the safe direction to fail — the microphone then
  // behaves exactly as it did before M1f.
  c.listen_timeout = (float)std::atof(env_or("AII_LISTEN_TIMEOUT", "60").c_str());
  // M12.2. Seeds the wake phrase. The avatar reads `settings.json`'s
  // `wake.phrase` over this before the session is built and then pushes it down
  // as a level every frame, exactly as it does the listen timeout. **Empty
  // means off and empty is the default**, which is the same spelling
  // `listen_timeout`'s 0 uses. See `core/wake_word.h` for the matching rule and
  // for the minimum length a phrase has to clear to be armed at all.
  c.wake_phrase = env_or("AII_WAKE_PHRASE", "");
  // M3.15. Parsed and not clamped, exactly as the timeout above is and for the
  // same reason: 0 is "never hand over" and is a legal answer, and an
  // unparseable string reads as 0 through atof, which switches the feature off
  // rather than firing it at a number nobody meant. `handoff_due()` owns the
  // rest of the reading, including "40 means 40 per cent".
  c.handoff_threshold = (float)std::atof(env_or("AII_HANDOFF_AT", "0.40").c_str());
  c.worker_bypass = env_or("AII_WORKER_BYPASS", "1") != "0";
  // M3.9. The window reads the tool policy out of settings.json, where the
  // tick boxes write it; voiceloop has no settings.json, so this is how the
  // headless twin is run in a configuration other than the table's default —
  // and it is what `--dump-system-prompt` is driven by, since the prompt now
  // describes the grant. A comma-separated list of group keys, `none` for an
  // empty grant, unset for the defaults:
  //
  //     AII_TOOLS=web            AII_TOOLS=web,file_read            AII_TOOLS=none
  //
  // A key that is in the list but not offered stays off: `tool_group_active`
  // has the last word here as everywhere else, so this cannot grant what the
  // greyed-out row refuses.
  if (const std::string spec = env_or("AII_TOOLS", ""); !spec.empty()) {
    for (int i = 0; i < kToolGroupCount; ++i) c.tools.on[i] = false;
    std::string key;
    const auto take = [&] {
      if (key.empty() || key == "none") return;
      bool found = false;
      for (int i = 0; i < kToolGroupCount; ++i)
        if (key == tool_group(i).key) { c.tools.on[i] = true; found = true; }
      if (!found) std::fprintf(stderr, "[tools] AII_TOOLS: no tool group called `%s`\n", key.c_str());
    };
    for (const char ch : spec) {
      if (ch == ',' || ch == ' ') { take(); key.clear(); } else key += ch;
    }
    take();
  }

  c.models_dir = AII_MODELS_DIR;
  c.stt_dir = c.models_dir + "/sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-560ms-int8-2026-06-11";
  c.kokoro_dir = c.models_dir + "/kokoro-multi-lang-v1_0";
  c.vv_models_dir = c.models_dir + "/voicevox";
  c.vv_core_dir = AII_VV_CORE_DIR;
  return c;
}

}  // namespace aii
