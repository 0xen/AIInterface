#pragma once
// M10.5: the pop-up that asks whether a newly written action may run.
//
// **The user's own words, 19 Sep 2026:** *"add a setting in the settings called
// auto allow new scripts. if this is disabled, have a pop-up appear saying, we
// have created a new script. Would you like to see it? have a button that when
// you click it navigates you to the directory of the script and you can open it
// and you can click confirm or dismiss it."*
//
// ## Why this is a real OS window and not a region in the panel
//
// The settings surface is a region because *it had to be*: "an ImGui popup is a
// floating window, and nothing may be drawn outside this window's composition
// surface, so a taller popup would have been silently clipped" (`HANDOFF.md`).
// The widget is 360 px wide, borderless, transparent, pinned bottom-right, and
// its height follows its content — so a region here would **resize the main
// window** every time the model wrote a file, and a jumping avatar is already
// on the user's own test list.
//
// The deciding reason is not layout, though. The user has to be able to read
// the script *while this is up*: go to Explorer, open the `.py`, read it, come
// back, press Confirm. A region inside a panel that is behind Explorer is not
// readable, and an ImGui popup closes when it loses focus. This does neither.
//
// ## Four properties, each answering a specific way this could have failed
//
//  - **Borderless, and therefore immovable.** Not a style choice. A title bar
//    means a title-bar drag, and Windows runs a *modal* loop inside
//    `DefWindowProc` on the pumping thread for the whole of one — which is the
//    known freeze this app already has once, and adding a second way to stall
//    the frame loop was the first thing this task was told not to do. No title
//    bar, no drag, no modal loop. The frame loop cannot be stalled by this
//    window because there is nothing on it that enters one.
//  - **Topmost.** It has to survive the user alt-tabbing to Explorer and
//    reading a file, which is the whole errand it sends them on.
//  - **`WS_EX_NOACTIVATE`, plus `WM_MOUSEACTIVATE -> MA_NOACTIVATE`.** It can
//    appear mid-sentence while the user is typing in the chat box, and a
//    notification that takes the caret out of what they were writing is worse
//    than the thing it is notifying about. The clicks still arrive:
//    `WM_LBUTTONDOWN` follows regardless. (Measured by `sidebar_window.cpp`;
//    `WS_EX_NOACTIVATE` alone was not enough, because SDL's own procedure
//    answers `WM_MOUSEACTIVATE` before the ex-style is consulted.)
//  - **It never closes itself.** Not on focus loss, not on a timer. It is up
//    until every row on it has been answered, because the user may be five
//    minutes into reading the file.
//
// ## Several at once
//
// **One window listing all of them, never one window each.** The model can
// write several files in one turn; a stack of six windows on the user's desktop
// is a failure, and one window shown after another makes the second one a
// surprise arriving after the user thought they were done. So the window's
// height follows the queue, it scrolls past four, and it carries an "Arm all" /
// "Dismiss all" pair as soon as there is more than one row — which is also the
// only sensible answer to a model that wrote eight helpers in one go.
//
// ## What it does not do
//
// It holds no policy and decides nothing. It draws the queue it is handed and
// returns what was clicked, exactly as `SidebarWindow::draw` does; the frame
// loop is what tells `ActionStore` about it and what opens Explorer. That keeps
// arming — the security-relevant act — in one place with one caller.
#include <memory>
#include <string>
#include <vector>

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

// One row: what the app found, as the user needs to see it.
struct ApprovalRow {
  std::string name;
  std::string description;
  std::string path;
};

// What the user clicked this frame. Everything is a *wish*; the frame loop
// acts. `reveal` is the path to show in Explorer.
struct ApprovalResult {
  std::vector<std::string> arm;
  std::vector<std::string> dismiss;
  std::string reveal;
  bool arm_all = false;
  bool dismiss_all = false;
};

class ApprovalWindow {
 public:
  // Null on failure with the reason in `error`, and a failure is survivable:
  // the actions stay unarmed and the Scripts row is still there to arm them
  // from. `anchor_x`/`anchor_y` are the bottom-right corner the window grows
  // up and left from — the frame loop owns placement, as it does for every
  // other window here, because only it knows where the widget is.
  static std::unique_ptr<ApprovalWindow> create(rend::platform::IPlatformBackend& backend,
                                                rend::gpu::Instance& instance,
                                                rend::gpu::Device& device, float font_px,
                                                int anchor_x, int anchor_y, std::string* error);
  ~ApprovalWindow();
  ApprovalWindow(const ApprovalWindow&) = delete;
  ApprovalWindow& operator=(const ApprovalWindow&) = delete;

  // One frame. `rows` is the live queue, re-read every frame rather than kept
  // from when the window opened, so an action the model writes while the window
  // is already up joins the list instead of raising a second window.
  //
  // The window resizes itself to the queue **through `setSize()`**, never a raw
  // `SetWindowPos` size: SDL answers `WM_NCCALCSIZE` with the size it holds, so
  // a raw resize leaves the client rect pinned and the window silently clips.
  void draw(float dt, const std::vector<ApprovalRow>& rows, ApprovalResult* out);

  HWND hwnd() const;

 private:
  ApprovalWindow() = default;
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace aii
