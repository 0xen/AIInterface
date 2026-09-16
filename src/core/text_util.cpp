#include "core/text_util.h"

#include <cctype>

namespace aii {
namespace {

// Decode one UTF-8 code point starting at s[i]; advances i.
uint32_t next_cp(const std::string& s, size_t& i) {
  unsigned char c = (unsigned char)s[i];
  uint32_t cp;
  int extra;
  if (c < 0x80) { cp = c; extra = 0; }
  else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
  else { ++i; return 0xFFFD; }
  ++i;
  for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
  return cp;
}

bool is_japanese_cp(uint32_t cp) {
  return (cp >= 0x3040 && cp <= 0x30FF) ||   // hiragana + katakana
         (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK unified ideographs
         (cp >= 0xFF66 && cp <= 0xFF9F) ||   // half-width katakana
         (cp >= 0x3400 && cp <= 0x4DBF);
}

}  // namespace

bool has_japanese(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) if (is_japanese_cp(next_cp(s, i))) return true;
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
    const Script sc = script_of(next_cp(s, i));
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
    uint32_t cp = next_cp(s, i);
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

}  // namespace aii
