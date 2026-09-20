#include "core/wake_word.h"

#include <cstdint>

namespace aii {
namespace {

// The lead byte says how long the sequence is. A malformed lead is treated as
// one byte, which keeps the walk moving rather than running off the end — this
// text comes out of a recogniser and off a settings file, and neither is a
// guaranteed-valid UTF-8 source worth throwing over.
int seq_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

uint32_t decode(const std::string& s, size_t i, int len) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data()) + i;
  switch (len) {
    case 2: return ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
    case 3: return ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
    case 4:
      return ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) |
             (p[3] & 0x3Fu);
    default: return p[0];
  }
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// What survives normalisation, as one decision per codepoint. It returns the
// codepoint to keep, or 0 to drop it.
//
// The dropped set is small and named rather than "everything I do not
// recognise", because this text is Japanese as often as it is English and a
// greedy filter would eat kana. Concretely:
//
//   * ASCII: keep `a-z0-9`, fold `A-Z`. Everything else goes — spaces, commas,
//     apostrophes, the hyphen in a hyphenated name.
//   * Full-width Latin (U+FF21..FF3A, U+FF41..FF5A) and full-width digits
//     (U+FF10..FF19) fold to their ASCII selves. A recogniser emitting Japanese
//     can produce these for a Latin name, and a user typing the phrase on an
//     IME keyboard can produce them too; two spellings of "Aria" that do not
//     match each other would be an invisible failure.
//   * CJK symbols and punctuation (U+3000..U+303F) go. That block is 、。〜
//     「」 and the ideographic space — and it stops one codepoint short of
//     hiragana, which is why the whole block can be dropped safely.
//   * The katakana middle dot U+30FB goes. **U+30FC, the prolonged sound mark,
//     stays**, and the two sit next to each other on purpose: ー is part of
//     words (アリー, コーヒー) and dropping it would break the very names most
//     likely to be chosen as a wake phrase for a Latin-named assistant.
//   * Full-width ASCII punctuation (U+FF01..FF0F, U+FF1A..FF20, U+FF3B..FF40,
//     U+FF5B..FF65) goes, for the same reason its half-width twin does.
//   * Everything else is kept as it stands: kana, kanji, and any script this
//     recogniser might learn later.
uint32_t fold(uint32_t cp) {
  if (cp < 0x80) {
    if (cp >= 'A' && cp <= 'Z') return cp - 'A' + 'a';
    if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) return cp;
    return 0;
  }
  if (cp >= 0x3000 && cp <= 0x303F) return 0;
  if (cp == 0x30FB) return 0;
  if (cp >= 0xFF01 && cp <= 0xFF0F) return 0;
  if (cp >= 0xFF1A && cp <= 0xFF20) return 0;
  if (cp >= 0xFF3B && cp <= 0xFF40) return 0;
  if (cp >= 0xFF5B && cp <= 0xFF65) return 0;
  if (cp >= 0xFF10 && cp <= 0xFF19) return cp - 0xFF10 + '0';
  if (cp >= 0xFF21 && cp <= 0xFF3A) return cp - 0xFF21 + 'a';
  if (cp >= 0xFF41 && cp <= 0xFF5A) return cp - 0xFF41 + 'a';
  return cp;
}

}  // namespace

std::string normalise_wake(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const int len = seq_len(static_cast<unsigned char>(text[i]));
    if (i + static_cast<size_t>(len) > text.size()) break;  // truncated tail: stop, never split
    const uint32_t kept = fold(decode(text, i, len));
    if (kept) append_utf8(out, kept);
    i += static_cast<size_t>(len);
  }
  return out;
}

int wake_chars(const std::string& text) {
  const std::string n = normalise_wake(text);
  int count = 0;
  for (size_t i = 0; i < n.size(); i += static_cast<size_t>(seq_len(static_cast<unsigned char>(n[i]))))
    ++count;
  return count;
}

bool wake_phrase_armed(const std::string& phrase) { return wake_chars(phrase) >= kWakeMinChars; }

std::string wake_phrase_problem(const std::string& phrase) {
  const int n = wake_chars(phrase);
  if (n == 0) return std::string();  // empty, or punctuation only: off, not broken
  if (n < kWakeMinChars)
    return "Too short to listen for. It needs at least " + std::to_string(kWakeMinChars) +
           " letters, because the match has no word boundaries to stop it firing inside "
           "ordinary words.";
  return std::string();
}

bool wake_match(const std::string& heard, const std::string& phrase) {
  if (!wake_phrase_armed(phrase)) return false;
  const std::string needle = normalise_wake(phrase);
  const std::string hay = normalise_wake(heard);
  return hay.find(needle) != std::string::npos;
}

}  // namespace aii
