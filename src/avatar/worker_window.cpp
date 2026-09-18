#include "worker_window.h"

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

// The window's floor. Narrower than this and the activity lines wrap to one
// word each; shorter and the transcript child is a slot.
constexpr int kMinW = 280;
constexpr int kMinH = 240;

// How much of the window must land on a monitor's **work area** for a position
// to be honoured — enough frame to see it and enough title bar to drag it back.
// A window that fails it is centred rather than nudged, because a nudge from an
// unplugged monitor lands in a corner that is technically on screen and still
// wrong. (The inspector's rule, and the same numbers.)
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

// Makes a geometry legal against the desktop **as it is now**. The caller picks
// a slot to the left of the widget and that arithmetic can run off the left of
// the screen with enough windows open; this is where that is caught, once, for
// every caller.
void fit_to_desktop(WorkerGeometry& g) {
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
  log::info("worker window: {},{} is off-screen; centring", g.x, g.y);
  centre();
}

// This window's own input, queued rather than fed to ImGui as it arrives — the
// messages are pumped inside the *widget's* pumpEvents() call, where another
// ImGui context is current and another frame may be half built.
struct WorkerInput {
  std::vector<std::pair<int, bool>> buttons;  // (ImGui button index, pressed)
  float wheel = 0.0f;
  // The title bar's close box. Latched here and answered by the frame loop,
  // because a window may only be destroyed between frames.
  bool close_requested = false;
};

LRESULT CALLBACK workerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* in = reinterpret_cast<WorkerInput*>(ref);
  if (in) {
    switch (msg) {
      case WM_LBUTTONDOWN: in->buttons.emplace_back(0, true); break;
      case WM_LBUTTONUP: in->buttons.emplace_back(0, false); break;
      case WM_RBUTTONDOWN: in->buttons.emplace_back(1, true); break;
      case WM_RBUTTONUP: in->buttons.emplace_back(1, false); break;
      case WM_MOUSEWHEEL: in->wheel += GET_WHEEL_DELTA_WPARAM(wp) / 120.0f; break;
      case WM_CLOSE:
        // Swallowed, and a safety property rather than a nicety. The SDL3
        // backend maps both SDL_EVENT_QUIT and SDL_EVENT_WINDOW_CLOSE_REQUESTED
        // onto one identityless Event::CloseRequested, and the frame loop quits
        // the application on it — so letting this reach SDL would make a worker
        // window's close box shut the widget down with it. There may be four of
        // these on screen at once, which is four X buttons that would each have
        // been an exit. It is answered here instead, by the one window it is
        // about.
        in->close_requested = true;
        return 0;
      default: break;
    }
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

ImVec4 state_ink(WorkerPool::State s) {
  switch (s) {
    case WorkerPool::State::Working: return ui_color(0.44f, 0.80f, 0.53f);
    case WorkerPool::State::Done: return ui_color(0.78f, 0.62f, 0.95f);
    case WorkerPool::State::Failed: return ui_color(0.95f, 0.53f, 0.44f);
    default: return ui_color(0.62f, 0.65f, 0.72f);
  }
}

// "12s" / "3m 07s". The window's one ticking quantity, and it earns its place:
// a page of text that has not changed in a minute is indistinguishable from a
// page of text that has frozen, and this is what tells the two apart without
// the user having to trust that it would have updated if there were news.
std::string age_text(float seconds) {
  const int total = static_cast<int>(seconds);
  if (total < 60) return std::to_string(total) + "s";
  char buf[32];
  snprintf(buf, sizeof(buf), "%dm %02ds", total / 60, total % 60);
  return buf;
}

}  // namespace

struct WorkerWindow::Impl {
  std::unique_ptr<platform::PresentationTarget> target;
  std::unique_ptr<gpu::Swapchain> swapchain;
  std::unique_ptr<gpu::FrameRenderer> renderer;
  std::unique_ptr<ImGuiLayer> ui;
  std::unique_ptr<WorkerInput> input;
  HWND hwnd = nullptr;
  std::string worker;
  unsigned w = 0;
  unsigned h = 0;
  WorkerGeometry remembered;
  bool subclassed = false;
  // The last row the pool gave us, kept so the window can go on saying
  // something true after the worker has been removed from the pool.
  WorkerPool::Snapshot last;
  bool ever_seen = false;
  bool gone = false;
  // Seconds since the activity line last changed, and the transcript length it
  // was measured against — the "for 12s" beside what the worker is doing.
  float activity_age = 0.0f;
  std::string activity_at;
  std::size_t last_recent = 0;
};

std::unique_ptr<WorkerWindow> WorkerWindow::create(platform::IPlatformBackend& backend,
                                                   gpu::Instance& instance, gpu::Device& device,
                                                   float font_px, const std::string& worker,
                                                   WorkerGeometry wanted, std::string* error) {
  const auto fail = [&](std::string msg) -> std::unique_ptr<WorkerWindow> {
    if (error) *error = std::move(msg);
    return nullptr;
  };
  fit_to_desktop(wanted);

  auto self = std::unique_ptr<WorkerWindow>(new WorkerWindow());
  self->p_ = std::make_unique<Impl>();
  Impl& s = *self->p_;
  s.worker = worker;
  s.remembered = wanted;
  s.w = wanted.w;
  s.h = wanted.h;
  s.last.name = worker;

  // Decorated: a frame to drag, a corner to pull and a close box, all of which
  // the OS draws. The size handed over is the *client* size, which is what the
  // swapchain is about to be made at.
  auto t = backend.createTarget({
      .style = platform::WindowStyle::Decorated,
      .size = {s.w, s.h},
      .title = "AIInterface - worker: " + worker,
      .vulkan = false,
  });
  if (!t) return fail("createTarget: " + t.error().message);
  s.target = std::move(t).value();
  s.hwnd = static_cast<HWND>(backend.nativeWindowHandle(*s.target));
  if (!s.hwnd) return fail("no HWND for the worker window");

  // Position through Win32 — allowed, and only the *size* is not. SWP_NOSIZE
  // for exactly that reason: the size the backend holds is the one it just
  // created the window at, and this call must not touch it. A window rect that
  // grows while the client rect stays put is how this project clipped three
  // approved fixes in a row.
  SetWindowPos(s.hwnd, HWND_TOP, s.remembered.x, s.remembered.y, 0, 0,
               SWP_NOSIZE | SWP_NOOWNERZORDER);

  auto sc = gpu::Swapchain::create(instance, device,
                                   {
                                       .nativeSurface = s.hwnd,
                                       .width = s.w,
                                       .height = s.h,
                                       .transparent = false,  // a page of text needs no alpha
                                       .vsync = false,  // vsynced chains on one thread divide fps
                                   });
  if (!sc) return fail("swapchain: " + sc.error().message);
  s.swapchain = std::move(sc).value();

  auto fr = gpu::FrameRenderer::create(device, *s.swapchain);
  if (!fr) return fail("frame renderer: " + fr.error().message);
  s.renderer = std::move(fr).value();
  {
    // The panel's own background, so this reads as part of the same
    // application. Linearised: the swapchain view is sRGB and the hardware
    // encodes whatever is written, clear values included.
    const ImVec4 bg = ui_color(0.086f, 0.094f, 0.118f, 1.0f);
    s.renderer->setClearColor(bg.x, bg.y, bg.z, bg.w);
  }

  std::string err;
  s.ui = ImGuiLayer::create(device, s.swapchain->imageFormat(), font_px, &err);
  if (!s.ui) return fail("imgui: " + err);
  ImGuiLayer* layer = s.ui.get();
  s.renderer->setOverlayRecorder([layer](gpu::CommandContext& cmd) { layer->end_frame(cmd); });

  s.input = std::make_unique<WorkerInput>();
  s.subclassed =
      SetWindowSubclass(s.hwnd, workerProc, 1, reinterpret_cast<DWORD_PTR>(s.input.get())) != FALSE;
  if (!s.subclassed) {
    // Not survivable: without the subclass the close box reaches SDL, which
    // reports an identityless CloseRequested, which quits the whole
    // application. A window whose X kills the app is worse than no window.
    return fail("could not subclass the worker window for input");
  }
  log::info("worker window: {} up, hwnd {:p}, {}x{} at {},{}", worker,
            static_cast<void*>(s.hwnd), s.w, s.h, s.remembered.x, s.remembered.y);
  return self;
}

WorkerWindow::~WorkerWindow() {
  if (!p_) return;
  Impl& s = *p_;
  // The strip's order, for the strip's reasons: the subclass comes off the HWND
  // before the WorkerInput it points at is freed, the GPU is waited on, and the
  // renderer's callbacks are dropped before the objects they capture. This runs
  // on every close, not only at exit, and with N of these windows it is what
  // decides whether opening and closing them leaks an ImGui context each time.
  if (s.hwnd && s.subclassed) RemoveWindowSubclass(s.hwnd, workerProc, 1);
  if (s.renderer) {
    s.renderer->waitIdle();
    s.renderer->setOverlayRecorder(nullptr);
    s.renderer->setFramePasses({});
  }
  s.ui.reset();
  s.renderer.reset();
  s.swapchain.reset();
  s.target.reset();
  log::info("worker window: {} torn down", s.worker);
}

HWND WorkerWindow::hwnd() const { return p_->hwnd; }
const std::string& WorkerWindow::worker() const { return p_->worker; }
WorkerGeometry WorkerWindow::geometry() const { return p_->remembered; }

bool WorkerWindow::draw(float dt, const WorkerPool::Snapshot* live) {
  Impl& s = *p_;
  if (s.input && s.input->close_requested) return false;
  if (!s.ui) return true;

  // The frame's row, taken before anything is drawn. A worker that has left the
  // pool does not blank the window and does not close it: the last row stays,
  // greyed, with a line saying it is a record rather than a feed.
  if (live) {
    if (live->activity != s.activity_at) {
      s.activity_at = live->activity;
      s.activity_age = 0.0f;
    } else {
      s.activity_age += dt;
    }
    s.last = *live;
    s.ever_seen = true;
    s.gone = false;
  } else {
    s.activity_age += dt;
    if (s.ever_seen) s.gone = true;
  }

  // Minimised: nothing to draw, and nothing worth remembering either — a
  // minimised window's rect sits at -32000 and storing it would be exactly the
  // off-screen case fit_to_desktop has a rule against.
  if (IsIconic(s.hwnd)) return true;

  // The size the OS has given the window, which the user may be dragging right
  // now. Read back rather than driven: a resize performed with the mouse is the
  // OS's own and the backend has already seen it — it is the *create* path that
  // must never bypass setSize().
  const auto now = s.target->sizeInPixels();
  if (now.width != 0 && now.height != 0 && (now.width != s.w || now.height != s.h)) {
    s.w = now.width;
    s.h = now.height;
    s.renderer->resize(s.w, s.h);
  }
  if (s.w == 0 || s.h == 0) return true;

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
  // touched it last, and with N of these windows that is never a safe
  // assumption — the previous one to draw was another worker's.
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

  const WorkerPool::Snapshot& w = s.last;
  const ImVec4 ink = s.gone ? ui_color(0.50f, 0.52f, 0.58f) : state_ink(w.state);
  const ImVec4 dim = ui_color(0.62f, 0.65f, 0.72f);
  const ImVec4 bright = ui_color(0.91f, 0.92f, 0.94f);

  s.ui->begin_frame(s.w, s.h, dt);
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(s.w), static_cast<float>(s.h)));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_color(0.086f, 0.094f, 0.118f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
  // The OS draws this window's frame, so ImGui must not round the corners of a
  // surface that fills it: a rounded panel inside a square frame shows four
  // notches of clear colour.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::Begin("##worker", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  // ---- the header: the same figure the strip's icon uses, so the window the
  // click opened is visibly the thing that was clicked ----
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (const char* const* figure = icon_for_glyph(ButtonGlyph::Workers))
      draw_icon(dl, figure, ImVec2(p.x, p.y + 1.0f), ImGui::GetColorU32(ink),
                ImGui::GetColorU32(ink));
    ImGui::Dummy(ImVec2(kIconPx + 8.0f, kIconPx));
    ImGui::SameLine();
    ImGui::TextColored(bright, "%s", w.name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(ink, "[%s]", worker_state_name(w.state));
  }
  if (!w.task.empty()) ImGui::TextWrapped("%s", w.task.c_str());
  if (!w.cwd.empty()) {
    // Wrapped, not TextColored: a worker's cwd is routinely a path longer than
    // 360 px, and this app has already lost the end of one path off the right
    // edge of a 360 px surface once (the art button's tooltip). A directory
    // whose last component is the part that identifies it is a directory that
    // must not be clipped at the last component.
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
    ImGui::TextWrapped("in %s", w.cwd.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  if (s.gone) {
    // The worker has left the pool while its window was open. The window is not
    // closed — a window that vanished under the cursor mid-read is worse than a
    // stale one — and it is not blanked either. It says what it is: the last
    // thing this worker was seen doing, and no longer live.
    ImGui::TextColored(ui_color(0.95f, 0.80f, 0.44f), "This worker is no longer running.");
    ImGui::TextColored(dim, "What follows is the last thing it was seen doing.");
    ImGui::Spacing();
  }

  // ---- what it is doing right now ----
  ImGui::TextColored(dim, "Working on");
  ImGui::PushStyleColor(ImGuiCol_Text, ink);
  ImGui::TextWrapped("%s", s.activity_at.empty() ? "(nothing said yet)" : s.activity_at.c_str());
  ImGui::PopStyleColor();
  ImGui::TextColored(dim, "for %s   ·   %d tool call%s", age_text(s.activity_age).c_str(),
                     w.tool_calls, w.tool_calls == 1 ? "" : "s");

  ImGui::Spacing();

  // ---- the transcript: what it has been doing, oldest first ----
  //
  // The chat's own colours and its own shape, because this window is meant to
  // read as a sibling of the primary chat rather than as a different kind of
  // thing. The pool keeps the last six lines, so this is a window onto a
  // rolling tail and not a full history — which is why it says so rather than
  // pretending the worker started six tool calls ago.
  const float footer = w.result.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 5.0f;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_color(0.055f, 0.063f, 0.082f));
  ImGui::BeginChild("##worker_recent", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders);
  if (w.recent.empty()) {
    ImGui::TextColored(dim, "Nothing yet. Tool calls appear here as they happen.");
  } else {
    for (const std::string& line : w.recent) ImGui::TextWrapped("· %s", line.c_str());
  }
  // Follow the tail only while the user is already at it: scrolling back to
  // read something is the one moment an auto-scroll is an act of vandalism.
  if (w.recent.size() != s.last_recent) {
    s.last_recent = w.recent.size();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  if (!w.result.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(dim, "It reported");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_color(0.055f, 0.063f, 0.082f));
    ImGui::BeginChild("##worker_result", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    ImGui::TextWrapped("%s", w.result.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }

  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (auto r = s.renderer->waitFrameSlot(); !r) {
    log::error("worker window wait: {}", r.error().message);
    return true;
  }
  if (auto d = s.renderer->drawFrame(nullptr); !d)
    log::error("worker window draw: {}", d.error().message);
  return true;
}

}  // namespace aii
