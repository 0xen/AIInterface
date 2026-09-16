// voiceloop: push-to-talk -> local speech recognition -> Claude (streaming) ->
// local speech synthesis -> speakers. All audio stays in memory.
//
// Keys:  SPACE  start / stop listening (stopping sends what you said)
//        T      type a message instead of speaking
//        S      stop the assistant speaking
//        Q      quit
//
// Env:   AII_BACKEND        code (default, uses the Claude Code subscription) | api
//        ANTHROPIC_API_KEY  required only for AII_BACKEND=api
//        AII_CLAUDE_EXE     path to claude.exe (default %USERPROFILE%\.local\bin\claude.exe)
//        AII_MODEL          model override (default: backend default; api backend uses claude-opus-5)
//        AII_EFFORT         (default low)
//        AII_KOKORO_SID     (default 3 = af_heart)
//        AII_VOICEVOX_STYLE (default 2 = 四国めたん normal)
//        AII_STT_LANG       (default auto)
//
// Modes: --say "<text>"    one typed turn through Claude, spoken, then exit
//        --speak "<text>"  no Claude; speak the text and exit

#include <conio.h>
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_out.h"
#include "audio/mic_in.h"
#include "core/sentence_splitter.h"
#include "core/speech_queue.h"
#include "core/text_util.h"
#include "llm/claude_client.h"
#include "llm/claude_code_client.h"
#include "llm/llm_client.h"
#include "stt/recognizer.h"
#include "tts/kokoro_tts.h"
#include "tts/voicevox_tts.h"

using clk = std::chrono::steady_clock;

namespace {

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

double secs(clk::time_point a, clk::time_point b) { return std::chrono::duration<double>(b - a).count(); }

std::string utf8_from_wide(const wchar_t* w, int n = -1) {
  int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string s(n < 0 ? len - 1 : len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
  return s;
}

std::string read_console_line_utf8() {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  wchar_t wbuf[2048];
  DWORD n = 0;
  if (!ReadConsoleW(in, wbuf, 2047, &n, nullptr)) return {};
  while (n > 0 && (wbuf[n - 1] == L'\n' || wbuf[n - 1] == L'\r')) --n;
  return utf8_from_wide(wbuf, (int)n);
}

const char* kSystemPrompt =
    "You are a voice assistant running on the user's Windows PC. The user speaks English (primary) and Japanese. "
    "Always reply in the language the user just used; if they mix languages, use the dominant one. "
    "Your reply is read aloud by a text-to-speech engine, so write plain spoken prose: short sentences, "
    "one to three sentences unless the user asks for detail, no markdown, no lists, no headings, no emoji, no URLs. "
    "Write Japanese in normal Japanese script, never romaji. "
    "If something must be shown rather than spoken (code, a table, a long quote), put it inside a ``` fence and say "
    "briefly that it is shown on screen; text inside fences is displayed but not spoken.";

}  // namespace

int main(int argc, char** argv) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  // Arguments come from the wide command line so Japanese survives (argv is ANSI-mangled).
  (void)argc; (void)argv;
  std::string say_text, speak_text;
  {
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 1; wargv && i + 1 < wargc; ++i) {
      std::wstring a = wargv[i];
      if (a == L"--say") say_text = utf8_from_wide(wargv[i + 1]);
      if (a == L"--speak") speak_text = utf8_from_wide(wargv[i + 1]);
    }
    if (wargv) LocalFree(wargv);
  }
  const bool scripted = !say_text.empty() || !speak_text.empty();

  const std::string backend = env_or("AII_BACKEND", "code");
  const std::string effort = env_or("AII_EFFORT", "low");
  const std::string model_override = env_or("AII_MODEL", "");
  const int kokoro_sid = std::atoi(env_or("AII_KOKORO_SID", "3").c_str());
  const unsigned vv_style = (unsigned)std::atoi(env_or("AII_VOICEVOX_STYLE", "2").c_str());
  const std::string stt_lang = env_or("AII_STT_LANG", "auto");

  const std::string models = AII_MODELS_DIR;
  const std::string stt_dir = models + "/sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-560ms-int8-2026-06-11";
  const std::string kokoro_dir = models + "/kokoro-multi-lang-v1_0";
  const std::string vv_models = models + "/voicevox";
  const std::string vv_core = AII_VV_CORE_DIR;

  std::printf("voiceloop  backend=%s effort=%s\n", backend.c_str(), effort.c_str());

  // ---- LLM backend (started first: Claude Code takes a few seconds to initialise) ----
  std::unique_ptr<aii::LlmClient> llm;
  auto t_llm = clk::now();
  if (speak_text.empty()) {
    if (backend == "api") {
      const std::string api_key = env_or("ANTHROPIC_API_KEY", "");
      if (api_key.empty()) { std::fprintf(stderr, "AII_BACKEND=api needs ANTHROPIC_API_KEY\n"); return 1; }
      llm = std::make_unique<aii::ApiLlmClient>(api_key, model_override.empty() ? "claude-opus-5" : model_override,
                                                effort, kSystemPrompt);
      std::printf("  api backend           model=%s\n", model_override.empty() ? "claude-opus-5" : model_override.c_str());
    } else {
      aii::ClaudeCodeClient::Options o;
      o.exe = env_or("AII_CLAUDE_EXE", env_or("USERPROFILE", "C:\\Users\\Default") + "\\.local\\bin\\claude.exe");
      o.system_prompt = kSystemPrompt;
      o.model = model_override;
      o.effort = effort;
      o.tools = false;
      auto cc = std::make_unique<aii::ClaudeCodeClient>(o);
      std::string err;
      if (!cc->start(&err)) { std::fprintf(stderr, "claude code backend failed: %s\n", err.c_str()); return 1; }
      std::printf("  claude code launched  %.2f s  (%s)\n", secs(t_llm, clk::now()),
                  model_override.empty() ? "default model" : model_override.c_str());
      llm = std::move(cc);
    }
  }

  auto t0 = clk::now();
  aii::Recognizer stt(stt_dir, 8);
  if (!stt.ok()) { std::fprintf(stderr, "recogniser failed to load from %s\n", stt_dir.c_str()); return 1; }
  stt.set_language(stt_lang);
  std::printf("  recogniser ready      %.2f s\n", secs(t0, clk::now()));

  auto t1 = clk::now();
  aii::KokoroTts kokoro(kokoro_dir, kokoro_sid, 1.0f, 4);
  if (!kokoro.ok()) { std::fprintf(stderr, "kokoro failed to load from %s\n", kokoro_dir.c_str()); return 1; }
  std::printf("  kokoro ready          %.2f s  (%d Hz, sid %d)\n", secs(t1, clk::now()), kokoro.sample_rate(),
              kokoro_sid);

  auto t2 = clk::now();
  aii::VoicevoxTts voicevox(vv_core, vv_models, vv_style);
  if (!voicevox.ok()) { std::fprintf(stderr, "voicevox failed: %s\n", voicevox.last_error().c_str()); return 1; }
  std::printf("  voicevox ready        %.2f s  (style %u)\n", secs(t2, clk::now()), vv_style);

  aii::AudioOut speaker;
  if (!speaker.start(kokoro.sample_rate())) { std::fprintf(stderr, "no playback device\n"); return 1; }
  aii::MicIn mic;
  if (!mic.open(16000)) { std::fprintf(stderr, "no capture device\n"); return 1; }
  std::printf("  speaker: %s\n  mic:     %s\n", speaker.device_name().c_str(), mic.device_name().c_str());

  aii::SpeechQueue speech(&kokoro, &voicevox, &speaker);
  speech.set_on_status([](const std::string& s) { std::printf("\n  [tts] %s\n", s.c_str()); });

  if (!scripted) std::printf("\nSPACE talk/stop   T type   S silence   Q quit\n");

  // ---- one turn: text in, spoken reply out ----
  auto handle_turn = [&](const std::string& user_text, clk::time_point t_input_done) {
    clk::time_point t_first_token{}, t_first_audio{};
    bool got_token = false;
    speech.mark_new_reply();
    speech.set_on_first_audio([&] { t_first_audio = clk::now(); });

    aii::SentenceSplitter splitter([&](const std::string& s) { speech.enqueue(s); });
    std::printf("\nClaude: ");
    std::fflush(stdout);
    auto t_send = clk::now();
    std::atomic<bool> cancel{false};
    aii::ChatResult r = llm->turn(user_text, [&](const std::string& delta) {
      if (!got_token) { got_token = true; t_first_token = clk::now(); }
      std::fputs(delta.c_str(), stdout);
      std::fflush(stdout);
      splitter.feed(delta);
    }, &cancel);
    auto t_stream_end = clk::now();
    splitter.flush();
    std::printf("\n");

    if (!r.ok) {
      std::printf("  [error] %s\n", r.error.c_str());
      return;
    }

    // Let the reply finish playing, but allow S / SPACE to cut it off.
    while (!speech.idle()) {
      if (!scripted && _kbhit()) {
        int c = _getch();
        if (c == 's' || c == 'S' || c == ' ') { speech.clear(); break; }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    auto t_done = clk::now();

    std::printf("  [timing] input->first token %.2f s | first token->first audio %.2f s | "
                "input->first audio %.2f s | stream %.2f s | total %.2f s | tokens in %d (cached %d) out %d | stop %s",
                got_token ? secs(t_input_done, t_first_token) : -1.0,
                (got_token && t_first_audio != clk::time_point{}) ? secs(t_first_token, t_first_audio) : -1.0,
                t_first_audio != clk::time_point{} ? secs(t_input_done, t_first_audio) : -1.0,
                secs(t_send, t_stream_end), secs(t_input_done, t_done), r.input_tokens, r.cache_read_tokens,
                r.output_tokens, r.stop_reason.c_str());
    if (r.cost_usd >= 0) std::printf(" | est $%.4f", r.cost_usd);
    std::printf("\n");
    std::string status = llm->status_line();
    if (!status.empty()) std::printf("  [%s]\n", status.c_str());
  };

  if (!speak_text.empty()) {
    auto t_start = clk::now();
    clk::time_point t_audio{};
    speech.mark_new_reply();
    speech.set_on_first_audio([&] { t_audio = clk::now(); });
    aii::SentenceSplitter splitter([&](const std::string& s) {
      std::printf("  [sentence:%s] %s\n", aii::has_japanese(s) ? "ja" : "en", s.c_str());
      speech.enqueue(s);
    });
    splitter.feed(speak_text);
    splitter.flush();
    speech.wait_idle();
    std::printf("  first audio after %.2f s, finished after %.2f s\n",
                t_audio != clk::time_point{} ? secs(t_start, t_audio) : -1.0, secs(t_start, clk::now()));
    return 0;
  }

  if (!say_text.empty()) {
    std::printf("\n[you] %s\n", say_text.c_str());
    handle_turn(say_text, clk::now());
    speech.wait_idle();
    return 0;
  }

  std::vector<float> chunk;
  for (;;) {
    std::printf("\n> ");
    std::fflush(stdout);
    int c = _getch();
    if (c == 'q' || c == 'Q' || c == EOF || c == 0x1A || c == 0x03) break;  // q, EOF, Ctrl-Z, Ctrl-C
    if (c == 's' || c == 'S') { speech.clear(); continue; }

    if (c == 't' || c == 'T') {
      std::printf("type: ");
      std::fflush(stdout);
      std::string text = aii::trim(read_console_line_utf8());
      if (text.empty()) continue;
      handle_turn(text, clk::now());
      continue;
    }

    if (c != ' ') continue;

    // ---- listening ----
    speech.clear();  // barge-in: stop any reply in progress
    stt.begin();
    if (!mic.start()) { std::printf("mic failed to start\n"); continue; }
    std::printf("[listening] ");
    std::fflush(stdout);
    std::string last_partial;
    bool quit = false;
    for (;;) {
      if (_kbhit()) {
        int k = _getch();
        if (k == ' ' || k == '\r' || k == '\n') break;
        if (k == 'q' || k == 'Q') { quit = true; break; }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      chunk.clear();
      mic.drain(chunk);
      if (chunk.empty()) continue;
      stt.feed(chunk.data(), (int)chunk.size(), 16000);
      std::string p = stt.partial();
      if (p != last_partial) {
        last_partial = p;
        std::printf("\r[listening] %s", p.c_str());
        std::fflush(stdout);
      }
    }
    mic.stop();
    if (quit) break;
    auto t_stop = clk::now();
    chunk.clear();
    mic.drain(chunk);
    if (!chunk.empty()) stt.feed(chunk.data(), (int)chunk.size(), 16000);
    std::string text = aii::trim(stt.finish());
    auto t_final = clk::now();
    std::printf("\r[you] %s   (final text %.2f s after stop)\n", text.c_str(), secs(t_stop, t_final));
    if (text.empty()) continue;
    handle_turn(text, t_final);
  }
  speech.clear();
  std::printf("bye\n");
  return 0;
}
