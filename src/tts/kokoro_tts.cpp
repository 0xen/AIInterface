#include "tts/kokoro_tts.h"

#include <cstring>
#include <filesystem>

#include "sherpa-onnx/c-api/c-api.h"

namespace aii {

KokoroTts::KokoroTts(const std::string& model_dir, int speaker_id, float speed, int num_threads)
    : sid_(speaker_id), speed_(speed) {
  model_ = model_dir + "/model.onnx";
  voices_ = model_dir + "/voices.bin";
  tokens_ = model_dir + "/tokens.txt";
  data_dir_ = model_dir + "/espeak-ng-data";
  lexicon_ = model_dir + "/lexicon-us-en.txt";
  if (std::filesystem::exists(model_dir + "/lexicon-zh.txt")) lexicon_ += "," + model_dir + "/lexicon-zh.txt";

  SherpaOnnxOfflineTtsConfig cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.model.kokoro.model = model_.c_str();
  cfg.model.kokoro.voices = voices_.c_str();
  cfg.model.kokoro.tokens = tokens_.c_str();
  cfg.model.kokoro.data_dir = data_dir_.c_str();
  cfg.model.kokoro.lexicon = lexicon_.c_str();
  cfg.model.kokoro.length_scale = 1.0f;
  cfg.model.num_threads = num_threads;
  cfg.model.debug = 0;
  cfg.model.provider = "cpu";
  cfg.max_num_sentences = 1;
  cfg.silence_scale = 0.2f;
  tts_ = SherpaOnnxCreateOfflineTts(&cfg);
  if (tts_) rate_ = SherpaOnnxOfflineTtsSampleRate(tts_);
}

KokoroTts::~KokoroTts() {
  if (tts_) SherpaOnnxDestroyOfflineTts(tts_);
}

bool KokoroTts::synthesize(const std::string& text, AudioChunk& out) {
  if (!tts_) return false;
  const SherpaOnnxGeneratedAudio* audio = SherpaOnnxOfflineTtsGenerate(tts_, text.c_str(), sid_, speed_);
  if (!audio) return false;
  out.sample_rate = audio->sample_rate;
  out.samples.assign(audio->samples, audio->samples + audio->n);
  SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
  return !out.samples.empty();
}

}  // namespace aii
