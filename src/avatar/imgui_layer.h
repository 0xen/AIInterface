#pragma once
// Dear ImGui on the rend engine.
//
// D3D12 only, deliberately: the avatar window needs a per-pixel-alpha
// swapchain, which on this AMD GPU only D3D12's composition swapchain gives
// (see main.cpp), so there is no reason to carry a second render backend.
// create() fails under --vulkan and the window then runs without any UI.
//
// There is no ImGui platform backend either. The engine's SDL3 backend owns
// the window and drains its events, so input is fed in from the portable
// rend::platform::Event values through handle_event().
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
  bool handle_event(const rend::platform::Event& event);

  void begin_frame(std::uint32_t width, std::uint32_t height, float dt);
  // Records the frame's draw data. Call inside the overlay recorder, which
  // runs in an active rendering pass on the swapchain image.
  void end_frame(rend::gpu::CommandContext& cmd);

 private:
  ImGuiLayer() = default;
  std::unique_ptr<State> s_;
};

}  // namespace aii
