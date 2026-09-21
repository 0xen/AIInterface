// failure_reason_test: the client's jargon, mapped to something a person can
// hear, in both languages.
//
// What this stands between: a worker that fails says its reason out loud, and
// before this the reason was the CLI's own string -- "claude process exited",
// "CreateProcess failed (2): claude --dangerously-skip-permissions ...". The
// app's own pre-prompt asks Claude never to say a process name or an error
// code; the app was saying them itself, in a sentence no prompt can reach
// because the worker never spoke. So the mapping is the fix and this is the
// test of it.
//
// Two properties, and they are the whole of it:
//   1. every error string a client can actually produce maps to a plain
//      sentence, in English *and* in Japanese, with no jargon left in it;
//   2. an error string nobody has mapped yet degrades to `FailUnclear` -- a
//      sentence that admits it cannot explain and says where the reason is --
//      rather than falling through to the raw text.
//
// Same shape as the other tests here: a plain main, printf, non-zero exit.
#include <cstdio>
#include <string>

#include "core/app_strings.h"

using namespace aii;

static int failures = 0;

static void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

// The words a spoken line must never contain, whatever the error was. Lower
// case; the check lower-cases the sentence first.
static const char* kJargon[] = {"claude", "process", "exit", "createprocess", "createpipe",
                                "stdin",  "winhttp", "http", "error",         "--",
                                "0x",     "failed ("};

static std::string lower(const std::string& s) {
  std::string out;
  for (char c : s) out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  return out;
}

// One row of the mapping, as it is actually reached: the raw string a client
// produces, and the key it must resolve to.
struct Case {
  const char* raw;
  Msg want;
  const char* where;  // which file it comes from, for the printout
};

int main() {
  const Case cases[] = {
      // --- core/worker_pool.cpp ---
      {"a worker named scout is already running", Msg::FailAlreadyRunning, "worker_pool"},
      // --- llm/claude_code_client.cpp ---
      {"CreatePipe failed", Msg::FailCouldNotStart, "code_client"},
      {"CreateProcess failed (2): claude --print --output-format stream-json",
       Msg::FailCouldNotStart, "code_client"},
      {"claude exited during startup: ", Msg::FailCouldNotStart, "code_client"},
      {"claude process exited: ", Msg::FailStopped, "code_client"},
      {"claude process exited: Error: connect ECONNREFUSED", Msg::FailStopped, "code_client"},
      {"claude process is not running: ", Msg::FailNeverStarted, "code_client"},
      {"failed to write to claude stdin", Msg::FailCouldNotSend, "code_client"},
      // The CLI's own `is_error` result text. Free prose from the other side,
      // so most of it is unmapped on purpose -- but the limit line is common
      // enough, and actionable enough, to be worth its own sentence.
      {"Claude AI usage limit reached|1731369600", Msg::FailAtItsLimit, "code_client result"},
      // --- llm/claude_client.cpp ---
      {"refusal: violence - the model declined", Msg::FailWouldNotDo, "claude_client"},
      {"WinHttpOpen failed", Msg::FailCouldNotReach, "claude_client"},
      {"WinHttpSendRequest failed (12029)", Msg::FailCouldNotReach, "claude_client"},
      {"WinHttpReadData failed (12002)", Msg::FailCouldNotReach, "claude_client"},
      {"HTTP 500 internal", Msg::FailCouldNotReach, "claude_client"},
      {"HTTP 429 rate_limit_error", Msg::FailAtItsLimit, "claude_client"},
      {"cancelled", Msg::FailStoppedByUs, "claude_client"},
      // --- unmapped, which is the case that has to degrade ---
      {"Error: ENOSPC: no space left on device, write", Msg::FailUnclear, "unknown"},
      {"", Msg::FailUnclear, "unknown"},
      {"\xe4\xbd\x95\xe3\x81\x8b", Msg::FailUnclear, "unknown"},  // 何か
      // --- M20.4: free prose that used to be classified by one word in it ---
      //
      // All of these are result text the CLI can hand back through
      // `is_error`, which is prose written by the other side and not a string
      // this app built. Before M20.4 the first of them was spoken as "I
      // stopped it before it finished", which is a sentence about something
      // the app did and is a lie about every one of these.
      {"the user cancelled the subscription", Msg::FailUnclear, "cli result"},
      {"I cancelled the booking as you asked", Msg::FailUnclear, "cli result"},
      {"could not connect the two arguments in the draft", Msg::FailUnclear, "cli result"},
      {"the file has 429 lines and the last one is blank", Msg::FailUnclear, "cli result"},
      {"the report exited the building at noon", Msg::FailUnclear, "cli result"},
      // Still classified, because these are the phrases that mean what they
      // say wherever they appear -- the narrow half of the old rule, kept.
      {"Claude AI usage limit reached|1731369600", Msg::FailAtItsLimit, "cli result"},
      {"claude process exited: Claude AI usage limit reached|1731369600", Msg::FailAtItsLimit,
       "code_client"},
      // --- M20.4: the app's own strings still match, anchored ---
      {"a worker named cancel is already running", Msg::FailAlreadyRunning, "worker_pool"},
      {"HTTP 529 overloaded_error", Msg::FailAtItsLimit, "claude_client"},
      {"WinHttpQueryDataAvailable failed (12002)", Msg::FailCouldNotReach, "claude_client"},
  };

  std::printf("-- every client error string maps to a plain sentence --\n");
  bool all_mapped = true, all_ja = true, all_clean = true;
  for (const Case& c : cases) {
    const Msg got = failure_reason(c.raw);
    const std::string en = app_text_in(AppLang::English, got);
    const std::string ja = app_text_in(AppLang::Japanese, got);
    std::printf("     [%-18s] %-58.58s\n                        EN  %s\n                        JA  %s\n",
                c.where, c.raw[0] ? c.raw : "(empty)", en.c_str(), ja.c_str());
    if (got != c.want) {
      all_mapped = false;
      std::printf("     ^ wrong key: wanted %d, got %d\n", static_cast<int>(c.want),
                  static_cast<int>(got));
    }
    // Japanese must be *Japanese*, not the English fallen back to: this table
    // exists so the user is not handed English at the moment something breaks.
    if (ja == en) {
      all_ja = false;
      std::printf("     ^ Japanese is still the English line\n");
    }
    const std::string le = lower(en);
    for (const char* j : kJargon) {
      if (le.find(j) != std::string::npos) {
        all_clean = false;
        std::printf("     ^ the spoken English still contains \"%s\"\n", j);
      }
    }
  }
  check(all_mapped, "every known error string maps to the sentence it should");
  check(all_ja, "every one of those sentences is translated, not fallen back to English");
  check(all_clean, "no process name, call name or exit code survives into what is spoken");

  std::printf("\n-- what the user actually hears --\n");
  const std::string raw = "claude process exited: ";
  for (const AppLang lang : {AppLang::English, AppLang::Japanese}) {
    const std::string spoken =
        app_text_in(lang, Msg::TaskFailedSpoken, app_text_in(lang, failure_reason(raw)));
    std::printf("     %s\n", spoken.c_str());
    check(lower(spoken).find("claude") == std::string::npos &&
              lower(spoken).find("process") == std::string::npos,
          std::string(lang == AppLang::English ? "EN" : "JA") +
              ": the whole failure line names no process");
  }

  // The degradation rule, stated as its own assertion rather than left implied
  // by one row above: unknown must be a *different* sentence from any known
  // one, so that "I cannot say why" is never what a mapped failure says.
  const std::string unclear = app_text_in(AppLang::English, Msg::FailUnclear);
  bool distinct = true;
  for (int m = static_cast<int>(Msg::FailAlreadyRunning); m < static_cast<int>(Msg::FailUnclear);
       ++m)
    if (app_text_in(AppLang::English, static_cast<Msg>(m)) == unclear) distinct = false;
  check(distinct, "the unknown-error line is nobody else's line");
  check(unclear.find("on screen") != std::string::npos,
        "the unknown-error line points at where the real reason is, instead of hiding it");

  // M20.4, finding 22, as its own assertion because it is the one this rule
  // was rewritten for: the word "cancel" inside somebody else's sentence is
  // not this app having stopped a turn.
  std::printf("\n-- a word inside free prose is not a diagnosis --\n");
  const std::string stopped_by_us = app_text_in(AppLang::English, Msg::FailStoppedByUs);
  const std::string prose = "the user cancelled the subscription";
  const std::string said = app_text_in(AppLang::English, failure_reason(prose));
  std::printf("     %-40s -> %s\n", prose.c_str(), said.c_str());
  check(said != stopped_by_us, "\"the user cancelled the subscription\" is not read as \"stopped\"");
  check(failure_reason("cancelled") == Msg::FailStoppedByUs,
        "the backend's own \"cancelled\", written whole and alone, still is");

  std::printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
