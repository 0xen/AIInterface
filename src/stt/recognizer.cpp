#include "stt/recognizer.h"

#include <cstring>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

namespace aii {

Recognizer::Recognizer(const std::string& model_dir, int num_threads) {
  encoder_ = model_dir + "/encoder.int8.onnx";
  decoder_ = model_dir + "/decoder.int8.onnx";
  joiner_ = model_dir + "/joiner.int8.onnx";
  tokens_ = model_dir + "/tokens.txt";

  SherpaOnnxOnlineRecognizerConfig config;
  std::memset(&config, 0, sizeof(config));
  config.feat_config.sample_rate = 16000;
  config.feat_config.feature_dim = 80;
  config.model_config.transducer.encoder = encoder_.c_str();
  config.model_config.transducer.decoder = decoder_.c_str();
  config.model_config.transducer.joiner = joiner_.c_str();
  config.model_config.tokens = tokens_.c_str();
  config.model_config.num_threads = num_threads;
  config.model_config.provider = "cpu";
  config.model_config.debug = 0;
  config.decoding_method = "greedy_search";
  config.max_active_paths = 4;
  config.enable_endpoint = 0;  // push-to-talk: the user decides where the utterance ends
  config.rule1_min_trailing_silence = 2.4f;
  config.rule2_min_trailing_silence = 1.2f;
  config.rule3_min_utterance_length = 300.0f;
  recognizer_ = SherpaOnnxCreateOnlineRecognizer(&config);
}

Recognizer::~Recognizer() {
  destroy_stream();
  if (recognizer_) SherpaOnnxDestroyOnlineRecognizer(recognizer_);
}

void Recognizer::set_language(const std::string& lang) { language_ = lang; }

void Recognizer::create_stream() {
  stream_ = SherpaOnnxCreateOnlineStream(recognizer_);
  SherpaOnnxOnlineStreamSetOption(stream_, "language", language_.c_str());
}

void Recognizer::destroy_stream() {
  if (stream_) SherpaOnnxDestroyOnlineStream(stream_);
  stream_ = nullptr;
}

void Recognizer::begin() {
  destroy_stream();
  create_stream();
}

void Recognizer::feed(const float* samples, int n, int rate) {
  if (!stream_) create_stream();
  SherpaOnnxOnlineStreamAcceptWaveform(stream_, rate, samples, n);
  while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_)) {
    SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
  }
}

std::string Recognizer::partial() {
  if (!stream_) return {};
  const SherpaOnnxOnlineRecognizerResult* r = SherpaOnnxGetOnlineStreamResult(recognizer_, stream_);
  std::string text = (r && r->text) ? r->text : "";
  SherpaOnnxDestroyOnlineRecognizerResult(r);
  return text;
}

std::string Recognizer::finish() {
  if (!stream_) return {};
  std::vector<float> tail(16000, 0.0f);  // 1 s of silence flushes the encoder
  SherpaOnnxOnlineStreamAcceptWaveform(stream_, 16000, tail.data(), static_cast<int>(tail.size()));
  SherpaOnnxOnlineStreamInputFinished(stream_);
  while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_)) {
    SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
  }
  std::string text = partial();
  destroy_stream();
  return text;
}

}  // namespace aii
