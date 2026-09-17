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
// M5.3: a **rough** token count for UTF-8 text. There is no tokenizer in this
// process and this is not one -- it is a ratio, and every caller must present
// it as such.
//
// Two ratios, because one would be wrong for half of what this app says:
// English runs at roughly 3.6 characters per token and Japanese at roughly
// 1.6, since a kana or a kanji rarely shares a token with its neighbour while
// English packs whole words into one. Mixed text is normal here, so the string
// is split with `split_by_script` -- the same split the speech path uses to
// route a sentence between two voices -- and each run is measured with its own
// ratio and the runs summed. That also settles digits and punctuation without
// a third rule: `split_by_script` folds a neutral stretch into the run it
// follows, so they are counted at the ratio of the script around them.
//
// **Characters, not bytes.** The whole point of the Japanese ratio is that
// those characters are dense; measuring the same text in UTF-8 bytes would
// count each one three times and then divide by 1.6, which is wrong by a
// factor of three in the direction that most flatters the estimate.
//
// Never returns 0 for text that has anything in it: a row reading "0" would
// claim its prompt was free.
int estimate_tokens(const std::string& utf8);

}  // namespace aii
