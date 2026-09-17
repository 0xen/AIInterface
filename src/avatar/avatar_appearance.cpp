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
  }

  fade_ = std::clamp(fade_ + (showing_ ? dt / kPopSeconds : -dt / kLeaveSeconds),
                     0.0f, 1.0f);
  // Hermite, so neither end of the pop has a visible corner in it. Over five
  // frames this matters more than it would over ten, not less: a linear ramp
  // this short is read as two hard edges with a gradient between them.
  out.alpha = fade_ * fade_ * (3.0f - 2.0f * fade_);
  return out;
}

}  // namespace aii
