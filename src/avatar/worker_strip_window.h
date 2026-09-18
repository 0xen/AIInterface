#pragma once
// M9.1: the **worker strip** — the secondary sidebar. A second 48 px column of
// icons, one per running worker, that appears to the left of the primary strip
// when its button is pressed and goes away when it is pressed again.
//
// This is a near-copy of `sidebar_window.h`/`.cpp`, in the same way
// `inspector_window.*` is, and for the same reason: everything one extra window
// needs is owned together so it can be created and torn down as a unit — its
// own PresentationTarget, Swapchain, FrameRenderer, ImGui context and HWND
// subclass, sharing only the Instance, the Device and the platform backend.
//
// Where it differs from the primary strip, and why:
//
//  - **It has a life of its own.** The primary strip is created with the widget
//    and destroyed with it; this one is created on a click and destroyed on the
//    next one, so create() and the destructor run repeatedly inside a live
//    frame loop. That is the inspector's shape, not the strip's, and it is why
//    the click only *asks* — the frame loop answers between windows.
//  - **It keeps no list.** The primary strip draws the ButtonRegistry; this one
//    draws whatever worker rows the frame loop hands it, which came from a
//    `WorkerPool::Snapshot` taken under the pool's own mutex. Nothing here
//    reaches for the pool: it is written from the workers' own threads.
//  - **Empty is the normal state.** This app runs with zero workers almost all
//    of the time, so an empty strip must read as "nothing is running", not as a
//    window that failed. It draws one dimmed, unclickable slot that says so in
//    its tooltip, rather than collapsing to nothing.
//
// The three things every extra window in this app has to get right (see
// sidebar_window.h for the long version) apply here unchanged: `vsync = false`,
// **every entry point makes its own ImGui context current** because
// `ImGui::CreateContext()` restores the previously current one, and the size
// goes through `PresentationTarget::setSize()` and never a raw `SetWindowPos`.
// WM_CLOSE is swallowed: like the primary strip this window has no close box of
// its own (it is borderless), but the subclass swallows it anyway, because the
// backend maps a close on *any* window onto one identityless `CloseRequested`
// that the frame loop quits the whole application on.
#include <memory>
#include <string>
#include <vector>

#include "core/worker_pool.h"

struct HWND__;
using HWND = HWND__*;
struct tagRECT;
using RECT = tagRECT;

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

// One icon's worth of worker, as the frame loop hands it over. A projection of
// `WorkerPool::Snapshot` rather than the snapshot itself, because the strip
// needs one fact the pool does not have — whether this worker's chat window is
// already open — and because a 48 px column has no use for the transcript.
struct WorkerStripRow {
  std::string name;
  WorkerPool::State state = WorkerPool::State::Starting;
  std::string activity;
  bool window_open = false;
};

// What one frame of the strip did, for the frame loop to act on.
struct WorkerStripResult {
  // The worker whose icon was clicked, or empty. The strip cannot open a chat
  // window — creating one inside this window's ImGui frame would build GPU
  // objects and an ImGui context with a half-built frame on the stack — so it
  // only says which one, exactly as the primary strip only says that the cog
  // was pressed.
  std::string toggled;
};

// How many worker slots the strip will draw. The same physical bound as
// `kSidebarButtonsMax`: the strip is docked to the top of the panel, which at
// its shortest is ~168 px, and eight 40 px buttons plus their gaps is already
// taller than the widget ever is with the chat shut. Past this the strip stops
// looking like part of the widget.
constexpr std::size_t kWorkerSlotsMax = 8;

class WorkerStripWindow {
 public:
  // Null on failure, with the reason in `error`. A failure is survivable: the
  // button stays and the next click tries again.
  static std::unique_ptr<WorkerStripWindow> create(rend::platform::IPlatformBackend& backend,
                                                   rend::gpu::Instance& instance,
                                                   rend::gpu::Device& device, float font_px,
                                                   std::string* error);
  ~WorkerStripWindow();
  WorkerStripWindow(const WorkerStripWindow&) = delete;
  WorkerStripWindow& operator=(const WorkerStripWindow&) = delete;

  // This frame's workers. Call before dock(), because the strip's height is
  // sized from them and dock() is where a resize may happen.
  void set_rows(std::vector<WorkerStripRow> rows);

  // Docks the strip so its **right edge** sits `kDockGap` px left of
  // `right_edge`, with its top at the panel's top — the same rule the primary
  // strip uses, so the two columns line up. `right_edge` is the primary strip's
  // left edge when there is one and the widget's when there is not.
  //
  // Call at the **top of the frame**, in the same block that applies the
  // widget's own height and avatar band and before any of that frame's input is
  // read: the widget is anchored bottom-right, so growing it moves its top edge
  // and a strip docked later in the frame arrives one present behind.
  void dock(const RECT& widget, unsigned band, int right_edge);

  // One frame: its own ImGui pass, its own present. Call it beside the primary
  // strip's draw, before the widget's own ImGui frame.
  WorkerStripResult draw(float dt);

  // The hovered slot's label, drawn in the *widget's* frame — a 48 px window
  // cannot hold an ImGui tooltip, which is a floating window clamped to its own
  // viewport. Same mechanism, and same reason, as SidebarWindow's.
  void draw_tooltip_into_widget(const RECT& widget) const;

  HWND hwnd() const;
  unsigned width() const;
  unsigned height() const;
  // The strip's left edge in screen pixels, for whatever docks further left.
  int left() const;

 private:
  WorkerStripWindow() = default;
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace aii
