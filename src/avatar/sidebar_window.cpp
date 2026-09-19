#include "sidebar_window.h"

#include "rend/core/log.h"
#include "rend/gpu/command_context.h"
#include "rend/gpu/device.h"
#include "rend/gpu/frame_renderer.h"
#include "rend/gpu/instance.h"
#include "rend/gpu/swapchain.h"
#include "rend/platform/backend.h"

#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <utility>
#include <vector>

#include "imgui_layer.h"
#include "pixel_icons.h"

using namespace rend;

namespace aii {
namespace {

// ---- layout (pixels) ----
//
// The strip is sized from its buttons, and the buttons from the icon: a 13x13
// grid at 2x is 26 px, and 40 px leaves 7 px of air all round — a comfortable
// target on a 4K display at 150%, and larger than the transport row's 30 px
// because these buttons are not in a crowded row.
//
// It is **as tall as its content and no taller**, which is the one layout
// decision here that is not cosmetic. The strip is a real always-on-top window
// on the user's desktop and every pixel of it eats a click that was meant for
// whatever is underneath. A 48 px column running the widget's full 693 px
// would swallow clicks down the whole left side of the widget, and there is no
// cheap way out of it: WS_EX_TRANSPARENT needs WS_EX_LAYERED, which the D3D12
// composition swapchain strips, and HTTRANSPARENT only falls through to
// windows in the same thread (both measured in the B2 spike).
constexpr unsigned kStripW = 48;
constexpr float kButton = 40.0f;
constexpr float kButtonGap = 6.0f;
// The strip's own frame, the same on all four sides. It matters most at the
// bottom now: the strip is bottom-anchored (see dock()), its bottom edge is
// laid on the widget's bottom edge, and this is the only thing between the
// last button and that edge. 3 px keeps the two panel surfaces ending on
// exactly the same line — a strip that overhung the widget's bottom edge in
// order to get the *button* flush would be a few pixels of always-on-top
// window hanging below the widget and eating desktop clicks, which is the
// same overhang this number was chosen to avoid in the first place.
constexpr float kStripPad = 3.0f;
// Air between the strip's right edge and the widget's left edge. Zero would
// read as one window with a seam down it; this reads as two pieces of one
// widget, and it is narrow enough that the eye groups them.
constexpr int kDockGap = 6;

unsigned strip_height(std::size_t buttons) {
  if (buttons == 0) return static_cast<unsigned>(kButton);  // never a 0-px swapchain
  return static_cast<unsigned>(2.0f * kStripPad + buttons * kButton +
                               (buttons - 1) * kButtonGap + 0.5f);
}

// The strip's own input. `platform::Event` has no window identity and
// `pumpEvents()` is backend-global, so this window's mouse comes off its own
// HWND and goes into its own ImGui context — nothing is shared with the
// widget, in either direction.
//
// The messages are queued rather than fed to ImGui here, on purpose: they
// arrive inside the *widget's* pumpEvents() call, where the widget's ImGui
// context is current and its frame may be half built. Draining them at the top
// of the strip's own frame means the context is never switched behind anyone's
// back — the one mistake in this area that produces a bare access violation.
struct SidebarInput {
  std::vector<std::pair<int, bool>> buttons;  // (ImGui button index, pressed)
  float wheel = 0.0f;
};

LRESULT CALLBACK sidebarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                             DWORD_PTR ref) {
  auto* in = reinterpret_cast<SidebarInput*>(ref);
  if (in) {
    switch (msg) {
      case WM_LBUTTONDOWN: in->buttons.emplace_back(0, true); break;
      case WM_LBUTTONUP: in->buttons.emplace_back(0, false); break;
      case WM_MOUSEWHEEL: in->wheel += GET_WHEEL_DELTA_WPARAM(wp) / 120.0f; break;
      case WM_MOUSEACTIVATE:
        // WS_EX_NOACTIVATE is not enough, measured: a click on the strip made
        // it the foreground window anyway, because SDL's own window procedure
        // answers WM_MOUSEACTIVATE before the ex-style is ever consulted. The
        // click still arrives — WM_LBUTTONDOWN follows regardless — so the
        // strip stays fully usable while the caret stays in whatever the user
        // was typing in. This is the difference between a tool strip and a
        // window that interrupts them every time they press a button on it.
        return MA_NOACTIVATE;
      case WM_CLOSE:
        // Swallowed, and this is a safety property rather than a nicety. The
        // SDL3 backend maps both SDL_EVENT_QUIT and SDL_EVENT_WINDOW_CLOSE_
        // REQUESTED onto one identityless Event::CloseRequested, so a close
        // arriving on *this* window would quit the whole app — the widget
        // included. The strip has no close affordance and no independent
        // life: it is created with the widget and destroyed with it.
        return 0;
      default: break;
    }
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

struct SidebarWindow::Impl {
  std::unique_ptr<platform::PresentationTarget> target;
  std::unique_ptr<gpu::Swapchain> swapchain;
  std::unique_ptr<gpu::FrameRenderer> renderer;
  std::unique_ptr<ImGuiLayer> ui;
  std::unique_ptr<SidebarInput> input;
  HWND hwnd = nullptr;
  // Whoever had the foreground before this window existed. See dock().
  HWND prev_foreground = nullptr;
  unsigned w = kStripW;
  unsigned h = 0;
  int x = 0, y = 0;
  bool shown = false;
  bool subclassed = false;
  // What the pointer is over this frame, for the widget to letter.
  std::string tooltip;
  float tooltip_y = 0.0f;  // screen space
};

std::unique_ptr<SidebarWindow> SidebarWindow::create(platform::IPlatformBackend& backend,
                                                     gpu::Instance& instance, gpu::Device& device,
                                                     float font_px, std::string* error) {
  const auto fail = [&](std::string msg) -> std::unique_ptr<SidebarWindow> {
    if (error) *error = std::move(msg);
    return nullptr;
  };
  auto self = std::unique_ptr<SidebarWindow>(new SidebarWindow());
  self->p_ = std::make_unique<Impl>();
  Impl& s = *self->p_;
  // Before the window exists, because it is about to take the foreground away
  // from whoever has it — see dock().
  s.prev_foreground = GetForegroundWindow();
  s.h = strip_height(ButtonRegistry::instance().snapshot_for(ButtonSurface::Sidebar).size());

  auto t = backend.createTarget({
      .style = platform::WindowStyle::BorderlessTransparent,
      .size = {s.w, s.h},
      .title = "AIInterface sidebar",
      .vulkan = false,
  });
  if (!t) return fail("createTarget: " + t.error().message);
  s.target = std::move(t).value();
  s.hwnd = static_cast<HWND>(backend.nativeWindowHandle(*s.target));
  if (!s.hwnd) return fail("no HWND for the sidebar");

  // createTarget ends in SDL_ShowWindow, which *activates* the window — the
  // spike measured the second window as the foreground window before any click
  // had been sent to it, and a strip that steals focus from whatever the user
  // is typing in is the first failure mode this window has to not have. A
  // style added afterwards cannot undo an activation that already happened, so
  // it is hidden, restyled, and shown again with SW_SHOWNOACTIVATE — by dock(),
  // which is also the first thing that knows where it belongs. Showing it here
  // would put it wherever SDL happened to place it for a frame.
  ShowWindow(s.hwnd, SW_HIDE);
  const LONG_PTR ex = GetWindowLongPtrW(s.hwnd, GWL_EXSTYLE);
  SetWindowLongPtrW(s.hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);

  auto sc = gpu::Swapchain::create(instance, device, {
      .nativeSurface = s.hwnd,
      .width = s.w,
      .height = s.h,
      .transparent = true,
      .vsync = false,  // two vsynced swapchains on one thread halve the frame rate
  });
  if (!sc) return fail("swapchain: " + sc.error().message);
  s.swapchain = std::move(sc).value();

  auto fr = gpu::FrameRenderer::create(device, *s.swapchain);
  if (!fr) return fail("frame renderer: " + fr.error().message);
  s.renderer = std::move(fr).value();
  s.renderer->setClearColor(0.0f, 0.0f, 0.0f, 0.0f);  // premultiplied: desktop shows through

  std::string err;
  s.ui = ImGuiLayer::create(device, s.swapchain->imageFormat(), font_px, &err);
  if (!s.ui) return fail("imgui: " + err);
  ImGuiLayer* layer = s.ui.get();
  s.renderer->setOverlayRecorder([layer](gpu::CommandContext& cmd) { layer->end_frame(cmd); });

  s.input = std::make_unique<SidebarInput>();
  s.subclassed = SetWindowSubclass(s.hwnd, sidebarProc, 1,
                                   reinterpret_cast<DWORD_PTR>(s.input.get())) != FALSE;
  if (!s.subclassed) log::warn("sidebar: could not subclass the strip for input");
  log::info("sidebar: strip up, hwnd {:p}, {}x{}", static_cast<void*>(s.hwnd), s.w, s.h);
  return self;
}

SidebarWindow::~SidebarWindow() {
  if (!p_) return;
  Impl& s = *p_;
  // The order the spike found and recommended: the subclass comes off the HWND
  // before anything it points at is freed, the GPU is waited on, and the
  // renderer's callbacks are dropped before the objects they capture.
  if (s.hwnd && s.subclassed) RemoveWindowSubclass(s.hwnd, sidebarProc, 1);
  if (s.renderer) {
    s.renderer->waitIdle();
    s.renderer->setOverlayRecorder(nullptr);
    s.renderer->setFramePasses({});
  }
  s.ui.reset();
  s.renderer.reset();
  s.swapchain.reset();
  s.target.reset();
  log::info("sidebar: strip torn down");
}

HWND SidebarWindow::hwnd() const { return p_->hwnd; }
unsigned SidebarWindow::width() const { return p_->w; }
unsigned SidebarWindow::height() const { return p_->h; }
int SidebarWindow::left() const { return p_->x; }

void SidebarWindow::dock(const RECT& widget) {
  Impl& s = *p_;
  const unsigned want =
      strip_height(ButtonRegistry::instance().snapshot_for(ButtonSurface::Sidebar).size());
  const int x = static_cast<int>(widget.left) - static_cast<int>(s.w) - kDockGap;
  // **Bottom-anchored**: the strip's bottom edge is the widget's bottom edge,
  // and the column grows upward from it. From `want` rather than `s.h`, which
  // is the whole point — the origin and the size have to come from the same
  // button count and be applied together, or the frame a button appears on is
  // a frame where the strip is the new height at the old origin and its bottom
  // edge visibly jumps.
  const int y = static_cast<int>(widget.bottom) - static_cast<int>(want);
  const bool resize = want != s.h;
  if (!resize && x == s.x && y == s.y && s.shown) return;

  if (resize) {
    // Through the target, never a raw SetWindowPos: SDL answers WM_NCCALCSIZE
    // with the size it holds, so a raw resize leaves the client rect — the
    // part DWM composites — pinned at the creation size, and the strip would
    // silently clip. (Renderer 2c4d4df / AIInterface be2183b.)
    s.h = want;
    s.target->setSize({s.w, s.h});
  }
  s.x = x;
  s.y = y;
  SetWindowPos(s.hwnd, HWND_TOPMOST, x, y, static_cast<int>(s.w), static_cast<int>(s.h),
               SWP_NOACTIVATE);
  if (resize) s.renderer->resize(s.w, s.h);
  if (!s.shown) {
    // Only SW_SHOWNOACTIVATE, and only after the window is where it belongs.
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    s.shown = true;
    // And then give the foreground back, because none of the above is enough
    // on its own. Measured: with WS_EX_NOACTIVATE set, the window hidden
    // immediately after createTarget's SDL_ShowWindow, and re-shown here with
    // SW_SHOWNOACTIVATE, the strip was *still* the foreground window ten
    // seconds into a run — the activation SDL performed at creation outlives
    // every later "do not activate". A strip that takes the caret out of
    // whatever the user was typing in is the first thing this window must not
    // do, so the foreground is put back where it was found. It is allowed:
    // this process owns the foreground at this instant, which is the one case
    // Windows lets SetForegroundWindow succeed.
    if (s.prev_foreground && GetForegroundWindow() == s.hwnd) {
      SetForegroundWindow(s.prev_foreground);
      log::info("sidebar: foreground handed back to {:p}",
                static_cast<void*>(s.prev_foreground));
    }
  }
}

SidebarResult SidebarWindow::draw(float dt, bool loading) {
  Impl& s = *p_;
  SidebarResult out;
  s.tooltip.clear();
  if (!s.ui) return out;

  // Its own context, first: ImGui's ambient current context belongs to whoever
  // touched it last, and with two layers that is never a safe assumption.
  s.ui->make_current();
  ImGuiIO& io = ImGui::GetIO();

  // The pointer, polled rather than tracked from WM_MOUSEMOVE. This window
  // moves under a still pointer every time the chat opens, and a position that
  // only changes when the mouse does would leave a button hovered that the
  // pointer is no longer over. Outside the strip it is pushed off-screen
  // explicitly: with no motion events of our own, nothing else would ever
  // clear a hover the pointer has walked out of.
  POINT cursor{};
  if (GetCursorPos(&cursor)) {
    POINT local = cursor;
    if (ScreenToClient(s.hwnd, &local) && local.x >= 0 && local.y >= 0 &&
        local.x < static_cast<LONG>(s.w) && local.y < static_cast<LONG>(s.h))
      io.AddMousePosEvent(static_cast<float>(local.x), static_cast<float>(local.y));
    else
      io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }
  for (const auto& [button, pressed] : s.input->buttons) io.AddMouseButtonEvent(button, pressed);
  s.input->buttons.clear();
  if (s.input->wheel != 0.0f) {
    io.AddMouseWheelEvent(0.0f, s.input->wheel);
    s.input->wheel = 0.0f;
  }

  s.ui->begin_frame(s.w, s.h, dt);
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(s.w), static_cast<float>(s.h)));
  // The same panel colour the widget uses, so the two read as one thing.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_color(0.086f, 0.094f, 0.118f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kStripPad, kStripPad));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, kButtonGap));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  ImGui::Begin("##sidebar", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  // M4.4: straight off the registry, in registration order. There is no list
  // of buttons in this file and adding one never means editing it.
  for (const ToolbarButton& b : ButtonRegistry::instance().snapshot_for(ButtonSurface::Sidebar)) {
    const bool off = loading && b.action.kind == ButtonActionKind::OpenSettings;
    ImGui::BeginDisabled(off);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(("##strip_" + b.id).c_str(),
                                                ImVec2(kButton, kButton));
    const ImGuiCol bg = ImGui::IsItemActive()    ? ImGuiCol_ButtonActive
                        : ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered
                                                 : ImGuiCol_Button;
    dl->AddRectFilled(p, ImVec2(p.x + kButton, p.y + kButton), ImGui::GetColorU32(bg), 5.0f);
    if (const char* const* rows = icon_for_glyph(b.glyph)) {
      const ImU32 ink = ImGui::GetColorU32(off ? ui_color(0.44f, 0.46f, 0.51f)
                                               : ui_color(0.91f, 0.92f, 0.94f));
      draw_icon(dl, rows, ImVec2(p.x + (kButton - kIconPx) * 0.5f,
                                 p.y + (kButton - kIconPx) * 0.5f),
                ink, ink);
    }
    if (ImGui::IsItemHovered() && !b.tooltip.empty()) {
      s.tooltip = b.tooltip;
      // Screen space: ImGui's coordinates here are this window's client area,
      // and the widget that letters the tooltip has its own.
      s.tooltip_y = static_cast<float>(s.y) + p.y + kButton * 0.5f;
    }
    ImGui::EndDisabled();
    if (!clicked) continue;
    switch (b.action.kind) {
      case ButtonActionKind::OpenSettings:
        out.open_settings = true;
        break;
      case ButtonActionKind::OpenPath:
        if (std::string err; !open_directory(b.action.path, &err)) out.refusal = err;
        break;
      case ButtonActionKind::Invoke:
        if (b.action.callback) b.action.callback();
        break;
    }
  }

  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();

  if (auto r = s.renderer->waitFrameSlot(); !r) {
    log::error("sidebar wait: {}", r.error().message);
    return out;
  }
  if (auto d = s.renderer->drawFrame(nullptr); !d) log::error("sidebar draw: {}", d.error().message);
  return out;
}

void SidebarWindow::draw_tooltip_into_widget(const RECT& widget) const {
  const Impl& s = *p_;
  if (s.tooltip.empty()) return;
  // Called inside the widget's ImGui frame: its context is current, its
  // viewport is the widget's 360 px, and its foreground draw list is over
  // everything the panel drew. The strip's own context is untouched.
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float pad = 6.0f;
  const ImVec2 size = ImGui::CalcTextSize(s.tooltip.c_str());
  // Widget-client coordinates: the widget's window rect is the same thing as
  // its client rect here (borderless), so screen minus its origin is enough.
  const float x = 4.0f;
  // Centred on the icon, then kept inside the widget: the foreground draw
  // list is clipped to the widget's own viewport, so a two-line label beside
  // the *last* button would have its second line silently cut off at the
  // widget's bottom edge — which is how the art button's path disappeared.
  const float height = static_cast<float>(widget.bottom - widget.top);
  const float y = std::clamp(s.tooltip_y - static_cast<float>(widget.top) - size.y * 0.5f, 4.0f,
                             std::max(4.0f, height - size.y - 4.0f));
  const ImVec2 a(x, y - pad * 0.5f);
  const ImVec2 b(x + size.x + 2.0f * pad, y + size.y + pad * 0.5f);
  dl->AddRectFilled(a, b, ImGui::GetColorU32(ui_color(0.16f, 0.17f, 0.21f, 0.96f)), 4.0f);
  dl->AddRect(a, b, ImGui::GetColorU32(ui_color(0.32f, 0.34f, 0.40f)), 4.0f);
  dl->AddText(ImVec2(x + pad, y), ImGui::GetColorU32(ui_color(0.91f, 0.92f, 0.94f)),
              s.tooltip.c_str());
}

}  // namespace aii
