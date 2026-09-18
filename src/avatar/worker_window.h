#pragma once
// M9.2: a **worker chat window** — one per worker the user has asked to watch,
// opened from the worker strip and placed to the left of the widget.
//
// It is a near-copy of `inspector_window.h`/`.cpp`, which is itself a near-copy
// of `sidebar_window.*`: its own PresentationTarget, Swapchain, FrameRenderer,
// ImGui context and HWND subclass, sharing only the Instance, the Device and
// the platform backend with the widget. What is new here is that there may be
// **several of these at once**, which changes exactly two things and nothing
// else:
//
//  - **N ImGui contexts, not three.** `ImGui::CreateContext()` restores the
//    previously current context before returning, so nothing anywhere may rely
//    on the ambient one; `ImGuiLayer` already answers this (its `make_current()`
//    moves both the context and the `g_state` the D3D12 SRV allocator reads),
//    and every entry point below calls it. The number of contexts is not the
//    hazard — *assuming* which one is current is — so N is no worse than three
//    as long as that rule holds. The frame loop caps how many may be open at
//    once for a different reason: each layer carries its own font atlas.
//  - **Placement has to be decided by the caller.** A window that put itself
//    "to the left of the widget" would put every window in the same place; the
//    frame loop owns the slot, and hands it in as `wanted`.
//
// The rest is the inspector's contract unchanged: decorated, resizable, opaque,
// not always-on-top; the size goes through `createTarget`/`setSize()` and never
// a raw `SetWindowPos` (position through Win32 is fine, size is not); and
// WM_CLOSE is **swallowed and latched**, because the SDL backend maps both quit
// paths onto one identityless `Event::CloseRequested` that the frame loop quits
// the application on. If the subclass cannot be installed the window refuses to
// open, rather than shipping an X that kills the app.
//
// **Read-only, by the user's decision.** This window shows what a worker is
// doing. It cannot talk to one, cancel one or edit anything, and there is no
// seam here for it to grow one later — `draw()` returns whether the window is
// still wanted and nothing else.
#include <memory>
#include <string>

#include "core/worker_pool.h"

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

// Where the window is and how big it is, in screen pixels. The position is the
// **window** rect's top-left (what SetWindowPos takes) and the size is the
// **client** area (what createTarget and setSize take) — mixing the two is how
// a remembered window creeps down the screen by the height of its title bar on
// every restart, so they are named rather than commented.
//
// The default size is the widget's own 360 px wide by a chat's worth tall: this
// window is meant to read as a sibling of the primary chat, and a sibling that
// is twice as wide is a different kind of thing.
struct WorkerGeometry {
  int x = 0;
  int y = 0;
  unsigned w = 360;
  unsigned h = 520;
  bool placed = false;
};

class WorkerWindow {
 public:
  // Null on failure, with the reason in `error`. A failure is survivable: the
  // strip's icon stays and the next click tries again. `wanted` is fitted to
  // the desktop as it stands *now* before the window is made, so a slot that
  // falls off the left of the screen cannot put the window somewhere the user
  // cannot reach it.
  static std::unique_ptr<WorkerWindow> create(rend::platform::IPlatformBackend& backend,
                                              rend::gpu::Instance& instance,
                                              rend::gpu::Device& device, float font_px,
                                              const std::string& worker, WorkerGeometry wanted,
                                              std::string* error);
  ~WorkerWindow();
  WorkerWindow(const WorkerWindow&) = delete;
  WorkerWindow& operator=(const WorkerWindow&) = delete;

  // One frame: its own ImGui pass, its own present. Returns **false when the
  // window wants to go away** — the user pressed the title bar's close box —
  // and the caller should destroy it, which it may only do between frames.
  //
  // `live` is this worker's row out of the frame's `WorkerPool::Snapshot`, or
  // **null when the worker is no longer in the pool**. It is passed rather than
  // reached for: the pool is written from each worker's own thread, and a
  // window that read it directly would be reading it mid-write. A fresh copy
  // every frame, not a snapshot kept from when the window opened — a tool call
  // that lands while the user is looking appears on the next frame.
  //
  // A null `live` does not close the window and does not blank it. The window
  // keeps the last row it was given, greys it, and says the worker is gone: a
  // window that vanished under the user's cursor mid-read would be worse than
  // one that is honest about being a record rather than a feed.
  bool draw(float dt, const WorkerPool::Snapshot* live);

  // Where the window is now, for the caller to remember. Read after draw().
  WorkerGeometry geometry() const;

  const std::string& worker() const;
  HWND hwnd() const;

 private:
  WorkerWindow() = default;
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace aii
