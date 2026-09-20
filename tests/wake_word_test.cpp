// M12: the two rules that decide when the microphone is open.
//
// Both halves of M12 come down to a pure decision that is easy to get wrong
// and impossible to reproduce reliably by hand -- one needs a person saying a
// word into a real microphone, the other needs three background workers
// finishing in a particular order while somebody changes their mind halfway.
// So both were written as pure code (`core/wake_word.h`, `core/listen_restore.h`)
// and both are checked here, with no engines, no window and no microphone.
//
// **The negative cases are the point.** A wake word that fires on a near-miss
// costs the user their privacy; a worker that reopens a microphone the user
// shut on purpose is the app arguing with the person using it. Every positive
// case below has its negative twin.
//
//   wake_word_test          prints every case and exits non-zero on failure
#include <cstdio>
#include <string>

#include "core/listen_restore.h"
#include "core/wake_word.h"

using aii::ListenRestore;
using aii::normalise_wake;
using aii::wake_chars;
using aii::wake_match;
using aii::wake_phrase_armed;
using aii::wake_phrase_problem;

namespace {

int g_failures = 0;

void ok(const char* what, bool cond) {
  if (!cond) ++g_failures;
  std::printf("  %-64s %s\n", what, cond ? "ok  " : "FAIL");
}

// `heard` is what the recogniser produced; `phrase` is what the user typed.
void match(const char* what, const std::string& heard, const std::string& phrase, bool want) {
  const bool got = wake_match(heard, phrase);
  if (got != want) ++g_failures;
  std::printf("  %-64s %s  heard=\"%s\" -> \"%s\"\n", what, got == want ? "ok  " : "FAIL",
              heard.c_str(), normalise_wake(heard).c_str());
}

// Japanese, as UTF-8 escapes so that the file's own encoding cannot be what
// this test is really measuring -- the same care app_strings_test takes.
const std::string kAria = "\xE3\x82\xA2\xE3\x83\xAA\xE3\x82\xA2";                  // アリア
const std::string kAriaSentence =
    "\xE3\x82\xA2\xE3\x83\xAA\xE3\x82\xA2\xE3\x80\x81"                            // アリア、
    "\xE4\xBB\x8A\xE6\x97\xA5\xE3\x81\xAE\xE5\xA4\xA9\xE6\xB0\x97\xE3\x81\xAF";   // 今日の天気は
const std::string kMari = "\xE3\x83\x9E\xE3\x83\xAA\xE3\x82\xA2";                 // マリア
const std::string kAriiLong =
    "\xE3\x82\xA2\xE3\x83\xAA\xE3\x83\xBC";                                       // アリー
const std::string kFullWidthAria = "\xEF\xBC\xA1\xEF\xBD\x92\xEF\xBD\x89\xEF\xBD\x81";  // Ａｒｉａ

void wake_rule() {
  std::printf("wake_word: normalisation\n");
  ok("\"Hey, Aria!\" -> \"heyaria\"", normalise_wake("Hey, Aria!") == "heyaria");
  ok("case and spaces are gone", normalise_wake("  A R I A  ") == "aria");
  ok("a hyphenated name closes up", normalise_wake("ari-a") == "aria");
  ok("digits survive", normalise_wake("unit-7") == "unit7");
  ok("full-width Latin folds to ascii", normalise_wake(kFullWidthAria) == "aria");
  ok("japanese survives untouched", normalise_wake(kAria) == kAria);
  ok("japanese punctuation is dropped", normalise_wake(kAriaSentence) != kAriaSentence);
  // The trap this project has been bitten by before: a rule that counts bytes
  // instead of characters. アリア is nine bytes and three characters.
  ok("three japanese characters count as three, not nine", wake_chars(kAria) == 3);
  ok("a truncated tail is dropped whole, never split",
     normalise_wake(std::string("ari") + kAria.substr(0, 2)) == "ari");

  std::printf("\nwake_word: what is armed\n");
  ok("empty is off", !wake_phrase_armed(""));
  ok("empty is off and says nothing about it", wake_phrase_problem("").empty());
  ok("punctuation only is off", !wake_phrase_armed("!!! ..."));
  ok("two letters are refused", !wake_phrase_armed("hi"));
  ok("two letters are refused out loud", !wake_phrase_problem("hi").empty());
  ok("two japanese characters are refused",
     !wake_phrase_armed(kAria.substr(0, 6)));
  ok("three letters are armed", wake_phrase_armed("ada"));
  ok("three japanese characters are armed", wake_phrase_armed(kAria));
  ok("an armed phrase has nothing to complain about", wake_phrase_problem("aria").empty());

  std::printf("\nwake_word: matching (english)\n");
  match("exact", "aria", "Aria", true);
  match("wrong case", "ARIA", "aria", true);
  match("inside a sentence", "hey aria what time is it", "Aria", true);
  match("with the punctuation a recogniser adds", "Aria, what time is it?", "aria", true);
  match("extra spaces in the setting", "aria", "   Aria   ", true);
  match("a two-word phrase heard as one", "heyaria", "hey aria", true);
  match("a two-word phrase heard with a comma", "Hey, Aria.", "hey aria", true);
  // The near-misses. Each of these is a decode the recogniser really can
  // produce for someone saying "Aria", and none of them may open the mic.
  match("NEAR MISS: \"area\" does not match", "what is the area of it", "aria", false);
  match("NEAR MISS: \"ariel\" does not match", "ariel is on the phone", "aria", false);
  match("NEAR MISS: a missing vowel", "arla", "aria", false);
  match("NEAR MISS: the empty phrase never matches anything", "aria", "", false);
  match("NEAR MISS: an under-length phrase never matches", "hi there", "hi", false);

  std::printf("\nwake_word: matching (japanese -- no spaces to lean on)\n");
  match("exact", kAria, kAria, true);
  match("inside a spaceless sentence", kAriaSentence, kAria, true);
  match("NEAR MISS: a different name ending the same way", kMari, kAria, false);
  match("NEAR MISS: the prolonged sound mark is not a comma", kAriiLong, kAria, false);
  match("a latin phrase inside a japanese sentence", std::string("aria") + kAriaSentence, "aria",
        true);

  // What "no word boundaries" costs, stated rather than hidden. Both of these
  // are the *same* fact from two directions: separation is thrown away, so the
  // phrase matches inside a longer word, and it also matches a decode that
  // split it apart. Throwing separation away is not optional -- it is what
  // makes a two-word phrase survive a recogniser that ran it together, and it
  // is the only rule that behaves identically in a language with spaces and
  // one without. `kWakeMinChars` is what pays for it.
  std::printf("\nwake_word: the known cost of having no word boundaries\n");
  match("a phrase inside a longer word DOES match (documented)", "malaria", "aria", true);
  match("a phrase the decoder split up DOES match (documented)", "a ri a", "aria", true);
}

void restore_rule() {
  std::printf("\nlisten_restore: the positive case\n");
  {
    ListenRestore r;
    r.wait_began(true);  // the latch was on when the worker went out
    ok("armed while the worker is out", r.armed());
    ok("the report restores listening", r.worker_reported());
    r.wait_ended();
    ok("the memory is gone once the last worker is back", !r.armed());
  }

  std::printf("\nlisten_restore: THE NEGATIVE CASE -- the mic was shut\n");
  {
    ListenRestore r;
    r.wait_began(false);  // the user had already shut the microphone
    ok("not armed", !r.armed());
    ok("the report does NOT open the microphone", !r.worker_reported());
  }

  std::printf("\nlisten_restore: THE NEGATIVE CASE -- the user shut it while waiting\n");
  {
    ListenRestore r;
    r.wait_began(true);
    ok("armed to begin with", r.armed());
    r.user_shut_the_mic();
    ok("the user wins", !r.worker_reported());
    // And permanently, for this wait: a second and third worker coming back
    // must not reopen what the user closed.
    ok("a second worker cannot undo that", !r.worker_reported());
    ok("nor a third", !r.worker_reported());
    // ...until the user opens it again themselves, which is them choosing
    // "listening" and so re-arms the memory for the timeout that may follow.
    r.user_opened_the_mic();
    ok("the user opening it again re-arms the memory", r.worker_reported());
  }

  std::printf("\nlisten_restore: the user opened it mid-wait\n");
  {
    ListenRestore r;
    r.wait_began(false);  // shut when the worker went out
    ok("not armed to begin with", !r.worker_reported());
    r.user_opened_the_mic();
    ok("now that they are listening, a report restores it", r.worker_reported());
  }

  std::printf("\nlisten_restore: opening the mic outside a wait arms nothing\n");
  {
    ListenRestore r;
    r.user_opened_the_mic();
    ok("no wait is open, so there is nothing to arm", !r.worker_reported());
  }

  std::printf("\nlisten_restore: the listen timeout is the app, not the user\n");
  {
    ListenRestore r;
    r.wait_began(true);
    // close_latch_after_silence() deliberately does NOT call user_shut_the_mic().
    // This is the whole feature: the mic closing itself is the event the
    // restore exists to undo.
    ok("a timed-out latch is still restored", r.worker_reported());
  }

  std::printf("\nlisten_restore: several workers\n");
  {
    ListenRestore r;
    r.wait_began(true);            // worker one goes out
    r.wait_began(false);           // worker two joins the SAME wait
    ok("a second spawn does not re-read a latch that has since gone quiet", r.armed());
    ok("worker one's report restores", r.worker_reported());
    ok("worker two's report restores as well (idempotent)", r.worker_reported());
    r.wait_ended();
    ok("only the last one back clears the memory", !r.armed());
  }

  std::printf("\nlisten_restore: no wait at all\n");
  {
    ListenRestore r;
    ok("nothing is restored when nothing was waited for", !r.worker_reported());
    r.user_shut_the_mic();
    ok("and that stays true", !r.worker_reported());
  }
}

}  // namespace

int main() {
  wake_rule();
  restore_rule();
  std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
