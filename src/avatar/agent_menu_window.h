#pragma once
// M11.1: the **agent menu** — the hamburger that holds the agents that have
// finished.
//
// **The user's own words, 20 Sep 2026:** *"I would like it so that we can
// concatenate all agents that are finished into a single hamburger menu that
// when you click it reveals them all."*
//
// ## What was actually wrong
//
// Nothing here filtered workers by state. `WorkerPool` never erases a finished
// worker — `update()` joins its thread and leaves the row in place, and the
// only eraser is the explicit `stop()` command — so a `Done` agent stayed in
// `snapshot()` for the rest of the session and every surface drew it forever:
// the chat's worker rows (`avatar_ui.cpp`), and the worker strip, which caps at
// `kWorkerSlotsMax = 8`. That cap is what made it a bug rather than clutter.
// **Eight finished agents pushed the running ones off the strip entirely**, so
// the surface for watching live work was full of work that had stopped. The
// user asked for a hamburger; the reason a hamburger helps is that one.
//
// So finished agents now leave those surfaces and collect here. Running agents
// are untouched and stay exactly where they were, which is the decision the
// user made when asked: live work is the thing worth watching.
//
// ## What clicking an entry reveals, and why it is not more
//
// **The menu is the index; the agent's own window is the page.** A row carries
// what you need to pick between agents — the name, whether it finished or
// failed, how many tools it used, and the first line of what it came back with
// — and clicking it opens that agent's existing `WorkerWindow`, which already
// shows the task, the activity history and the whole result, is already
// read-only, and already knows how to say "this worker is gone" when the row
// has been cleared out of the pool underneath it.
//
// The alternative was to render the result here, in place. It was rejected
// because it would be a *second* renderer for a worker's output, drifting
// against `worker_window.cpp`, to show the same text in a narrower window —
// and because the click then has nowhere left to go. Routing the click into
// machinery that already exists is why this window holds no transcript at all.
//
// ## What a long session does to the list
//
// Three bounds, each answering a different way a long session gets ugly:
//
//  - **Newest first.** Spawn order is the pool's order and is the wrong one
//    here: after four hours the agent worth looking at is the one that just
//    finished, not the first one of the morning.
//  - **`kAgentMenuMax` entries, and the oldest fall off the list.** They fall
//    off the *menu*, not the pool — nothing is killed to make the list shorter.
//  - **It scrolls past `kRowsShown`** rather than the window growing, which is
//    the approval window's rule and for its reason: a notification that filled
//    the screen because it had been a productive session is its own failure.
//
// "Clear all" is the one thing here that destroys something, and it is the
// existing `WorkerPool::stop()` on each finished agent — the same call the
// `stop` command already makes — applied by the frame loop, never here.
//
// ## Why this is a real OS window
//
// The approval window's reasoning (`approval_window.h`) applies unchanged and
// is not repeated: an ImGui popup is a floating window, nothing may be drawn
// outside the widget's composition surface, so a popup would be **silently
// clipped**; a region would resize the 360 px widget every time an agent
// finished, and a jumping avatar is already on the user's test list.
//
// Borderless for the same measured reason as every other extra window here: a
// title bar means a title-bar drag, and Windows runs a *modal* loop inside
// `DefWindowProc` on the pumping thread for the whole of one, which is the
// known way to freeze this app's frame loop. No title bar, no drag, no modal
// loop. `WS_EX_NOACTIVATE` plus `WM_MOUSEACTIVATE -> MA_NOACTIVATE`, because
// this can open while the user is typing in the chat box and a menu that took
// the caret would be worse than the clutter it replaces.
//
// Unlike the approval window, **this one closes.** It is a menu the user asked
// for, not a question the app is waiting on: the same hamburger press that
// opened it shuts it, and it goes away by itself when the last finished agent
// is cleared, because a menu of nothing is not a menu.
#include <memory>
#include <string>
#include <vector>

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

// One finished agent, as the frame loop hands it over. A projection of
// `WorkerPool::Snapshot` and not the snapshot itself, for the reason every
// other window here takes a projection: the pool is written from the workers'
// own threads, and this list is built once per frame from one snapshot taken
// under the pool's mutex.
struct AgentMenuRow {
  std::string name;
  WorkerPool::State state = WorkerPool::State::Done;
  std::string task;
  std::string result;
  int tool_calls = 0;
  bool window_open = false;  // its WorkerWindow is already up
};

// What the user clicked this frame. Everything is a *wish*; the frame loop
// acts, exactly as it does for the approval window and the strip. Nothing here
// touches the pool.
struct AgentMenuResult {
  // The agent whose own window to raise. Routed into the same `toggleWorker`
  // the strip already sets, so opening from here and opening from the strip
  // are one code path with one cap on how many windows may be up.
  std::string open;
  // The user pressed the menu's own close. A wish, not an act: a window may
  // only be destroyed between frames.
  bool close = false;
  // **There is deliberately no `dismiss` here.** Clearing an agent out for good
  // is `WorkerPool::stop()`, and the pool is private to `VoiceSession` with no
  // public way in — adding one was out of scope for this change. It is not
  // missed: the list is bounded and ordered (see the header), so a long session
  // does not grow it, and the `stop` command the conversational instance
  // already understands still removes one by name. If this surface ever grows a
  // clear, that is the seam — one accessor, and the frame loop applying it, so
  // that stopping a worker keeps its single caller.
};

// How many rows the window shows before it scrolls, and how many finished
// agents the list remembers at all. See the header's "long session" note.
constexpr std::size_t kAgentRowsShown = 6;
constexpr std::size_t kAgentMenuMax = 24;

class AgentMenuWindow {
 public:
  // Null on failure with the reason in `error`, and a failure is survivable:
  // the hamburger stays and the next click tries again. `anchor_x`/`anchor_y`
  // are the bottom-right corner this grows up and left from — the frame loop
  // owns placement, as it does for every other window here, because only it
  // knows where the widget and the strips are.
  static std::unique_ptr<AgentMenuWindow> create(rend::platform::IPlatformBackend& backend,
                                                 rend::gpu::Instance& instance,
                                                 rend::gpu::Device& device, float font_px,
                                                 int anchor_x, int anchor_y, std::string* error);
  ~AgentMenuWindow();
  AgentMenuWindow(const AgentMenuWindow&) = delete;
  AgentMenuWindow& operator=(const AgentMenuWindow&) = delete;

  // One frame. `rows` is re-read every frame rather than kept from when the
  // window opened, so an agent that finishes while the menu is up joins the
  // list instead of the list going stale under the user's cursor.
  //
  // The window resizes to the list **through `setSize()`**, never a raw
  // `SetWindowPos` size: SDL answers `WM_NCCALCSIZE` with the size it holds, so
  // a raw resize leaves the client rect pinned and the window silently clips.
  void draw(float dt, const std::vector<AgentMenuRow>& rows, AgentMenuResult* out);

  // Where the frame loop should anchor it, updated as the widget moves.
  void set_anchor(int anchor_x, int anchor_y);

  HWND hwnd() const;
  unsigned width() const;
  unsigned height() const;

 private:
  AgentMenuWindow() = default;
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace aii
