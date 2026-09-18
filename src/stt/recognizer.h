#pragma once
// Streaming speech recogniser (sherpa-onnx online transducer, Nemotron-3.5).
#include <string>
#include <vector>

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

  // M8.4, the segmental re-decode. Only ever runs while the language is
  // "auto" — that is, only when both languages are on, which is the one
  // configuration in which the deletion happens (a pinned recogniser does not
  // delete, which is what the language toggles already buy). Off switches it
  // for the offline harness, which measures the same audio both ways.
  void set_redecode(bool on) { redecode_ = on; }

  // What the last finish() did about it. Nothing reads this in the app; the
  // harness scores it, and it is what a log line would print.
  struct RedecodeInfo {
    bool fired = false;          // the detector found a suspect span
    float cut_a = -1.0f, cut_b = -1.0f;  // the window handed to the second decode
    float gap = 0.0f;            // the loud token-timestamp gap that fired it
    std::string decoded;         // raw text of that language=ja decode
    bool spliced = false;        // accepted and written into the returned text
    float ms = 0.0f;             // wall-clock cost of the second decode
  };
  const RedecodeInfo& last_redecode() const { return redecode_info_; }

 private:
  void create_stream();
  void destroy_stream();
  // M8.4. See the .cpp: find the span the decoder emitted nothing for, decode
  // it again pinned to Japanese, and splice the result back in.
  std::string redecode_and_splice(const std::vector<std::string>& tokens,
                                  const std::vector<float>& times);
  std::string decode_segment(const std::vector<float>& pcm, const char* lang);

  const SherpaOnnxOnlineRecognizer* recognizer_ = nullptr;
  const SherpaOnnxOnlineStream* stream_ = nullptr;
  std::string language_ = "auto";
  std::string encoder_, decoder_, joiner_, tokens_;

  // M8.4. The utterance's own PCM, 16 kHz mono, kept so that a span of it can
  // be decoded a second time. Capped (see kMaxBufferSec in the .cpp); past the
  // cap the re-decode is skipped rather than run against a buffer whose
  // timestamps no longer line up with the decoder's.
  std::vector<float> audio_;
  bool audio_usable_ = true;
  bool redecode_ = true;
  RedecodeInfo redecode_info_;
};

}  // namespace aii
