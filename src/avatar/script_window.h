#pragma once
// M28: the third layer of the script-window feature. `docs/design-script-ui.md`
// is the design; this is the replayer it describes in §1.3 and §3 — one OS
// window per `aii::UiBridge` key, copying the latest recorded frame and
// replaying it through real ImGui every render frame.
//
// It is `worker_window.h`'s shape exactly: `ToolWindowCore` on the Impl,
// decorated/resizable/opaque, WM_CLOSE swallowed and latched because the SDL
// backend's close event is identityless and would otherwise quit the whole
// app, input queued by an HWND subclass and drained at the top of `draw()`,
// size only ever through the target. What is new is the replay itself and the
// override map that keeps a dragged slider from snapping back between
// recordings — both are `docs/design-script-ui.md`'s, not this file's, and
// this file does not re-explain them.
#include <memory>
#include <string>

#include "core/ui_bridge.h"

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

// Shared with WorkerWindow's own geometry shape: x/y is the window rect's
// top-left, w/h is the client size, `placed` says whether x/y is meaningful
// yet. Kept as its own type rather than reusing WorkerGeometry so a script
// window's slot sequence can never be confused with a worker's.
struct ScriptWindowGeometry {
  int x = 0;
  int y = 0;
  unsigned w = 360;
  unsigned h = 240;
  bool placed = false;
};

class ScriptWindow {
 public:
  // Null on failure, with the reason in `error`. `wanted` is fitted to the
  // desktop as it stands now, exactly as WorkerWindow::create does it, so a
  // slot that has walked off the screen cannot put the window somewhere
  // unreachable.
  static std::unique_ptr<ScriptWindow> create(rend::platform::IPlatformBackend& backend,
                                               rend::gpu::Instance& instance,
                                               rend::gpu::Device& device, float font_px,
                                               const UiWindowSpec& spec,
                                               const ScriptWindowGeometry& wanted,
                                               std::string* error);
  ~ScriptWindow();
  ScriptWindow(const ScriptWindow&) = delete;
  ScriptWindow& operator=(const ScriptWindow&) = delete;

  // One frame. Copies the bridge's latest recording for this key when its
  // generation has moved on, replays it inside one full-client ImGui window
  // (no title bar — the OS window already has one), and writes every
  // interactive widget's outcome back with `bridge.set_results()`. Returns
  // false when the user closed the OS window; the caller then calls
  // `bridge.mark_closed()` and destroys this object between frames, the same
  // rule every other window in this app follows.
  bool draw(float dt, UiBridge& bridge);

  // The script reopened this key with a new title or size.
  void retitle_resize(const UiWindowSpec& spec);

  const std::string& key() const;
  ScriptWindowGeometry geometry() const;
  HWND hwnd() const;

  // Named here rather than under `private:` so `script_window.cpp`'s free
  // functions — the replayer and its override lookups, which are not member
  // functions because they are shared across every command in a frame's
  // switch rather than being one widget's own method — can name it. Its
  // definition is still only ever in `script_window.cpp`; nothing outside
  // that file has any business with its fields.
  struct Impl;

 private:
  ScriptWindow() = default;
  std::unique_ptr<Impl> p_;
};

}  // namespace aii
