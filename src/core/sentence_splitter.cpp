#include "core/sentence_splitter.h"

#include "core/text_util.h"

namespace aii {
namespace {

// Japanese full-width terminators as UTF-8.
const char* kJaEnds[] = {"\xE3\x80\x82" /* 。 */, "\xEF\xBC\x81" /* ！ */, "\xEF\xBC\x9F" /* ？ */};

// Returns the index one past the end of the first complete sentence in s, or npos.
// `final` allows a trailing ASCII terminator without a following space.
size_t find_sentence_end(const std::string& s, bool final) {
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\n') return i + 1;
    if (c == '.' || c == '!' || c == '?') {
      // Avoid splitting decimals like 3.14 or abbreviations followed by lowercase.
      if (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])) continue;
      if (i + 1 >= s.size()) return final ? i + 1 : std::string::npos;
      char n = s[i + 1];
      if (n == ' ' || n == '\n' || n == '\r' || n == '\t') return i + 1;
      // Run of terminators like "?!" or "..."
      if (n == '.' || n == '!' || n == '?') continue;
      // Closing quote/bracket directly after the terminator.
      if (n == '"' || n == '\'' || n == ')' || n == ']') return i + 2;
      continue;
    }
    for (const char* e : kJaEnds) {
      if (s.compare(i, 3, e) == 0) return i + 3;
    }
  }
  return std::string::npos;
}

}  // namespace

void SentenceSplitter::feed(const std::string& delta) {
  buf_ += delta;
  scan(false);
}

void SentenceSplitter::flush() {
  scan(true);
  if (!in_code_) {
    std::string rest = trim(buf_);
    if (!rest.empty()) emit_sentence(rest);
  }
  buf_.clear();
  in_code_ = false;
}

void SentenceSplitter::reset() {
  buf_.clear();
  in_code_ = false;
}

void SentenceSplitter::scan(bool final) {
  for (;;) {
    if (in_code_) {
      size_t close = buf_.find("```");
      if (close == std::string::npos) {
        if (final) buf_.clear();
        return;
      }
      buf_.erase(0, close + 3);
      in_code_ = false;
      continue;
    }
    size_t fence = buf_.find("```");
    size_t end = find_sentence_end(buf_, final);
    if (fence != std::string::npos && (end == std::string::npos || fence < end)) {
      std::string before = buf_.substr(0, fence);
      buf_.erase(0, fence + 3);
      in_code_ = true;
      std::string t = trim(before);
      if (!t.empty()) emit_sentence(t);
      continue;
    }
    if (end == std::string::npos) return;
    std::string sentence = buf_.substr(0, end);
    buf_.erase(0, end);
    emit_sentence(sentence);
  }
}

void SentenceSplitter::emit_sentence(const std::string& s) {
  std::string clean = strip_markdown(s);
  if (clean.empty() || !has_speakable_content(clean)) return;
  emit_(clean);
}

}  // namespace aii
