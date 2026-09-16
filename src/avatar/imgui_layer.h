#pragma once
// Dear ImGui on the rend engine.
//
// D3D12 only, deliberately: the avatar window needs a per-pixel-alpha
// swapchain, which on this AMD GPU only D3D12's composition swapchain gives
// (see main.cpp), so there is no reason to carry a second render backend.
// create() fails under --vulkan and the window then runs without any UI.
//
// There is no ImGui platform backend either. The engine's SDL3 backend owns
// the window and drains its events, so the mouse is fed in from the portable
// rend::platform::Event values through handle_event(). The keyboard cannot be:
// that event set carries sixteen keys and no character at all, so it comes
// instead from an HWND subclass (win_text_input.h), which owns it alone.
//
// The swapchain view is sRGB, which means the hardware encodes whatever the
// shader writes. ImGui has no colour management, so every colour handed to it
// must be linearised first — use ui_color() for literals; the style is
// converted once at create().
#include "rend/gpu/format.h"
#include "rend/platform/events.h"

#include <cstdint>
#include <memory>
#include <string>

struct ImVec4;

namespace rend::gpu {
class CommandContext;
class Device;
} // namespace rend::gpu

namespace aii {

// An sRGB colour literal (0..1 components, as picked in any colour tool)
// converted to what the sRGB render target must be given to display it.
ImVec4 ui_color(float r, float g, float b, float a = 1.0f);

class ImGuiLayer {
 public:
  struct State;

  // Null on failure, with the reason in `error`.
  static std::unique_ptr<ImGuiLayer> create(const rend::gpu::Device& device,
                                            rend::gpu::Format color_format, float font_px,
                                            std::string* error);
  ~ImGuiLayer();
  ImGuiLayer(const ImGuiLayer&) = delete;
  ImGuiLayer& operator=(const ImGuiLayer&) = delete;

  // Feed one platform event. Returns true when ImGui consumed it, i.e. the
  // pointer is over a widget and the app should not also act on the click.
  // The mouse only: keyboard events are dropped here because WinTextInput's
  // HWND subclass is ImGui's only keyboard path (see win_text_input.h).
  bool handle_event(const rend::platform::Event& event);

  // Whether ImGui is using the keyboard this frame — a text field has focus,
  // or some widget is active. The app's own hotkeys must stand down when it
  // is, or a space typed into the message field also toggles the microphone.
  bool wants_keyboard() const;

  // Re-read the pointer from the OS and hand ImGui its position in this
  // window's client space. Call once a frame, just before begin_frame().
  //
  // Motion events alone are not enough here: this window moves and resizes
  // itself (the avatar band appearing, the chat opening), and a window that
  // moves under a pointer that is holding still changes that pointer's client
  // coordinates without any mouse having moved. Until the next stray motion
  // event arrives, ImGui then believes the pointer is where it was before the
  // move — which, on the frame a button is released, loses the release off the
  // button it is still sitting on. Polling is also what imgui_impl_win32 does
  // for exactly this reason. `hwnd` is the window; a null one is a no-op.
  void sync_pointer(void* hwnd);

  // Makes this layer's ImGui context the current one. Every method below does
  // it for itself; this is public so a caller that draws widgets between
  // begin_frame() and end_frame() of a *different* layer can put itself back.
  // One window never needs it — ImGui's ambient context is then always ours.
  void make_current();

  void begin_frame(std::uint32_t width, std::uint32_t height, float dt);
  // Records the frame's draw data. Call inside the overlay recorder, which
  // runs in an active rendering pass on the swapchain image.
  void end_frame(rend::gpu::CommandContext& cmd);

 private:
  ImGuiLayer() = default;
  std::unique_ptr<State> s_;
};

}  // namespace aii
