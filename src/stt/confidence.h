#pragma once
// M23.1. How sure the recogniser was about the utterance it just returned.
//
// sherpa-onnx's C result struct carries text, tokens and timestamps and nothing
// else, but `SherpaOnnxOnlineRecognizerResult::json` also carries `ys_probs` --
// the per-token log-probability the greedy decoder came out with. A
// corpus of code-switched utterances measured clean English at
// -0.001 to -0.15 per token, and a Japanese insert garbled into invented
// English ("Fall to D") at a run of three tokens averaging about -0.55, and
// proposed -0.5 on that worst run as the flag for "this is not the word it
// says it is".
//
// Split out of `recognizer.cpp` so that both halves can be tested without a
// 657 MB model: the arithmetic is arithmetic, and the JSON is a string written
// by a library this project does not build, which is exactly the shape of
// input that deserves a test with a malformed case in it.
//
// The JSON half is declared here and defined in `confidence.cpp` on purpose.
// `recognizer.h` includes this header, `settings.cpp` includes `recognizer.h`
// by a long chain, and `settings.cpp` has its own file-local `member()` helper
// -- pulling `llm/json_shape.h` in through a public header made every call
// there ambiguous. A declaration costs the parse nothing and keeps json.hpp
// out of the app's headers entirely.
#include <vector>

namespace aii {

// Two numbers, because they answer different questions. `mean` is how the
// utterance went as a whole; `worst3` is the worst *run* in it -- one bad token
// is a hesitation, three in a row is a word the decoder did not have.
//
// `available` is false when the field was absent or unreadable, which is the
// honest answer for a library version that renames it. A caller that gates on
// confidence must read that as "no opinion" and never as "very unsure": a gate
// that treats a missing field as a bad score would silently start dropping
// every utterance the day the field moves.
struct Confidence {
  bool available = false;
  int tokens = 0;
  float mean = 0.0f;    // mean log-prob over every token
  float worst3 = 0.0f;  // worst mean over any run of 3 consecutive tokens
                        // (the whole utterance when it has fewer than 3)
};

// The per-token log-probabilities out of a result's JSON string.
//
// Read the way `src/llm/json_shape.h` reads the CLI's stream-json: a field that
// is missing, null, or of another shape yields an empty vector rather than
// throwing on the frame-loop thread, where an escaped exception is
// std::terminate and the app is gone with no message. An array with a
// non-number in it is rejected whole -- a partially understood shape is not
// evidence about anything.
std::vector<float> ys_probs_from_json(const char* json);

// The worst run of three, not the three worst tokens anywhere: the flag above
// is "a run of >= 3 tokens averaging worse than about -0.5", and three bad
// tokens scattered through a long sentence are three hesitations, not a word
// the decoder did not have. An utterance with fewer than three tokens has one
// run, itself.
inline Confidence confidence_from_probs(const std::vector<float>& probs) {
  Confidence c;
  if (probs.empty()) return c;
  double sum = 0.0;
  for (float p : probs) sum += p;
  c.available = true;
  c.tokens = static_cast<int>(probs.size());
  c.mean = static_cast<float>(sum / probs.size());
  if (probs.size() < 3) {
    c.worst3 = c.mean;
    return c;
  }
  float worst = 0.0f;
  bool first = true;
  for (size_t i = 0; i + 3 <= probs.size(); ++i) {
    const float m = (probs[i] + probs[i + 1] + probs[i + 2]) / 3.0f;
    if (first || m < worst) { worst = m; first = false; }
  }
  c.worst3 = worst;
  return c;
}

}  // namespace aii
