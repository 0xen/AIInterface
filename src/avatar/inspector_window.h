#pragma once
// M5.1: the prompt inspector — a **third** real OS window, opened and closed
// from the sidebar's own button.
//
// This file is deliberately a near-copy of `sidebar_window.h`/`.cpp`: the
// sidebar's header says M5 "should be able to copy this file rather than the
// spike", and copying it is what this is. Everything one extra window needs is
// owned together so it can be created and torn down as a unit — its own
// PresentationTarget, Swapchain, FrameRenderer, ImGui context and HWND
// subclass, sharing only the Instance, the Device and the platform backend
// with the widget.
//
// Where it differs from the strip, and why:
//
//  - **Decorated, resizable, opaque, not topmost.** The strip is a 48 px tool
//    window pinned beside the widget; this is a document window the user reads
//    and puts where they want. So it has a frame to drag and a corner to pull,
//    its swapchain is opaque (there is no desktop to show through a page of
//    text), and it does not float over other applications — a 1100x700 always-
//    on-top window is not a widget, it is an obstruction.
//  - **It has a life of its own.** The strip is created with the widget and
//    destroyed with it; this one is created on a click and destroyed on the
//    next one, so create() and the destructor run repeatedly inside a live
//    frame loop rather than once at each end of it.
//  - **It remembers where it was.** `geometry()` hands back the live window
//    position and client size every frame; the frame loop mirrors that into
//    settings.json and passes it back to the next create(). The window itself
//    owns the *rule* for a remembered rect that no longer lands on a monitor
//    (see fit_to_desktop in the .cpp) — the caller only stores four numbers.
//
// The three things a second window taught this app, which apply here unchanged
// (see sidebar_window.h for the long version):
//
//  - Its swapchain is `vsync = false`; two vsynced swapchains on one thread
//    halve the frame rate, and three would third it.
//  - **Every entry point makes its own ImGui context current.**
//    `ImGui::CreateContext()` restores the previously current context before it
//    returns, so with three layers nothing may rely on the ambient one. Getting
//    this wrong is an access violation inside ImGui_ImplDX12_NewFrame with its
//    assert compiled out — a crash in a window that is not the one at fault.
//  - **Its size goes through `PresentationTarget::setSize()`, never a raw
//    `SetWindowPos`.** The backend answers size queries with the size it holds;
//    a raw resize grows the window rect while the client rect — the part DWM
//    composites — stays put, and the window silently clips. Position through
//    Win32 is fine; size is not. Here that applies to the *restore* path; a
//    resize the user performs with the mouse is the OS's own and is only read
//    back (poll_size(), in draw()).
#include <memory>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace rend {
namespace gpu {
class Device;
class Instance;
}  // namespace gpu
namespace platform {
class IPlatformBackend;
}  // namespace platform
}  // namespace rend

namespace aii {

// Where the window is and how big it is, in screen pixels: the position is the
// **window** rect's top-left (what SetWindowPos takes) and the size is the
// **client** area (what createTarget and setSize take). Mixing the two is how a
// remembered window creeps down the screen by the height of its title bar on
// every restart, so they are named rather than commented.
struct InspectorGeometry {
  int x = 0;
  int y = 0;
  unsigned w = 1100;
  unsigned h = 700;
  // False means "no remembered position" — first run, or a settings file with
  // no inspector section — and the window is centred. It is a flag rather than
  // a sentinel coordinate because 0,0 is a perfectly good position and a
  // multi-monitor desktop has real negative ones.
  bool placed = false;
};

class InspectorWindow {
 public:
  // Null on failure, with the reason in `error`. A failure is survivable: the
  // sidebar button stays, and the next click tries again. `wanted` is the
  // remembered geometry; it is fitted to the desktop as it stands *now* before
  // the window is made, so an unplugged monitor or a resolution change cannot
  // put the window somewhere the user cannot reach it.
  static std::unique_ptr<InspectorWindow> create(rend::platform::IPlatformBackend& backend,
                                                 rend::gpu::Instance& instance,
                                                 rend::gpu::Device& device, float font_px,
                                                 InspectorGeometry wanted, std::string* error);
  ~InspectorWindow();
  InspectorWindow(const InspectorWindow&) = delete;
  InspectorWindow& operator=(const InspectorWindow&) = delete;

  // One frame: its own ImGui pass, its own present. Returns **false when the
  // window wants to go away** — the user pressed the title bar's close box —
  // and the caller should destroy it, which it may only do between frames.
  //
  // Call it from the same place the strip is drawn, before the widget's own
  // ImGui frame. It re-reads its client size first, so a resize the user is
  // in the middle of is picked up on the frame it happens.
  //
  // **Seam for M5.2.** The three-section list is drawn from a snapshot the
  // frame loop builds — `PromptStore`'s rows plus the session's injection
  // state — and passed in here as a second argument. It is passed rather than
  // reached for: the store is written on the turn thread, and a window that
  // read it directly would be reading it mid-write. M5.3's token totals and
  // M5.4's unloaded prompts ride in the same snapshot.
  bool draw(float dt);

  // Where the window is now, for the caller to remember. Read after draw().
  InspectorGeometry geometry() const;

  HWND hwnd() const;

 private:
  InspectorWindow() = default;
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace aii
