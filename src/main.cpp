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
//        AII_EARLY_WORDS   (default 12; first chunk of a reply is spoken at a comma or after this many words, 0 = off)
//        AII_STT_LANG       (default auto)
//
//        AII_PROMPTS_DIR    prompt store location (default %APPDATA%\AIInterface\prompts)
//
// Modes: --say "<text>"    one typed turn through Claude, spoken, then exit.
//                          Repeatable: several --say run as consecutive turns
//                          in one session, which is the only scripted way to
//                          see a per-session behaviour such as M3.3's promise
//                          that a lazy prompt is injected exactly once.
//        --speak "<text>"  no Claude; speak the text and exit
//        --dump-system-prompt <file>
//                          write the composed system prompt and exit; nothing
//                          else starts. This is M3.2's regression test — diff
//                          the file against a known-good capture.

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
#include "core/config.h"
#include "core/engines.h"
#include "core/prompt_store.h"
#include "core/sentence_splitter.h"
#include "core/speech_queue.h"
#include "core/worker_pool.h"
#include "core/text_util.h"
#include "llm/llm_client.h"
#include "stt/recognizer.h"
#include "tts/kokoro_tts.h"
#include "tts/voicevox_tts.h"

using clk = std::chrono::steady_clock;

namespace {

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

}  // namespace

int main(int argc, char** argv) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);

  // Arguments come from the wide command line so Japanese survives (argv is ANSI-mangled).
  (void)argc; (void)argv;
  std::vector<std::string> say_texts;
  std::string speak_text, dump_prompt;
  {
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    for (int i = 1; wargv && i + 1 < wargc; ++i) {
      std::wstring a = wargv[i];
      if (a == L"--say") say_texts.push_back(utf8_from_wide(wargv[i + 1]));
      if (a == L"--speak") speak_text = utf8_from_wide(wargv[i + 1]);
      if (a == L"--dump-system-prompt") dump_prompt = utf8_from_wide(wargv[i + 1]);
    }
    if (wargv) LocalFree(wargv);
  }
  // M3.2's own regression test, and the only way to see the composed prompt
  // without reading it out of a running process. It writes the exact bytes
  // that become `--system-prompt`, opened in binary so nothing here turns an
  // LF into a CRLF — the file is meant to be diffed, and a diff that reports
  // every line as changed would be worse than no test at all.
  if (!dump_prompt.empty()) {
    const std::string& composed = aii::system_prompt();
    FILE* f = nullptr;
    if (fopen_s(&f, dump_prompt.c_str(), "wb") != 0 || !f) {
      std::fprintf(stderr, "cannot write %s\n", dump_prompt.c_str());
      return 1;
    }
    std::fwrite(composed.data(), 1, composed.size(), f);
    std::fclose(f);
    std::printf("wrote %zu bytes to %s\n", composed.size(), dump_prompt.c_str());
    return 0;
  }
  const bool scripted = !say_texts.empty() || !speak_text.empty();

  const aii::Config cfg = aii::Config::from_env();
  const int early_words = cfg.early_words;  // 0 = wait for full sentences
  std::printf("voiceloop  backend=%s effort=%s\n", cfg.backend.c_str(), cfg.effort.c_str());

  // ---- engines (Claude Code first: it takes a few seconds to initialise) ----
  aii::Engines eng;
  std::string err;
  auto logger = [](const std::string& s) { std::printf("  %s\n", s.c_str()); };
  if (speak_text.empty() && !aii::build_llm(cfg, eng, logger, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  if (!aii::build_speech(cfg, eng, logger, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::unique_ptr<aii::LlmClient>& llm = eng.llm;
  aii::Recognizer& stt = *eng.stt;
  aii::KokoroTts& kokoro = *eng.kokoro;
  aii::VoicevoxTts& voicevox = *eng.voicevox;

  aii::AudioOut speaker;
  if (!speaker.start(kokoro.sample_rate())) { std::fprintf(stderr, "no playback device\n"); return 1; }
  aii::MicIn mic;
  if (!mic.open(16000)) { std::fprintf(stderr, "no capture device\n"); return 1; }
  std::printf("  speaker: %s\n  mic:     %s\n", speaker.device_name().c_str(), mic.device_name().c_str());

  aii::SpeechQueue speech(&kokoro, &voicevox, &speaker);
  speech.set_on_status([](const std::string& s) { std::printf("\n  [tts] %s\n", s.c_str()); });

  if (!scripted) std::printf("\nSPACE talk/stop   T type   S silence   Q quit\n");

  // M3.3 / M3.4, the same lazy injection the window does — voiceloop is the
  // headless twin of this loop and the place a prompt change is checked
  // without a window on the user's desktop.
  aii::PromptStore prompts;
  aii::PromptInjector injector;
  if (speak_text.empty()) {
    std::string perr;
    if (!prompts.load(&perr) && !perr.empty()) std::fprintf(stderr, "[prompts] %s\n", perr.c_str());
    injector.reset(prompts);
  }

  // ---- one turn: text in, spoken reply out ----
  auto handle_turn = [&](const std::string& user_text, clk::time_point t_input_done) {
    clk::time_point t_first_token{}, t_first_audio{};
    bool got_token = false;
    speech.mark_new_reply();
    speech.set_on_first_audio([&] { t_first_audio = clk::now(); });

    aii::SentenceSplitter splitter([&](const std::string& s) { speech.enqueue(s); }, early_words);
    std::printf("\nClaude: ");
    std::fflush(stdout);
    auto t_send = clk::now();
    std::atomic<bool> cancel{false};
    const std::string sent = injector.decorate(user_text);
    if (sent.size() != user_text.size()) std::printf("[prompts] context injected\n");
    aii::ChatResult r = llm->turn(sent, [&](const std::string& delta) {
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
    // M3.4: the model may have asked for a prompt itself. It lands on the next
    // turn, not this one — see PromptInjector.
    for (const aii::Command& c : aii::parse_commands(r.text)) {
      if (c.verb != "load") continue;
      std::printf("[prompts] load name=%s -> %s\n", c.name.c_str(),
                  injector.request(c.name) ? "queued" : "refused (not in the store)");
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
    }, early_words);
    // Feed one UTF-8 code point at a time so the splitter behaves as it does with streamed deltas.
    for (size_t i = 0; i < speak_text.size();) {
      size_t n = 1;
      unsigned char b = (unsigned char)speak_text[i];
      if (b >= 0xF0) n = 4; else if (b >= 0xE0) n = 3; else if (b >= 0xC0) n = 2;
      splitter.feed(speak_text.substr(i, n));
      i += n;
    }
    splitter.flush();
    speech.wait_idle();
    std::printf("  first audio after %.2f s, finished after %.2f s\n",
                t_audio != clk::time_point{} ? secs(t_start, t_audio) : -1.0, secs(t_start, clk::now()));
    return 0;
  }

  if (!say_texts.empty()) {
    // Several `--say` arguments run as consecutive turns in one session, which
    // is the only scripted way to see anything that is *per session* rather
    // than per turn: M3.3's loaded set, whose whole claim is that a prompt is
    // injected on one turn and never again, is invisible to a run that can
    // only take one turn and then exits.
    for (const std::string& t : say_texts) {
      std::printf("\n[you] %s\n", t.c_str());
      handle_turn(t, clk::now());
      speech.wait_idle();
    }
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
