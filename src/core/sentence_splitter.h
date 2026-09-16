#pragma once
// Turns a stream of text deltas into complete sentences for synthesis.
// Text inside ``` code fences is shown but never spoken.
#include <functional>
#include <string>

namespace aii {

class SentenceSplitter {
 public:
  using Emit = std::function<void(const std::string& sentence)>;
  explicit SentenceSplitter(Emit emit) : emit_(std::move(emit)) {}

  void feed(const std::string& delta);
  void flush();   // emit whatever is left (end of reply)
  void reset();

 private:
  void scan(bool final);
  void emit_sentence(const std::string& s);

  Emit emit_;
  std::string buf_;
  bool in_code_ = false;
};

}  // namespace aii
