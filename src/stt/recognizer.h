#pragma once
// Streaming speech recogniser (sherpa-onnx online transducer, Nemotron-3.5).
#include <string>

struct SherpaOnnxOnlineRecognizer;
struct SherpaOnnxOnlineStream;

namespace aii {

class Recognizer {
 public:
  Recognizer(const std::string& model_dir, int num_threads);
  ~Recognizer();
  Recognizer(const Recognizer&) = delete;
  Recognizer& operator=(const Recognizer&) = delete;

  bool ok() const { return recognizer_ != nullptr; }

  // "auto", "en", "ja", ...  Applies to the next utterance.
  void set_language(const std::string& lang);

  void begin();                                     // start a fresh utterance
  void feed(const float* samples, int n, int rate); // decode as much as available
  std::string partial();                            // text so far
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
