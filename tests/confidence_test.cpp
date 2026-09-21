// M23.1. The two halves of `src/stt/confidence.h`: reading `ys_probs` out of a
// result JSON written by sherpa-onnx, and turning it into the two numbers a
// caller would gate on.
//
// Neither half needs the 657 MB model, which is the point of them living in a
// header of their own. The malformed cases below are the ones that matter: the
// JSON comes from a library this project does not build, and the app's history
// already has one crash (M15.2) caused by reading a stream like that with
// `operator[]` on the wrong thread. A throw here is not a failed assertion, it
// is the app gone with no message -- so every one of these runs uncaught on
// purpose.
#include <cmath>
#include <cstdio>
#include <string>

#include "stt/confidence.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

}  // namespace

int main() {
  std::puts("ys_probs, out of JSON that may be any shape at all");
  // The real thing: what v1.13.8 writes into `result->json`, trimmed to the
  // fields this reads plus enough of its neighbours to be representative.
  const char* real =
      "{\"text\": \"What does mean\", \"tokens\": [\" What\", \" does\", \" mean\"], "
      "\"timestamps\": [0.32, 0.64, 2.08], \"ys_probs\": [-0.0012, -0.6000, -1.2000], "
      "\"lm_probs\": [], \"context_scores\": [], \"segment\": 0, \"start_time\": 0.00, "
      "\"is_final\": false}";
  check(aii::ys_probs_from_json(real).size() == 3, "the real shape yields one number per token");
  check(near(aii::ys_probs_from_json(real)[1], -0.6f), "and the numbers are the ones in it");

  check(aii::ys_probs_from_json(nullptr).empty(), "a null pointer yields nothing");
  check(aii::ys_probs_from_json("").empty(), "an empty string yields nothing");
  check(aii::ys_probs_from_json("not json at all").empty(), "unparseable yields nothing");
  check(aii::ys_probs_from_json("[1, 2, 3]").empty(), "a top-level array yields nothing");
  check(aii::ys_probs_from_json("\"just a string\"").empty(), "a top-level string yields nothing");
  check(aii::ys_probs_from_json("{\"text\": \"hello\"}").empty(), "no ys_probs at all");
  check(aii::ys_probs_from_json("{\"ys_probs\": null}").empty(), "ys_probs null");
  check(aii::ys_probs_from_json("{\"ys_probs\": -0.5}").empty(), "ys_probs not an array");
  check(aii::ys_probs_from_json("{\"ys_probs\": \"-0.5\"}").empty(), "ys_probs a string");
  check(aii::ys_probs_from_json("{\"ys_probs\": []}").empty(), "ys_probs empty");
  // Half-understood is not understood: a caller must not average over the
  // three numbers it recognised and call that the utterance's confidence.
  check(aii::ys_probs_from_json("{\"ys_probs\": [-0.1, \"x\", -0.2]}").empty(),
        "an array with a non-number in it is rejected whole");

  std::puts("\nthe two numbers");
  check(!aii::confidence_from_probs({}).available, "no probs means no opinion, not a bad score");
  check(aii::confidence_from_probs({}).mean == 0.0f, "and the numbers stay zero");

  const aii::Confidence one = aii::confidence_from_probs({-0.4f});
  check(one.available && one.tokens == 1, "one token is available");
  check(near(one.mean, -0.4f) && near(one.worst3, -0.4f),
        "an utterance shorter than the window is its own worst run");

  const aii::Confidence two = aii::confidence_from_probs({-0.2f, -0.8f});
  check(near(two.mean, -0.5f) && near(two.worst3, -0.5f), "two tokens likewise");

  // Clean English, the research document's range.
  const aii::Confidence clean =
      aii::confidence_from_probs({-0.001f, -0.05f, -0.02f, -0.15f, -0.01f, -0.03f});
  check(clean.worst3 > -0.5f, "clean English does not trip the -0.5 flag");

  // "Fall to D": the measured garbled-insert run, verbatim from the document.
  const aii::Confidence garbled =
      aii::confidence_from_probs({-0.57f, -1.23f, -0.85f, -0.13f, -1.18f});
  check(garbled.worst3 <= -0.5f, "the measured garbled run trips it");
  check(near(garbled.worst3, (-0.57f - 1.23f - 0.85f) / 3.0f),
        "and the worst run is the first three, not the three worst tokens");

  // The distinction the whole design rests on: three bad tokens spread through
  // a long fluent sentence are three hesitations. Picking the three worst
  // tokens wherever they are would flag this; the worst *run* does not.
  const aii::Confidence scattered = aii::confidence_from_probs(
      {-0.9f, -0.02f, -0.01f, -0.03f, -1.1f, -0.02f, -0.01f, -0.04f, -1.0f, -0.02f});
  check(scattered.worst3 > -0.5f, "three bad tokens far apart are not a bad run");
  check(scattered.tokens == 10, "and the token count is the whole utterance");

  // A run that only exists because of where the window falls.
  const aii::Confidence edge =
      aii::confidence_from_probs({-0.02f, -0.01f, -0.6f, -0.7f, -0.8f, -0.01f});
  check(near(edge.worst3, (-0.6f - 0.7f - 0.8f) / 3.0f), "the window finds a run in the middle");

  std::printf("\n%s\n", failures ? "FAILURES" : "all good");
  return failures ? 1 : 0;
}
