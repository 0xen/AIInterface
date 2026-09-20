#include "core/directive_filter.h"

namespace aii {
namespace {

bool is_token_start(char c) { return c >= 'a' && c <= 'z'; }
bool is_token_rest(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
bool is_value_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
         c == '_' || c == '-';
}

enum class Verdict { Complete, Incomplete, Prose };

// Reads a candidate directive starting at `s[i]`, which the caller has already
// checked is '['.
//
// `Incomplete` means "grammar-legal so far and the buffer ran out" -- the only
// case that is ever held back. Everything else is decided on bytes already in
// hand, which is why prose costs no latency at all.
Verdict match(const std::string& s, size_t i, size_t* len, std::string* token,
              std::string* value) {
  const size_t n = s.size();
  size_t j = i + 1;
  // The cap is checked as we go rather than at the end: a candidate that has
  // already run past it is prose *now*, and must not keep the tail held back
  // waiting for a ']' that would be too late to matter.
  const auto over_cap = [&](size_t at) { return at - i + 1 > kMaxDirective; };

  if (j >= n) return Verdict::Incomplete;
  if (!is_token_start(s[j])) return Verdict::Prose;
  const size_t tok_start = j;
  ++j;
  while (j < n && is_token_rest(s[j])) {
    if (j - tok_start >= 16) return Verdict::Prose;  // token := [a-z][a-z0-9_]{0,15}
    if (over_cap(j)) return Verdict::Prose;
    ++j;
  }
  if (j >= n) return over_cap(j - 1) ? Verdict::Prose : Verdict::Incomplete;
  token->assign(s, tok_start, j - tok_start);

  if (s[j] == ']') {
    if (over_cap(j)) return Verdict::Prose;
    value->clear();
    *len = j + 1 - i;
    return Verdict::Complete;
  }
  if (s[j] != ':') return Verdict::Prose;  // a space, an uppercase letter, anything else

  ++j;
  const size_t val_start = j;
  while (j < n && is_value_char(s[j])) {
    if (j - val_start >= 15) return Verdict::Prose;  // value := [...]{1,15}
    if (over_cap(j)) return Verdict::Prose;
    ++j;
  }
  if (j == val_start) {
    // `[v2:]` and `[v2:` differ: the first has seen a character that cannot be
    // a value and is prose; the second has seen nothing yet and may still become
    // one.
    return j >= n ? Verdict::Incomplete : Verdict::Prose;
  }
  if (j >= n) return over_cap(j - 1) ? Verdict::Prose : Verdict::Incomplete;
  if (s[j] != ']') return Verdict::Prose;
  if (over_cap(j)) return Verdict::Prose;
  value->assign(s, val_start, j - val_start);
  *len = j + 1 - i;
  return Verdict::Complete;
}

}  // namespace

void DirectiveFilter::feed(const std::string& delta) {
  buf_ += delta;
  scan(false);
}

void DirectiveFilter::flush() {
  scan(true);
  if (!buf_.empty()) {
    // Whatever is left was a candidate that never terminated. It goes out as
    // written: a reply that ends `trailing [v2` is shown and spoken that way
    // rather than losing its last four characters.
    on_text_(buf_);
    buf_.clear();
  }
  in_code_ = false;
}

void DirectiveFilter::reset() {
  buf_.clear();
  in_code_ = false;
}

void DirectiveFilter::scan(bool final) {
  size_t i = 0;
  std::string out;
  // Text is flushed to the consumer at every directive, because the order the
  // two callbacks fire in is what the caller relies on: the words before a
  // marker must reach the splitter before the marker moves the voice, or the
  // tail of one speaker's line is spoken in the next speaker's voice.
  const auto emit_text = [&] {
    if (out.empty()) return;
    on_text_(out);
    out.clear();
  };

  bool stalled = false;  // a live candidate at the tail: stop, keep it, wait
  while (i < buf_.size() && !stalled) {
    const size_t n = buf_.size();

    if (in_code_) {
      const size_t close = buf_.find("```", i);
      if (close == std::string::npos) {
        // Hold back a trailing partial fence so it is never half-seen. Anything
        // before it is ordinary fenced text and goes now. At most two, because
        // three would have been the fence itself.
        size_t keep = n;
        if (!final) {
          for (size_t bt = 0; bt < 2 && keep > i && buf_[keep - 1] == '`'; ++bt) --keep;
        }
        out.append(buf_, i, keep - i);
        i = keep;
        break;
      }
      out.append(buf_, i, close + 3 - i);
      i = close + 3;
      in_code_ = false;
      continue;
    }

    const char c = buf_[i];

    if (c == '`') {
      if (n - i >= 3 && buf_.compare(i, 3, "```") == 0) {
        out.append("```");
        i += 3;
        in_code_ = true;
        continue;
      }
      if (!final && n - i < 3) break;  // might yet become a fence
      out.push_back(c);
      ++i;
      continue;
    }

    if (c == '[') {
      size_t len = 0;
      std::string token, value;
      switch (match(buf_, i, &len, &token, &value)) {
        case Verdict::Complete:
          emit_text();
          on_directive_(token, value);
          i += len;
          continue;
        case Verdict::Incomplete:
          if (!final) {
            // The one case that costs anything: this and everything after it
            // waits for the next delta.
            stalled = true;
            continue;
          }
          // At end of reply an unterminated candidate is just text.
          out.push_back(c);
          ++i;
          continue;
        case Verdict::Prose:
          // Resume at the character *after* the bracket, not after the point
          // the grammar failed: `nested [see [v2] figure]` must still find the
          // real directive inside it.
          out.push_back(c);
          ++i;
          continue;
      }
    }

    out.push_back(c);
    ++i;
  }

  emit_text();
  buf_.erase(0, i);
}

}  // namespace aii
