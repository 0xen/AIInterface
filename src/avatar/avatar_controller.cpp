#include "avatar_controller.h"

#include <algorithm>
#include <cstring>

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
    for (const auto& w : snap.workers) worker_state_[w.name] = w.state;
    blink_wait_ = roll_blink_wait();
  }
  if (!woke_ && snap.state != VoiceSession::State::Loading) {
    woke_ = true;
    start_oneshot("wake", Yield::ToUser);
  }
  if (snap.turn_failed_seq != last_failed_seq_) {
    last_failed_seq_ = snap.turn_failed_seq;
    start_oneshot("confused", Yield::ToMic);
  }
  for (const auto& w : snap.workers) {
    const auto it = worker_state_.find(w.name);
    const bool changed = it == worker_state_.end() || it->second != w.state;
    worker_state_[w.name] = w.state;
    if (!changed) continue;
    if (w.state == WorkerPool::State::Done) start_oneshot("happy", Yield::ToMic);
    // A worker that fell over is the same news as a turn that fell over, and
    // the avatar has one vocabulary for it.
    else if (w.state == WorkerPool::State::Failed) start_oneshot("confused", Yield::ToMic);
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
      if (idle_age_ >= tune_.sleepy_seconds) {
        want = "sleepy";
        why = "sleepy";
      }
      break;
  }

  // ---- reactions outrank the ambient state, for as long as they last ----
  if (oneshot_left_ > 0.0f) {
    oneshot_left_ -= dt;
    const bool preempt =
        oneshot_yield_ == Yield::ToUser
            ? (snap.state == VoiceSession::State::Listening ||
               snap.state == VoiceSession::State::Thinking ||
               snap.state == VoiceSession::State::Speaking)
            : (snap.state == VoiceSession::State::Listening);
    if (preempt || oneshot_left_ <= 0.0f) {
      oneshot_left_ = 0.0f;
      oneshot_.clear();
    } else {
      want = oneshot_.c_str();
      why = "reaction";
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
    const bool urgent =
        std::strcmp(want, "listen") == 0 && avatar_clip_sprite(current_) == nullptr;
    // Blink is exempt at both ends. It is shorter than the floor by design,
    // so a floor would strand the lid shut.
    const bool blinking = current_ == "blink" || std::strcmp(want, "blink") == 0;
    const float floor =
        avatar_clip_sprite(current_) ? tune_.sprite_dwell : tune_.min_dwell;
    if (urgent || blinking || entered_ >= floor) {
      last_dwell_ = entered_;
      current_ = want;
      reason_ = why;
      entered_ = 0.0f;
    }
  }

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
