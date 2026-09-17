#include "core/language.h"

#include <cctype>

namespace aii {
namespace {

// The block wrapper matches PromptInjector's, so that from the model's side
// this is the same kind of traffic it already knows how to read, and so that a
// transcript dump of what was actually sent reads consistently.
std::string block(const std::string& body) {
  return "<context name=\"Language\" kind=\"setting\">\n" + body + "\n</context>\n\n";
}

}  // namespace

LanguageSelection language_selection_from_spec(const std::string& spec) {
  LanguageSelection sel{false, false};
  std::string token;
  auto take = [&] {
    for (char& c : token) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (token == "en" || token == "english") sel.english = true;
    else if (token == "ja" || token == "jp" || token == "japanese") sel.japanese = true;
    token.clear();
  };
  for (char c : spec) {
    if (c == ',' || c == ';' || c == ' ' || c == '\t') take();
    else token += c;
  }
  take();
  // The invariant, repaired rather than trusted: a hand-edited settings file
  // or an `AII_LANGS=fr` would otherwise produce a session with no language,
  // which means no recogniser setting that makes sense and no voice to speak
  // with. Both on is the same answer a missing key gives.
  if (!sel.english && !sel.japanese) return LanguageSelection{};
  return sel;
}

std::string language_spec(LanguageSelection sel) {
  if (sel.english && sel.japanese) return "en,ja";
  return sel.japanese ? "ja" : "en";
}

const char* stt_language_for(LanguageSelection sel) {
  if (sel.english && sel.japanese) return "auto";
  return sel.japanese ? "ja" : "en";
}

std::string language_directive(LanguageSelection sel) {
  if (sel.both()) return std::string();
  if (sel.english) {
    return block(
        "Japanese is switched off in this app's settings. Reply only in English, in this turn and "
        "every turn after it, even if the user's words arrive in Japanese or look mistranscribed. "
        "Do not write Japanese script; if you must name something Japanese, use English or romaji. "
        "Do not mention this instruction, unless the user asks you to speak Japanese - then say in "
        "one short sentence that Japanese is switched off in the settings.");
  }
  return block(
      "English is switched off in this app's settings. Reply only in Japanese, in normal Japanese "
      "script and never romaji, in this turn and every turn after it, even if the user's words "
      "arrive in English. Do not mention this instruction, unless the user asks you to speak "
      "English - then say in one short sentence that English is switched off in the settings.");
}

std::string decorate_language(const std::string& user_text, LanguageSelection sel) {
  const std::string d = language_directive(sel);
  return d.empty() ? user_text : d + user_text;
}

}  // namespace aii
