#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace aii {

// One stretch of a mixed-script sentence, already routed to a voice.
struct ScriptRun {
  bool japanese = false;
  std::string text;
};

// True if the UTF-8 text contains hiragana, katakana or CJK ideographs.
bool has_japanese(const std::string& utf8);
// Splits mixed English/Japanese text into consecutive runs so each one can be
// spoken by the voice that suits it. Digits, spaces and punctuation are
// script-neutral and stay with the run they follow; a run with nothing
// speakable in it is folded into its neighbour instead of being emitted on its
// own. Single-script text comes back as one run.
std::vector<ScriptRun> split_by_script(const std::string& utf8);
// True if the text has at least one letter, digit or CJK character (worth speaking).
bool has_speakable_content(const std::string& utf8);
// Remove light markdown decoration (*, _, `, leading #/-/* bullets) for speech.
std::string strip_markdown(const std::string& utf8);
std::string trim(const std::string& s);

// Bound a string to `max_bytes` **bytes** without ever leaving a half-written
// UTF-8 sequence behind. The cap is a byte cap on purpose: it is what protects
// the fixed buffers and the wire format downstream, and a character cap would
// let a Japanese string through at three times the byte size. What it will not
// do is cut in the middle of a code point, because a label that ends in a
// broken glyph is worse than one that ends a character early -- so when the cut
// lands inside a multi-byte sequence, that whole sequence is dropped and the
// result is a little shorter than the cap.
//
// Text already within the cap comes back byte-identical, including text that is
// not valid UTF-8: this clips, it does not sanitise. Invalid bytes *at* the cut
// are cut at the cap rather than searched backwards forever.
std::string clip_utf8(std::string s, std::size_t max_bytes);

}  // namespace aii
