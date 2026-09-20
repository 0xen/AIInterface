#pragma once
// M13.1: the inline directive channel, pulled out of the spoken stream.
//
// A reply can carry machine traffic in square brackets -- `[v2]` to change
// voice today, `[pause]` or `[speed:0.8]` later -- and this is the one parser
// that takes it out. It sits in front of *both* consumers of a delta (the
// transcript, which is plain concatenation, and the SentenceSplitter, which
// feeds synthesis), so "a marker reaches neither the eyes nor the ears" is a
// property of where this runs rather than an agreement between two strippers
// that could drift apart. See `docs/design-directives.md`.
//
// ## The grammar, and why it is this strict
//
//     directive := '[' token ( ':' value )? ']'
//     token     := [a-z] [a-z0-9_]{0,15}
//     value     := [A-Za-z0-9._-]{1,15}
//
// No whitespace anywhere inside, no nesting, a hard cap of `kMaxDirective`
// bytes including both brackets. The strictness is what makes the namespace
// safe to expand into: `[see figure 1]` and `[WARN]` are prose and survive
// untouched, so adding `[emote:happy]` later cannot start eating English.
//
// The cap is load-bearing rather than cosmetic -- it is what bounds the
// streaming holdback below to a known number of bytes.
//
// ## Streaming, which is the whole difficulty
//
// Deltas arrive at whatever size the CLI chose, so `[v` can land in one and
// `2]` in the next. The rule is: **hold back only a live candidate at the tail
// of the buffer, and nothing else.**
//
//   * No `[` in the buffer -> everything is emitted at once. Zero holdback.
//   * A `[` whose following bytes already violate the grammar -> prose,
//     emitted immediately, scanning resumes at the character *after* that `[`
//     so a stray bracket cannot swallow a real directive later in the line.
//   * A `[` still grammar-legal but unterminated, running to the end of the
//     buffer -> held back until the next delta, or released verbatim at
//     `flush()`. **Nothing is ever lost.**
//
// Measured on a real dialogue reply: 0 bytes of holdback for text containing no
// `[`, 3 bytes at worst with markers present, and a marker at the head of a
// reply delays first audio by exactly its own length.
//
// ## Fences
//
// A directive is never valid inside a ``` fence: a model that writes `[v2]` in
// a code block has written code. So this tracks fences itself and passes their
// contents through verbatim. That leaves two backtick state machines in series
// -- this one and the splitter's -- which is not a divergence risk, because
// both key off the same three-backtick token in the same byte stream and this
// one emits the fence markers unchanged.
#include <functional>
#include <string>

namespace aii {

// Including both brackets. Over this, a candidate is prose.
inline constexpr size_t kMaxDirective = 32;

class DirectiveFilter {
 public:
  // `on_text` receives the reply with every directive removed, in order, in
  // whatever chunks scanning produces -- it is a stream, not a line.
  // `on_directive` receives a parsed one: token, and value (empty when the
  // directive carried none). A directive that parses is *always* consumed,
  // including one this build does not recognise, which is the point of having
  // a grammar at all -- an unknown marker is logged, never read aloud.
  using OnText = std::function<void(const std::string& text)>;
  using OnDirective = std::function<void(const std::string& token, const std::string& value)>;

  DirectiveFilter(OnText on_text, OnDirective on_directive)
      : on_text_(std::move(on_text)), on_directive_(std::move(on_directive)) {}

  void feed(const std::string& delta);
  // End of reply: releases any held-back candidate verbatim, so a truncated
  // `trailing [v2` is shown and spoken exactly as it arrived.
  void flush();
  void reset();

 private:
  void scan(bool final);

  OnText on_text_;
  OnDirective on_directive_;
  std::string buf_;        // holdback only: a live candidate at the tail
  bool in_code_ = false;   // inside a ``` fence
};

}  // namespace aii
