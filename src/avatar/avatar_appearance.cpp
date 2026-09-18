#include "avatar_appearance.h"

#include <algorithm>

namespace aii {

AvatarAppearance::Frame AvatarAppearance::update(bool wanted, bool loader_on_screen,
                                                 float dt) {
  // A stall -- a hot reload, a resize, the debugger -- must not fast-forward
  // the pop-in past the frames it is made of. Same clamp, same reason, as
  // AvatarController::update().
  dt = std::clamp(dt, 0.0f, 0.25f);

  // The loading screen owns the whole window, so "wanted" is not yet "here".
  // This one `&&` is the startup fix: the entrance no longer fires when the
  // session leaves Loading, it fires when the loader has finished leaving.
  const bool show = wanted && !loader_on_screen;

  Frame out;
  if (show != showing_) {
    showing_ = show;
    // Fired on the rising edge only, and fired even if the avatar is still
    // fading out from a previous appearance -- a turn that starts inside the
    // tail of the last one is a new arrival, not a resumed one.
    out.summoned = show;
    // M2.3c, the falling edge. The exit is announced and the alpha is left
    // alone this frame: see hold_exit().
    out.dismissed = !show;
    awaiting_exit_ = !show;
    // An arrival during a departure abandons the departure outright rather
    // than letting it run out underneath the entrance. Two clips in the band
    // at once is the mush M2.3c's design note says to avoid, and the entrance
    // that `summoned` is about to fire replaces the exit one-shot anyway.
    if (show) exit_left_ = 0.0f;
  }
  if (out.dismissed) return finish(out);

  // Waiting a frame for hold_exit() and not getting it: whoever owns this
  // object does not know about exits, so this is the M1.6 dissolve.
  if (awaiting_exit_) {
    awaiting_exit_ = false;
    exit_had_art_ = false;
  }
  // The clip plays in a band held at full opacity. Nothing fades while it
  // runs -- the art is doing the leaving, and a dissolve over the top of it
  // would be the second transition this milestone exists to delete.
  if (exit_left_ > 0.0f) {
    exit_left_ = std::max(exit_left_ - dt, 0.0f);
    return finish(out);
  }

  const float leave = exit_had_art_ ? kLeaveAfterExitSeconds : kLeaveSeconds;
  fade_ = std::clamp(fade_ + (showing_ ? dt / kPopSeconds : -dt / leave),
                     0.0f, 1.0f);
  return finish(out);
}

void AvatarAppearance::hold_exit(float seconds) {
  awaiting_exit_ = false;
  exit_had_art_ = seconds > 0.0f;
  exit_left_ = std::max(seconds, 0.0f);
}

AvatarAppearance::Frame AvatarAppearance::finish(Frame out) const {
  // Hermite, so neither end of the pop has a visible corner in it. Over five
  // frames this matters more than it would over ten, not less: a linear ramp
  // this short is read as two hard edges with a gradient between them.
  out.alpha = fade_ * fade_ * (3.0f - 2.0f * fade_);
  return out;
}

}  // namespace aii
