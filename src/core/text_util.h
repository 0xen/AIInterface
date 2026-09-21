#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aii {

// What `utf8_next` returns for a byte that does not begin a well-formed
// sequence: U+FFFD, the replacement character.
constexpr std::uint32_t kUtf8Replacement = 0xFFFDu;

// M24.2. The one UTF-8 lead-byte decoder. There were three of these — this
// file's `next_cp`, `wake_word.cpp`'s `seq_len`/`decode` pair, and a one-line
// ternary inside `button_registry.cpp`'s `truncate_chars` — and they disagreed
// about every kind of malformed input. This project has already shipped one
// byte-level UTF-8 bug; it does not need three chances at the next one.
//
// Decodes the sequence starting at `s[i]` and advances `i` past what it
// consumed. `i` must be less than `s.size()`. The returned value is the code
// point, and `i` always moves by at least one byte, so a loop over a string of
// any bytes at all terminates.
//
// **Malformed input yields `kUtf8Replacement` and consumes exactly one byte**,
// which is what the three old decoders each did differently. The cases, and
// what each of them used to do:
//
//   * a stray continuation byte (0x80..0xBF): replacement. `next_cp` also gave
//     U+FFFD; `wake_word` decoded it as a code point in 0x80..0xBF and *kept*
//     it, so a broken byte could survive into a normalised wake phrase;
//     `truncate_chars` counted it as one character.
//   * a lead byte no sequence starts with (0xF8..0xFF): replacement, as all
//     three effectively did.
//   * a sequence truncated by the end of the string: replacement per remaining
//     byte. `next_cp` consumed the rest of the string and returned whatever
//     partial value it had accumulated; `wake_word` stopped the walk and
//     dropped the tail whole (still the visible behaviour there, because its
//     fold drops the replacement character); `truncate_chars` counted the tail
//     as one character.
//   * a sequence whose continuation bytes are not continuation bytes
//     ("\xE3" "ab"): replacement for the lead byte, and the following bytes are
//     then decoded on their own. All three previously swallowed them into one
//     garbage code point, so an ASCII letter after a bad lead byte disappeared.
//   * an overlong encoding ("\xC0\x80"), a surrogate, or a value above
//     U+10FFFF: replacement. `next_cp` and `wake_word` both decoded these,
//     which is how a NUL arrives spelled as two bytes.
//
// It does not sanitise, it decodes: the caller decides what a replacement
// character means. `clip_utf8` below deliberately does not use it, because it
// walks *backwards* from a byte cap and never needs a code point's value.
std::uint32_t utf8_next(const std::string& s, std::size_t& i);

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

// M26.3, finding 35. UTF-8 to UTF-16, for the Windows API calls that will not
// take anything else: a command line, a path, a shell verb. This app holds
// every string as UTF-8 and Windows holds none of them that way, so the
// conversion happens at the edge, and until now it happened at three edges
// with three copies of the same six lines -- `claude_code_client.cpp`,
// `claude_client.cpp` and `button_registry.cpp`. A project that has already
// paid once for a byte-level UTF-8 bug should have one of these.
//
// Empty in, empty out. Invalid UTF-8 is passed to `MultiByteToWideChar`
// without `MB_ERR_INVALID_CHARS`, so a bad byte becomes U+FFFD rather than
// failing the call: these strings are paths and labels on their way to the
// OS, and a conversion that returned nothing would turn a mangled path into
// no path at all, which is the harder fault to read.
std::wstring widen(const std::string& utf8);

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
