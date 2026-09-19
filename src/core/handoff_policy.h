#pragma once
// M3.15. Whether the context window has grown far enough that the session
// should hand over to a fresh one — the whole of the decision, as a function
// of three numbers and nothing else.
//
// It is here, header-only and standard-library-only, for the reason
// `cwd_policy.h` is: the interesting part of this feature is *when* it fires,
// and the answer had to be testable without a microphone, a `claude` child or
// a frame loop. `VoiceSession` owns everything that follows from a `Yes`; it
// owns none of the arithmetic.
//
// ---------------------------------------------------------------------------
// 40% of what
// ---------------------------------------------------------------------------
//
// The threshold is a **fraction of the model's own context window**, because
// that is the number the user was looking at when they said "around 40%":
// `UsageStats::ctx` is what the footer draws, and it is
// `ctx_tokens / ctx_window` as the CLI itself reports both. The consequence is
// worth stating rather than discovering — `ctx_window` is 200000 normally and
// 1000000 for a `[1m]` model, so 40% is 80k tokens on one and 400k on the
// other. That is the right behaviour (the window really is five times bigger,
// and the model is no closer to the end of it) but it means the setting must
// never be compared with, or described as, a token count. `llm_client.h` says
// the same thing from the other side.
//
// ---------------------------------------------------------------------------
// The three ways this goes wrong, and where each is answered
// ---------------------------------------------------------------------------
//
//  1. **`ctx` is -1.0 until the CLI first reports.** A fresh child has said
//     nothing about its context, and -1.0 is "unknown", not "very low". Read
//     as a number it is below every threshold, which is harmless; read as
//     evidence it is a lie. Answered here: unknown is `No`, always, and it is
//     never recorded as a session's floor either.
//
//  2. **Crossing 40% is a level, not an edge.** Once over, the session stays
//     over until it restarts, so the *caller* must be a state machine that
//     runs once and only re-arms after the new child is up. This function is
//     deliberately memoryless and will keep saying `Yes` for as long as the
//     numbers say `Yes`; it is not the thing that stops it firing twice.
//
//  3. **A threshold no fresh session can sit under is an infinite loop.** A
//     system prompt, the tool list and the CLI's own preamble cost something
//     before a word is said — a few per cent today, more if the prompt tree
//     grows — so a threshold of 2%, or of 40% against a pathological prompt,
//     would have a brand-new session already over the line and handing over
//     again, one summary turn per turn, forever. That is the single most
//     expensive failure available here, so it is not left to luck: the caller
//     records what the session cost *at its floor* (its first reported
//     reading, before the user has said anything) and passes it in, and this
//     returns `Unattainable` rather than `Yes`. The caller's answer to that is
//     to switch the feature off for the run and say so in the log — a handoff
//     that cannot help is worse than no handoff, because it also burns the
//     tokens it exists to save.
#include <cmath>

namespace aii {

enum class HandoffVerdict {
  No,            // nothing to do (below the line, unknown, or switched off)
  Yes,           // hand over now
  Unattainable,  // the threshold is at or under what a *fresh* session costs
};

// The on-disk / in-settings threshold, normalised to the fraction this file
// works in. Two forgiving readings and one refusal, all of them here so that
// no caller has a second opinion:
//
//   * `<= 0` means **off**, and returns 0. That is the same spelling
//     `listen_timeout` uses for "never", so the file stays learnable.
//   * `> 1` is read as a **percentage**: a person hand-editing `settings.json`
//     after hearing themselves say "around 40%" writes `40`, and a fraction of
//     a context window can never legitimately be 40. Accepting it is not the
//     free-text-model-field mistake — there is exactly one other reading and
//     it is absurd, where a mistyped model name has a plausible one that fails
//     an hour later at launch.
//   * Anything left is clamped to 0.95. A threshold above that is not a
//     handover policy; it is a session that hands over during the sentence
//     that overflows it.
inline double normalise_handoff_threshold(double v) {
  if (!(v > 0.0)) return 0.0;  // NaN included, deliberately: `!(NaN > 0)` is true
  if (v > 1.0) v /= 100.0;
  if (v > 0.95) v = 0.95;
  return v;
}

// `ctx` and `floor` are `UsageStats::ctx` readings: a fraction, or negative
// for "not reported yet". `floor` is the first reading this session made,
// i.e. what it costs to exist; pass a negative value when the session has not
// reported one yet, which only suppresses the `Unattainable` answer.
inline HandoffVerdict handoff_due(double ctx, double threshold, double floor) {
  const double at = normalise_handoff_threshold(threshold);
  if (at <= 0.0) return HandoffVerdict::No;
  // Unknown is not a number. See (1) above.
  if (!(ctx >= 0.0)) return HandoffVerdict::No;
  // Checked before the comparison, not after, so a threshold under the floor
  // is reported as broken even on the turn it would otherwise have fired: the
  // caller wants to switch the feature off, and "fire once, then switch off"
  // would spend a summary turn proving what the numbers already said.
  if (floor >= 0.0 && at <= floor) return HandoffVerdict::Unattainable;
  return ctx >= at ? HandoffVerdict::Yes : HandoffVerdict::No;
}

}  // namespace aii
