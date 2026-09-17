// The canned-line table: does it answer in both languages, and do the Japanese
// bytes survive the compiler?
//
// Scope, cut deliberately on 18 Sep 2026: this is the minimum worth having
// standing between the table and the user's ears, not a coverage suite. The
// byte half is the half that cannot be cut -- mojibake from an MSVC source
// encoding is invisible in the editor, invisible in a diff, and only shows up
// as noise coming out of VOICEVOX after someone has waited ten minutes for a
// timer. Everything else about the table is checked by reading it.
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

// The bytes, written out by hand rather than as a literal, so that this side of
// the comparison cannot be mangled by the same thing that would mangle the
// table. If the compiler read app_strings.cpp as anything but UTF-8, these stop
// matching and the test says so.
//
// "あずかってた用事を始められなかったよ。まだ何もやれてないんだ。"
// -- the deferred worker that could not start, the line the whole task is about.
static const char kDeferredJa[] =
    "\xe3\x81\x82\xe3\x81\x9a\xe3\x81\x8b\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x9f"   // あずかってた
    "\xe7\x94\xa8\xe4\xba\x8b\xe3\x82\x92"                                       // 用事を
    "\xe5\xa7\x8b\xe3\x82\x81\xe3\x82\x89\xe3\x82\x8c\xe3\x81\xaa\xe3\x81\x8b"   // 始められなか
    "\xe3\x81\xa3\xe3\x81\x9f\xe3\x82\x88\xe3\x80\x82"                           // ったよ。
    "\xe3\x81\xbe\xe3\x81\xa0\xe4\xbd\x95\xe3\x82\x82\xe3\x82\x84\xe3\x82\x8c"   // まだ何もやれ
    "\xe3\x81\xa6\xe3\x81\xaa\xe3\x81\x84\xe3\x82\x93\xe3\x81\xa0\xe3\x80\x82";  // てないんだ。

// "うまくいかなかったみたい。" -- the front of the failed-worker line, which is
// the one that carries an English tail after it.
static const char kTaskFailedJa[] =
    "\xe3\x81\x86\xe3\x81\xbe\xe3\x81\x8f\xe3\x81\x84\xe3\x81\x8b\xe3\x81\xaa"
    "\xe3\x81\x8b\xe3\x81\xa3\xe3\x81\x9f\xe3\x81\xbf\xe3\x81\x9f\xe3\x81\x84"
    "\xe3\x80\x82";

int main() {
  std::printf("-- the table answers in both languages --\n");

  // Every key, both columns. Not per-key assertions: one sweep that says a
  // sentence always comes back, which is the property the fallback exists for.
  bool all_en = true, all_ja = true, no_slots_left = true;
  for (int i = 0; i < static_cast<int>(Msg::Count); ++i) {
    const Msg m = static_cast<Msg>(i);
    const std::string en = app_text_in(AppLang::English, m, "A", "B");
    const std::string ja = app_text_in(AppLang::Japanese, m, "A", "B");
    if (en.empty()) { all_en = false; std::printf("     key %d has no English\n", i); }
    if (ja.empty()) { all_ja = false; std::printf("     key %d resolved to nothing\n", i); }
    if (en.find("{}") != std::string::npos || ja.find("{}") != std::string::npos) {
      no_slots_left = false;
      std::printf("     key %d left a slot in the text\n", i);
    }
  }
  check(all_en, "every key has an English sentence");
  check(all_ja, "every key resolves in Japanese (translated, or fallen back)");
  check(no_slots_left, "no {} survives into what the user hears");

  // The fallback rule itself, on a row that is deliberately half filled, so
  // that proving it does not require leaving a real message untranslated.
  const AppLine half{"only English here", ""};
  check(std::string(pick(half, AppLang::Japanese)) == "only English here",
        "a missing translation falls back to English, not to empty");

  // A translated row really does come back translated -- the failure this
  // whole table exists to remove is a line that is looked up and still English.
  check(std::string(pick(app_line(Msg::DeferredStartFailedSpoken), AppLang::Japanese)) !=
            std::string(app_line(Msg::DeferredStartFailedSpoken).en),
        "a translated key is not still the English one");

  std::printf("\n-- the Japanese bytes survive the compiler --\n");

  const std::string deferred = app_text_in(AppLang::Japanese, Msg::DeferredStartFailedSpoken);
  check(deferred == kDeferredJa, "the deferred-worker line is byte-for-byte the expected UTF-8");
  if (deferred != kDeferredJa)
    std::printf("     got %zu bytes, wanted %zu\n", deferred.size(), sizeof(kDeferredJa) - 1);

  const std::string failed = app_text_in(AppLang::Japanese, Msg::TaskFailedSpoken, "exit code 1.");
  check(failed == std::string(kTaskFailedJa) + "exit code 1.",
        "a Japanese sentence with an English tail spliced into it is intact");

  // Nothing in the Japanese column may be anything but well-formed UTF-8 in the
  // Japanese ranges: a CP932 misread would land as stray high bytes here long
  // before anyone heard it.
  bool bytes_sane = true;
  for (int i = 0; i < static_cast<int>(Msg::Count); ++i) {
    const char* ja = app_line(static_cast<Msg>(i)).ja;
    if (!ja || !*ja) continue;
    const std::string s = ja;
    bool saw_japanese = false;
    for (size_t k = 0; k < s.size();) {
      const unsigned char c = static_cast<unsigned char>(s[k]);
      size_t len = 0;
      if (c < 0x80) len = 1;
      else if ((c & 0xE0) == 0xC0) len = 2;
      else if ((c & 0xF0) == 0xE0) len = 3;
      else if ((c & 0xF8) == 0xF0) len = 4;
      else { bytes_sane = false; std::printf("     key %d: stray byte 0x%02X\n", i, c); break; }
      if (k + len > s.size()) { bytes_sane = false; std::printf("     key %d: truncated\n", i); break; }
      for (size_t t = 1; t < len; ++t) {
        if ((static_cast<unsigned char>(s[k + t]) & 0xC0) != 0x80) {
          bytes_sane = false;
          std::printf("     key %d: bad continuation byte\n", i);
        }
      }
      if (len >= 3) saw_japanese = true;
      k += len;
    }
    if (!saw_japanese) {
      bytes_sane = false;
      std::printf("     key %d: a Japanese cell with no Japanese in it\n", i);
    }
  }
  check(bytes_sane, "every Japanese cell is well-formed UTF-8 with Japanese in it");

  std::printf("\n-- which language a canned line speaks in --\n");

  check(app_language_for({true, false}, true) == AppLang::English,
        "Japanese switched off: English even after a Japanese turn");
  check(app_language_for({false, true}, false) == AppLang::Japanese,
        "English switched off: Japanese even after an English turn");
  check(app_language_for({true, true}, true) == AppLang::Japanese,
        "both on: the user's last turn decides (Japanese)");
  check(app_language_for({true, true}, false) == AppLang::English,
        "both on: the user's last turn decides (English)");

  set_enabled_languages({true, true});
  note_user_language("あと十分でストレッチって言って");
  const bool spoke_ja = app_text(Msg::RefuseTooMany) ==
                        app_text_in(AppLang::Japanese, Msg::RefuseTooMany);
  note_user_language("   ...   ");
  const bool still_ja = app_text(Msg::RefuseTooMany) ==
                        app_text_in(AppLang::Japanese, Msg::RefuseTooMany);
  note_user_language("remind me in ten minutes");
  const bool back_to_en = app_text(Msg::RefuseTooMany) ==
                          app_text_in(AppLang::English, Msg::RefuseTooMany);
  check(spoke_ja, "a Japanese turn puts the canned lines into Japanese");
  check(still_ja, "a turn with nothing speakable in it does not flip the language");
  check(back_to_en, "an English turn puts them back");

  std::printf("\n%s\n", failures ? "FAILED" : "all good");
  return failures ? 1 : 0;
}
