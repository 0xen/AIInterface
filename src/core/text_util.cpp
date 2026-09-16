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
