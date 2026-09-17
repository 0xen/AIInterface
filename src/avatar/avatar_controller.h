#pragma once
// AvatarController (M2.4): the policy that decides what the avatar is doing.
// Without it the slime plays `idle` forever and the clip set M2.3 authored is
// reachable only from the command line.
//
// It is deliberately a small, dull state machine over signals the process
// already has — `VoiceSession::State`, the microphone level measured against
// the session's own noise gate, the speaker's playback level, `UsageStats.ctx`,
// the worker list and how long we have been idle. It starts no threads, reads
// no files and asks the session for nothing: it is handed the Snapshot the
// frame loop already took.
//
// The split is: `update()` is a pure function of that snapshot plus this
// object's own timers, and returns what *should* be on screen; `avatar_apply`
// is the only thing that touches an AvatarSource. That is what makes the
// policy testable without a window, and what will let M2.5's bus override it
// by simply not calling update() for a frame.
//
// Two things are decided here rather than in the data, and both are on
// purpose:
//
//   *Which accessory belongs to which clip* is a property of the clip, not of
//   the moment — see avatar_clip_sprite(). Deriving the sprite from the clip
//   instead of switching them on side by side makes "the bubble arrived a
//   frame before the think clip" unrepresentable rather than merely unlikely.
//
//   *`teaching` has no trigger.* Nothing in the session distinguishes Claude
//   explaining something from Claude merely replying; the only signal
//   available is the reply text, and a keyword heuristic over it would be
//   guesswork dressed as policy. The clip stays authored and reachable by
//   explicit selection (`--clip teaching`, and M2.5's `play` message), where
//   something that actually knows — Claude itself, or a script — can ask for
//   it.
#include <cstddef>
#include <map>
#include <random>
#include <string>

#include "avatar_def.h"
#include "voice_session.h"

namespace aii {

// The accessory that belongs to `clip`, or nullptr. This is the whole of the
// sprite policy; everything else follows from it.
const char* avatar_clip_sprite(const std::string& clip);

// What the controller wants on screen this frame.
struct AvatarPose {
  std::string clip;
  // Multiplier on the clip's clock. The frames are fixed art, so this is the
  // only continuous expression available: see the `listen` and `talk` cases
  // in the .cpp for what it is standing in for and what it is not.
  float speed = 1.0f;
};

// Everything with a number in it, in one place, because these are the values
// that decide whether this reads as alive or as twitchy and they will be
// retuned by eye.
struct AvatarTuning {
  // A clip holds for at least this long before an *ambient* change may
  // replace it. The listening → thinking → speaking sequence can cross two
  // states in a couple of hundred milliseconds; without a floor that is a
  // stutter, not an animation.
  float min_dwell = 0.45f;
  // Clips that carry an accessory get a longer floor, because putting one up
  // also slides the body sideways to make room for it (AvatarSource's slide,
  // 0.2 s). A sprite that comes and goes faster than the slide it caused
  // leaves the body mid-move, which is the ugliest failure available here.
  float sprite_dwell = 0.80f;
  // Thinking has to last this long before the thought bubble goes up. The
  // first token often arrives in 0.2-0.6 s, and you do not get a thought
  // bubble for a pause that short: filtering at entry is much better than
  // showing one and yanking it back.
  float think_delay = 0.35f;
  // Idle for this long and the slime droops. The milestone says two minutes.
  //
  // M1f.3 gives `sleepy` a *second* entrance that has no number here on
  // purpose: the auto-listen timeout dozes the slime the moment it fires,
  // because that timeout is itself the measured silence and a second waiting
  // period stacked on top of it would mean the reaction arrived minutes after
  // the thing it is reacting to -- quite possibly after the user was back.
  float sleepy_seconds = 120.0f;
  // Context fullness, with hysteresis so a reading that sits on the line does
  // not switch the steam on and off.
  double ctx_enter = 0.85;
  double ctx_exit = 0.78;
  // Frustration is a recurring burst rather than a permanent state: shaking
  // and steaming forever stops being an expression and becomes a fault. One
  // burst of the clip, then back to idle, then another after this long.
  float frustrated_period = 20.0f;
  // Blink interval, randomised in this range.
  float blink_min = 3.0f;
  float blink_max = 7.0f;
  // No blink may *start* within this long of a state change. A lid closing on
  // the same frame the avatar changes what it is doing does not read as a
  // blink, it reads as a dropped frame.
  float blink_guard = 0.50f;
  // Playback RMS that counts as "full volume" for the talk bounce. Measured
  // rather than assumed; see the note in the .cpp.
  float speak_full = 0.15f;
};

class AvatarController {
 public:
  explicit AvatarController(AvatarTuning tuning = {});

  // Clip lengths for the one-shots, taken from the definition rather than
  // guessed, and re-taken on every hot reload so retiming `happy.txt` retimes
  // how long the policy gives it.
  void note_definition(const AvatarDefinition& def);

  AvatarPose update(const VoiceSession::Snapshot& snap, float dt);

  // M2.5. A script asks for a clip, for a while.
  //
  // **The override goes through the policy rather than around it.** The note
  // at the top of this file guessed the bus would take the avatar by simply
  // not calling update() for a frame; that would give the animation two
  // writers, and every invariant here — the dwell floors, the sprite slide,
  // blink suppression, the one-shot yield rules — would hold for one of them
  // and not the other. So a script request is a *lease on a clip* inside the
  // same state machine, and there is still exactly one thing deciding what is
  // on screen.
  //
  // Precedence, highest first:
  //
  //   1. the user taking the microphone (State::Listening)
  //   2. a script's lease
  //   3. the built-in reactions (wake, happy, confused, frustrated)
  //   4. the ambient state -> clip policy
  //
  // A script beats a reaction because a reaction is automatic and a lease is
  // somebody deliberately asking. It loses to the microphone because the one
  // failure that must be impossible is a script wedging the avatar while the
  // user is trying to talk to it — and that is the same rule, and the same
  // code, as `Yield::ToMic`, which `happy` and `confused` already use.
  //
  // **The lease is a duration, not a mode**, which is what stops "play bounce"
  // being overwritten on the next frame *and* stops it lasting forever:
  //
  //   * `seconds <= 0` leases the clip for its own authored length, taken from
  //     the definition, so `avatar.play` with nothing else said is one play of
  //     the animation and then the policy has it back.
  //   * `seconds > 0` leases it for that long, clamped to
  //     `kScriptLeaseMax`. A script that wants longer re-leases, which is one
  //     line in a loop and means a script that dies stops holding the avatar
  //     within half a minute rather than until the app is restarted.
  //   * `release_clip()` hands it back immediately.
  //
  // Returns false if the definition does not declare the clip (the lease is
  // then not taken and `error` says so): the alternative is a lease that
  // believes it is playing art the source refused.
  bool request_clip(const std::string& clip, float seconds, std::string* error);
  void release_clip();
  bool script_holds() const { return script_left_ > 0.0f; }

  // The longest a single lease may run. Half a minute is long enough for a
  // scripted performance and short enough that a crashed script is a hiccup.
  static constexpr float kScriptLeaseMax = 30.0f;

  // The current clip and a word for why, for a trace line. Not for display.
  const std::string& clip() const { return current_; }
  const char* reason() const { return reason_; }
  // How long the previous clip had been up when this one replaced it. The
  // dwell floors are the whole anti-flap mechanism and they are invisible in
  // a screenshot, so the trace line carries the number they were checked
  // against.
  float last_dwell() const { return last_dwell_; }

 private:
  // What a one-shot will stand aside for. The question requirement 5 asks is
  // whether a one-shot may be interrupted mid-play, and the honest answer is
  // "by some things": an entrance is never a reason to ignore the user, but a
  // reaction that has no other channel is worth a second of the floor.
  enum class Yield {
    ToUser,  // wake: any state the user caused takes it (listen/think/talk)
    ToMic,   // happy, confused, frustrated: only the user taking the
             // microphone. In particular `happy` holds the floor through the
             // spoken worker report it is reacting to, which is the whole
             // point of it.
  };

  struct Timing {
    float total = 0.0f;      // the clip start to finish, at speed 1
    float after_first = 0.0f;  // the same, skipping frame 0 (blink enters at 1)
  };

  void start_oneshot(const char* clip, Yield yield);
  float clip_length(const char* clip, float fallback) const;
  float roll_blink_wait();

  AvatarTuning tune_;
  std::map<std::string, Timing> timing_;

  std::string current_ = "idle";
  const char* reason_ = "start";
  float entered_ = 0.0f;  // how long `current_` has been on screen
  float last_dwell_ = 0.0f;

  VoiceSession::State last_state_ = VoiceSession::State::Loading;
  float state_age_ = 0.0f;
  float idle_age_ = 0.0f;

  std::string oneshot_;
  float oneshot_left_ = 0.0f;
  Yield oneshot_yield_ = Yield::ToMic;

  // The script's lease: which clip and how much of it is left.
  std::string script_;
  float script_left_ = 0.0f;

  bool seeded_ = false;  // first update() records the world without reacting to it
  bool woke_ = false;
  unsigned last_failed_seq_ = 0;
  // M1f.3. The latched microphone closed itself on silence, and the slime has
  // not been given a reason to wake up since.
  //
  // **A latch rather than a one-shot, and that is the whole design.** The
  // reaction has to still be on screen when the user comes back, which may be
  // minutes; `start_oneshot()` would play the droop for the length of
  // sleepy.txt and hand the avatar back to `idle` long before anybody saw it,
  // which is the same as not reacting at all. So this feeds the *ambient*
  // branch instead -- the `sleepy` the two-minute idle rule already reaches --
  // and the clip loops for as long as nothing has happened.
  //
  // **What clears it is the one thing that can never be got wrong here.** A
  // slime stuck asleep is worse than a slime that never slept, so the rule is
  // not a list of events to remember to handle: it is `snap.state != Idle`,
  // checked every frame, which is exactly the test `idle_age_` beside it
  // already uses. A click, the latch reopening, a typed turn, a reply, a
  // failure -- every one of them leaves Idle, and every one of them therefore
  // wakes it, including any that get added later without reading this comment.
  bool dozed_ = false;
  unsigned last_timeout_seq_ = 0;
  std::map<std::string, WorkerPool::State> worker_state_;

  bool ctx_high_ = false;
  float frustrated_wait_ = 0.0f;

  float blink_wait_ = 0.0f;
  float blink_left_ = 0.0f;

  float mic_ = 0.0f;    // smoothed levels: the raw ones are per-audio-block
  float speak_ = 0.0f;  // and would put the clock rate on a jitter

  std::mt19937 rng_{0xA71A};
};

// Puts a pose on an AvatarSource: the clip, its accessory and its speed, in
// one call, on the frame the pose was decided. Everything here lands in the
// same compose(), so the sprite and the clip change together or not at all.
//
// `status_sprite`, when given, replaces whatever accessory the clip would have
// put up. It is how a *condition* — as opposed to a mood — gets onto the
// avatar: the muted speech bubble is the first and so far only one (16 Sep
// 2026, shown when Claude's voice is muted and the chat is closed, which is
// when the Mute button's own colour is not on screen to say so).
//
// **Precedence: the status sprite wins, always.** Two reasons, and they point
// the same way. The clip accessories restate something the panel is already
// saying out loud — the state line reads "thinking", the body is playing the
// think clip, and the bubble is decoration on top of both; the muted bubble is
// the *only* sign of a condition that is otherwise invisible with the chat
// shut. And an indicator that yielded would blink out exactly while Claude was
// thinking or asleep, which is to say it would flicker through precisely the
// moments it exists to describe, and a flapping indicator is the failure this
// project keeps having to design out. The body clip is untouched either way:
// the slime still thinks and still sleeps, it just does it under the one
// accessory that has something to add.
void avatar_apply(const AvatarPose& pose, AvatarSource& src,
                  const char* status_sprite = nullptr);

}  // namespace aii
