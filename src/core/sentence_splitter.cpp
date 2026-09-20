#include "core/sentence_splitter.h"

#include <cctype>

#include "core/text_util.h"

namespace aii {
namespace {

// Japanese full-width terminators as UTF-8.
const char* kJaEnds[] = {"\xE3\x80\x82" /* 。 */, "\xEF\xBC\x81" /* ！ */, "\xEF\xBC\x9F" /* ？ */};
const char* kJaComma = "\xE3\x80\x81"; /* 、 */

bool is_space(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

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
      if (is_space(n)) return i + 1;
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

// Early break for the first chunk of a reply: one past a ", " or "、", or the
// whitespace boundary after `max_words` words. npos if none is available yet.
size_t find_early_break(const std::string& s, int max_words) {
  if (max_words <= 0) return std::string::npos;
  int words = 0;
  bool in_word = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == ',' && i + 1 < s.size() && is_space(s[i + 1])) return i + 1;
    if (s.compare(i, 3, kJaComma) == 0) return i + 3;
    if (is_space(c)) {
      if (in_word && words >= max_words) return i;   // cut at the boundary after the Nth word
      in_word = false;
    } else if (!in_word) {
      in_word = true;
      ++words;
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
  emitted_any_ = false;
}

// M13.1. A sentence boundary that is not the end of the reply.
//
// `flush()` looks like the right call for this and is not, which a probe caught
// before any of it was built (`docs/design-directives.md`, §3.3). flush() clears
// `emitted_any_`, and that re-arms the early-chunk rule for the *rest* of the
// reply: every later utterance then gets cut at its first comma, so the same
// text spoke differently depending on how the CLI happened to chunk its deltas.
// It clears `in_code_` too, which would desynchronise the fence state from the
// filter's copy of it.
//
// So this emits what is pending and touches neither flag. A directive is a hard
// boundary -- the voice must not change inside an utterance already handed to
// the queue -- and that is the whole of what it has to buy.
void SentenceSplitter::break_now() {
  scan(false);
  if (!in_code_) {
    std::string rest = trim(buf_);
    if (!rest.empty()) {
      emit_sentence(rest);
      buf_.clear();
    }
  }
}

void SentenceSplitter::reset() {
  buf_.clear();
  in_code_ = false;
  emitted_any_ = false;
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
    if (end == std::string::npos && !emitted_any_) {
      end = find_early_break(buf_, early_words_);
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
  emitted_any_ = true;
  emit_(clean);
}

}  // namespace aii
