// M18.1. When the user talking over the reply becomes a barge-in, checked
// against synthetic frame sequences drawn from `docs/bargein-measurements.md`
// rather than by sitting in a room with the speakers on.
//
// Every level and every duration in this file is a number out of that
// document's tables. The point of the exercise is that the two interesting
// signals -- a person, and a click -- are **the same loudness**, so the only
// evidence that separates them is how long they last, and a test made of
// invented levels would prove nothing about that. The tables:
//
//   Silence control (nothing played):  mic p50 0.00431, one burst to 0.1248,
//                                      longest continuous voiced run 0.26 s
//   English playback alone:            mic p90 0.00036, burst to 0.0205,
//                                      longest run 0.18 s (nine frames)
//   Japanese playback alone:           mic p90 0.00375, max 0.0105, run 0.12 s
//   Playback + a person (run 4):       mic p50 0.0269, max 0.1071, run 0.72 s
//   Speaker's own level while playing: p50 0.039-0.043, max ~0.17
//
// The gate passed in is the one the session already computes
// (`floor * kGateOverFloor`, floored at `kGateAbsMin` = 0.004). During playback
// the measured mic floor collapses to 0.0001-0.0009, so `kGateAbsMin` is the
// binding term and 0.004 is the faithful value; in the silence control the room
// floor is 0.0043 and the app's own gate sits at about three times that, which
// is why that one case is driven at 0.013 -- the measured "voiced" fraction
// there was 3.8%, not 100%, which is only possible with a gate above the floor.
#include <cstdio>
#include <string>

#include "core/barge_policy.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

constexpr float kFrame = 0.02f;   // 20 ms, as everything in this app is
constexpr float kSpk = 0.040f;    // the speaker's measured median while playing
constexpr float kGate = 0.004f;   // kGateAbsMin, the binding gate during playback

// Drives one reply. `t` is reply time; `fire_t` is when the rule first fired,
// or -1 if it never did.
struct Reply {
  aii::BargePolicy p;
  float gate = kGate;
  float t = 0.0f;
  float fire_t = -1.0f;
  int learning_frames = 0;

  void feed(float seconds, float mic, float spk = kSpk) {
    const int n = static_cast<int>(seconds / kFrame + 0.5f);
    for (int i = 0; i < n; ++i) {
      const aii::BargeVerdict v = p.frame(mic, gate, spk, kFrame);
      t += kFrame;
      if (v == aii::BargeVerdict::Learning) ++learning_frames;
      if (v == aii::BargeVerdict::Fire && fire_t < 0.0f) fire_t = t;
    }
  }
  bool fired() const { return fire_t >= 0.0f; }
};

// The first second of a reply with the machine that was measured: the mic floor
// has collapsed and there is nothing to learn but the collapse. Fed a little
// past `kBargeLearnSec` so that no case in this file turns on whether fifty
// additions of 0.02f land a hair over or a hair under one second.
void learn_quiet(Reply& r) { r.feed(1.2f, 0.0001f); }

bool near(float a, float b, float tol) { return a > b - tol && a < b + tol; }

}  // namespace

int main() {
  using namespace aii;

  std::printf("the constants are the measured ones\n");
  check(kBargeOnsetSec == 0.30f,
        "**onset 0.30 s** -- 0.12 s and 0.25 s were both defeated by the measured "
        "0.18 s and 0.26 s transients");
  check(kBargeLeakMult == 3.0f, "the learned leak is required three times over");
  check(kBargeLearnSec >= 0.50f,
        "the learning window is at least the 0.5 s warm-up the investigation asked for");

  std::printf("the learning phase is audible reply, not wall clock\n");
  {
    Reply r;
    r.feed(2.0f, 0.0001f, 0.0f);  // the reply has not started making a sound yet
    check(r.p.learning(), "**two seconds of silent speaker leaves the rule unarmed**");
    check(r.p.leak_p99() == 0.0f, "and nothing has been learned from it");
    r.feed(1.2f, 0.0001f);
    check(!r.p.learning(), "one second of audible reply arms it");
    check(near(r.p.leak_p99(), 0.0001f, 1e-6f), "having learned the collapsed floor");
    check(near(r.p.threshold(kGate), kGate, 1e-6f),
          "**0.0001 * 3 is far below kGateAbsMin, so the gate still decides** -- "
          "the learned value is a clamp, not a threshold");
  }

  std::printf("playback alone, English (run 3): a 0.18 s transient at 0.0205\n");
  {
    Reply r;
    learn_quiet(r);
    r.feed(2.0f, 0.00036f);   // p90 of the run
    r.feed(0.18f, 0.0205f);   // 3.12-3.28 s, the nine consecutive frames
    r.feed(6.0f, 0.00036f);
    check(!r.fired(), "**the 0.18 s room transient does not fire**");
  }

  std::printf("playback alone, Japanese (run 5): a 0.12 s run at 0.0105\n");
  {
    Reply r;
    learn_quiet(r);
    r.feed(2.0f, 0.00375f);
    r.feed(0.12f, 0.0105f);
    r.feed(7.0f, 0.00093f);
    check(!r.fired(), "the Japanese reply never fires on itself either");
  }

  std::printf("the silence control (run 2): a 0.26 s burst to 0.1248\n");
  {
    Reply r;
    r.gate = 0.013f;  // three times the 0.0043 room floor, as the app would learn it
    learn_quiet(r);
    r.feed(8.0f, 0.00431f);
    r.feed(0.26f, 0.1248f);  // the burst the recogniser decoded as "Sorry"
    r.feed(2.0f, 0.00431f);
    check(!r.fired(),
          "**the 0.26 s event that defeats a 0.25 s onset does not fire at 0.30 s** -- "
          "this is the case the whole constant is set by");
  }

  std::printf("a person over the reply (run 4): 0.72 s from 4.52 s\n");
  {
    Reply r;
    learn_quiet(r);
    r.feed(3.5f, 0.0001f);  // reply only, to 4.5 s
    const float onset = r.t;
    r.feed(0.72f, 0.0269f);  // the measured median of their voiced frames
    check(r.fired(), "**a person fires**");
    check(near(r.fire_t - onset, 0.30f, 0.045f),
          "about 0.30 s after the first syllable, which is the barge latency");
    check(r.p.fired(), "and the verdict latches");
  }
  {
    Reply r;  // the same person at their peak rather than their median
    learn_quiet(r);
    r.feed(3.5f, 0.0001f);
    r.feed(0.72f, 0.1071f);
    check(r.fired(), "at their peak too, 48 dB clear of the reply");
  }

  std::printf("the run is continuous, so a gap is the whole run\n");
  {
    Reply r;
    learn_quiet(r);
    r.feed(0.28f, 0.0269f);
    r.feed(kFrame, 0.0001f);  // one frame under the line
    r.feed(0.28f, 0.0269f);
    check(!r.fired(), "**0.28 s, a gap, 0.28 s is not 0.56 s of voice**");
    r.feed(0.06f, 0.0269f);
    check(r.fired(), "and the second run finishes the job on its own");
  }

  std::printf("a barge that begins during the learning window\n");
  {
    Reply r;
    r.feed(0.6f, 0.0001f);   // audible reply, quiet room
    r.feed(0.3f, 0.0269f);   // the person starts at 0.6 s, still learning
    check(!r.fired(), "**cannot fire before the rule is armed**");
    r.feed(1.0f, 0.0269f);
    check(!r.fired(),
          "and does not fire afterwards either: their voice was learned as leak, the "
          "threshold went up, and that reply cannot be barged -- the safe direction, "
          "and the price of learning at all");
    check(r.p.leak_p99() > 0.02f, "the contaminated estimate is visible rather than hidden");
  }
  {
    Reply r;  // the reply's audio is paused: neither samples nor learning clock
    r.feed(0.6f, 0.0001f);
    r.feed(2.00f, 0.0001f, 0.0f);
    check(r.p.learning(), "a silent speaker never finishes the learning window");
    check(near(r.p.elapsed_sec(), 2.6f, 0.05f), "though reply time passes regardless");
  }

  std::printf("nothing is carried out of the learning window\n");
  {
    Reply r;  // a loudspeaker leak sits above the gate for the whole window
    r.feed(1.2f, 0.0060f);
    check(!r.p.learning() && near(r.p.threshold(kGate), 0.0180f, 1e-5f),
          "armed, with the threshold raised to three times the leak");
    check(r.p.voiced_run_sec() == 0.0f,
          "**the run is zero at arming, not the second of leak that cleared the gate**");
    r.feed(kFrame, 0.0500f);  // a single loud transient, one frame after arming
    check(!r.fired(),
          "so one loud frame at the moment of arming fires nothing -- had the run been "
          "carried, this is where a click would have barged the reply with no onset");
    r.feed(0.32f, 0.0500f);
    check(r.fired(), "and a sustained one still fires, 0.30 s in");
  }

  std::printf("loudspeakers: a leak that sits ABOVE the gate\n");
  {
    Reply r;
    r.feed(1.2f, 0.0060f);  // learned leak, above kGateAbsMin: nobody measured this, it is the case the clamp exists for
    check(near(r.p.leak_p99(), 0.0060f, 1e-5f), "the leak is learned at 0.0060");
    check(near(r.p.threshold(kGate), 0.0180f, 1e-5f),
          "**the threshold is raised to 0.018, three times the leak** -- the gate alone "
          "would have been 0.004 and the reply would have barged itself");
    r.feed(8.0f, 0.0060f);
    check(!r.fired(),
          "eight unbroken seconds of the app's own voice above the gate never fire");
    const float onset = r.t;
    r.feed(0.40f, 0.0569f);  // a person 10 dB above the raised threshold
    check(r.fired(), "**a person 10 dB above the raised threshold still fires**");
    check(near(r.fire_t - onset, 0.30f, 0.045f), "at the same 0.30 s");
  }

  std::printf("the volume turned up mid-reply, which nothing here guards\n");
  {
    Reply r;
    learn_quiet(r);  // learned at 0.0001; the threshold is the gate, 0.004
    r.feed(2.0f, 0.0030f);
    check(!r.fired(),
          "a modest rise that stays under the threshold changes nothing");
  }
  {
    Reply r;
    learn_quiet(r);
    r.feed(0.40f, 0.0300f);  // loud enough, and unbroken
    check(r.fired(),
          "**a large rise whose leak sustains 0.30 s unbroken DOES fire, and the reply "
          "silences itself** -- the learned margin is stale and there is no re-learn; "
          "this is the documented hole, asserted so that the day it closes, it shows");
  }
  {
    Reply r;
    learn_quiet(r);
    for (int i = 0; i < 8; ++i) {  // the same loud leak, gappy as speech actually is
      r.feed(0.18f, 0.0300f);      // the longest run measured at the volume it was at
      r.feed(0.04f, 0.0002f);
    }
    check(!r.fired(),
          "the same level in 0.18 s bursts does not, which is why the hole is narrower "
          "than it sounds");
  }

  std::printf("reset is a new reply\n");
  {
    Reply r;
    learn_quiet(r);
    r.feed(0.40f, 0.0269f);
    check(r.fired(), "fired once");
    check(r.p.frame(0.0f, kGate, kSpk, kFrame) == BargeVerdict::Fire,
          "**and keeps saying so: a reply is barged once, not once per frame**");
    r.p.reset();
    check(!r.p.fired() && r.p.learning(), "reset returns it to learning");
    check(r.p.leak_p99() == 0.0f && r.p.voiced_run_sec() == 0.0f,
          "with the leak estimate and the run both gone");
    check(r.p.frame(0.0269f, kGate, kSpk, kFrame) == BargeVerdict::Learning,
          "and the loud frame that fired a moment ago is now just a learning sample");
  }

  std::printf("arithmetic that must not misbehave\n");
  {
    Reply r;
    learn_quiet(r);
    r.p.frame(0.0269f, kGate, kSpk, 0.0f);
    check(near(r.p.voiced_run_sec(), 0.0f, 1e-6f),
          "a zero-length frame advances nothing, so a stalled caller cannot fire it");
    check(r.p.threshold(0.5f) == 0.5f,
          "a gate above the learned leak wins: the clamp only ever raises");
  }

  std::printf("\n%s\n", failures == 0 ? "all cases pass" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
