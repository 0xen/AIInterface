#include "tool_window_core.h"

#include <utility>

#include "rend/core/log.h"
#include "rend/gpu/command_context.h"

using namespace rend;

namespace aii {

bool ToolWindowCore::open(platform::IPlatformBackend& backend, gpu::Instance& instance,
                          gpu::Device& device, float font_px, const ToolWindowDesc& desc,
                          std::string* error) {
  const auto fail = [&](std::string msg) {
    if (error) *error = std::move(msg);
    return false;
  };
  w = desc.w;
  h = desc.h;

  // The size handed over is the *client* size, which is what the swapchain is
  // about to be made at.
  auto t = backend.createTarget({
      .style = desc.style,
      .size = {w, h},
      .title = desc.title,
      .vulkan = false,
  });
  if (!t) return fail("createTarget: " + t.error().message);
  target = std::move(t).value();
  hwnd = static_cast<HWND>(backend.nativeWindowHandle(*target));
  if (!hwnd) return fail("no HWND for the " + desc.noun);

  // Both branches are explained at ToolWindowPlacement in the header. Neither
  // is cosmetic: the first is the measured answer to createTarget activating
  // the window, and the second is why SWP_NOSIZE is there.
  switch (desc.placement) {
    case ToolWindowPlacement::NoActivate: {
      ShowWindow(hwnd, SW_HIDE);
      const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
      SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
      break;
    }
    case ToolWindowPlacement::Position:
      SetWindowPos(hwnd, HWND_TOP, desc.x, desc.y, 0, 0, SWP_NOSIZE | SWP_NOOWNERZORDER);
      break;
  }

  auto sc = gpu::Swapchain::create(instance, device,
                                   {
                                       .nativeSurface = hwnd,
                                       .width = w,
                                       .height = h,
                                       .transparent = desc.transparent,
                                       .vsync = false,  // vsynced chains on one thread divide fps
                                   });
  if (!sc) return fail("swapchain: " + sc.error().message);
  swapchain = std::move(sc).value();

  auto fr = gpu::FrameRenderer::create(device, *swapchain);
  if (!fr) return fail("frame renderer: " + fr.error().message);
  renderer = std::move(fr).value();
  renderer->setClearColor(desc.clear_r, desc.clear_g, desc.clear_b, desc.clear_a);

  std::string err;
  ui = ImGuiLayer::create(device, swapchain->imageFormat(), font_px, &err);
  if (!ui) return fail("imgui: " + err);
  ImGuiLayer* layer = ui.get();
  renderer->setOverlayRecorder([layer](gpu::CommandContext& cmd) { layer->end_frame(cmd); });
  return true;
}

bool ToolWindowCore::subclass(SUBCLASSPROC proc, void* ref) {
  proc_ = proc;
  subclassed = SetWindowSubclass(hwnd, proc, 1, reinterpret_cast<DWORD_PTR>(ref)) != FALSE;
  return subclassed;
}

void ToolWindowCore::shutdown() {
  if (hwnd && subclassed && proc_) {
    RemoveWindowSubclass(hwnd, proc_, 1);
    subclassed = false;
  }
  if (renderer) {
    renderer->waitIdle();
    renderer->setOverlayRecorder(nullptr);
    renderer->setFramePasses({});
  }
  ui.reset();
  renderer.reset();
  swapchain.reset();
  target.reset();
}

}  // namespace aii
