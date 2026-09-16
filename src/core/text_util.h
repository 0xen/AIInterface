#pragma once
#include <string>

namespace aii {

// True if the UTF-8 text contains hiragana, katakana or CJK ideographs.
bool has_japanese(const std::string& utf8);
// True if the text has at least one letter, digit or CJK character (worth speaking).
bool has_speakable_content(const std::string& utf8);
// Remove light markdown decoration (*, _, `, leading #/-/* bullets) for speech.
std::string strip_markdown(const std::string& utf8);
std::string trim(const std::string& s);

}  // namespace aii
