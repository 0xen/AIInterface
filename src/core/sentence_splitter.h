#pragma once
// Turns a stream of text deltas into complete sentences for synthesis.
// Text inside ``` code fences is shown but never spoken.
//
// To cut time-to-first-audio, the FIRST chunk of a reply may be emitted early:
// at a comma (", " or "、") or after `early_words` words at a whitespace
// boundary. Every later chunk waits for a full sentence so prosody stays natural.
#include <functional>
#include <string>

namespace aii {

class SentenceSplitter {
 public:
  using Emit = std::function<void(const std::string& sentence)>;
  explicit SentenceSplitter(Emit emit, int early_words = 12)
      : emit_(std::move(emit)), early_words_(early_words) {}

  void feed(const std::string& delta);
  void flush();   // emit whatever is left (end of reply)
  void reset();

 private:
  void scan(bool final);
  void emit_sentence(const std::string& s);

  Emit emit_;
  int early_words_;          // 0 disables the early first chunk
  std::string buf_;
  bool in_code_ = false;
  bool emitted_any_ = false;
};

}  // namespace aii
