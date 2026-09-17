#pragma once
// Streaming speech recogniser (sherpa-onnx online transducer, Nemotron-3.5).
#include <string>

struct SherpaOnnxOnlineRecognizer;
struct SherpaOnnxOnlineStream;

namespace aii {

class Recognizer {
 public:
  // `endpoint_silence` is the trailing pause, in seconds, after which the
  // decoder is willing to call the utterance finished. It counts frames the
  // transducer decoded as blank, which is not the same as the microphone
  // being quiet — a slow or hesitant speaker produces blanks mid-sentence —
  // so callers that act on is_endpoint() should corroborate it with the
  // actual input level rather than trusting it alone.
  Recognizer(const std::string& model_dir, int num_threads, float endpoint_silence = 1.0f);
  ~Recognizer();
  Recognizer(const Recognizer&) = delete;
  Recognizer& operator=(const Recognizer&) = delete;

  bool ok() const { return recognizer_ != nullptr; }

  // "auto", "en", "ja", ...
  //
  // Applies to the next utterance *and* to the one in flight. sherpa-onnx
  // re-reads this option through `GetOption()` on every 560 ms chunk inside
  // `DecodeStreams()` (measured 16 Sep 2026), so setting it on the live stream
  // takes effect from the next chunk rather than from the next `begin()`. That
  // is what makes a settings toggle honest: a user who switches Japanese off
  // while the microphone is open does not get one more utterance of the old
  // behaviour. Setting it to the value it already holds is free.
  void set_language(const std::string& lang);

  void begin();                                     // start a fresh utterance
  void feed(const float* samples, int n, int rate); // decode as much as available
  std::string partial();                            // text so far
  // True once the decoder has seen enough trailing silence to call the
  // utterance finished (the rule* thresholds in the .cpp). Push-to-talk
  // ignores this; hold-to-talk uses it to send on a natural pause.
  bool is_endpoint();
  std::string finish();                             // flush, return final text

 private:
  void create_stream();
  void destroy_stream();

  const SherpaOnnxOnlineRecognizer* recognizer_ = nullptr;
  const SherpaOnnxOnlineStream* stream_ = nullptr;
  std::string language_ = "auto";
  std::string encoder_, decoder_, joiner_, tokens_;
};

}  // namespace aii
