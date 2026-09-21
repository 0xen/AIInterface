#pragma once
// M24.3, finding 35. The eighty lines every extra OS window in this app was
// repeating.
//
// There are six of them — the sidebar, the worker strip, a worker window, the
// agent menu, the approval notice and the prompt inspector — and each one was
// a near-copy of the one before it, by design: `sidebar_window.h` says M5
// "should be able to copy this file rather than the spike", and every window
// since has. What was copied is not the drawing, which is genuinely different
// in all six; it is the *shell*: a PresentationTarget, a Swapchain, a
// FrameRenderer, an ImGui context and an HWND subclass, created in one order
// and destroyed in the opposite one.
//
// Copying that six times is how a fix reaches five windows. This owns it once.
//
// **It is a base class, not a member, and that is deliberate.** Every one of
// the six already spells its state `s.renderer`, `s.hwnd`, `s.w`; inheriting
// leaves all of that and all six `draw()` bodies untouched, so this change is
// provably the create and the teardown and nothing else. The windows' own
// state — their input queue, their remembered geometry, their rows — stays on
// their own Impl, because none of it is shared.
//
// What is *not* here, and why:
//
//  - The input queue and the window procedure. Every window queues a different
//    set of messages into a different struct, and the one thing they share —
//    that the queue is drained at the top of the window's own frame rather
//    than fed to ImGui as the message arrives — is a property of when `draw()`
//    runs, not of what this owns. The subclass is installed and removed here
//    because its *ordering against the teardown* is the part that was
//    copy-pasted and the part that can be got wrong silently.
//  - Everything `draw()` does. The frame is where these six windows differ.
//  - The remembered-geometry rule. Only two of the six have one, they have
//    different minimum sizes, and it is already one function in each file.
#include <memory>
#include <string>

#include <windows.h>
#include <commctrl.h>

#include "rend/gpu/device.h"
#include "rend/gpu/frame_renderer.h"
#include "rend/gpu/instance.h"
#include "rend/gpu/swapchain.h"
#include "rend/platform/backend.h"

#include "imgui_layer.h"

namespace aii {

// What a window created after the HWND exists and before the swapchain is made
// needs doing to it. Both of these were copied verbatim between windows, with
// the same paragraph of comment attached, and both matter:
//
//  - `NoActivate` is the sidebar's measured sequence. `createTarget` ends in
//    `SDL_ShowWindow`, which *activates* the window — the spike measured the
//    second window as the foreground window before any click had been sent to
//    it — and a style added afterwards cannot undo an activation that already
//    happened. So the window is hidden, restyled `WS_EX_TOOLWINDOW |
//    WS_EX_NOACTIVATE`, and shown again with `SW_SHOWNOACTIVATE` by whoever
//    first knows where it belongs. Showing it here would put it wherever SDL
//    happened to place it, for a frame.
//  - `Position` is the restore path of a window that remembers where it was.
//    `SWP_NOSIZE` is not optional: the backend answers size queries with the
//    size it holds, so a raw resize grows the window rect while the client
//    rect — the part DWM composites — stays put, and the window silently
//    clips. Position through Win32 is fine; size is not.
enum class ToolWindowPlacement {
  NoActivate,  // hide, restyle as a non-activating tool window, leave hidden
  Position,    // move to (x, y) without touching the size, and leave shown
};

struct ToolWindowDesc {
  rend::platform::WindowStyle style = rend::platform::WindowStyle::Borderless;
  std::string title;
  // The **client** size, which is what `createTarget` and the swapchain take.
  unsigned w = 0;
  unsigned h = 0;
  // A transparent swapchain is premultiplied and lets the desktop through; an
  // opaque one is for a page of text, which needs no alpha. `vsync` is never
  // on and is not a field: two vsynced swapchains on one thread halve the
  // frame rate, and six would be worse.
  bool transparent = false;
  ToolWindowPlacement placement = ToolWindowPlacement::NoActivate;
  int x = 0;  // ToolWindowPlacement::Position only
  int y = 0;
  // The renderer's clear colour, already linearised by the caller (the
  // swapchain view is sRGB and the hardware encodes whatever is written, clear
  // values included). All zero is the transparent windows' premultiplied
  // clear.
  float clear_r = 0.0f;
  float clear_g = 0.0f;
  float clear_b = 0.0f;
  float clear_a = 0.0f;
  // Names this window in the two failure messages this file produces, e.g.
  // "sidebar" gives "no HWND for the sidebar".
  std::string noun;
};

class ToolWindowCore {
 public:
  ToolWindowCore() = default;
  ~ToolWindowCore() { shutdown(); }
  ToolWindowCore(const ToolWindowCore&) = delete;
  ToolWindowCore& operator=(const ToolWindowCore&) = delete;

  // The create sequence, in the one order that works: target, HWND, the
  // placement above, swapchain, renderer and its clear colour, ImGui layer,
  // and the overlay recorder that ties the last two together. Returns false
  // with a one-line reason in `error` and leaves whatever it built to be
  // released by the destructor, which is why a failed create is survivable.
  //
  // `w`/`h` are taken from the desc and are then this object's own: a window
  // that resizes updates them and calls `renderer->resize()` itself, because
  // when that may happen is a property of the window, not of the shell.
  bool open(rend::platform::IPlatformBackend& backend, rend::gpu::Instance& instance,
            rend::gpu::Device& device, float font_px, const ToolWindowDesc& desc,
            std::string* error);

  // Installs the window procedure, remembering it so `shutdown()` can take it
  // off again without the caller naming it twice. `ref` is the input struct
  // the procedure queues into and must outlive this object — which it does,
  // because it lives on the derived Impl and this is its base.
  //
  // Returns false if Windows refused. Whether that is fatal is the caller's
  // decision and differs between these windows: for most of them it is, since
  // without the subclass a WM_CLOSE reaches SDL, SDL's close is identityless,
  // and the whole application quits.
  bool subclass(SUBCLASSPROC proc, void* ref);

  // The teardown sequence, which the spike found and every one of the six then
  // copied: the subclass comes off the HWND **before** the input struct it
  // points at can be freed, the GPU is waited on, the renderer's callbacks are
  // dropped before the objects they capture, and the four owned objects are
  // released in the reverse of the order they were made.
  //
  // This runs on every close and not only at exit, so it is what decides
  // whether opening and closing a window fifty times leaks fifty ImGui
  // contexts. It is idempotent: the destructor calls it, and a derived
  // destructor that calls it first to log afterwards is free to.
  void shutdown();

  std::unique_ptr<rend::platform::PresentationTarget> target;
  std::unique_ptr<rend::gpu::Swapchain> swapchain;
  std::unique_ptr<rend::gpu::FrameRenderer> renderer;
  std::unique_ptr<ImGuiLayer> ui;
  HWND hwnd = nullptr;
  // The live client size, which the swapchain follows.
  unsigned w = 0;
  unsigned h = 0;
  bool subclassed = false;

 private:
  SUBCLASSPROC proc_ = nullptr;
};

}  // namespace aii
