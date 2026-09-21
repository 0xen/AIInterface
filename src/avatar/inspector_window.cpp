#include "inspector_window.h"

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
#include "inspector_list.h"
#include "tool_window_core.h"

using namespace rend;

namespace aii {
namespace {

// The window's floor. Not a cosmetic bound: below this the three sections M5.2
// draws stop being readable at all, and a remembered size smaller than it can
// only have come from a desktop that no longer exists.
constexpr int kMinW = 640;
constexpr int kMinH = 400;

// How much of a restored window must land on a monitor's **work area** for the
// remembered position to be honoured. This is the off-screen rule, and it is
// stated in pixels rather than as a percentage because what it protects is a
// physical act: enough frame to see that the window is there, and enough title
// bar to put a pointer on and drag it back. A window that fails it is not
// nudged - it is centred, because a nudge from an unplugged second monitor
// lands it in a corner that is technically on-screen and still wrong.
constexpr int kMinVisibleW = 160;
constexpr int kMinVisibleH = 32;

struct WorkAreas {
  std::vector<RECT> rects;
};

BOOL CALLBACK collect_work_area(HMONITOR mon, HDC, LPRECT, LPARAM user) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(mon, &mi)) reinterpret_cast<WorkAreas*>(user)->rects.push_back(mi.rcWork);
  return TRUE;
}

// The work area of the monitor a rect mostly sits on, or the primary one's
// when the OS will not say.
RECT work_area_for(const RECT& r) {
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
      mon && GetMonitorInfoW(mon, &mi))
    return mi.rcWork;
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  return work;
}

bool reachable(const RECT& window) {
  WorkAreas areas;
  EnumDisplayMonitors(nullptr, nullptr, collect_work_area, reinterpret_cast<LPARAM>(&areas));
  if (areas.rects.empty()) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    areas.rects.push_back(work);
  }
  for (const RECT& work : areas.rects) {
    RECT hit{};
    if (!IntersectRect(&hit, &window, &work)) continue;
    if (hit.right - hit.left >= kMinVisibleW && hit.bottom - hit.top >= kMinVisibleH) return true;
  }
  return false;
}

// Makes a remembered geometry legal against the desktop **as it is now**,
// which is not the desktop it was written on: a monitor may have been
// unplugged, the resolution changed, or the window saved on a 4K screen and
// restored on a laptop panel. Two rules, in this order:
//
//  1. The size is clamped into [640x400 .. the work area it lands on], so a
//     window remembered larger than the screen it comes back on is shrunk to
//     fit rather than hanging off two edges at once.
//  2. The position is honoured only if at least 160x32 px of the window lands
//     on some monitor's work area - otherwise the window is centred on the
//     work area nearest where it was asked to go.
void fit_to_desktop(InspectorGeometry& g) {
  RECT want{g.x, g.y, g.x + static_cast<int>(g.w), g.y + static_cast<int>(g.h)};
  const RECT work = work_area_for(want);
  const int work_w = std::max(kMinW, static_cast<int>(work.right - work.left));
  const int work_h = std::max(kMinH, static_cast<int>(work.bottom - work.top));
  g.w = static_cast<unsigned>(std::clamp(static_cast<int>(g.w), kMinW, work_w));
  g.h = static_cast<unsigned>(std::clamp(static_cast<int>(g.h), kMinH, work_h));

  const auto centre = [&] {
    g.x = static_cast<int>(work.left) +
          (static_cast<int>(work.right - work.left) - static_cast<int>(g.w)) / 2;
    g.y = static_cast<int>(work.top) +
          (static_cast<int>(work.bottom - work.top) - static_cast<int>(g.h)) / 2;
    g.placed = true;
  };
  if (!g.placed) {
    centre();
    return;
  }
  want = {g.x, g.y, g.x + static_cast<int>(g.w), g.y + static_cast<int>(g.h)};
  if (reachable(want)) return;
  log::info("inspector: remembered position {},{} is off-screen now; centring", g.x, g.y);
  centre();
}

// This window's own input, queued rather than fed to ImGui as it arrives - the
// messages are pumped inside the *widget's* pumpEvents() call, where another
// ImGui context is current and another frame may be half built. Draining them
// at the top of this window's own frame means no context is ever switched
// behind anyone's back. (Same reasoning, same shape, as SidebarInput.)
struct InspectorInput {
  std::vector<std::pair<int, bool>> buttons;  // (ImGui button index, pressed)
  float wheel = 0.0f;
  // The title bar's close box. Latched here and answered by the frame loop,
  // because a window may only be destroyed between frames.
  bool close_requested = false;
};

LRESULT CALLBACK inspectorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                               DWORD_PTR ref) {
  auto* in = reinterpret_cast<InspectorInput*>(ref);
  if (in) {
    switch (msg) {
      case WM_LBUTTONDOWN: in->buttons.emplace_back(0, true); break;
      case WM_LBUTTONUP: in->buttons.emplace_back(0, false); break;
      case WM_RBUTTONDOWN: in->buttons.emplace_back(1, true); break;
      case WM_RBUTTONUP: in->buttons.emplace_back(1, false); break;
      case WM_MOUSEWHEEL: in->wheel += GET_WHEEL_DELTA_WPARAM(wp) / 120.0f; break;
      case WM_CLOSE:
        // Swallowed, and this is a safety property rather than a nicety. The
        // SDL3 backend maps both SDL_EVENT_QUIT and SDL_EVENT_WINDOW_CLOSE_
        // REQUESTED onto one identityless Event::CloseRequested, and the frame
        // loop quits the application on it - so letting this reach SDL would
        // make the inspector's close box shut the widget down with it. It is
        // answered here instead, by the only window it is about.
        in->close_requested = true;
        return 0;
      default: break;
    }
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

// M24.3. The target, swapchain, renderer, ImGui layer, HWND, client size and
// subclass flag are ToolWindowCore's, along with the order they are created
// and destroyed in. Everything below is this window's own, and `s.renderer`,
// `s.hwnd`, `s.w` and `s.h` still mean what they meant.
struct InspectorWindow::Impl : ToolWindowCore {
  std::unique_ptr<InspectorInput> input;
  // What the caller should remember. Updated from the OS every frame the
  // window is in its ordinary state - never while it is minimised or
  // maximised, so that a restored window comes back where the user last
  // actually placed it rather than at the work area's full size.
  InspectorGeometry remembered;
};

std::unique_ptr<InspectorWindow> InspectorWindow::create(platform::IPlatformBackend& backend,
                                                         gpu::Instance& instance,
                                                         gpu::Device& device, float font_px,
                                                         InspectorGeometry wanted,
                                                         std::string* error) {
  const auto fail = [&](std::string msg) -> std::unique_ptr<InspectorWindow> {
    if (error) *error = std::move(msg);
    return nullptr;
  };
  fit_to_desktop(wanted);

  auto self = std::unique_ptr<InspectorWindow>(new InspectorWindow());
  self->p_ = std::make_unique<Impl>();
  Impl& s = *self->p_;
  s.remembered = wanted;

  // Decorated: a frame to drag, a corner to pull and a close box, all of which
  // the OS draws and none of which this app has to.
  //
  // Position, not NoActivate: this window is not topmost and is not held back
  // from the foreground either. createTarget's own SDL_ShowWindow has already
  // brought it up, which is what a window the user just asked for should do,
  // and is the opposite of the strip's rule.
  //
  // The panel's own background, so the third window reads as part of the same
  // application. Linearised: the swapchain view is sRGB and the hardware
  // encodes whatever is written, clear values included.
  const ImVec4 bg = ui_color(0.086f, 0.094f, 0.118f, 1.0f);
  std::string err;
  if (!s.open(backend, instance, device, font_px,
              {
                  .style = platform::WindowStyle::Decorated,
                  .title = "AIInterface - prompt inspector",
                  .w = wanted.w,
                  .h = wanted.h,
                  .transparent = false,  // a page of text needs no alpha
                  .placement = ToolWindowPlacement::Position,
                  .x = wanted.x,
                  .y = wanted.y,
                  .clear_r = bg.x,
                  .clear_g = bg.y,
                  .clear_b = bg.z,
                  .clear_a = bg.w,
                  .noun = "inspector",
              },
              &err)) {
    return fail(err);
  }

  s.input = std::make_unique<InspectorInput>();
  if (!s.subclass(inspectorProc, s.input.get())) {
    // Not survivable, unlike the strip's: without the subclass the close box
    // reaches SDL, which reports an identityless CloseRequested, which quits
    // the whole application. A window whose X button kills the app is worse
    // than no window at all.
    return fail("could not subclass the inspector for input");
  }
  log::info("inspector: window up, hwnd {:p}, {}x{} at {},{}", static_cast<void*>(s.hwnd), s.w,
            s.h, s.remembered.x, s.remembered.y);
  return self;
}

InspectorWindow::~InspectorWindow() {
  if (!p_) return;
  // The strip's order, and for the strip's reasons — now ToolWindowCore's, and
  // stated there. Called explicitly rather than left to ~Impl so that the line
  // below is logged after the window is actually gone, which is what a run
  // reads to see that it went. This runs on every close, not only at exit, so
  // it is what decides whether opening and closing the window fifty times
  // leaks fifty ImGui contexts.
  p_->shutdown();
  log::info("inspector: window torn down");
}

HWND InspectorWindow::hwnd() const { return p_->hwnd; }

InspectorGeometry InspectorWindow::geometry() const { return p_->remembered; }

bool InspectorWindow::draw(float dt, const PromptInventory& inv) {
  Impl& s = *p_;
  if (s.input && s.input->close_requested) return false;
  if (!s.ui) return true;

  // Minimised: nothing to draw, and nothing worth remembering either - a
  // minimised window's rect sits at -32000 and storing it would be exactly the
  // off-screen bug this window has a rule against.
  if (IsIconic(s.hwnd)) return true;

  // The size the OS has given the window, which the user may be dragging right
  // now. Read back rather than driven: a resize performed with the mouse is the
  // OS's own and the backend has already seen it, so setSize() has nothing to
  // say here - it is the *restore* path, in create(), that must never bypass it.
  const auto now = s.target->sizeInPixels();
  if (now.width != 0 && now.height != 0 && (now.width != s.w || now.height != s.h)) {
    s.w = now.width;
    s.h = now.height;
    s.renderer->resize(s.w, s.h);
  }
  if (s.w == 0 || s.h == 0) return true;

  // What to remember, taken from the window rather than from anything this file
  // believes about it. Skipped while maximised: the geometry worth keeping is
  // the one the user placed, not the work area's full size.
  if (!IsZoomed(s.hwnd)) {
    RECT wr{};
    if (GetWindowRect(s.hwnd, &wr)) {
      s.remembered.x = wr.left;
      s.remembered.y = wr.top;
      s.remembered.w = s.w;
      s.remembered.h = s.h;
      s.remembered.placed = true;
    }
  }

  // Its own context, first: ImGui's ambient current context belongs to whoever
  // touched it last, and with three layers that is never a safe assumption.
  s.ui->make_current();
  ImGuiIO& io = ImGui::GetIO();

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
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_color(0.086f, 0.094f, 0.118f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  // The OS draws this window's frame, so ImGui must not round the corners of a
  // surface that fills it: a rounded panel inside a square frame shows four
  // notches of clear colour.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::Begin("##inspector", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImGui::TextColored(ui_color(0.91f, 0.92f, 0.94f), "Prompt inspector");
  ImGui::TextColored(ui_color(0.62f, 0.65f, 0.72f),
                     "Read-only: what is populating Claude right now.");
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ---- the body ----
  //
  // **This child is the seam M5.2, M5.3 and M5.4 fill, and it is the whole of
  // what they touch in this file.** M5.1 is the shell: the window, its
  // lifetime, its input and its geometry. What belongs here:
  //
  //  - M5.2: three sections - Global, Project (loaded), Skills - each row
  //    naming the prompt, its source file and when it was injected, drawn from
  //    a snapshot the frame loop builds from PromptStore and the session's
  //    injection state and passes in as draw()'s second argument. Passed
  //    rather than reached for: the store is written on the turn thread.
  //  - M5.3: an estimated token count per row, and a footer totalling them
  //    against UsageStats.ctx.
  //  - M5.4: project prompts that exist but have not fired, greyed out with
  //    the trigger words that would fire them.
  //
  // Nothing above this comment is theirs; nothing inside the child is M5.1's.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_color(0.11f, 0.12f, 0.15f));
  ImGui::BeginChild("##inspector_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
  // M5.2, and everything M5.3 and M5.4 add, is in inspector_list.cpp. The
  // comment above says this child is the whole of what they touch in this
  // file; drawing the list needs file-scope helpers, and every one of them
  // would have had to live outside the child. One call keeps that promise
  // exactly rather than approximately.
  draw_prompt_list(inv);
  ImGui::EndChild();
  ImGui::PopStyleColor();

  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (auto r = s.renderer->waitFrameSlot(); !r) {
    log::error("inspector wait: {}", r.error().message);
    return true;
  }
  if (auto d = s.renderer->drawFrame(nullptr); !d)
    log::error("inspector draw: {}", d.error().message);
  return true;
}

}  // namespace aii
