// SentenceSplitter: what the speech queue is handed, and when.
//
// The header states the contract and this test is written against it rather
// than against the scan loop:
//
//   * the FIRST chunk of a reply may be cut early -- at a comma (", " or "、")
//     or at the whitespace boundary after `early_words` words (12 by default,
//     `AII_EARLY_WORDS` in config.cpp) -- to cut time-to-first-audio;
//   * every LATER chunk waits for a full sentence, so prosody stays natural;
//   * text inside ``` fences is shown but never spoken;
//   * `break_now()` emits what is pending as one utterance and **keeps the
//     stream state**: the fence flag and the early-chunk latch both survive.
//     `flush()` is the call that does not, and the difference is the whole of
//     M13.1 (commit 20676d5, docs/design-directives.md §3.3). Finding 26 of
//     the 21 Sep review is that the probe which found this necessary left no
//     case behind; the two `break_now vs flush` pairs below are that case.
//
// Each group feeds the same deltas to a fresh splitter and compares the exact
// list of utterances handed to the queue.
//
//   sentence_splitter_test    prints every case and exits non-zero on failure
#include <cstdio>
#include <string>
#include <vector>

#include "core/sentence_splitter.h"

namespace {

int g_failures = 0;

// A splitter plus the utterances it has emitted, so a case reads as a script.
struct Rig {
  std::vector<std::string> out;
  aii::SentenceSplitter splitter;

  explicit Rig(int early_words = 12)
      : splitter([this](const std::string& s) { out.push_back(s); }, early_words) {}

  void feed(const std::string& s) { splitter.feed(s); }
  void flush() { splitter.flush(); }
  void brk() { splitter.break_now(); }
  void reset() { splitter.reset(); }
};

std::string show(const std::vector<std::string>& v) {
  std::string s = "[";
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) s += " | ";
    s += v[i];
  }
  return s + "]";
}

void check(const char* name, const std::vector<std::string>& got,
           const std::vector<std::string>& want) {
  const bool ok = got == want;
  if (!ok) ++g_failures;
  std::printf("  %-56s %s  %zu utterance%s\n", name, ok ? "ok  " : "FAIL", got.size(),
              got.size() == 1 ? "" : "s");
  if (!ok) {
    std::printf("      wanted %s\n", show(want).c_str());
    std::printf("      got    %s\n", show(got).c_str());
  }
}

// Japanese as escaped UTF-8, so this file's meaning does not depend on how an
// editor or a compiler flag happens to read it. app_strings_test guards the
// /utf-8 switch itself; here the bytes are written out.
const std::string kJaComma = "\xE3\x80\x81";   // 、
const std::string kJaStop = "\xE3\x80\x82";    // 。
const std::string kJaBang = "\xEF\xBC\x81";    // ！
const std::string kKore = "\xE3\x81\x93\xE3\x82\x8C\xE3\x81\xAF";              // これは
const std::string kTesuto = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";            // テスト
const std::string kDesu = "\xE3\x81\xA7\xE3\x81\x99";                          // です
const std::string kSugoi = "\xE3\x81\x99\xE3\x81\x94\xE3\x81\x84";             // すごい
const std::string kHontou = "\xE6\x9C\xAC\xE5\xBD\x93\xE3\x81\xA7\xE3\x81\x99";  // 本当です

}  // namespace

int main() {
  std::printf("sentence_splitter_test\n\n");

  // --- 1. the early first chunk ---------------------------------------------
  std::printf("the early first chunk (comma, or %d words)\n", 12);
  {
    // A comma with a space after it cuts the first chunk and nothing later.
    Rig r;
    r.feed("Well, that is interesting.");
    check("comma cuts the first chunk", r.out, {"Well,"});
    r.flush();
    check("  ...and the rest follows on flush", r.out, {"Well,", "that is interesting."});
  }
  {
    // Thirteen words, no punctuation: the cut lands after the twelfth, at the
    // whitespace boundary, and the thirteenth stays buffered.
    Rig r;
    r.feed("one two three four five six seven eight nine ten eleven twelve thirteen");
    check("twelve words cut the first chunk", r.out,
          {"one two three four five six seven eight nine ten eleven twelve"});
  }
  {
    // A comma with no space after it is not a break -- "3,000" must not be one.
    Rig r;
    r.feed("It cost 3,000 yen and change");
    check("comma with no space is not a break", r.out, {});
  }
  {
    // The Japanese comma needs no following space.
    Rig r;
    r.feed(kKore + kJaComma + kTesuto + kDesu);
    check("japanese comma cuts the first chunk", r.out, {kKore + kJaComma});
  }
  {
    // early_words = 0 disables the whole rule: full sentences only.
    Rig r(0);
    r.feed("one two three four five six seven eight nine ten eleven twelve thirteen, more");
    check("early_words=0 waits for a sentence", r.out, {});
    r.feed(" Done. ");
    check("  ...then emits the sentence whole", r.out,
          {"one two three four five six seven eight nine ten eleven twelve thirteen, more Done."});
  }
  {
    // The latch is per reply, not per chunk: only the first chunk is early.
    // Note the two deltas: the early rule is only consulted when the buffer
    // holds no sentence end yet, so a delta that already contains one is cut
    // there instead (the case below this one).
    Rig r;
    r.feed("Right, here we go");
    check("first chunk early", r.out, {"Right,"});
    r.feed(". ");
    check("  ...then the rest of that sentence", r.out, {"Right,", "here we go."});
    r.feed("Second, with a comma in it, keeps going");
    check("later chunks ignore commas", r.out, {"Right,", "here we go."});
  }
  {
    // A reply that reaches a sentence end before any comma never uses the
    // early rule at all.
    Rig r;
    r.feed("Short one. ");
    check("sentence end beats the early rule", r.out, {"Short one."});
  }

  // --- 2. per-sentence chunks after the first --------------------------------
  std::printf("\nper-sentence chunks after the first\n");
  {
    Rig r(0);
    r.feed("First one. Second one! Third one? ");
    check("three terminators, three utterances", r.out,
          {"First one.", "Second one!", "Third one?"});
  }
  {
    // A decimal point is not a sentence end.
    Rig r(0);
    r.feed("Pi is 3.14 and that is that. ");
    check("a decimal is not a sentence end", r.out, {"Pi is 3.14 and that is that."});
  }
  {
    // A run of terminators is one boundary, not three.
    Rig r(0);
    r.feed("Really?! Yes. ");
    check("\"?!\" is one boundary", r.out, {"Really?!", "Yes."});
  }
  {
    // A closing quote rides with the sentence it closes.
    Rig r(0);
    r.feed("He said \"go.\" Then he went. ");
    check("closing quote rides with the sentence", r.out,
          {"He said \"go.\"", "Then he went."});
  }
  {
    // Deltas arrive a few bytes at a time; the boundary is the same.
    Rig whole(0), pieces(0);
    const std::string reply = "First one. Second one! ";
    whole.feed(reply);
    for (char c : reply) pieces.feed(std::string(1, c));
    check("byte-at-a-time deltas give the same utterances", pieces.out, whole.out);
  }
  {
    // A newline ends a sentence even with no terminator, and markdown
    // decoration is stripped before the queue sees it.
    Rig r(0);
    r.feed("## A heading\n- *a bullet*\n");
    check("newline ends a line; markdown is stripped", r.out, {"A heading", "a bullet"});
  }

  // --- 3. japanese boundaries and mixed script -------------------------------
  std::printf("\njapanese boundaries and mixed script\n");
  {
    Rig r(0);
    r.feed(kKore + kTesuto + kDesu + kJaStop);
    check("japanese 。 ends a sentence with no space", r.out,
          {kKore + kTesuto + kDesu + kJaStop});
  }
  {
    Rig r(0);
    r.feed(kSugoi + kJaBang + kHontou + kJaStop);
    check("japanese ！ is a boundary too", r.out, {kSugoi + kJaBang, kHontou + kJaStop});
  }
  {
    // A mixed-script sentence stays ONE utterance. Choosing a voice per run is
    // split_by_script's job downstream, not the splitter's; cutting here would
    // put a queue boundary in the middle of a sentence.
    Rig r(0);
    r.feed("Hello " + kKore + kTesuto + kDesu + kJaStop + " Bye. ");
    check("mixed script is one utterance per sentence", r.out,
          {"Hello " + kKore + kTesuto + kDesu + kJaStop, "Bye."});
  }
  {
    // An English sentence end inside otherwise Japanese text still ends it.
    Rig r(0);
    r.feed(kKore + " is a test. " + kSugoi + kJaStop);
    check("an ascii terminator inside japanese text", r.out,
          {kKore + " is a test.", kSugoi + kJaStop});
  }

  // --- 4. code fences: shown, never spoken -----------------------------------
  std::printf("\ncode fences (shown, never spoken)\n");
  {
    Rig r(0);
    r.feed("Here is code:\n```\nint x = 1;\n```\nDone.\n");
    check("fenced text is not spoken; prose around it is", r.out,
          {"Here is code:", "Done."});
  }
  {
    // An unterminated fence swallows the rest of the reply, including at flush.
    Rig r(0);
    r.feed("Look:\n```\nint x = 1;\n");
    check("unterminated fence emits only the prose before it", r.out, {"Look:"});
    r.flush();
    check("  ...and flush does not leak the code", r.out, {"Look:"});
  }
  {
    // A fence opened and closed inside a single delta.
    Rig r(0);
    r.feed("Try ```x=1``` now. ");
    check("a fence inside one delta", r.out, {"Try", "now."});
  }
  {
    // Fence markers split across deltas: the state is the splitter's, not the
    // delta's.
    Rig r(0);
    r.feed("Here:\n``");
    r.feed("`\nsecret\n``");
    r.feed("`\nAfter. ");
    check("fence markers split across deltas", r.out, {"Here:", "After."});
  }

  // --- 5. break_now: the mid-reply boundary ----------------------------------
  // The header's invariant, in its own words: "Emit what is pending as one
  // utterance and keep the stream state -- the fence flag and the early-chunk
  // latch both survive." Each case below pairs break_now() with the flush()
  // that looks like it would do and does not.
  std::printf("\nbreak_now: emits, and keeps the stream state\n");
  {
    Rig r;
    r.feed("Hello there");
    check("break_now emits an unterminated buffer", r.out, {});
    r.brk();
    check("  ...as one utterance", r.out, {"Hello there"});
  }
  {
    // The latch: after break_now, a LATER chunk must still not be cut at its
    // first comma.
    Rig r;
    r.feed("Right, here we go");   // arms the latch via the early chunk
    r.feed(". ");
    r.feed("Now this part");
    r.brk();
    const std::vector<std::string> before = r.out;
    r.feed("Then, more words follow here");
    check("break_now leaves the early-chunk latch armed", r.out, before);
  }
  {
    // ...and the same script through flush(), which re-arms it. This is the
    // bug the probe found: identical text, cut differently, depending only on
    // which call made the boundary.
    Rig r;
    r.feed("Right, here we go");
    r.feed(". ");
    r.feed("Now this part");
    r.flush();
    r.feed("Then, more words follow here");
    check("flush re-arms it (the behaviour break_now must not have)", r.out,
          {"Right,", "here we go.", "Now this part", "Then,"});
  }
  {
    // The fence flag: break_now inside a fence emits nothing and stays inside.
    Rig r(0);
    r.feed("Look:\n```\nsecret code");
    check("break_now inside a fence emits nothing", r.out, {"Look:"});
    r.brk();
    check("  ...and does not leak the fenced text", r.out, {"Look:"});
    r.feed("\nmore secret\n```\nAfter. ");
    check("  ...and the fence flag survived it", r.out, {"Look:", "After."});
  }
  {
    // ...and the same script through flush(), which clears the flag and so
    // desynchronises the splitter from the filter's copy of it.
    Rig r(0);
    r.feed("Look:\n```\nsecret code");
    r.flush();
    r.feed("\nmore secret\n```\nAfter. ");
    // Two failures at once: the fenced code is spoken, and the fence flag --
    // now set by the *closing* marker -- swallows the prose that followed it.
    check("flush clears the fence flag (break_now must not)", r.out,
          {"Look:", "more secret"});
  }
  {
    Rig r;
    r.brk();
    check("break_now on an empty buffer emits nothing", r.out, {});
    r.feed("   \n  ");
    r.brk();
    check("break_now on whitespace emits nothing", r.out, {});
    r.feed(" ... !!! ");
    r.brk();
    check("break_now on unspeakable text emits nothing", r.out, {});
  }
  {
    // break_now does not lose the buffer: what it emitted must not be emitted
    // again by the flush that ends the reply.
    Rig r;
    r.feed("Pending text here");
    r.brk();
    r.flush();
    check("break_now consumes the buffer (no double emit)", r.out, {"Pending text here"});
  }

  // --- 6. reset --------------------------------------------------------------
  // reset() is the between-replies call, and unlike break_now() it keeps
  // nothing: the pending text is dropped rather than spoken, and the
  // early-chunk latch is re-armed for the next reply.
  std::printf("\nreset\n");
  {
    Rig r;
    r.feed("Right, here we go");
    r.feed(". ");
    r.feed("Pending text");
    const std::vector<std::string> before = r.out;
    r.reset();
    r.flush();
    check("reset drops the pending buffer (never spoken)", r.out, before);
    r.feed("New, reply here");
    check("reset re-arms the early chunk (unlike break_now)", r.out,
          {"Right,", "here we go.", "New,"});
  }

  std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
