#include "core/text_util.h"

// The one thing in this file that is not standard library, and the reason is
// that `widen()` is a Windows conversion and there is no portable spelling of
// it. It costs the standalone tests that compile this file nothing: every one
// of them is an MSVC target already and `MultiByteToWideChar` lives in
// kernel32, which links by default.
#include <windows.h>

#include <cctype>

namespace aii {

// The contract, including every malformed case and which of the three old
// decoders did what with it, is in the header.
std::uint32_t utf8_next(const std::string& s, std::size_t& i) {
  const auto at = [&s](std::size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char c = at(i);
  if (c < 0x80) {
    ++i;
    return c;
  }
  std::size_t len;
  std::uint32_t cp;
  std::uint32_t lowest;  // the smallest code point this length may legally spell
  if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; lowest = 0x80u; }
  else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; lowest = 0x800u; }
  else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; lowest = 0x10000u; }
  else { ++i; return kUtf8Replacement; }
  if (i + len > s.size()) {  // truncated by the end of the string
    ++i;
    return kUtf8Replacement;
  }
  for (std::size_t k = 1; k < len; ++k) {
    const unsigned char t = at(i + k);
    if ((t & 0xC0) != 0x80) {  // not a continuation byte: the lead was a lie
      ++i;
      return kUtf8Replacement;
    }
    cp = (cp << 6) | (t & 0x3Fu);
  }
  // Overlongs, surrogates and anything past the last plane are well-formed as
  // bit patterns and are not characters. Accepting them is how a NUL arrives
  // spelled as two bytes and how a lone surrogate reaches a draw call.
  if (cp < lowest || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
    ++i;
    return kUtf8Replacement;
  }
  i += len;
  return cp;
}

namespace {

bool is_japanese_cp(uint32_t cp) {
  return (cp >= 0x3040 && cp <= 0x30FF) ||   // hiragana + katakana
         (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK unified ideographs
         (cp >= 0xFF66 && cp <= 0xFF9F) ||   // half-width katakana
         (cp >= 0x3400 && cp <= 0x4DBF);
}

}  // namespace

bool has_japanese(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) if (is_japanese_cp(utf8_next(s, i))) return true;
  return false;
}

// A code point either picks a voice or goes along with whichever voice is
// already speaking. Digits and punctuation are shared between the two
// languages, so they are Neutral and never force a switch.
enum class Script { Neutral, Latin, Japanese };

Script script_of(uint32_t cp) {
  if (is_japanese_cp(cp)) return Script::Japanese;
  // Fullwidth forms and CJK punctuation belong to whatever is being read.
  if (cp >= 0x3000 && cp <= 0x303F) return Script::Neutral;   // 、。「」…
  if (cp >= 0xFF00 && cp <= 0xFF65) return Script::Neutral;   // fullwidth ASCII
  if (cp < 0x80) return std::isalpha((int)cp) ? Script::Latin : Script::Neutral;
  // Latin-1 and the Latin extensions: accented letters are still English-ish.
  if (cp <= 0x024F) return Script::Latin;
  return Script::Neutral;
}

std::vector<ScriptRun> split_by_script(const std::string& s) {
  std::vector<ScriptRun> runs;
  std::string pending;  // neutral text seen before any voice was chosen
  size_t i = 0;
  while (i < s.size()) {
    const size_t start = i;
    const Script sc = script_of(utf8_next(s, i));
    const std::string piece = s.substr(start, i - start);
    if (sc == Script::Neutral) {
      if (runs.empty()) pending += piece;
      else runs.back().text += piece;
      continue;
    }
    const bool ja = sc == Script::Japanese;
    if (runs.empty() || runs.back().japanese != ja) {
      runs.push_back({ja, std::string()});
      if (!pending.empty()) {
        runs.back().text = std::move(pending);
        pending.clear();
      }
    }
    runs.back().text += piece;
  }
  if (runs.empty()) {
    if (!pending.empty()) runs.push_back({false, std::move(pending)});
    return runs;
  }
  // Fold away anything not worth its own trip through an engine: a run with
  // no speakable content, or one so short it is almost certainly a loanword
  // or a stray letter inside the other language's sentence.
  constexpr size_t kMinRunBytes = 2;
  std::vector<ScriptRun> out;
  std::string carry;  // folded text waiting for a run to attach to
  for (const auto& r : runs) {
    const bool substantial = has_speakable_content(r.text) && trim(r.text).size() >= kMinRunBytes;
    if (!substantial || (!out.empty() && out.back().japanese == r.japanese)) {
      if (out.empty()) carry += r.text;
      else out.back().text += r.text;
      continue;
    }
    out.push_back({r.japanese, carry + r.text});
    carry.clear();
  }
  if (!carry.empty()) {
    if (out.empty()) out.push_back({false, carry});
    else out.back().text += carry;
  }
  return out;
}

bool has_speakable_content(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    uint32_t cp = utf8_next(s, i);
    if (cp < 0x80 ? std::isalnum((int)cp) != 0 : is_japanese_cp(cp) || cp >= 0xC0) return true;
  }
  return false;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') ++a;
  while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
  return s.substr(a, b - a);
}

std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                    nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
  return w;
}

std::string clip_utf8(std::string s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  // s[max_bytes] is the first byte that will be dropped. If it is a lead byte
  // (or plain ASCII) the cap already falls on a code point boundary; if it is a
  // continuation byte, a sequence straddles the cap and must go entirely.
  std::size_t cut = max_bytes;
  // A code point is at most 4 bytes, so a valid straddling sequence starts no
  // more than 3 bytes back. Anything further is malformed input, and for that
  // the honest answer is the byte cap itself.
  const std::size_t floor = max_bytes >= 3 ? max_bytes - 3 : 0;
  while (cut > floor && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  if ((static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut = max_bytes;
  s.resize(cut);
  return s;
}

std::string strip_markdown(const std::string& in) {
  std::string s = trim(in);
  // Leading heading marks / bullets.
  size_t k = 0;
  while (k < s.size() && s[k] == '#') ++k;
  if (k > 0 && k < s.size() && s[k] == ' ') s.erase(0, k + 1);
  if (s.size() >= 2 && (s[0] == '-' || s[0] == '*') && s[1] == ' ') s.erase(0, 2);
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '*' || c == '`' || c == '_') continue;
    out.push_back(c);
  }
  return trim(out);
}

int estimate_tokens(const std::string& utf8) {
  // Per script run, so a Japanese sentence inside an English paragraph is
  // charged at the Japanese rate and nothing else is.
  double tokens = 0.0;
  bool any = false;
  for (const ScriptRun& run : split_by_script(utf8)) {
    size_t chars = 0, i = 0;
    while (i < run.text.size()) { utf8_next(run.text, i); ++chars; }
    if (chars == 0) continue;
    any = true;
    tokens += static_cast<double>(chars) / (run.japanese ? 1.6 : 3.6);
  }
  if (!any) return 0;
  const int n = static_cast<int>(tokens + 0.5);
  return n < 1 ? 1 : n;
}

}  // namespace aii
