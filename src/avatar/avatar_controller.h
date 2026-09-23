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
#include <cstdint>
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

  // M7.2. The avatar is arriving on screen: play an entrance.
  //
  // **This is the only thing that starts one, and it is not this object's
  // decision.** It used to be: `update()` fired `wake` the first time the
  // session left Loading, which is a fair guess at "the avatar is appearing"
  // and is wrong in both directions. It is too early on the startup path --
  // the loading screen is still dissolving over the band and the clip is over
  // before the avatar is opaque -- and it never happens at all on the three
  // paths that appear *later*, a mode switch and every turn in "shown when
  // talking". Only the thing that owns the band's alpha knows when the avatar
  // is actually arriving, so AvatarAppearance says so and this plays it.
  //
  // Idempotence is not promised and is not wanted: called twice, the entrance
  // restarts, because two arrivals are two arrivals.
  //
  // `mic_open` is the one fact the snapshot cannot be trusted on at the moment
  // an entrance starts: M1f.5's auto-listen latch opens the microphone *below*
  // the frame's snapshot, so on the boot path the summon and the snapshot
  // disagree about whether the user is listening. The caller knows, because it
  // is the same frame that latched. See the note in update() -- getting this
  // wrong made the entrance pre-empt itself one frame in, on every auto-listen
  // boot. Callers that have no session to ask leave it false and get exactly
  // the behaviour they had.
  void appear(bool mic_open = false);

  // M2.3c. The avatar is leaving: play an exit, and say how long the band has
  // to stay up for it. Zero means this definition declares no exit art, and
  // the caller should fall back to the plain dissolve.
  //
  // The counterpart of appear() in every respect, including that it is not
  // this object's decision -- AvatarAppearance owns the band and therefore
  // owns when the avatar is going. The returned length is the trigger's, which
  // M7.1 measures as its *longest* variant: the choice of variant happens in
  // the source at play time and has not been made when this returns, and a
  // hold shorter than the clip would cut the departure off, while one that is
  // too long merely holds an empty frame -- which by the exit contract is
  // exactly nothing on screen.
  float depart();

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
  // "by some things": nothing the avatar is playing is a reason to ignore the
  // user reaching for the microphone, but a reaction that has no other channel
  // is worth a second of the floor.
  //
  // `ToUser` -- any state the user caused takes it -- was the third member
  // and is gone with M7.2: `wake` was its only holder, and see Entrance below
  // for why that stopped being a coherent rule for an entrance. Nothing else
  // ever wanted it, so it is deleted rather than kept warm for a caller that
  // does not exist.
  enum class Yield {
    ToMic,   // happy, confused, frustrated: only the user taking the
             // microphone. In particular `happy` holds the floor through the
             // spoken worker report it is reacting to, which is the whole
             // point of it.
    // M7.2's entrance: ToMic, except that the microphone cannot cancel an
    // entrance it *summoned*.
    //
    // The entrance used to be `ToUser` -- pre-empted by listen, think and
    // talk alike -- which was coherent while it only ever fired at startup:
    // the user talking a second after the app opened wanted the listen clip,
    // not an opening. It is incoherent the moment the
    // entrance fires on every appearance, because in "shown when talking" the
    // avatar appears *because* the state went to Listening or Speaking -- so
    // under ToUser the entrance was pre-empted on the frame it started, every
    // time, and under plain ToMic the Listening half of that is still true.
    // An animation that its own trigger cancels is not an animation.
    //
    // So: whatever the state was when the entrance fired cannot end it, and
    // anything after can. If the user picks up the microphone *during* an
    // entrance that something else summoned, they still take it instantly,
    // which is the guarantee ToMic exists for and the only one that matters
    // here -- capture itself was never gated on the avatar, so this is about
    // what the slime is doing and nothing else.
    Entrance,
    // M2.3c's exit: yields to nothing at all.
    //
    // Not stubbornness -- there is nothing left for it to yield *to*. The
    // avatar is leaving because the band is being taken away, and the one
    // thing that could reasonably interrupt a departure is the avatar being
    // wanted again, which does not arrive here as a state to be pre-empted by:
    // it arrives as a summon, and a summon starts an entrance one-shot that
    // replaces this one outright. Letting the microphone cancel the exit as
    // well would leave the slime standing in a band that is about to be
    // removed, which is the dissolve with extra steps.
    Exit,
  };

  struct Timing {
    float total = 0.0f;      // the clip start to finish, at speed 1
    float after_first = 0.0f;  // the same, skipping frame 0 (blink enters at 1)
  };

  void start_oneshot(const char* clip, Yield yield);
  // M7.5. A one-shot with another one booked behind it: `child_merge` then
  // `happy`.
  // If the definition has no art for `clip` the follow-up is started on its
  // own, so an avatar that declares no `child_merge` still reacts to a
  // worker exactly as it did before this milestone existed.
  void start_reaction(const char* clip, const char* follow, Yield yield);
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
  // What plays the instant `oneshot_` finishes, if anything. Dropped if the
  // one-shot is pre-empted instead of finishing: whatever took it away has a
  // better claim than a reaction that had not started yet.
  std::string oneshot_follow_;
  // M2.3c, the residual exit flash (user, 18 Sep 2026): an exit one-shot that
  // has run out **keeps its clip** instead of handing the avatar back to the
  // ambient policy, and this is the latch that says so.
  //
  // The bug it deletes is the same shape as the flicker that was just fixed
  // above -- an animation finishing before the disappearance commits, and
  // something ordinary drawn in the gap. `oneshot_left_` and `exit_left_` are
  // both started at the exit trigger's length on the dismissed frame, but they
  // are decremented from different frames: `depart()` now runs *above* the
  // controller, so update() takes a dt off the one-shot on the dismissed frame
  // itself while AvatarAppearance returns early on that frame and starts
  // counting on the next. The controller therefore finishes one frame sooner,
  // the ambient branch answered `idle`, and the whole slime was drawn again at
  // alpha 1.0 for ~2 frames after the departure had visually finished --
  // measured at f=501..502, with the fade starting at f=503.
  //
  // Holding is the fix rather than lining the two clocks up because it needs
  // no agreement between them. An exit clip is `loop: false` and its last
  // frame is *empty by contract* (see avatar.json's `_clips` note and the head
  // of depart.txt), precisely so that the animation, and not the compositor,
  // makes the avatar absent -- and AvatarSource::advance_clip already holds a
  // non-looping clip's last drawing forever. So "keep playing the exit" is
  // exactly "keep drawing nothing", for as long as nobody has anything better
  // to draw. Whatever the two clocks do relative to each other -- a frame
  // apart, a stall, a dt clamp, a variant shorter than the trigger's longest
  // -- there is no window in which the controller has run out of exit and has
  // to name a clip, because it names the exit.
  //
  // What replaces it is the one thing that can: another one-shot. In practice
  // that is the entrance, which is the only way the avatar comes back
  // (AvatarAppearance::summoned -> appear() -> start_oneshot("wake")), and
  // start_oneshot() clears this for every caller without any of them knowing
  // it exists.
  bool oneshot_holds_ = false;
  // Yield::Entrance's memory: the microphone was already open when this
  // entrance began, so it is the cause and not an interruption. Cleared the
  // moment the session leaves Listening, after which the next Listening is
  // somebody reaching for the mic and does take the avatar.
  bool oneshot_from_mic_ = false;
  // What appear()'s caller said about the microphone, held until the update()
  // that consumes the entrance reads it. Separate from `appear_pending_`
  // because the answer is about the arrival, not about the arming.
  bool appear_mic_ = false;
  // appear() latches; update() starts the clip. The frame loop settles the
  // band's alpha after it has run the policy for the frame, so the edge
  // arrives just past this object's turn -- and an entrance has to be started
  // against the session state it is entering on anyway (see Yield::Entrance),
  // which only update() is holding. The cost is that the clip's first frame
  // lands one frame into an eighty-millisecond reveal.
  bool appear_pending_ = false;
  // True for the single update() that starts an entrance or (M2.3c) an exit:
  // the dwell floors are suspended for it. For an entrance because they exist
  // to stop the *visible* clip flapping and the avatar was not visible; for an
  // exit because the band is held open for a fixed length, so a floor would
  // not delay the departure, it would shorten it.
  bool entered_first_frame_ = false;

  // The script's lease: which clip and how much of it is left.
  std::string script_;
  float script_left_ = 0.0f;

  bool seeded_ = false;  // first update() records the world without reacting to it
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
  // Keyed by `WorkerPool::Snapshot::id`, not by name (M32). A name can be
  // spawned again the moment its earlier holder is Done or Failed, and
  // before M32 that reuse could put a finished row and a fresh one in the
  // same snapshot for one frame -- two states, one map entry, so the second
  // write here flipped it back and forth every frame and re-armed
  // `pending_child_merge_` on each flip. `id` is unique per worker for the
  // life of the pool, so two rows sharing a name now occupy two entries and
  // neither can overwrite the other's state.
  std::map<std::uint64_t, WorkerPool::State> worker_state_;

  // ---- M7.5: the child slime that leaves with a worker and comes back -----
  //
  // **Children are counted, not named.** `running_` is the number of workers
  // in the snapshot that are Starting or Working, recomputed from scratch
  // every frame; a rise in it sends a child, a fall brings one home. Nothing
  // in the band ties a blob to a worker — the panel's rows already do that
  // job, with the name and the activity line — so identity here would be a
  // claim the art cannot make good on.
  //
  // **Counting from a level rather than from edges is what makes a stranded
  // child impossible**, and it is the answer to the failure the milestone
  // names by hand. There is no persistent "a child is away" drawing to get
  // stuck: the departure and the return are each a one-shot that plays and
  // ends, and what is off screen between them is nothing at all. On top of
  // that, every way a worker can stop being a worker is a fall in this
  // number and therefore a return — it reported, it fell over, it was
  // paused, it died without saying anything, the pool dropped it. A spawn
  // that never starts never raises the count and so never sends anybody, and
  // an app that closes with three workers running has no state to leak,
  // because the count lives only in this object and this object goes with the
  // window.
  //
  // **Overlapping workers do not overlap animations.** At most one departure
  // or return is on screen at a time; the two flags below are booleans and
  // not counters, so several workers spawning on one frame send one child
  // between them and several reporting on one frame bring one home. Workers
  // that start or finish far enough apart to be separate moments — which is
  // nearly always, since a report arrives with a sentence of speech attached
  // — still get an animation each, because by then the previous one has
  // finished and the flag is free. The alternative, a queue, would keep
  // playing departures after the thing they described had already finished.
  int running_ = 0;
  bool pending_child_depart_ = false;
  bool pending_child_merge_ = false;
  // The reaction the pending `child_merge` is carrying: `happy` for a worker
  // that reported, `confused` for one that fell over. It rides with the merge
  // rather than firing beside it so the two are one animation in sequence and
  // not two fighting over the same frame.
  const char* child_merge_follow_ = nullptr;

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
