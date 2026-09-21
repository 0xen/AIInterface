// text_util: the four helpers the speech path leans on, and the split that
// decides which voice says what.
//
// Finding 26 of the 21 Sep review: `split_by_script` had no test, and the
// language analysis in the same handoff records that it is one of the four
// places in this app that decide "is this Japanese?" -- the one that chooses a
// voice per stretch of a sentence. The contract, from the header:
//
//   * digits, spaces and punctuation are script-neutral and **stay with the
//     run they follow**;
//   * a run with nothing speakable in it is folded into its neighbour rather
//     than emitted on its own;
//   * single-script text comes back as one run.
//
// `clip_utf8` has its own test (clip_utf8_test) and is not repeated here.
//
//   text_util_test          prints every case and exits non-zero on failure
#include <cstdio>
#include <string>
#include <vector>

#include "core/text_util.h"

using aii::has_japanese;
using aii::has_speakable_content;
using aii::ScriptRun;
using aii::split_by_script;
using aii::strip_markdown;
using aii::trim;

namespace {

int g_failures = 0;

void check_bool(const char* fn, const std::string& in, bool got, bool want) {
  const bool ok = got == want;
  if (!ok) ++g_failures;
  std::printf("  %-22s %-34s %s  got=%s\n", fn, ("[" + in + "]").c_str(),
              ok ? "ok  " : "FAIL", got ? "true" : "false");
  if (!ok) std::printf("      wanted %s\n", want ? "true" : "false");
}

void check_str(const char* fn, const std::string& in, const std::string& got,
               const std::string& want) {
  const bool ok = got == want;
  if (!ok) ++g_failures;
  std::printf("  %-22s %-34s %s  got=[%s]\n", fn, ("[" + in + "]").c_str(),
              ok ? "ok  " : "FAIL", got.c_str());
  if (!ok) std::printf("      wanted [%s]\n", want.c_str());
}

// A run as "j:text" or "e:text", which is short enough to read in a failure
// line and carries both halves of what a run is.
std::string show(const std::vector<ScriptRun>& runs) {
  std::string s = "{";
  for (std::size_t i = 0; i < runs.size(); ++i) {
    if (i) s += " | ";
    s += runs[i].japanese ? "j:" : "e:";
    s += runs[i].text;
  }
  return s + "}";
}

void check_runs(const char* name, const std::string& in,
                const std::vector<ScriptRun>& want) {
  const std::vector<ScriptRun> got = split_by_script(in);
  bool ok = got.size() == want.size();
  for (std::size_t i = 0; ok && i < got.size(); ++i)
    ok = got[i].japanese == want[i].japanese && got[i].text == want[i].text;
  // Whatever the split decides, it must not lose or reorder a byte: the runs
  // concatenated are the input. That is the property a caller depends on and
  // it is checked on every case rather than only where one is expected.
  std::string joined;
  for (const ScriptRun& r : got) joined += r.text;
  const bool lossless = joined == in;
  if (!lossless) ok = false;
  if (!ok) ++g_failures;
  std::printf("  %-52s %s  %zu run%s%s\n", name, ok ? "ok  " : "FAIL", got.size(),
              got.size() == 1 ? "" : "s", lossless ? "" : "  <-- LOST BYTES");
  if (!ok) {
    std::printf("      wanted %s\n", show(want).c_str());
    std::printf("      got    %s\n", show(got).c_str());
  }
}

// Japanese written as escaped UTF-8 so the cases mean the same thing whatever
// the compiler thinks the source encoding is.
const std::string kKonnichiwa =
    "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF";  // こんにちは
const std::string kKore = "\xE3\x81\x93\xE3\x82\x8C\xE3\x81\xAF";     // これは
const std::string kWoTsukau = "\xE3\x82\x92\xE4\xBD\xBF\xE3\x81\x86";  // を使う
const std::string kWo = "\xE3\x82\x92";                                // を
const std::string kNihongo = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";   // 日本語
const std::string kJaStop = "\xE3\x80\x82";                            // 。
const std::string kJaComma = "\xE3\x80\x81";                           // 、
const std::string kHalfKata = "\xEF\xBD\xB6\xEF\xBE\x80\xEF\xBD\xB6\xEF\xBE\x85";  // ｶﾀｶﾅ
const std::string kFullAscii = "\xEF\xBC\xA1\xEF\xBC\xA2\xEF\xBC\xA3";             // ＡＢＣ

}  // namespace

int main() {
  std::printf("text_util_test\n\n");

  // --- has_japanese ---------------------------------------------------------
  // Presence of kana, katakana or a CJK ideograph. Note what is NOT Japanese
  // by this rule: the full-width stop 。 is CJK *punctuation* (U+3002), which
  // the code treats as neutral, so a string of only punctuation is not
  // Japanese. app_strings.cpp's note_user_language() is the caller that cares.
  std::printf("has_japanese\n");
  check_bool("has_japanese", "Hello there", has_japanese("Hello there"), false);
  check_bool("has_japanese", "", has_japanese(""), false);
  check_bool("has_japanese", "123 !?", has_japanese("123 !?"), false);
  check_bool("has_japanese", "kana", has_japanese(kKonnichiwa), true);
  check_bool("has_japanese", "kanji", has_japanese(kNihongo), true);
  check_bool("has_japanese", "halfwidth katakana", has_japanese(kHalfKata), true);
  check_bool("has_japanese", "one kana in english", has_japanese("ok " + kWo), true);
  check_bool("has_japanese", "japanese full stop only", has_japanese(kJaStop), false);
  check_bool("has_japanese", "fullwidth ascii only", has_japanese(kFullAscii), false);

  // --- has_speakable_content ------------------------------------------------
  // At least one letter, digit or CJK character. This is the gate that keeps
  // the synthesiser from being handed "---" or " ... ".
  std::printf("\nhas_speakable_content\n");
  check_bool("has_speakable", "", has_speakable_content(""), false);
  check_bool("has_speakable", "   ", has_speakable_content("   "), false);
  check_bool("has_speakable", "...", has_speakable_content("..."), false);
  check_bool("has_speakable", " -- !!! ", has_speakable_content(" -- !!! "), false);
  check_bool("has_speakable", "a", has_speakable_content("a"), true);
  check_bool("has_speakable", "7", has_speakable_content("7"), true);
  check_bool("has_speakable", "kana", has_speakable_content(kWo), true);
  check_bool("has_speakable", "accented", has_speakable_content("\xC3\xA9"), true);
  // Recorded, not endorsed: the header says "at least one letter, digit or
  // CJK character", and a lone 。 is none of those -- but the implementation's
  // catch-all is `cp >= 0xC0`, so every non-ASCII code point above Latin-1's
  // punctuation block counts as speakable. The visible effect is small (a
  // chunk of nothing but Japanese punctuation is handed to the synthesiser
  // instead of being dropped) and the rule is shared with split_by_script's
  // fold, where U+00AB below it is what makes the "unspeakable run" case
  // reachable at all. Behaviour left alone; flagged for the manager.
  check_bool("has_speakable", "japanese stop only (see comment)",
             has_speakable_content(kJaStop), true);
  check_bool("has_speakable", "ellipsis U+2026 (same rule)",
             has_speakable_content("\xE2\x80\xA6"), true);
  check_bool("has_speakable", "guillemet U+00AB is below the cut-off",
             has_speakable_content("\xC2\xAB"), false);

  // --- trim -----------------------------------------------------------------
  // Everything at or below a space byte goes, which includes tabs, newlines
  // and control bytes; multi-byte UTF-8 bytes are all above 0x7F and so are
  // never touched from either end.
  std::printf("\ntrim\n");
  check_str("trim", "  hi  ", trim("  hi  "), "hi");
  check_str("trim", "", trim(""), "");
  check_str("trim", "   ", trim("   "), "");
  check_str("trim", "\\t\\r\\n x \\n", trim("\t\r\n x \n"), "x");
  check_str("trim", "no padding", trim("no padding"), "no padding");
  check_str("trim", " a  b   (inner kept)", trim(" a  b "), "a  b");
  check_str("trim", "japanese untouched", trim(" " + kKonnichiwa + " "), kKonnichiwa);

  // --- strip_markdown -------------------------------------------------------
  // Light decoration only: a leading heading mark or bullet, and the three
  // inline characters. It is deliberately not a markdown parser.
  std::printf("\nstrip_markdown\n");
  check_str("strip_markdown", "## A heading", strip_markdown("## A heading"), "A heading");
  check_str("strip_markdown", "- a bullet", strip_markdown("- a bullet"), "a bullet");
  check_str("strip_markdown", "* a bullet", strip_markdown("* a bullet"), "a bullet");
  check_str("strip_markdown", "**bold**", strip_markdown("**bold**"), "bold");
  check_str("strip_markdown", "`code`", strip_markdown("`code`"), "code");
  check_str("strip_markdown", "_em_", strip_markdown("_em_"), "em");
  check_str("strip_markdown", "  padded  ", strip_markdown("  padded  "), "padded");
  // A hash with no space after it is not a heading and stays.
  check_str("strip_markdown", "#nospace", strip_markdown("#nospace"), "#nospace");
  // Known and accepted: an underscore inside a word goes too, because the
  // rule is "remove the character", not "parse emphasis". Speech never sees
  // an identifier often enough for this to be worth a parser.
  check_str("strip_markdown", "snake_case", strip_markdown("snake_case"), "snakecase");
  check_str("strip_markdown", "*", strip_markdown("*"), "");
  check_str("strip_markdown", "", strip_markdown(""), "");
  check_str("strip_markdown", "japanese", strip_markdown("*" + kKonnichiwa + "*"), kKonnichiwa);

  // --- split_by_script ------------------------------------------------------
  std::printf("\nsplit_by_script: single-script and empty input\n");
  check_runs("empty input -> no runs at all", "", {});
  check_runs("single-script english -> one run", "Hello there", {{false, "Hello there"}});
  check_runs("single-script japanese -> one run", kKonnichiwa + kJaStop,
             {{true, kKonnichiwa + kJaStop}});
  // All-neutral input has no script to pick, so it comes back as one run
  // marked not-Japanese: the English voice reads the digits.
  check_runs("all-neutral input -> one non-japanese run", "123 !!! ",
             {{false, "123 !!! "}});
  check_runs("neutral japanese punctuation alone -> one non-japanese run",
             kJaStop + kJaComma, {{false, kJaStop + kJaComma}});
  // Full-width ASCII is neutral by design (it is the same letters), so it does
  // not by itself select the Japanese voice.
  check_runs("fullwidth ascii alone -> one non-japanese run", kFullAscii,
             {{false, kFullAscii}});

  std::printf("\nsplit_by_script: neutral characters ride with the current run\n");
  // The space between the two scripts stays with the run it follows, so the
  // English run keeps its trailing space and the Japanese run starts clean.
  check_runs("space rides with the run it follows", "Hello " + kKonnichiwa,
             {{false, "Hello "}, {true, kKonnichiwa}});
  check_runs("digits and punctuation ride along", "I have 3 cats, really",
             {{false, "I have 3 cats, really"}});
  check_runs("digits inside japanese stay japanese", kKore + " 3 " + kNihongo,
             {{true, kKore + " 3 " + kNihongo}});
  // Neutral text before any script has been chosen waits and joins the first
  // run, whichever it turns out to be.
  check_runs("leading neutral text joins the first run", "  " + kNihongo,
             {{true, "  " + kNihongo}});
  check_runs("trailing neutral text stays on the last run", kNihongo + " ... ",
             {{true, kNihongo + " ... "}});

  std::printf("\nsplit_by_script: run boundaries at the ends\n");
  check_runs("a latin run at the very start", "Test" + kKore,
             {{false, "Test"}, {true, kKore}});
  check_runs("a latin run at the very end", kKore + "test",
             {{true, kKore}, {false, "test"}});
  check_runs("japanese, english, japanese", kNihongo + " and " + kKonnichiwa,
             {{true, kNihongo + " "}, {false, "and "}, {true, kKonnichiwa}});
  check_runs("half-width katakana selects the japanese voice", "Say " + kHalfKata,
             {{false, "Say "}, {true, kHalfKata}});
  check_runs("fullwidth ascii inside japanese rides along",
             kKore + kFullAscii + kNihongo, {{true, kKore + kFullAscii + kNihongo}});

  std::printf("\nsplit_by_script: short runs\n");
  // A one-letter Latin run is folded into the Japanese around it -- it is a
  // stray letter, not a sentence, and a separate trip through an engine for it
  // costs more than it buys.
  check_runs("a one-letter latin run is folded", "A" + kWoTsukau,
             {{true, "A" + kWoTsukau}});
  check_runs("a one-letter latin run mid-sentence is folded",
             kKore + "A" + kWoTsukau, {{true, kKore + "A" + kWoTsukau}});
  // TWO letters is not: "AI" in "AIを使う" is kept as its own English run and
  // so is spoken by Kokoro on a separate trip. The threshold is
  // kMinRunBytes = 2 in text_util.cpp. Recorded here as the behaviour, not
  // endorsed: the 21 Sep handoff's language section lists exactly this case as
  // a smaller observation, and whether a two-letter loanword deserves its own
  // engine call is a decision for the manager, not for this test. If it is
  // ever changed, this case is the one to change with it.
  check_runs("a two-letter latin run is NOT folded (see comment)", "AI" + kWoTsukau,
             {{false, "AI"}, {true, kWoTsukau}});
  // A run with nothing speakable never stands on its own, however long. The
  // guillemet U+00AB is Latin by script (it is under U+024F) and has nothing
  // in it worth saying, which is the only way that clause of the rule can be
  // reached -- an ASCII run of punctuation is neutral and never becomes a run
  // in the first place.
  const std::string kGuillemets = "\xC2\xAB\xC2\xAB\xC2\xAB";
  check_runs("an unspeakable latin run is folded into its neighbour",
             kNihongo + kGuillemets + kKonnichiwa,
             {{true, kNihongo + kGuillemets + kKonnichiwa}});
  // ASCII punctuation between two Japanese stretches is neutral, so it never
  // breaks the run at all.
  check_runs("ascii punctuation between japanese keeps one run",
             kNihongo + " --- " + kKonnichiwa, {{true, kNihongo + " --- " + kKonnichiwa}});

  std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
