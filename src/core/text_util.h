#pragma once
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

}  // namespace aii
