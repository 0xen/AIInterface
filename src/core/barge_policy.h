#pragma once
// M18.1. Whether the user is talking over the reply — the whole of the
// decision, as a function of four numbers per 20 ms frame and nothing else.
//
// It is here, header-only and standard-library-only, for the reason
// `cwd_policy.h` and `handoff_policy.h` are: the interesting part of barge-in
// is *when it fires*, and the answer had to be testable without a microphone,
// a speaker, a room or a reply. `VoiceSession` owns everything that follows
// from a `Fire` — clearing the speech queue, the per-reply silence flag, the
// pre-roll, `begin_listening()` — and it owns none of the arithmetic. This
// file is not wired into it yet; M18.2 does that, after M17.
//
// ---------------------------------------------------------------------------
// Every number below was measured, and the document is `docs/bargein-measurements.md`
// ---------------------------------------------------------------------------
//
// One desktop, 21 Sep 2026, five runs through `spikes/bargein/bargein_probe.exe`,
// which compiles the app's own `AudioOut`, `MicIn`, the two TTS engines and the
// recogniser rather than copies of them. Per-frame tables of mic RMS against
// the speaker's own level, with and (by accident, run 4) with a person talking
// over the reply. What that data says, in the order it constrains this file:
//
//  * **The binding constraint is not the app's own voice. It is room
//    transients.** Through that machine's default output the reply arrived at
//    the microphone at about −80 dBFS: mic median 0.0000-0.0009 against a gate
//    of `kGateAbsMin` = 0.004, and cross-correlating speaker level against mic
//    level over 0-500 ms gave a best r of 0.03. There was no acoustic path to
//    have a lag. Meanwhile a click or a keypress reached 0.02-0.12, which is
//    indistinguishable from a person **on level alone**.
//
//  * **Duration separates them, and 0.30 s is where.** Longest continuous run
//    of above-gate frames: **0.18 s** in the English playback run (a transient
//    at 3.12-3.28 s, nine consecutive frames, uncorrelated with the speaker),
//    **0.12 s** in the clean Japanese run, **0.26 s** in the *silence* control
//    (which is also where the recogniser decoded "Sorry" out of a room with
//    nobody speaking). The person in run 4 produced **0.72 s** continuous. So
//    0.12 s is defeated twice over and 0.25 s is defeated by the 0.26 s event;
//    0.30 s rejects everything measured and costs 50-100 ms on a trigger that
//    already lands 240-300 ms after the first syllable. This is the single
//    most load-bearing guard in the file and it is also the cheapest.
//
//  * **The learned leakage margin is a clamp, never a threshold.** The plan in
//    `docs/bargein-investigation.md` proposed learning the mic level during the
//    first second of a reply and requiring a multiple of it. On the machine that
//    was measured that learned value is about **0.0002** — twenty times *below*
//    `kGateAbsMin`. Used as a threshold it would contribute nothing but
//    sensitivity to noise, so it is taken as `max(gate, leak_p99 * 3)`: the
//    learned number can only ever make the rule **stricter**. That is what it
//    is for. Nobody has measured a loudspeaker configuration here — there were
//    no loudspeakers to measure — and on one the leak may sit above the gate,
//    which is the case the clamp exists for and which
//    `tests/barge_policy_test.cpp` covers synthetically.
//
//  * **The recogniser is not an input to this file at all.** Deliberately, and
//    it is the absence that is the decision. It cannot trigger: it decoded
//    "Sorry" from a silent control. It cannot veto: the app's own voice decoded
//    to *nothing* in `en`, `ja` and `auto`, for both engines, so "no partial"
//    is not evidence that nobody is talking. Its only honest use is the
//    post-hoc check on the pre-roll in M18.3, which is a different decision in
//    a different place.
//
// ---------------------------------------------------------------------------
// The three states, and what each buys
// ---------------------------------------------------------------------------
//
//  * **Learning** — the first `kBargeLearnSec` of *audible reply*, during which
//    the leak estimate is collected and the rule cannot fire. The clock only
//    advances on frames where the speaker is actually playing, so a reply whose
//    first audio arrives late is learned from its audio rather than from the
//    silence before it. The cost is real and is stated rather than hidden: a
//    barge in the first second of a reply is held. The user has heard about one
//    second of it, so there is little to interrupt; and firing on a leak
//    estimate that does not exist yet is the failure this milestone is named
//    after. Nothing is carried out of the window: the onset run starts at zero
//    the moment the rule arms, for the reason given at the code.
//
//  * **Holding** — armed, watching, not yet convinced. `voiced_run_sec` is the
//    only memory.
//
//  * **Fire** — and it **latches** until `reset()`. Unlike `handoff_due()`,
//    which is memoryless and leaves re-arming to its caller, this one already
//    owns per-reply state, so it owns this too: a reply is barged once.
//    `reset()` is a new reply, and is the only way back.
//
// ---------------------------------------------------------------------------
// What this rule does NOT defend against, named because the investigation named it
// ---------------------------------------------------------------------------
//
// **A user who turns the volume up mid-reply defeats the learned margin.** The
// leak is learned once, from the first second, and never re-learned — there is
// no way to re-learn it during a reply without the risk of learning the user's
// own voice as leakage, which would silently disarm the feature for the rest of
// that reply. So after a volume increase the only remaining guards are the
// ordinary gate and the 0.30 s continuous run. A modest rise stays under the
// clamp and changes nothing; a large one whose leak clears the threshold *and*
// sustains for 0.30 s unbroken will fire, and the reply will silence itself.
// That is a false trigger, it is the failure mode that reads as a broken app,
// and this file does not prevent it. What makes it less likely than it sounds
// is that speech is gappy — the measured leak runs were 0.18 s and 0.12 s at
// the volume the machine was already set to — and what would actually fix it
// is stage 2's echo cancellation, not a fourth constant.
// `tests/barge_policy_test.cpp` asserts both halves of this so that the day it
// changes, it changes visibly.
//
// The mirror risk is equally unguarded and equally deliberate: if the user is
// *already* talking during the learning window, their voice is learned as leak,
// the threshold is raised far above them, and that reply cannot be barged. That
// direction is the safe one, which is the only reason it is acceptable.
#include <algorithm>
#include <cstddef>
#include <vector>

namespace aii {

// How long the gate has to stay open **continuously** before a voice is
// believed. Measured: 0.18 s and 0.26 s of non-speech cleared the gate; 0.72 s
// of a person did not come close to this bar. See the header comment.
inline constexpr float kBargeOnsetSec = 0.30f;

// The multiple of the learned leak the mic must clear, when that is the larger
// of the two. Three, from the investigation's proposal, kept because on a
// configuration with a real leak it is the whole of the margin; on the one
// measured it is 0.0006 and the gate wins.
inline constexpr float kBargeLeakMult = 3.0f;

// How much audible reply is learned from before the rule arms. The
// investigation asked for at least 0.5 s so that the reply's own first syllable
// is not the only sample; a second is what it proposed to learn over, and a
// second is used, which satisfies both.
inline constexpr float kBargeLearnSec = 1.00f;

// What counts as the speaker actually making a sound, for the purpose of
// advancing the learning clock. `AudioOut::level()` is the RMS of the block
// just played and is exactly 0 between replies; measured playback sat at a
// median of 0.039-0.043, so this is three orders of magnitude clear of both.
inline constexpr float kBargeSpeakerActive = 1e-5f;

// M18.4. A run over the bar that lasts this long is worth a line in the tuning
// log even though it did not fire: it is longer than a keypress and shorter
// than the onset, which is the band where a person who was refused, and a
// transient that nearly was not, both live. A third of the onset.
inline constexpr float kBargeNoticeSec = 0.10f;

// The leak estimate is a p99 of the learning frames, not a maximum, so that one
// door slam during the first second cannot arm a threshold no voice will clear.
// At 20 ms a second is only 50 frames, and a nearest-rank p99 of 50 samples *is*
// the maximum — which is precisely the sensitivity being avoided — so the
// estimator here always discards at least the single loudest frame. The cap
// exists so the buffer stays bounded if a caller ever feeds shorter frames.
inline constexpr std::size_t kBargeLearnMaxSamples = 256;

enum class BargeVerdict {
  Learning,  // the first second of audible reply; the estimate is being built
  Holding,   // armed, and not convinced
  Fire,      // the user is talking over the reply; latched until reset()
};

// One reply's worth of barge watching. Fed one frame at a time in the order the
// frames were captured; nothing here reads a clock of its own.
class BargePolicy {
 public:
  BargePolicy() { samples_.reserve(64); }

  // A new reply. Everything learned, counted and latched goes.
  void reset() {
    samples_.clear();
    learn_elapsed_ = 0.0f;
    elapsed_ = 0.0f;
    voiced_run_ = 0.0f;
    leak_p99_ = 0.0f;
    learning_ = true;
    fired_ = false;
    armed_peak_ = 0.0f;
    longest_run_ = 0.0f;
    longest_run_at_ = -1.0f;
    runs_noticed_ = 0;
    run_ended_ = 0.0f;
    gate_run_ = 0.0f;
    gate_longest_ = 0.0f;
  }

  // One captured frame.
  //
  //   mic_rms      the frame's RMS from `MicIn`
  //   gate         the gate the Listening branch already computes for this
  //                frame (floor * kGateOverFloor, floored at kGateAbsMin)
  //   speaker_rms  `AudioOut::level()` for the same frame
  //   dt_sec       the frame's duration; 0.02 in this app
  //
  // There is no recogniser partial in that list and there is not going to be
  // one; see the header.
  BargeVerdict frame(float mic_rms, float gate, float speaker_rms, float dt_sec) {
    if (fired_) return BargeVerdict::Fire;
    if (!(dt_sec > 0.0f)) dt_sec = 0.0f;  // NaN included, deliberately
    elapsed_ += dt_sec;

    const bool speaker_on = speaker_rms > kBargeSpeakerActive;
    if (learning_) {
      // Only audible reply teaches the estimate, and only audible reply moves
      // the clock towards arming.
      if (speaker_on) {
        if (samples_.size() < kBargeLearnMaxSamples) {
          samples_.push_back(mic_rms > 0.0f ? mic_rms : 0.0f);
        }
        learn_elapsed_ += dt_sec;
      }
      if (learn_elapsed_ < kBargeLearnSec) return BargeVerdict::Learning;
      leak_p99_ = percentile99(samples_);
      learning_ = false;
      // The onset run **starts at zero here**, and no run is carried out of the
      // learning window. It was tempting to count one, so that somebody who
      // starts speaking at 0.9 s is not made to start again -- but there is no
      // threshold to count it against yet (that is what the window is for), so
      // it would have to be the plain gate, and on a configuration where the
      // leak sits above the gate the reply's own voice would hand the armed
      // rule a full second of "voice" on its first frame. A transient at 1.0 s
      // would then fire with no onset at all, which is the one thing this file
      // exists to stop. The cost is that a barge begun in the last 0.3 s of the
      // window waits out its onset again; it is paid once per reply and only by
      // somebody who was already inaudible to the rule.
      voiced_run_ = 0.0f;
      return BargeVerdict::Holding;
    }

    accumulate(mic_rms, threshold(gate), gate, dt_sec);
    if (voiced_run_ >= kBargeOnsetSec) {
      fired_ = true;
      return BargeVerdict::Fire;
    }
    return BargeVerdict::Holding;
  }

  // The level a frame must clear once armed: the ordinary gate, raised — and
  // only ever raised — by what the reply was measured to leak.
  float threshold(float gate) const {
    return (std::max)(gate, leak_p99_ * kBargeLeakMult);
  }

  bool learning() const { return learning_; }
  bool fired() const { return fired_; }
  float leak_p99() const { return leak_p99_; }
  float voiced_run_sec() const { return voiced_run_; }
  float elapsed_sec() const { return elapsed_; }

  // M18.4. What the armed rule saw, for the tuning line and for nothing else.
  // "Did not fire" on its own cannot be acted on: the person tuning has to
  // know whether nothing cleared the bar at all, or something cleared it for
  // 0.2 s and was refused by the onset, or the bar itself sat above their
  // voice. These four numbers are that distinction. They are collected only
  // once the rule is armed, so the learning window's own leak is not in them.
  //
  //   armed_peak         the loudest frame since arming
  //   longest_run_sec    the longest continuous run over the bar, fired or not
  //   longest_run_at_sec reply time at which that run *began*
  //   runs_noticed       runs that reached kBargeNoticeSec; a run that fires
  //                      counts once, like any other
  float armed_peak() const { return armed_peak_; }
  float longest_run_sec() const { return longest_run_; }
  float longest_run_at_sec() const { return longest_run_at_; }
  int runs_noticed() const { return runs_noticed_; }
  // Non-zero on exactly the frame a run of at least kBargeNoticeSec ended
  // under the bar, and then its length; the caller's chance to log it. A run
  // that fires never "ends" this way, because Fire latches.
  float run_just_ended_sec() const { return run_ended_; }
  // The longest continuous run over the *plain gate* since arming, ignoring
  // the learned margin. Above kBargeOnsetSec means the reply's own voice would
  // fire the rule without the clamp, so the clamp is what is keeping it quiet.
  float gate_longest_run_sec() const { return gate_longest_; }

 private:
  // Continuous, so one frame under the line is the whole run gone. That is the
  // property being relied on: a transient is loud, but it is loud in bursts.
  void accumulate(float mic_rms, float at, float gate, float dt_sec) {
    run_ended_ = 0.0f;
    if (mic_rms > armed_peak_) armed_peak_ = mic_rms;
    // The shadow run: what the onset would have seen at the plain gate, with
    // no learned margin at all. Never fires anything; it is the number that
    // says whether the clamp is load-bearing on this configuration.
    if (mic_rms > gate) {
      gate_run_ += dt_sec;
      if (gate_run_ > gate_longest_) gate_longest_ = gate_run_;
    } else {
      gate_run_ = 0.0f;
    }
    if (mic_rms > at) {
      if (voiced_run_ <= 0.0f) run_began_ = elapsed_ - dt_sec;
      voiced_run_ += dt_sec;
      if (voiced_run_ > longest_run_) {
        longest_run_ = voiced_run_;
        longest_run_at_ = run_began_;
      }
      if (voiced_run_ >= kBargeNoticeSec && voiced_run_ - dt_sec < kBargeNoticeSec)
        ++runs_noticed_;
    } else {
      if (voiced_run_ >= kBargeNoticeSec) run_ended_ = voiced_run_;
      voiced_run_ = 0.0f;
    }
  }

  // The top 1% of frames discarded, and never fewer than one of them: at the
  // 50 frames a second of 20 ms audio actually provides, "the top 1%" rounds to
  // nothing and the estimate would be the maximum. One frame is exactly the
  // door slam, the keypress and the chair, which is the whole reason this is a
  // percentile and not a `max`.
  static float percentile99(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    const std::size_t drop = (std::max)(std::size_t{1}, n / 100);
    const std::size_t rank = (n > drop) ? (n - drop) : std::size_t{1};  // 1-based
    return v[rank - 1];
  }

  std::vector<float> samples_;
  float learn_elapsed_ = 0.0f;
  float elapsed_ = 0.0f;
  float voiced_run_ = 0.0f;
  float leak_p99_ = 0.0f;
  bool learning_ = true;
  bool fired_ = false;
  float armed_peak_ = 0.0f;
  float longest_run_ = 0.0f;
  float longest_run_at_ = -1.0f;
  float run_began_ = 0.0f;
  int runs_noticed_ = 0;
  float run_ended_ = 0.0f;
  float gate_run_ = 0.0f;
  float gate_longest_ = 0.0f;
};

// M18.3. How much of the watch's kept-back audio the fresh recogniser stream
// is given when the rule fires: the onset run, and nothing earlier.
//
// The trim is the whole of the decision and the reason it is here rather than
// inline at the call site is that it is the one part of the pre-roll anybody
// can be wrong about. `docs/bargein-measurements.md` has the recogniser
// emitting the word "Sorry" from a silence control with nobody in the room, so
// audio from before the user started talking is not neutral padding -- it is a
// source of words the user did not say, arriving at the front of their turn
// where they are least likely to notice them. Everything before the run began
// is the reply's leakage and the room, so it is dropped; the run itself is
// what `kBargeOnsetSec` just spent 0.30 s proving was a voice.
//
// Clamped to what is actually held, so a caller whose buffer is shorter than
// the run (a watch that opened mid-run, a cap set below the onset) gets what
// there is rather than reading off the front of it.
inline std::size_t barge_preroll_samples(std::size_t have, float voiced_run_sec, int rate) {
  if (rate <= 0 || !(voiced_run_sec > 0.0f)) return 0;
  const double want = double(voiced_run_sec) * double(rate);
  if (want >= double(have)) return have;
  return static_cast<std::size_t>(want);
}

}  // namespace aii
