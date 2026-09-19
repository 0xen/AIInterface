#pragma once
// M4.2-M4.4: the sidebar. A vertical strip of icon buttons down the left of
// the avatar widget, as a **real second OS window** — the user's own decision
// after the B2 spike (docs/spike-two-windows.md), taken over regions inside
// the widget, and the same decision covers M5's inspector and M6's editor.
//
// This is the first real second window, so it is also the factoring the spike
// asked for: everything one extra window needs, owned together so it can be
// created and torn down as a unit — its own PresentationTarget, Swapchain,
// FrameRenderer, ImGui context and HWND subclass, sharing only the Instance,
// the Device and the platform backend with the widget. M5 and M6 should be
// able to copy this file rather than the spike.
//
// Four things about a second window in this app, each of them learned at cost:
//
//  - **Its swapchain is `vsync = false`.** Two vsynced swapchains presented in
//    sequence on one thread halve the frame rate.
//  - **Every entry point makes its own ImGui context current.**
//    `ImGui::CreateContext()` restores the previously current context before it
//    returns, so with two layers nothing may rely on the ambient one. Getting
//    this wrong is an access violation inside ImGui_ImplDX12_NewFrame with its
//    assert compiled out — a bare crash that says nothing about two windows.
//  - **Its size goes through `PresentationTarget::setSize()`, never a raw
//    `SetWindowPos`.** SDL answers WM_NCCALCSIZE for a borderless window with
//    the size *it* holds, so a raw resize grows the window rect while the
//    client rect — which is what DWM composites — stays put. Position through
//    Win32 is fine; size is not.
//  - **Its input is its own.** `platform::Event` carries no window identity, so
//    the engine's pump can never be trusted to say which window an event is
//    about; the strip owns its HWND through a subclass and the widget keeps
//    `pumpEvents()` to itself. The same subclass swallows WM_CLOSE, because the
//    backend maps a close on *either* window onto one identityless
//    `CloseRequested` that would quit the whole app.
#include <memory>
#include <string>
#include <vector>

#include "core/button_registry.h"

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

// What one frame of the strip did, for the frame loop to act on. The strip
// performs the actions it can perform by itself (open a directory, invoke a
// registered callback); these are the two that belong to the widget.
struct SidebarResult {
  // The settings cog was clicked. The settings surface is a region *inside*
  // the panel, so the strip cannot open it — it can only say so.
  bool open_settings = false;
  // Why a click did nothing, for the panel's reserved refusal row. A path
  // button whose folder has gone away since it was registered is the case.
  std::string refusal;
};

class SidebarWindow {
 public:
  // Null on failure, with the reason in `error`. A failure is survivable: the
  // registry hands the strip's buttons back to the panel's toolbar row, which
  // is also what happens under --opaque and --vulkan.
  static std::unique_ptr<SidebarWindow> create(rend::platform::IPlatformBackend& backend,
                                               rend::gpu::Instance& instance,
                                               rend::gpu::Device& device, float font_px,
                                               std::string* error);
  ~SidebarWindow();
  SidebarWindow(const SidebarWindow&) = delete;
  SidebarWindow& operator=(const SidebarWindow&) = delete;

  // Docks the strip against the widget, and re-sizes it if the number of
  // buttons has changed. Call at the **top of the frame**, in the same block
  // that applies the widget's own height and avatar band and before any of
  // that frame's input is read (the f713297 invariant) — the strip is part of
  // that geometry now, and a strip that arrived a frame after the widget grew
  // would be seen as a slide.
  //
  // `widget` is the widget's window rect in screen pixels, and the strip is
  // anchored to its **bottom** edge: the strip's bottom edge is the widget's
  // bottom edge, and every button the registry gains extends the column
  // *upward*.
  //
  // It used to hang from the panel's top (`widget.top + band`) and grow down,
  // which is the defect the user reported: past a few buttons the column ran
  // off the bottom of the widget and the last ones ended up underneath it.
  // Bottom-anchoring is also the stabler of the two rules — the widget is
  // anchored to the bottom-right corner of the work area, so `widget.bottom`
  // is the one edge of it that never moves. Opening the chat, showing or
  // hiding the avatar, and the band being reserved by mode all move the
  // widget's *top* edge by hundreds of pixels and none of them move the strip
  // at all. That is why `band` is no longer a parameter.
  //
  // Gaining or losing a button moves the top edge by exactly one button plus
  // one gap; the new height and the new origin are derived from the same count
  // and applied in one SetWindowPos, so the bottom edge does not move by a
  // pixel while the column changes length.
  void dock(const RECT& widget);

  // One frame: its own ImGui pass over the registry, then its own present.
  // Call it *before* the widget's own ImGui frame, so a tooltip picked up here
  // is drawn by the widget on the same frame rather than the next one.
  //
  // `loading` stands the settings cog down while the loading screen owns the
  // widget, exactly as the panel's toolbar row does and for the same reason:
  // the region it opens is withheld until the engines are up, so the click
  // would do nothing visible. The path buttons are unaffected — opening
  // Explorer works from the first frame.
  SidebarResult draw(float dt, bool loading);

  // The hovered button's tooltip, and the screen y to centre it on. Empty when
  // nothing is hovered.
  //
  // The strip cannot draw its own tooltip: it is 48 px wide and an ImGui
  // tooltip is a floating window clamped to *its* viewport, which is this
  // window — "Open the working directory" would be silently cut off at the
  // strip's edge, the same trap the settings surface avoided by not being a
  // popup. So the widget draws it, immediately right of the icon, which is
  // where a flyout label belongs anyway. draw_tooltip_into_widget() is called
  // inside the widget's ImGui frame.
  void draw_tooltip_into_widget(const RECT& widget) const;

  HWND hwnd() const;
  unsigned width() const;
  unsigned height() const;
  // The strip's left edge in screen pixels, as of the last dock(). What docks
  // further left — M9.1's worker strip — needs it, and computing it again at
  // the call site would mean a second copy of the dock gap.
  int left() const;

 private:
  SidebarWindow() = default;
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace aii
