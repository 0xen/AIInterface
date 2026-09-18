#include "stt/recognizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

namespace aii {
namespace {

// ---------------------------------------------------------------------------
// M8.4. Segmental re-decode.
// ---------------------------------------------------------------------------
//
// The failure this exists for, measured twice (16 Sep 2026 and 18 Sep 2026,
// 192 code-switched utterances, `docs/research-multilingual-input.md`): with
// the language left at "auto", a short Japanese word inside an English
// sentence is **silently deleted** — 78% of the time when any English precedes
// it — and the output is byte-identical to the same sentence with the Japanese
// removed. The language toggles fixed the case where one language is off, by
// pinning the recogniser. With both on there is nothing to pin to and the
// number is unchanged, so this is what is left.
//
// The shape: the deletion leaves a hole that is visible without any second
// model. The decoder emits no token for the span, so there is a gap between
// consecutive token timestamps; the microphone was not quiet across it, so the
// app's own noise gate says the user was talking. "Loud, and nothing came out"
// is the whole detector. Measured over the corpus it catches 90% of the
// deletions and fires on **none** of 88 s of ordinary English speech.
//
// Then: cut that span out of the utterance's own PCM, pad it with silence,
// decode it on a second stream of the *same* recogniser with language="ja" —
// isolated, the model gets it right 69-75% of the time — and splice the text
// back in where the hole was. No second model, no second load.

// Long enough to be a word and not a breath, and above the largest loud gap
// found in 88 s of ordinary English (0.56 s). The threshold sweep is in the
// research document; 0.5 s costs 2 false fires per English minute, 0.6 s costs
// none and gives up 4% of the recall.
constexpr float kMinGapSec = 0.6f;
// Share of 20 ms frames across the gap the noise gate has to call speech. A
// gap that is genuinely silent is a pause, not a deleted word — and the pause
// that ends the utterance is always the largest raw gap there is, so without
// this the detector would fire on every sentence ever spoken.
constexpr float kGapLoudFrac = 0.6f;
// Silence around the cut, because the recogniser eats the first phoneme of a
// clip that starts abruptly.
constexpr float kPadSec = 0.3f;
// Past this the buffer stops growing and the re-decode is skipped. A minute of
// held microphone is 3.8 MB; an utterance that long is not the short insert
// this is for.
constexpr float kMaxBufferSec = 60.0f;
constexpr int kRate = 16000;

// The app's noise gate (voice_session.cpp), re-run over the buffered audio in
// 20 ms frames. The constants are deliberately the same numbers: "loud" here
// has to mean what it means to the rest of the app, or the detector would be a
// second opinion about the room rather than a reading of the same one.
constexpr float kFloorRate = 0.05f;
constexpr float kGateOverFloor = 3.0f;
constexpr float kGateAbsMin = 0.004f;
constexpr float kCalibrateSec = 0.3f;
constexpr float kFloorMax = 0.02f;

std::vector<char> gate_frames(const std::vector<float>& a) {
  const size_t F = kRate / 50;  // 20 ms
  std::vector<char> g(a.size() / F + 1, 0);
  float floor_v = 0.0f;
  for (size_t f = 0; f * F < a.size(); ++f) {
    const size_t n = std::min(F, a.size() - f * F);
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double v = a[f * F + i];
      sum += v * v;
    }
    const float level = static_cast<float>(std::sqrt(sum / static_cast<double>(n)));
    const float t = f * 0.02f;
    float gate = std::max(floor_v * kGateOverFloor, kGateAbsMin);
    if (t < kCalibrateSec) {
      floor_v = std::min(std::max(floor_v, level), kFloorMax);
      gate = std::max(floor_v * kGateOverFloor, kGateAbsMin);
    } else if (level < gate) {
      floor_v += (level - floor_v) * kFloorRate;
    }
    g[f] = level > gate ? 1 : 0;
  }
  return g;
}

float loud_fraction(const std::vector<char>& g, float a, float b) {
  const size_t fa = static_cast<size_t>(std::max(0.0f, a) / 0.02f);
  const size_t fb = std::min(g.size(), static_cast<size_t>(std::max(0.0f, b) / 0.02f));
  if (fb <= fa) return 0.0f;
  size_t loud = 0;
  for (size_t i = fa; i < fb; ++i) loud += static_cast<size_t>(g[i]);
  return static_cast<float>(loud) / static_cast<float>(fb - fa);
}

// A token is "not English" when it carries a byte outside ASCII. The model's
// token strings concatenate to exactly the text it returns, so this is the
// whole test — kana and kanji are multi-byte, Latin letters and punctuation
// are not.
bool non_ascii(const std::string& s) {
  for (unsigned char c : s)
    if (c >= 0x80) return true;
  return false;
}

// How many non-ASCII characters (not bytes) a string holds: UTF-8 lead bytes
// of a multi-byte sequence.
size_t non_ascii_chars(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if (c >= 0xC0) ++n;
  return n;
}

}  // namespace

Recognizer::Recognizer(const std::string& model_dir, int num_threads, float endpoint_silence) {
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
  // On, so callers can ask is_endpoint(). Nothing happens automatically:
  // sherpa only reports the boundary, the caller decides what to do with it.
  config.enable_endpoint = 1;
  config.rule1_min_trailing_silence = 2.4f;  // silence with nothing said yet
  // The pause that ends an utterance. Corroborated against the real input
  // level in the caller, because these are blank-decode frames, not silence.
  config.rule2_min_trailing_silence = endpoint_silence;
  config.rule3_min_utterance_length = 300.0f;
  recognizer_ = SherpaOnnxCreateOnlineRecognizer(&config);
}

Recognizer::~Recognizer() {
  destroy_stream();
  if (recognizer_) SherpaOnnxDestroyOnlineRecognizer(recognizer_);
}

void Recognizer::set_language(const std::string& lang) {
  if (lang == language_) return;
  language_ = lang;
  // The member *and* the live stream: create_stream() is what applies this to
  // the next utterance, and this line is what applies it to the one being
  // decoded right now. See the header for why that works.
  if (stream_) SherpaOnnxOnlineStreamSetOption(stream_, "language", language_.c_str());
}

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
  audio_.clear();
  audio_usable_ = true;
  redecode_info_ = RedecodeInfo{};
}

void Recognizer::feed(const float* samples, int n, int rate) {
  if (!stream_) create_stream();
  // M8.4. Keep the utterance's own audio. Only at the native rate: sherpa
  // resamples internally, and a buffer at a different rate would put the
  // decoder's timestamps and this buffer's offsets on different clocks.
  if (audio_usable_ && rate == kRate) {
    if (audio_.size() + static_cast<size_t>(n) > static_cast<size_t>(kMaxBufferSec * kRate)) {
      audio_usable_ = false;
      audio_.clear();
      audio_.shrink_to_fit();
    } else {
      audio_.insert(audio_.end(), samples, samples + n);
    }
  } else if (rate != kRate) {
    audio_usable_ = false;
  }
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

bool Recognizer::is_endpoint() {
  if (!stream_) return false;
  return SherpaOnnxOnlineStreamIsEndpoint(recognizer_, stream_) != 0;
}

std::string Recognizer::finish() {
  if (!stream_) return {};
  std::vector<float> tail(16000, 0.0f);  // 1 s of silence flushes the encoder
  SherpaOnnxOnlineStreamAcceptWaveform(stream_, 16000, tail.data(), static_cast<int>(tail.size()));
  SherpaOnnxOnlineStreamInputFinished(stream_);
  while (SherpaOnnxIsOnlineStreamReady(recognizer_, stream_)) {
    SherpaOnnxDecodeOnlineStream(recognizer_, stream_);
  }

  std::string text;
  std::vector<std::string> tokens;
  std::vector<float> times;
  const SherpaOnnxOnlineRecognizerResult* r = SherpaOnnxGetOnlineStreamResult(recognizer_, stream_);
  if (r) {
    text = r->text ? r->text : "";
    if (r->timestamps) {
      for (int i = 0; i < r->count; ++i) {
        tokens.emplace_back(r->tokens_arr[i] ? r->tokens_arr[i] : "");
        times.push_back(r->timestamps[i]);
      }
    }
  }
  SherpaOnnxDestroyOnlineRecognizerResult(r);
  destroy_stream();

  redecode_info_ = RedecodeInfo{};
  // Only with both languages on. Pinned to one language the deletion does not
  // happen, and re-decoding into a language the user switched off would undo
  // the toggle they set.
  if (redecode_ && language_ == "auto" && audio_usable_ && !tokens.empty()) {
    std::string spliced = redecode_and_splice(tokens, times);
    if (!spliced.empty()) text = spliced;
  }
  audio_.clear();
  audio_.shrink_to_fit();
  return text;
}

// Decode one span of PCM on a second stream of the same recogniser, pinned to
// `lang`. Same flush as finish(): 1 s of silence, then InputFinished.
std::string Recognizer::decode_segment(const std::vector<float>& pcm, const char* lang) {
  const SherpaOnnxOnlineStream* st = SherpaOnnxCreateOnlineStream(recognizer_);
  if (!st) return {};
  SherpaOnnxOnlineStreamSetOption(st, "language", lang);
  const int step = 1024;  // the same bite the frame loop feeds
  for (size_t i = 0; i < pcm.size(); i += step) {
    const int n = static_cast<int>(std::min<size_t>(step, pcm.size() - i));
    SherpaOnnxOnlineStreamAcceptWaveform(st, kRate, pcm.data() + i, n);
    while (SherpaOnnxIsOnlineStreamReady(recognizer_, st)) SherpaOnnxDecodeOnlineStream(recognizer_, st);
  }
  std::vector<float> tail(kRate, 0.0f);
  SherpaOnnxOnlineStreamAcceptWaveform(st, kRate, tail.data(), static_cast<int>(tail.size()));
  SherpaOnnxOnlineStreamInputFinished(st);
  while (SherpaOnnxIsOnlineStreamReady(recognizer_, st)) SherpaOnnxDecodeOnlineStream(recognizer_, st);
  const SherpaOnnxOnlineRecognizerResult* r = SherpaOnnxGetOnlineStreamResult(recognizer_, st);
  std::string out = (r && r->text) ? r->text : "";
  SherpaOnnxDestroyOnlineRecognizerResult(r);
  SherpaOnnxDestroyOnlineStream(st);
  return out;
}

// Find the hole, decode it again in Japanese, put the answer in it. Returns ""
// when nothing fired or nothing came back worth splicing, and the caller keeps
// the original text.
std::string Recognizer::redecode_and_splice(const std::vector<std::string>& tokens,
                                            const std::vector<float>& times) {
  const float dur = static_cast<float>(audio_.size()) / kRate;
  if (dur <= 0.0f) return {};
  const std::vector<char> gate = gate_frames(audio_);

  // 1. The largest gap between consecutive token timestamps that the gate
  //    calls speech. The run from the last token to the end of the audio
  //    counts: a word at the very end of the utterance is deleted the same
  //    way, and the endpoint pause that follows it is not loud.
  float best = 0.0f, gap_a = 0.0f, gap_b = 0.0f;
  size_t after = tokens.size();  // index of the first token past the gap
  // i == 0 is the run from the start of the utterance to the first token: a
  // word spoken before anything the decoder kept is deleted the same way.
  for (size_t i = 0; i <= times.size(); ++i) {
    const float a = (i == 0) ? 0.0f : times[i - 1];
    const float b = (i < times.size()) ? times[i] : dur;
    const float gap = b - a;
    if (gap < kMinGapSec || gap <= best) continue;
    if (loud_fraction(gate, a, b) < kGapLoudFrac) continue;
    best = gap;
    gap_a = a;
    gap_b = b;
    after = i;
  }
  if (best <= 0.0f) return {};

  // 2. Grow the suspect span over the non-English tokens either side of it.
  //    When the insert is not deleted but mangled, the decoder emits a partial
  //    reading of it — `が遅れていま` for `電車が遅れています` — pressed up
  //    against the hole where the rest of it went. Those tokens are part of
  //    the same damaged word, so they belong inside the window that is decoded
  //    again and inside the range that gets replaced. Without this the correct
  //    reading would be spliced in *beside* the broken one.
  size_t first = after;  // index of the first replaced token
  while (first > 0 && non_ascii(tokens[first - 1])) --first;
  size_t last = after;   // one past the last replaced token
  while (last < tokens.size() && non_ascii(tokens[last])) ++last;

  // 3. The cut runs between the surviving tokens either side, so it cannot
  //    clip the onset of a word whose timestamp sits later than its first
  //    phoneme — which transducer timestamps routinely do.
  float cut_a = (first > 0) ? std::min(times[first - 1], gap_a) : 0.0f;
  float cut_b = (last < times.size()) ? std::max(times[last], gap_b) : dur;
  // Clamped to the buffer at both ends: a token can carry a timestamp inside
  // the second of silence finish() flushes with, which is not in the buffer.
  cut_a = std::min(std::max(0.0f, cut_a), dur);
  cut_b = std::min(std::max(cut_b, cut_a), dur);
  // M8.2 found that widening the cut into the neighbouring English costs
  // accuracy (81% at a tight cut, 63% at ±150 ms), so this one is not widened
  // at all — the token boundaries either side are as tight as the app can be
  // without knowing where the word really started. Pulling both edges 100 ms
  // further in was measured here and changed nothing (55% either way), so the
  // simpler rule stands.
  if (cut_b - cut_a < 0.1f) return {};

  redecode_info_.fired = true;
  redecode_info_.gap = best;
  redecode_info_.cut_a = cut_a;
  redecode_info_.cut_b = cut_b;

  std::vector<float> seg;
  const size_t pad = static_cast<size_t>(kPadSec * kRate);
  const size_t sa = static_cast<size_t>(cut_a * kRate);
  const size_t sb = std::min(audio_.size(), static_cast<size_t>(cut_b * kRate));
  seg.reserve(pad * 2 + (sb - sa));
  seg.assign(pad, 0.0f);
  seg.insert(seg.end(), audio_.begin() + sa, audio_.begin() + sb);
  seg.insert(seg.end(), pad, 0.0f);

  const auto t0 = std::chrono::steady_clock::now();
  std::string ja = decode_segment(seg, "ja");
  redecode_info_.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  redecode_info_.decoded = ja;

  // 4. Accept only a Japanese answer. Pinned to `ja` the model answers an
  //    English span with an empty string far more often than with a wrong
  //    word, but "far more often" is not "always", and a re-decode that can
  //    replace English with English could damage an utterance that was right.
  if (ja.empty() || !non_ascii(ja)) return {};

  // 5. When the span being replaced already held Japanese, the re-decode has
  //    to bring back *more* of it than it takes away. Measured: without this
  //    rule two of 192 clips got worse — a long phrase that decoded perfectly
  //    at the start of an utterance was replaced by a shorter misreading of a
  //    sub-span of itself (`電車が遅れています` -> `茶が遅れていま`). A second
  //    opinion that is allowed to shorten a good answer is a coin toss; one
  //    that may only lengthen it cannot lose ground.
  size_t had = 0;
  for (size_t i = first; i < last; ++i) had += non_ascii_chars(tokens[i]);
  if (had > 0 && non_ascii_chars(ja) < had) return {};

  std::string out;
  for (size_t i = 0; i < first; ++i) out += tokens[i];
  if (!out.empty() && static_cast<unsigned char>(out.back()) < 0x80 && out.back() != ' ') out += ' ';
  out += ja;
  std::string rest;
  for (size_t i = last; i < tokens.size(); ++i) rest += tokens[i];
  if (!rest.empty() && rest.front() != ' ' && static_cast<unsigned char>(rest.front()) < 0x80) out += ' ';
  out += rest;
  redecode_info_.spliced = true;
  return out;
}

}  // namespace aii
