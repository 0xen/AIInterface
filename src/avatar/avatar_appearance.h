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
//   `wanted`           -- AvatarPresence::update(mode, engagement): the
//                         mode switch, the
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

  // M2.3c. There are now two leave fades, and which one runs depends on
  // whether there was any art behind the departure.
  //
  // `kLeaveSeconds` is M1.6's original and it is the *fallback*: an avatar
  // whose definition declares no exit clip has nothing to watch leave, so it
  // still dissolves over a tenth of a second exactly as it did, and M7.2's
  // reasoning for that number is untouched for the case it was written about.
  //
  // `kLeaveAfterExitSeconds` is what runs behind a real exit clip, and it is
  // the pop-in's number because it now has the pop-in's job and no other. The
  // exit's last frame is empty by contract, so by the time this ramp starts
  // there is nothing on screen to dissolve: the animation made the avatar
  // absent, and all the ramp is still good for is the one case where the clip
  // did *not* finish -- a departure cut short by the app closing or by the
  // band being taken some other way -- where a body is still drawn and a hard
  // cut would read as a dropped frame. Five frames covers that, and running
  // the old 0.16 s here would instead put a tenth of a second of empty band on
  // screen after every normal exit, waiting for nothing.
  static constexpr float kLeaveSeconds = 0.16f;
  static constexpr float kLeaveAfterExitSeconds = kPopSeconds;

  struct Frame {
    float alpha = 0.0f;
    // True on exactly the frame the avatar starts arriving. The single
    // appearance event: whoever consumes it is the only thing that gets to
    // decide what an arrival looks like.
    bool summoned = false;
    // M2.3c, and the exact mirror of `summoned`: true on the single frame the
    // avatar starts leaving. Whoever consumes it is the only thing that gets
    // to decide what a departure looks like, and it must answer with
    // hold_exit() on that same frame -- see there for why the alpha does not
    // move until it has.
    bool dismissed = false;
  };

  Frame update(bool wanted, bool loader_on_screen, float dt);

  // M2.3c. The answer to a `dismissed` frame: keep the band at full opacity
  // for `seconds` so an exit clip can play in it, then fade.
  //
  // `seconds <= 0` means "no exit art", and that is not an error -- it is
  // every avatar definition written before this milestone, and the M1.6
  // dissolve is still the right thing for one. The fade then starts
  // immediately and takes kLeaveSeconds, which is byte-for-byte what M7.2
  // did.
  //
  // It has to be a second call rather than an argument to update() because
  // only the controller can say how long the departure is, and the controller
  // has not chosen a variant until it is told the avatar is going. So update()
  // freezes the alpha on the dismissed frame and waits exactly one frame for
  // this; if it never comes, the frame after resumes as the no-art case,
  // which is the same behaviour a caller that has never heard of exits gets.
  void hold_exit(float seconds);

  // Whether the avatar is on screen at all -- showing, leaving, or holding
  // still while an exit clip plays in it.
  //
  // **The window's height no longer follows this** (user, 18 Sep 2026). It
  // used to, and that was the flicker: the band was reserved only while the
  // avatar had alpha, so every appearance was also a window resize, and a
  // geometry change a frame away from an alpha change on a per-pixel-alpha
  // surface is a visible flash. The band is reserved by *mode* now -- see
  // `nextBand` in main.cpp -- so the avatar fades into and out of an empty
  // region that was already the right size, and M2.3c's exit clip plays in a
  // band that was never going to be taken out from under it. Nothing here
  // changed: this object still owns the alpha and the two edges, which is one
  // fewer thing than it owned before.
  //
  // Kept as the honest answer to "is the avatar on screen", for a caller that
  // needs to know without inspecting the alpha. It stays true across the exit
  // hold for that reason, even though no window geometry reads it any more.
  bool present() const { return showing_ || exit_left_ > 0.0f || fade_ > 0.0f; }

 private:
  // The one place the linear fade becomes the eased alpha, so every early
  // return out of update() leaves by the same door.
  Frame finish(Frame out) const;

  bool showing_ = false;
  float fade_ = 0.0f;  // linear 0..1; the Frame's alpha is this, eased
  // The exit clip's remaining length. While it is positive the alpha is
  // pinned at 1 and the band stays reserved.
  float exit_left_ = 0.0f;
  // Set on the dismissed frame, cleared by hold_exit() or by the next update()
  // if nobody answered.
  bool awaiting_exit_ = false;
  // Which of the two leave fades this departure gets.
  bool exit_had_art_ = false;
};

}  // namespace aii
