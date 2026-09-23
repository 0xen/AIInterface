#include "stt/recognizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"
#include "stt/confidence.h"

namespace aii {
namespace {

// ---------------------------------------------------------------------------
// M8.4. Segmental re-decode.
// ---------------------------------------------------------------------------
//
// The failure this exists for, measured twice (16 Sep 2026 and 18 Sep 2026,
// 192 code-switched utterances): with
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

// M23.2. The mirror of the two above, for a re-decode pinned to English rather
// than to Japanese. A letter, not merely a byte under 0x80: the model emits
// spaces and punctuation between tokens, and a "hypothesis" made of those is
// not a word the pin recovered.
bool has_ascii_letter(const std::string& s) {
  for (unsigned char c : s)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
  return false;
}

size_t ascii_letters(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ++n;
  return n;
}

// Is `text` in `lang`'s script, and how much of it is. One pair of functions
// selected by the language the second decode was pinned to, so the acceptance
// rules below read the same whichever direction the recovery is going.
bool in_script(const std::string& s, const char* lang) {
  return std::strcmp(lang, "ja") == 0 ? non_ascii(s) : has_ascii_letter(s);
}

size_t script_len(const std::string& s, const char* lang) {
  return std::strcmp(lang, "ja") == 0 ? non_ascii_chars(s) : ascii_letters(s);
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
  confidence_ = Confidence{};
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
  std::vector<float> probs;
  const SherpaOnnxOnlineRecognizerResult* r = SherpaOnnxGetOnlineStreamResult(recognizer_, stream_);
  if (r) {
    text = r->text ? r->text : "";
    if (r->timestamps) {
      for (int i = 0; i < r->count; ++i) {
        tokens.emplace_back(r->tokens_arr[i] ? r->tokens_arr[i] : "");
        times.push_back(r->timestamps[i]);
      }
    }
    probs = ys_probs_from_json(r->json);
  }
  SherpaOnnxDestroyOnlineRecognizerResult(r);
  destroy_stream();

  confidence_ = confidence_from_probs(probs);  // M23.1, see stt/confidence.h

  redecode_info_ = RedecodeInfo{};
  // Under `auto`, M8.4 unchanged: both languages are on, the deletion happens,
  // and the span is always re-decoded into Japanese because Japanese is the
  // language that gets deleted.
  if (redecode_ && language_ == "auto" && audio_usable_ && !tokens.empty()) {
    std::string spliced = redecode_and_splice(tokens, times, "ja");
    if (!spliced.empty()) text = spliced;
  } else if (pinned_recovery_ && audio_usable_ && (language_ == "en" || language_ == "ja")) {
    // M23.2. Pinned: the same detector, aimed at whichever language the stream
    // is *not* pinned to, and generalised to the case where the hole is the
    // whole utterance.
    const char* other = language_ == "ja" ? "en" : "ja";
    // Which shape the hole is. "No tokens at all" is what a whole off-language
    // utterance usually produces under a pin, but not always: measured, two of
    // twenty whole Japanese sentences came back from an English-pinned stream
    // with a token or two of rubbish in them. One or two tokens over seconds
    // of continuous speech is the same event as none, and handing it to the
    // gap detector splices the right answer *around* the rubbish rather than
    // replacing it. So the whole-buffer branch takes anything under three
    // tokens from an utterance at least 1.5 s long; `recover_whole` then still
    // insists the gate heard someone speaking in it, so a long silence with
    // one stray token goes nowhere near a second decode.
    //
    // This is the rule the corpus was measured with. A token *density* test
    // (under two tokens per second of voiced audio) is the better-shaped
    // version of it and would also catch the one clip this misses -- four
    // tokens of rubbish over 3.2 s -- but the re-measurement was not finished,
    // so it is written down in the research document and not in the code.
    const bool empty_utterance = tokens.size() < 3 && audio_.size() >= kRate * 3 / 2;
    if (tokens.empty() || empty_utterance) {
      std::string whole = recover_whole(other);
      if (!whole.empty()) text = whole;
    } else {
      std::string spliced = redecode_and_splice(tokens, times, other);
      if (!spliced.empty()) text = spliced;
    }
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
                                            const std::vector<float>& times, const char* other) {
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
  //    Over tokens in the language being *recovered*, not in the carrier's:
  //    the damaged fragment is a partial reading of the off-language word, so
  //    growing the other way would swallow the sentence around it.
  size_t first = after;  // index of the first replaced token
  while (first > 0 && in_script(tokens[first - 1], other)) --first;
  size_t last = after;   // one past the last replaced token
  while (last < tokens.size() && in_script(tokens[last], other)) ++last;

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
  redecode_info_.lang = other;
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
  std::string ja = decode_segment(seg, other);
  redecode_info_.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  redecode_info_.decoded = ja;

  // 4. Accept only a Japanese answer. Pinned to `ja` the model answers an
  //    English span with an empty string far more often than with a wrong
  //    word, but "far more often" is not "always", and a re-decode that can
  //    replace English with English could damage an utterance that was right.
  if (ja.empty() || !in_script(ja, other)) return {};

  // 5. When the span being replaced already held Japanese, the re-decode has
  //    to bring back *more* of it than it takes away. Measured: without this
  //    rule two of 192 clips got worse — a long phrase that decoded perfectly
  //    at the start of an utterance was replaced by a shorter misreading of a
  //    sub-span of itself (`電車が遅れています` -> `茶が遅れていま`). A second
  //    opinion that is allowed to shorten a good answer is a coin toss; one
  //    that may only lengthen it cannot lose ground.
  size_t had = 0;
  for (size_t i = first; i < last; ++i) had += script_len(tokens[i], other);
  if (had > 0 && script_len(ja, other) < had) return {};

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

// M23.2. The whole-utterance case.
//
// A stream pinned to English, handed a whole sentence of Japanese, returns
// nothing at all -- no text, no tokens, no timestamps. There is no gap between
// tokens to find because there are no tokens, so the detector above cannot see
// it, and the utterance reaches the app as silence. The hole is the utterance.
//
// The test is the same one in a different shape: the app's own noise gate says
// someone spoke for a while and the decoder produced nothing. "A while" is the
// same 0.6 s the gap detector uses -- below that there is nothing worth a
// second decode, and a cough or a door is exactly what should not trigger one.
std::string Recognizer::recover_whole(const char* other) {
  const float dur = static_cast<float>(audio_.size()) / kRate;
  if (dur < kMinGapSec) return {};
  const std::vector<char> gate = gate_frames(audio_);
  size_t loud = 0;
  for (char c : gate) loud += static_cast<size_t>(c);
  const float voiced = loud * 0.02f;
  if (voiced < kMinGapSec) return {};

  redecode_info_.fired = true;
  redecode_info_.whole = true;
  redecode_info_.lang = other;
  redecode_info_.gap = voiced;
  redecode_info_.cut_a = 0.0f;
  redecode_info_.cut_b = dur;

  std::vector<float> seg;
  const size_t pad = static_cast<size_t>(kPadSec * kRate);
  seg.reserve(pad * 2 + audio_.size());
  seg.assign(pad, 0.0f);
  seg.insert(seg.end(), audio_.begin(), audio_.end());
  seg.insert(seg.end(), pad, 0.0f);

  const auto t0 = std::chrono::steady_clock::now();
  std::string alt = decode_segment(seg, other);
  redecode_info_.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
  redecode_info_.decoded = alt;

  // The same acceptance rule as the spliced case, and for the same reason: an
  // answer that is not in the language the second stream was pinned to is not
  // evidence of a switch. There is nothing to compare lengths against here --
  // the first pass produced no text at all -- so this is the whole test.
  if (alt.empty() || !in_script(alt, other)) return {};
  redecode_info_.spliced = true;
  return alt;
}

}  // namespace aii
