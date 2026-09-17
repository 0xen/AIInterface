#pragma once
// M7.2. The one place the avatar appears and disappears.
//
// Before this there were four, and they disagreed. The loading handoff faded
// the band in across M1.5's 0.42 s crossfade; M1.6's visibility fade ran a
// separate 0.16 s alpha ramp for a mode switch, for "shown when talking"
// following a turn in and out, and for hold-to-dictate's release; and M2.4
// fired the entrance clip off a third signal again -- the session leaving
// Loading -- which is the *start* of the handoff, so the entrance played
// underneath a dissolving loading screen and behind a rising alpha and was
// over before the avatar was opaque. The clip was authored twice (M7.1) and
// visible on none of the paths that matter.
//
// So: one object, two inputs, and everything else is derived from them.
//
//   `wanted`           -- avatar_visible(mode, state): the mode switch, the
//                         turn start, the hold-to-dictate release and every
//                         later reason all arrive as this one bool. They are
//                         not special-cased here and must not be: a path this
//                         file has to learn about is a path that can be
//                         forgotten, which is exactly how the four came to
//                         disagree. (f0d678f's delayed appearance on release
//                         is one such reason and nothing here knows its name,
//                         so vetoing it changes nothing in this file.)
//   `loader_on_screen` -- the M1.5 loading overlay still has opacity. The
//                         avatar may not appear underneath it, which is the
//                         whole of the startup fix.
//
// and two outputs: the alpha to draw the band at, and a one-frame edge that
// says *the avatar is arriving now* -- which is what fires the entrance clip,
// from here and nowhere else.
//
// **Nothing in here can delay speech.** It is a float and two bools updated
// from the frame loop; the session's audio runs on its own threads and is
// never consulted, waited on or gated. The avatar arriving because Claude is
// about to talk cannot make Claude talk later, because the only thing this
// object does with that fact is set an alpha.

namespace aii {

class AvatarAppearance {
 public:
  // How long the pop-in takes, against the 0.16 s alpha fade it replaces.
  //
  // Half, and half deliberately rather than nothing. The reveal is no longer
  // carrying the transition -- the entrance clip is, and it starts on the
  // same frame -- so all this has to do is stop a per-pixel-alpha surface
  // snapping into existence against the desktop in one frame. At 60 Hz this
  // is five frames: enough that the arrival reads as a materialise, few
  // enough that the 0.8 s entrance is 90% of what the eye actually sees.
  //
  // The floor is about three frames (~0.05 s). Below that there is no ramp
  // left to perceive at 60 Hz and the result is a hard cut, which reads as a
  // dropped frame rather than as an entrance -- the same failure this project
  // keeps having to design out. 0.08 s sits just above it with room for a
  // slower display.
  static constexpr float kPopSeconds = 0.08f;

  // Leaving keeps M1.6's 0.16 s. Symmetry would be wrong here and the
  // asymmetry is the point: an arrival has 0.8-1.9 s of authored animation
  // behind it and a departure has none yet (M2.3c owns the exit clip), so
  // snapping out in five frames would be the glitch the pop-in is not.
  static constexpr float kLeaveSeconds = 0.16f;

  struct Frame {
    float alpha = 0.0f;
    // True on exactly the frame the avatar starts arriving. The single
    // appearance event: whoever consumes it is the only thing that gets to
    // decide what an arrival looks like.
    bool summoned = false;
  };

  Frame update(bool wanted, bool loader_on_screen, float dt);

  // Whether the band is still needed at all -- showing, or on the way out.
  // The window's height follows this, and it has to stay true across the
  // frame the pop-in starts on, when the alpha is still 0.
  bool present() const { return showing_ || fade_ > 0.0f; }

 private:
  bool showing_ = false;
  float fade_ = 0.0f;  // linear 0..1; the Frame's alpha is this, eased
};

}  // namespace aii
