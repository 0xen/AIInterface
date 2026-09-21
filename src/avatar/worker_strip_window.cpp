#include "worker_strip_window.h"

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
#include "tool_window_core.h"
#include "pixel_icons.h"

using namespace rend;

namespace aii {
namespace {

// ---- layout (pixels) ----
//
// Deliberately the primary strip's numbers, not new ones: the two columns sit
// side by side and the eye reads them as one control surface, which it stops
// doing the moment the buttons are a different size.
constexpr unsigned kStripW = 48;
constexpr float kButton = 40.0f;
constexpr float kButtonGap = 6.0f;
constexpr float kStripPad = 3.0f;
constexpr int kDockGap = 6;

unsigned strip_height(std::size_t slots) {
  if (slots == 0) slots = 1;  // never a 0-px swapchain; see the empty slot below
  return static_cast<unsigned>(2.0f * kStripPad + slots * kButton + (slots - 1) * kButtonGap +
                               0.5f);
}

// M11.1: the hamburger. Three bars, which is the one icon on the internet
// everybody already reads as "the rest of it is in here", and the user asked
// for it by that name.
//
// File-local rather than in `pixel_icons.cpp` on purpose: that file holds the
// grids reachable through `ButtonGlyph`, and a glyph there is a promise that a
// registered toolbar button may ask for it. This one is chrome belonging to
// one window, like the twelve grids `avatar_ui.cpp` keeps to itself, so it
// costs `core/button_registry.h` nothing — which also keeps it out of a header
// another agent is editing this session.
//
// Four cells of bar and three of gap: at 2x a 2-cell bar is 4 px and reads as
// a line, and three lines with 2 px between them is a smear at this size.
constexpr IconRows kIconAgentMenu = {
    ".............",
    ".............",
    "..#########..",
    "..#########..",
    ".............",
    ".............",
    "..#########..",
    "..#########..",
    ".............",
    ".............",
    "..#########..",
    "..#########..",
    ".............",
};

// The state colours the panel's worker rows already use (avatar_ui.cpp's
// worker_color). Repeated rather than shared because avatar_ui's palette
// helpers are file-local there, and because a worker's icon and its panel row
// agreeing is a property worth one duplicated switch.
ImVec4 state_ink(WorkerPool::State s) {
  switch (s) {
    case WorkerPool::State::Working: return ui_color(0.44f, 0.80f, 0.53f);
    case WorkerPool::State::Done: return ui_color(0.78f, 0.62f, 0.95f);
    case WorkerPool::State::Failed: return ui_color(0.95f, 0.53f, 0.44f);
    default: return ui_color(0.62f, 0.65f, 0.72f);
  }
}

// This window's own input, queued rather than fed to ImGui as it arrives — the
// messages are pumped inside the *widget's* pumpEvents() call, where another
// ImGui context is current and another frame may be half built. (Same
// reasoning, same shape, as SidebarInput and InspectorInput.)
struct WorkerStripInput {
  std::vector<std::pair<int, bool>> buttons;  // (ImGui button index, pressed)
  float wheel = 0.0f;
};

LRESULT CALLBACK workerStripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                                 DWORD_PTR ref) {
  auto* in = reinterpret_cast<WorkerStripInput*>(ref);
  if (in) {
    switch (msg) {
      case WM_LBUTTONDOWN: in->buttons.emplace_back(0, true); break;
      case WM_LBUTTONUP: in->buttons.emplace_back(0, false); break;
      case WM_MOUSEWHEEL: in->wheel += GET_WHEEL_DELTA_WPARAM(wp) / 120.0f; break;
      case WM_MOUSEACTIVATE:
        // WS_EX_NOACTIVATE is not enough, measured on the primary strip: SDL's
        // own window procedure answers WM_MOUSEACTIVATE before the ex-style is
        // consulted. The click still arrives; the caret stays where it was.
        return MA_NOACTIVATE;
      case WM_CLOSE:
        // Swallowed, and a safety property rather than a nicety: the SDL3
        // backend maps both SDL_EVENT_QUIT and SDL_EVENT_WINDOW_CLOSE_REQUESTED
        // onto one identityless Event::CloseRequested that the frame loop quits
        // the whole application on. This window is borderless and has no close
        // box, so nothing should ever send it one — which is exactly why the
        // day something does, it must not take the widget with it.
        return 0;
      default: break;
    }
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

// M24.3. The shell is ToolWindowCore's; everything below is this strip's own.
struct WorkerStripWindow::Impl : ToolWindowCore {
  std::unique_ptr<WorkerStripInput> input;
  HWND prev_foreground = nullptr;
  int x = 0, y = 0;
  bool shown = false;
  std::vector<WorkerStripRow> rows;
  std::size_t finished = 0;   // M11.1: agents in the menu
  bool menu_open = false;
  std::string tooltip;
  float tooltip_y = 0.0f;  // screen space
};

std::unique_ptr<WorkerStripWindow> WorkerStripWindow::create(platform::IPlatformBackend& backend,
                                                             gpu::Instance& instance,
                                                             gpu::Device& device, float font_px,
                                                             std::string* error) {
  const auto fail = [&](std::string msg) -> std::unique_ptr<WorkerStripWindow> {
    if (error) *error = std::move(msg);
    return nullptr;
  };
  auto self = std::unique_ptr<WorkerStripWindow>(new WorkerStripWindow());
  self->p_ = std::make_unique<Impl>();
  Impl& s = *self->p_;
  // Before the window exists, because it is about to take the foreground away
  // from whoever has it — the same measured behaviour the primary strip hands
  // it back for.
  s.prev_foreground = GetForegroundWindow();

  // NoActivate: hidden, restyled, and shown again by dock() with
  // SW_SHOWNOACTIVATE, for the primary strip's reason — createTarget ends in
  // SDL_ShowWindow, which *activates*, and a style added afterwards cannot
  // undo an activation that already happened. Showing it in create() would
  // also put it wherever SDL happened to place it for a frame. Transparent,
  // and cleared to premultiplied zero so the desktop shows through.
  std::string err;
  if (!s.open(backend, instance, device, font_px,
              {
                  .style = platform::WindowStyle::BorderlessTransparent,
                  .title = "AIInterface workers",
                  .w = kStripW,
                  .h = strip_height(0),
                  .transparent = true,
                  .placement = ToolWindowPlacement::NoActivate,
                  .noun = "worker strip",
              },
              &err)) {
    return fail(err);
  }

  s.input = std::make_unique<WorkerStripInput>();
  if (!s.subclass(workerStripProc, s.input.get())) {
    // Not survivable, and this is the inspector's rule rather than the primary
    // strip's: without the subclass a WM_CLOSE reaching SDL reports an
    // identityless CloseRequested, which quits the whole application. A window
    // that can kill the app is worse than no window at all.
    return fail("could not subclass the worker strip for input");
  }
  log::info("workers: strip up, hwnd {:p}, {}x{}", static_cast<void*>(s.hwnd), s.w, s.h);
  return self;
}

WorkerStripWindow::~WorkerStripWindow() {
  if (!p_) return;
  Impl& s = *p_;
  // The primary strip's order, for the primary strip's reasons — ToolWindowCore's
  // now, and stated there. This runs on every close, not only at exit, so it is
  // what decides whether opening and closing the strip fifty times leaks fifty
  // ImGui contexts.
  s.shutdown();
  log::info("workers: strip torn down");
}

HWND WorkerStripWindow::hwnd() const { return p_->hwnd; }
unsigned WorkerStripWindow::width() const { return p_->w; }
unsigned WorkerStripWindow::height() const { return p_->h; }
int WorkerStripWindow::left() const { return p_->x; }

void WorkerStripWindow::set_rows(std::vector<WorkerStripRow> rows) {
  if (rows.size() > kWorkerSlotsMax) rows.resize(kWorkerSlotsMax);
  p_->rows = std::move(rows);
}

void WorkerStripWindow::set_finished(std::size_t count, bool menu_open) {
  p_->finished = count;
  p_->menu_open = menu_open;
}

// How many 40 px slots the column is drawing: the running workers, plus the
// hamburger when there is anything behind it. The empty-strip slot is not
// counted here — `strip_height` already floors at one.
std::size_t WorkerStripWindow::slot_count() const {
  return p_->rows.size() + (p_->finished ? 1u : 0u);
}

void WorkerStripWindow::dock(const RECT& widget, int right_edge) {
  Impl& s = *p_;
  const unsigned want = strip_height(slot_count());
  const int x = right_edge - static_cast<int>(s.w) - kDockGap;
  // **Bottom-anchored**: the strip's bottom edge is the widget's bottom edge,
  // and the column grows upward from it as workers arrive. From `want` rather
  // than `s.h`, which is the whole point on this window — it changes height at
  // runtime, and an origin computed from the old height would put the new
  // height at the old top edge for a frame, which is the bottom edge jumping a
  // slot every time a worker starts or stops.
  const int y = static_cast<int>(widget.bottom) - static_cast<int>(want);
  const bool resize = want != s.h;
  if (!resize && x == s.x && y == s.y && s.shown) return;

  if (resize) {
    // Through the target, never a raw SetWindowPos: SDL answers WM_NCCALCSIZE
    // with the size *it* holds, so a raw resize grows the window rect while the
    // client rect — the part DWM composites — stays put, and the strip silently
    // clips. This window resizes every time a worker starts or finishes, which
    // makes it the one in this app most likely to have found that out the hard
    // way.
    s.h = want;
    s.target->setSize({s.w, s.h});
  }
  s.x = x;
  s.y = y;
  SetWindowPos(s.hwnd, HWND_TOPMOST, x, y, static_cast<int>(s.w), static_cast<int>(s.h),
               SWP_NOACTIVATE);
  if (resize) s.renderer->resize(s.w, s.h);
  if (!s.shown) {
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    s.shown = true;
    // And then give the foreground back, because none of the above is enough on
    // its own: measured on the primary strip, the activation SDL performed at
    // creation outlives every later "do not activate". This window is opened by
    // a click, so the thing it would steal the caret from is whatever the user
    // was typing in a moment ago.
    if (s.prev_foreground && GetForegroundWindow() == s.hwnd)
      SetForegroundWindow(s.prev_foreground);
  }
}

WorkerStripResult WorkerStripWindow::draw(float dt) {
  Impl& s = *p_;
  WorkerStripResult out;
  s.tooltip.clear();
  if (!s.ui) return out;

  // Its own context, first: ImGui's ambient current context belongs to whoever
  // touched it last, and with four window types — and N chat windows — that is
  // never a safe assumption.
  s.ui->make_current();
  ImGuiIO& io = ImGui::GetIO();

  // The pointer, polled rather than tracked from WM_MOUSEMOVE: this window
  // moves under a still pointer every time the chat opens *and* resizes itself
  // every time a worker starts or stops, and a position that only changes when
  // the mouse does would leave a slot hovered that the pointer is no longer
  // over — or, worse, hovered over a different worker than the one it was on.
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
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kStripPad, kStripPad));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, kButtonGap));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  ImGui::Begin("##worker_strip", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const char* const* figure = icon_for_glyph(ButtonGlyph::Workers);

  if (s.rows.empty() && s.finished == 0) {
    // **The empty strip, which is the normal one.** This app runs with zero
    // workers nearly all of the time, so the case the user will see most often
    // is this one, and it must read as "nothing is running" rather than as a
    // window that failed to draw. A dimmed, unclickable slot with the same
    // figure in it says that; an empty 48x46 rectangle says nothing at all, and
    // a strip that collapsed to zero height would be a zero-px swapchain.
    //
    // It is an InvisibleButton rather than a disabled one because a disabled
    // item is not hovered, and the tooltip is the whole of the explanation.
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##worker_none", ImVec2(kButton, kButton));
    dl->AddRect(p, ImVec2(p.x + kButton, p.y + kButton),
                ImGui::GetColorU32(ui_color(0.22f, 0.24f, 0.29f)), 5.0f);
    if (figure) {
      const ImU32 ink = ImGui::GetColorU32(ui_color(0.28f, 0.30f, 0.36f));
      draw_icon(dl, figure, ImVec2(p.x + (kButton - kIconPx) * 0.5f, p.y + (kButton - kIconPx) * 0.5f),
                ink, ink);
    }
    if (ImGui::IsItemHovered()) {
      s.tooltip = "No workers running";
      s.tooltip_y = static_cast<float>(s.y) + p.y + kButton * 0.5f;
    }
  }

  for (const WorkerStripRow& row : s.rows) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(("##worker_" + row.name).c_str(),
                                                ImVec2(kButton, kButton));
    const ImGuiCol bg = ImGui::IsItemActive()    ? ImGuiCol_ButtonActive
                        : ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered
                                                 : ImGuiCol_Button;
    dl->AddRectFilled(p, ImVec2(p.x + kButton, p.y + kButton), ImGui::GetColorU32(bg), 5.0f);
    const ImU32 ink = ImGui::GetColorU32(state_ink(row.state));
    if (figure)
      draw_icon(dl, figure, ImVec2(p.x + (kButton - kIconPx) * 0.5f, p.y + (kButton - kIconPx) * 0.5f),
                ink, ink);
    // The worker's initial, bottom-right. Two workers are two identical figures
    // otherwise, and "which of these is `scout`" is the first question the strip
    // is asked — the tooltip answers it, but only after a hover, and a column
    // you have to hover to read is a column you stop using.
    if (!row.name.empty()) {
      const char badge[2] = {row.name[0], '\0'};
      const ImVec2 size = ImGui::CalcTextSize(badge);
      dl->AddText(ImVec2(p.x + kButton - size.x - 3.0f, p.y + kButton - size.y - 1.0f), ink, badge);
    }
    // Open: a bar down the left edge, in the state's own colour. A second
    // *shape* rather than a second colour, because the colour is already
    // carrying the state and two meanings on one channel is how a legend
    // becomes necessary.
    if (row.window_open)
      dl->AddRectFilled(ImVec2(p.x + 1.0f, p.y + 5.0f), ImVec2(p.x + 3.0f, p.y + kButton - 5.0f),
                        ink, 1.0f);
    if (ImGui::IsItemHovered()) {
      s.tooltip = row.name + "  [" + worker_state_name(row.state) + "]";
      if (!row.activity.empty()) s.tooltip += "\n" + row.activity;
      s.tooltip += row.window_open ? "\nClick to close its window" : "\nClick to watch it";
      // Screen space: ImGui's coordinates here are this window's client area,
      // and the widget that letters the tooltip has its own.
      s.tooltip_y = static_cast<float>(s.y) + p.y + kButton * 0.5f;
    }
    if (clicked) out.toggled = row.name;
  }

  // ---- M11.1: the hamburger ------------------------------------------
  //
  // **Last, so it is the bottom slot**, and the bottom slot is the only one
  // that never moves: this window is bottom-anchored and grows upward, so a
  // worker starting lifts the top edge and leaves everything measured from the
  // bottom exactly where it was. The hamburger is the one control here that is
  // pressed rather than watched, and a click target that walked up the screen
  // every time an agent spawned would be the f713297 bug wearing a different
  // hat.
  //
  // It is also why the running workers are drawn above it rather than below:
  // they are the list that changes, and the list that changes goes on the end
  // that is allowed to move.
  if (s.finished) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##agent_menu", ImVec2(kButton, kButton));
    const ImGuiCol bg = ImGui::IsItemActive()    ? ImGuiCol_ButtonActive
                        : ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered
                                                 : ImGuiCol_Button;
    dl->AddRectFilled(p, ImVec2(p.x + kButton, p.y + kButton), ImGui::GetColorU32(bg), 5.0f);
    // Dimmed against a running worker's green: this is history, and history
    // must not compete with the thing that is happening now.
    const ImU32 ink = ImGui::GetColorU32(s.menu_open ? ui_color(0.88f, 0.90f, 0.94f)
                                                     : ui_color(0.62f, 0.65f, 0.72f));
    draw_icon(dl, kIconAgentMenu,
              ImVec2(p.x + (kButton - kIconPx) * 0.5f, p.y + (kButton - kIconPx) * 0.5f), ink, ink);
    // The count, bottom-right, where a worker slot puts its initial — the same
    // corner means the same question ("which one is this?") has its answer in
    // the same place on every slot in the column.
    {
      const std::string badge = std::to_string(s.finished);
      const ImVec2 size = ImGui::CalcTextSize(badge.c_str());
      dl->AddText(ImVec2(p.x + kButton - size.x - 3.0f, p.y + kButton - size.y - 1.0f), ink,
                  badge.c_str());
    }
    // Open: the same left-edge bar a watched worker gets, for the same reason.
    if (s.menu_open)
      dl->AddRectFilled(ImVec2(p.x + 1.0f, p.y + 5.0f), ImVec2(p.x + 3.0f, p.y + kButton - 5.0f),
                        ink, 1.0f);
    if (ImGui::IsItemHovered()) {
      s.tooltip = s.finished == 1 ? "1 agent has finished" : std::to_string(s.finished) +
                                                                 " agents have finished";
      s.tooltip += s.menu_open ? "\nClick to close the list" : "\nClick to see them";
      s.tooltip_y = static_cast<float>(s.y) + p.y + kButton * 0.5f;
    }
    if (clicked) out.menu_toggled = true;
  }

  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();

  if (auto r = s.renderer->waitFrameSlot(); !r) {
    log::error("worker strip wait: {}", r.error().message);
    return out;
  }
  if (auto d = s.renderer->drawFrame(nullptr); !d)
    log::error("worker strip draw: {}", d.error().message);
  return out;
}

void WorkerStripWindow::draw_tooltip_into_widget(const RECT& widget) const {
  const Impl& s = *p_;
  if (s.tooltip.empty()) return;
  // Called inside the widget's ImGui frame: its context is current, its
  // viewport is the widget's 360 px, and its foreground draw list is over
  // everything the panel drew. This window's own context is untouched.
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float pad = 6.0f;
  const ImVec2 size = ImGui::CalcTextSize(s.tooltip.c_str());
  const float x = 4.0f;
  // Centred on the icon, then kept inside the widget: the foreground draw list
  // is clipped to the widget's viewport, so a three-line label beside the last
  // slot would have its last line silently cut off at the widget's bottom edge.
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
