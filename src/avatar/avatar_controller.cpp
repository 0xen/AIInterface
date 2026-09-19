#include "avatar_controller.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace aii {
namespace {

// The clip → accessory table. It is short because it is meant to be: an
// accessory is part of what a clip *means*, so there are exactly four of
// them and they are the four the definition declares.
struct ClipSprite {
  const char* clip;
  const char* sprite;
};
constexpr ClipSprite kClipSprites[] = {
    {"think", "thought_bubble"},
    {"sleepy", "zzz"},
    {"confused", "question"},
    {"frustrated", "steam"},
};

}  // namespace

const char* avatar_clip_sprite(const std::string& clip) {
  for (const ClipSprite& cs : kClipSprites) {
    if (clip == cs.clip) return cs.sprite;
  }
  return nullptr;
}

AvatarController::AvatarController(AvatarTuning tuning) : tune_(tuning) {}

void AvatarController::note_definition(const AvatarDefinition& def) {
  // Re-measured on every load, hot reloads included: the rhythm of a clip is
  // authored in its .txt, so how long the policy gives `happy` to finish has
  // to be whatever `happy.txt` currently says and not a number in this file
  // that quietly goes stale the first time the art is retimed.
  timing_.clear();
  for (const AvatarClip& c : def.clips) {
    if (c.fps <= 0.0f || c.frames.empty()) continue;
    Timing t;
    for (std::size_t i = 0; i < c.frames.size(); ++i) {
      const float d = static_cast<float>(c.frames[i].hold) / c.fps;
      t.total += d;
      if (i > 0) t.after_first += d;
    }
    timing_[c.name] = t;
  }
  // M7.1. A trigger is as askable as a clip — `request_clip("wake")` has to
  // mean "any entrance" for a script the same way it does for the policy
  // below — so the trigger names go in too, at the length of their *longest*
  // variant. Longest rather than the one that will actually be chosen because
  // the choice has not been made yet when the lease is measured, and a lease
  // shorter than the clip it covers cuts the animation off; too long merely
  // holds the last drawing a moment, which is what a one-shot does anyway.
  //
  // Single-member triggers are the whole of a no-variant definition and write
  // back the value the loop above already put there, so this cannot change
  // what such a definition does.
  for (const AvatarTrigger& trigger : def.triggers) {
    Timing longest;
    for (const AvatarVariant& v : trigger.variants) {
      if (v.clip_index >= def.clips.size()) continue;
      const auto it = timing_.find(def.clips[v.clip_index].name);
      if (it == timing_.end()) continue;
      if (it->second.total > longest.total) longest = it->second;
    }
    if (longest.total > 0.0f) timing_[trigger.name] = longest;
  }
}

float AvatarController::clip_length(const char* clip, float fallback) const {
  const auto it = timing_.find(clip);
  if (it != timing_.end()) return it->second.total;
  // A definition that does not declare the clip gets no reaction at all
  // rather than a second of the policy insisting on art that is not there —
  // the source would refuse the play() and the avatar would sit on the
  // previous clip while this object believed otherwise. The fallback is only
  // for the moments before any definition has loaded.
  return timing_.empty() ? fallback : 0.0f;
}

float AvatarController::roll_blink_wait() {
  std::uniform_real_distribution<float> d(tune_.blink_min, tune_.blink_max);
  return d(rng_);
}

void AvatarController::start_oneshot(const char* clip, Yield yield) {
  const float len = clip_length(clip, 1.2f);
  if (len <= 0.0f) return;
  oneshot_ = clip;
  oneshot_left_ = len;
  oneshot_yield_ = yield;
  // A new one-shot is the only thing that takes a held exit away, and every
  // caller does it by simply being one -- see `oneshot_holds_`.
  oneshot_holds_ = false;
  // Every one-shot starts with nothing booked behind it; start_reaction() is
  // the only thing that books one, and it does so immediately after this.
  // Clearing here rather than trusting the previous reaction to have been
  // consumed means a `child_merge` cut short by a `confused` cannot drag its
  // `happy` along behind the clip that replaced it.
  oneshot_follow_.clear();
}

void AvatarController::start_reaction(const char* clip, const char* follow, Yield yield) {
  start_oneshot(clip, yield);
  if (oneshot_left_ > 0.0f) {
    oneshot_follow_ = follow ? follow : "";
    return;
  }
  // `clip` is art this definition does not have. Play the follow-up on its
  // own rather than nothing: the reaction to a finished worker predates this
  // milestone and an avatar that never gained a `child_merge` still owes it.
  if (follow) start_oneshot(follow, yield);
}

void AvatarController::appear(bool mic_open) {
  appear_pending_ = true;
  // Sticky rather than assigned: a caller that cannot answer the question must
  // not be able to un-answer a caller that could.
  appear_mic_ = appear_mic_ || mic_open;
}

float AvatarController::depart() {
  // The trigger name rather than a clip name, exactly as the entrance uses
  // "wake": M7.1 picks between `depart` and `depart_fling`, and a definition
  // that declares only one of them -- or neither -- needs no special case
  // here, because find_trigger() answers for a group of one and clip_length()
  // answers 0 for a group of none.
  //
  // `depart` is the *avatar's own* exit and not M7.5's `child_depart`, which
  // is the blob that leaves when a worker spawns. The two were drawn under the
  // same word by two milestones at once; they are separate triggers now so
  // that neither can ever select the other's art.
  const float len = clip_length("depart", 0.0f);
  if (len <= 0.0f) return 0.0f;
  start_oneshot("depart", Yield::Exit);
  // The floor is suspended for the frame that starts the exit, for the mirror
  // of the reason it is suspended for an entrance. The dwell floors exist to
  // stop the visible clip flapping; here the band is already being held open
  // for a fixed length and every millisecond the floor delayed the exit by
  // would be a millisecond cut off its end, so the floor would not prevent a
  // flap, it would truncate a departure.
  entered_first_frame_ = true;
  return len;
}

bool AvatarController::request_clip(const std::string& clip, float seconds,
                                    std::string* error) {
  if (clip.empty()) {
    if (error) *error = "no clip";
    return false;
  }
  // Before any definition has loaded there is nothing to check against, and
  // refusing then would make a script's first message depend on how fast the
  // disk was. Once one has loaded, an unknown clip is refused outright.
  if (!timing_.empty() && timing_.find(clip) == timing_.end()) {
    if (error) *error = "no such clip: " + clip;
    return false;
  }
  float lease = seconds;
  if (lease <= 0.0f) lease = clip_length(clip.c_str(), 1.0f);
  if (lease <= 0.0f) lease = 1.0f;
  script_ = clip;
  script_left_ = std::min(lease, kScriptLeaseMax);
  return true;
}

void AvatarController::release_clip() {
  script_.clear();
  script_left_ = 0.0f;
}

AvatarPose AvatarController::update(const VoiceSession::Snapshot& snap, float dt) {
  // A stall — a hot reload, a resize, the debugger — must not fast-forward
  // the policy through a blink and half a one-shot on the frame it resumes.
  dt = std::clamp(dt, 0.0f, 0.25f);

  if (snap.state != last_state_) {
    last_state_ = snap.state;
    state_age_ = 0.0f;
  } else {
    state_age_ += dt;
  }
  idle_age_ = snap.state == VoiceSession::State::Idle ? idle_age_ + dt : 0.0f;
  // M1f.3's wake, first and unconditionally. Anything that is not Idle is the
  // user back at the desk (or Claude working for them), and both are reasons
  // to stop dozing. Placed above the edge test below so that a timeout landing
  // on a frame where the session has not yet settled into Idle still takes:
  // the clear is a level, the set is an edge, and a level cannot swallow an
  // edge that comes after it.
  if (snap.state != VoiceSession::State::Idle) dozed_ = false;
  entered_ += dt;

  // Fast to rise, slow to fall. The raw levels are per audio block and jump
  // around inside a syllable; feeding that straight into a clock rate makes
  // the body judder, and a slow release is also what a body does — it keeps
  // moving for a moment after the sound stops.
  const auto follow = [dt](float cur, float target) {
    const float tau = target > cur ? 0.08f : 0.25f;
    return cur + (target - cur) * std::min(1.0f, dt / tau);
  };
  mic_ = follow(mic_, std::clamp(snap.mic_level, 0.0f, 1.0f));
  speak_ = follow(speak_, std::clamp(snap.speak_level / tune_.speak_full, 0.0f, 1.0f));

  // ---- the world, and what changed in it ----
  if (!seeded_) {
    // The first snapshot is recorded, not reacted to. Otherwise a worker that
    // was already finished, or a failure count that is not zero, would fire a
    // reaction to something that happened before this object existed.
    seeded_ = true;
    last_failed_seq_ = snap.turn_failed_seq;
    last_timeout_seq_ = snap.listen_timeout_seq;
    for (const auto& w : snap.workers) {
      worker_state_[w.name] = w.state;
      // M7.5. The running count is recorded with the rest of the world, so a
      // worker already in flight when this object was built is not a child
      // that departed while nobody was looking.
      if (w.state == WorkerPool::State::Starting || w.state == WorkerPool::State::Working) {
        ++running_;
      }
    }
    blink_wait_ = roll_blink_wait();
  }
  // Cleared as a level, not an edge: once the session has left Listening the
  // entrance no longer has the microphone as an excuse, whatever happens next.
  //
  // **Above the entrance rather than below it** (19 Sep 2026). It used to run
  // afterwards, so the flag an entrance had just set for itself could be wiped
  // by the same frame's state. That was harmless while the only thing that set
  // it was `snap.state == Listening` -- which the clear then agreed with -- and
  // is not harmless now that appear() can say the microphone is the cause while
  // the snapshot has not caught up. Clearing first and setting second makes the
  // two orders say the same thing.
  if (snap.state != VoiceSession::State::Listening) oneshot_from_mic_ = false;
  // M7.2. The entrance, started here and asked for from outside — see
  // appear(). The trigger name rather than a clip name, so M7.1 picks the
  // variant and an avatar that declares none still gets its single `wake`.
  if (appear_pending_) {
    appear_pending_ = false;
    // The snapshot is taken at the top of the frame and M1f.5's auto-listen
    // latch opens the microphone *below* it, so on the one frame that matters
    // most -- the app booting straight into listening -- `snap.state` still
    // says Idle while the microphone that summoned the avatar is already open.
    // Asking the snapshot alone therefore called the entrance's own cause an
    // interruption, and the next frame's Listening pre-empted it: measured in a
    // boot log as `avatar clip: wake (reaction)` followed by
    // `avatar clip: listen (listening) after 0.06s` -- an entrance authored at
    // 1.87 s cut to four frames, on every auto-listen boot. `mic_open` is the
    // caller's answer to the same question, read after the latch; either one is
    // enough, because either one means the microphone is why the avatar came.
    oneshot_from_mic_ = appear_mic_ || snap.state == VoiceSession::State::Listening;
    appear_mic_ = false;
    start_oneshot("wake", Yield::Entrance);
    // The dwell floor protects what is *on screen* from being replaced too
    // soon, and nothing was: the clip the policy had been playing was behind a
    // zero alpha for as long as the avatar was hidden. Measured before this
    // line existed: a turn in "shown when talking" appeared mid-`think`, and
    // the thought bubble's 0.80 s sprite floor held the entrance off for
    // 0.47 s of it — so the summon put an unrelated clip on screen and the
    // entrance arrived late to its own appearance.
    entered_first_frame_ = true;
  }
  if (snap.turn_failed_seq != last_failed_seq_) {
    last_failed_seq_ = snap.turn_failed_seq;
    start_oneshot("confused", Yield::ToMic);
  }
  // M1f.3. The latched microphone gave up on a room with nobody in it. This is
  // the only place the timeout is read, and `listen_timeout_seq` is the only
  // thing read: the status line's words and the microphone icon's face are
  // other readers of the same edge, and none of them can fire on a Stop, on a
  // finished reply or on a hand closing the latch, because none of those
  // touches this counter (voice_session.cpp::close_latch_after_silence is its
  // single writer).
  if (snap.listen_timeout_seq != last_timeout_seq_) {
    last_timeout_seq_ = snap.listen_timeout_seq;
    dozed_ = true;
  }
  // M7.5. Workers, as a count and as two edges. See the block on `running_`
  // in the header for why the count is a level and what that buys.
  int running = 0;
  for (const auto& w : snap.workers) {
    if (w.state == WorkerPool::State::Starting || w.state == WorkerPool::State::Working) {
      ++running;
    }
    const auto it = worker_state_.find(w.name);
    const bool changed = it == worker_state_.end() || it->second != w.state;
    worker_state_[w.name] = w.state;
    if (!changed) continue;
    // The reaction rides home with the child rather than firing beside it.
    // `happy` still holds the floor through the spoken report, which was
    // always the point of it; it just starts a second later, behind the
    // arrival it is reacting to.
    if (w.state == WorkerPool::State::Done) child_merge_follow_ = "happy";
    // A worker that fell over is the same news as a turn that fell over, and
    // the avatar has one vocabulary for it.
    else if (w.state == WorkerPool::State::Failed) child_merge_follow_ = "confused";
    else continue;
    // Either way the child is back. Set here as well as by the count below,
    // because a worker that reports and is removed from the list on the same
    // frame would otherwise be a fall nobody saw.
    pending_child_merge_ = true;
  }
  // Names are forgotten when the pool forgets them; otherwise a long session
  // accumulates one map entry per worker that ever ran.
  if (worker_state_.size() > snap.workers.size()) {
    for (auto it = worker_state_.begin(); it != worker_state_.end();) {
      bool present = false;
      for (const auto& w : snap.workers) present = present || w.name == it->first;
      it = present ? std::next(it) : worker_state_.erase(it);
    }
  }
  if (running > running_) pending_child_depart_ = true;
  if (running < running_) pending_child_merge_ = true;
  running_ = running;
  // One at a time, and a return before a departure: a merge is tied to a
  // sentence the user is hearing right now, a departure is tied to work that
  // has already started without it.
  if (oneshot_left_ <= 0.0f) {
    if (pending_child_merge_) {
      pending_child_merge_ = false;
      const char* carried = child_merge_follow_;
      child_merge_follow_ = nullptr;
      start_reaction("child_merge", carried, Yield::ToMic);
    } else if (pending_child_depart_) {
      pending_child_depart_ = false;
      // The trigger name, not a clip name, so M7.1 picks which of the child
      // departures this one is and a third drawing is a line of JSON rather
      // than a line here. `child_depart`, not M2.3c's `depart`: that one is
      // the whole avatar leaving the band, and a worker spawning must never
      // be able to reach it.
      start_reaction("child_depart", nullptr, Yield::ToMic);
    }
  }

  // Context fullness, latched with hysteresis so a reading sitting on the
  // threshold does not switch the steam on and off once a second.
  if (snap.usage_stats.ctx >= 0.0) {
    if (!ctx_high_ && snap.usage_stats.ctx >= tune_.ctx_enter) {
      ctx_high_ = true;
      frustrated_wait_ = 1.0f;  // react soon, but not on the same frame
    } else if (ctx_high_ && snap.usage_stats.ctx < tune_.ctx_exit) {
      ctx_high_ = false;
    }
  }

  // ---- what the session says we should be doing ----
  // nullptr means "no opinion, keep whatever is playing".
  const char* want = "idle";
  const char* why = "idle";
  switch (snap.state) {
    case VoiceSession::State::Loading:
      // The loading overlay owns the whole window and the avatar is drawn at
      // zero alpha behind it, so there is nothing to decide.
      want = "idle";
      why = "loading";
      break;
    case VoiceSession::State::Listening:
      want = "listen";
      why = "listening";
      break;
    case VoiceSession::State::Speaking:
      want = "talk";
      why = "speaking";
      break;
    case VoiceSession::State::Thinking:
      if (state_age_ >= tune_.think_delay) {
        want = "think";
        why = "thinking";
      } else {
        // Short of the delay, hold whatever was there — usually `listen`,
        // which reads as attention persisting past the end of the sentence.
        want = nullptr;
        why = "thinking";
      }
      break;
    case VoiceSession::State::Failed:
      // Engine bring-up failed and nothing is coming back. `confused` is the
      // only honest thing in the set, and it is a standing state here rather
      // than a burst because the condition does not clear.
      want = "confused";
      why = "failed";
      break;
    case VoiceSession::State::Idle:
      if (ctx_high_) {
        frustrated_wait_ -= dt;
        if (frustrated_wait_ <= 0.0f) {
          frustrated_wait_ = tune_.frustrated_period;
          start_oneshot("frustrated", Yield::ToMic);
        }
      }
      // Two entrances, one clip. The droop means the same thing either way --
      // nobody is talking to me -- and the difference is only how it was
      // found out, which is why `why` distinguishes them for the trace line
      // and nothing else does.
      if (dozed_) {
        want = "sleepy";
        why = "listen-timeout";
      } else if (idle_age_ >= tune_.sleepy_seconds) {
        want = "sleepy";
        why = "sleepy";
      }
      break;
  }

  // ---- reactions outrank the ambient state, for as long as they last ----
  if (oneshot_left_ > 0.0f || oneshot_holds_) {
    if (oneshot_left_ > 0.0f) oneshot_left_ -= dt;
    const bool at_mic = snap.state == VoiceSession::State::Listening;
    bool preempt = at_mic;
    switch (oneshot_yield_) {
      case Yield::ToMic:
        break;
      case Yield::Entrance:
        // The microphone that was already open when this entrance began is
        // the reason it began; only a later one takes it.
        preempt = at_mic && !oneshot_from_mic_;
        break;
      case Yield::Exit:
        // Nothing. See Yield::Exit: the avatar is going, and being wanted
        // again arrives as a summon that replaces this one-shot rather than as
        // a state that pre-empts it.
        preempt = false;
        break;
    }
    // M2.3c. An exit that has run out does not end: it holds its last drawing,
    // which the contract says is empty, until something else asks for the
    // avatar. The alternative -- letting it expire into the ambient branch --
    // puts `idle` on screen for however many frames the band's own hold has
    // left, and that is the residual flash. See `oneshot_holds_`.
    if (oneshot_yield_ == Yield::Exit && oneshot_left_ <= 0.0f) {
      oneshot_left_ = 0.0f;
      oneshot_holds_ = true;
      want = oneshot_.c_str();
      why = "exit";
    } else if (preempt || oneshot_left_ <= 0.0f) {
      const bool finished = !preempt;
      oneshot_left_ = 0.0f;
      oneshot_.clear();
      // M7.5. The clip booked behind this one — `child_merge` hands over to `happy`
      // without a frame of something else in between, which is what makes the
      // arrival and the reaction to it one animation. A one-shot that was
      // pre-empted hands over to nobody: the microphone took the avatar, and
      // a reaction that had not started yet does not get to take it back.
      std::string booked;
      booked.swap(oneshot_follow_);
      if (finished && !booked.empty()) {
        start_oneshot(booked.c_str(), Yield::ToMic);
        if (oneshot_left_ > 0.0f) {
          want = oneshot_.c_str();
          why = "reaction";
        }
      }
    } else {
      want = oneshot_.c_str();
      why = "reaction";
    }
  }

  // ---- a script's lease outranks the reactions, and loses to the mic ----
  // Placed after the one-shots deliberately: a script asking for a clip is
  // somebody's deliberate act and a reaction is the app's reflex, so the act
  // wins. The microphone still beats both, through the same test `Yield::ToMic`
  // uses, because a script must never be able to hold the avatar through the
  // user trying to speak to it.
  if (script_left_ > 0.0f) {
    script_left_ -= dt;
    // Released outright rather than parked: a lease that resumed after the
    // reply would put a scripted clip back on screen tens of seconds after the
    // moment it was describing.
    if (snap.state == VoiceSession::State::Listening || script_left_ <= 0.0f) {
      release_clip();
    } else {
      want = script_.c_str();
      why = "script";
    }
  }

  // ---- blink, woven into idle ----
  if (want && std::strcmp(want, "idle") == 0) {
    if (blink_left_ > 0.0f) {
      blink_left_ -= dt;
      if (blink_left_ > 0.0f) {
        want = "blink";
        why = "blink";
      }
    } else {
      blink_wait_ -= dt;
      // Never within blink_guard of a state change or of entering this clip:
      // an eyelid closing on the frame the avatar changes what it is doing
      // does not read as a blink, it reads as a dropped frame.
      if (blink_wait_ <= 0.0f && state_age_ >= tune_.blink_guard &&
          entered_ >= tune_.blink_guard) {
        const auto it = timing_.find("blink");
        blink_left_ = it != timing_.end() ? it->second.after_first : 0.0f;
        blink_wait_ = roll_blink_wait();
        if (blink_left_ > 0.0f) {
          want = "blink";
          why = "blink";
        }
      }
    }
  } else if (blink_left_ > 0.0f) {
    // Something happened mid-blink. Drop the lid outright rather than finish
    // it: every other clip opens on an open eye, so the cut is invisible, and
    // 0.2 s is not a delay worth putting in front of the user's answer.
    blink_left_ = 0.0f;
    blink_wait_ = roll_blink_wait();
  }

  // ---- commit, against the dwell floor ----
  if (want && current_ != want) {
    // Listening is the one transition where latency is worse than a twitch:
    // the user has just pressed Talk and is waiting to be acknowledged, so it
    // goes through the plain floor.
    //
    // It does *not* go through a sprite's. An accessory costs a body slide in
    // and another back out, and tearing one down sooner than it took to raise
    // is the worst-looking thing this policy can do — worse than a third of a
    // second of lag. Measured, not theorised: VoiceSession flutters
    // Thinking → Speaking → Idle → Listening inside a second while a reply is
    // still arriving (its own race, seen in the M2.4 trace), and with the
    // bypass unconditional that flutter put the thought bubble up and pulled
    // it down again in 0.48 s. With it, the 0.80 s floor swallows the whole
    // flutter and the avatar goes think → talk, which is what happened.
    //
    // A script's lease is urgent for the same reason listening is: somebody is
    // waiting to see whether the message landed, and a third of a second of
    // nothing is indistinguishable from the message being dropped. It is held
    // to the same exception — an accessory that is up has already cost a body
    // slide and is not torn down early for anybody.
    //
    // M7.5's `child_merge` is urgent for the third version of the same reason. The
    // child coming home and the sentence the worker's report is being read
    // out in are one event to the user, and the speech does not wait for the
    // floor — so a merge held back a third of a second is a merge that lands
    // after "Finished" and reads as a reaction to it rather than as the same
    // thing. `child_depart` is deliberately *not* on this list: nothing is
    // being
    // said over it, and the spawn it decorates has already happened.
    const bool urgent =
        (std::strcmp(want, "listen") == 0 || std::strcmp(want, "child_merge") == 0 ||
         std::strcmp(why, "script") == 0) &&
        avatar_clip_sprite(current_) == nullptr;
    // Blink is exempt at both ends. It is shorter than the floor by design,
    // so a floor would strand the lid shut.
    const bool blinking = current_ == "blink" || std::strcmp(want, "blink") == 0;
    const float floor =
        avatar_clip_sprite(current_) ? tune_.sprite_dwell : tune_.min_dwell;
    if (urgent || blinking || entered_first_frame_ || entered_ >= floor) {
      last_dwell_ = entered_;
      current_ = want;
      reason_ = why;
      entered_ = 0.0f;
    }
  }
  // One frame only, and cleared whether or not the commit above took it: the
  // exemption is "the avatar was not on screen a moment ago", which stops
  // being true immediately.
  entered_first_frame_ = false;

  AvatarPose pose;
  pose.clip = current_;
  if (current_ == "listen") {
    // The clip table asks for "body pulses with mic RMS". The frames are
    // fixed art, so what is actually available is the rate at which the slime
    // cycles into and out of its lean: quiet and it holds the lean, which is
    // what the clip was authored for, louder and it leans again more often.
    pose.speed = 1.0f + 0.50f * mic_;
  } else if (current_ == "talk") {
    // Likewise "bounce amplitude driven by the playback level" is not
    // reachable without new art — the hop is drawn, not computed — so the hop
    // *rate* carries it instead: 0.8 s a hop at a speaking level, down to
    // ~0.6 s when the reply is loud, and slightly slack through the gaps
    // between sentences.
    pose.speed = 0.85f + 0.50f * speak_;
  }
  return pose;
}

void avatar_apply(const AvatarPose& pose, AvatarSource& src, const char* status_sprite) {
  // A condition outranks a mood; see the note on the declaration for why.
  const char* wanted = status_sprite ? status_sprite : avatar_clip_sprite(pose.clip);
  // Both the accessories and the clip are set on the same frame, before the
  // compose() that reads them, so there is no frame in which one is ahead of
  // the other. show_sprite() and play() are both no-ops when nothing changed.
  for (const AvatarSprite& s : src.definition().sprites) {
    src.show_sprite(s.name, wanted != nullptr && s.name == wanted);
  }
  // blink is entered at frame 1: frame 0 of blink.txt is a two-second hold of
  // the open eye, and that interval belongs to the controller (randomised) —
  // see AvatarSource::play.
  src.play(pose.clip, pose.clip == "blink" ? 1 : 0);
  src.set_speed(pose.speed);
}

}  // namespace aii
