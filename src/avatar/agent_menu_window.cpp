#include "agent_menu_window.h"

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
#include <string>
#include <utility>
#include <vector>

#include "core/text_util.h"
#include "imgui_layer.h"

using namespace rend;

namespace aii {
namespace {

// 360 px, which is the widget's width and the worker window's, and deliberately
// not the approval window's 420: this is a sibling of the agent windows it
// opens, and a sibling twice as wide is a different kind of thing.
constexpr unsigned kWinW = 360;
// The shell: the title line, the count line, the rule, and the footer.
constexpr unsigned kChrome = 106;
// Two lines of text plus air. One row is a name-and-state line and a clipped
// line of what the agent said, which is the least that lets a user tell two
// finished agents apart without opening either.
constexpr unsigned kRowH = 52;
constexpr unsigned kMinH = kChrome + kRowH;
constexpr unsigned kMaxH = kChrome + kRowH * static_cast<unsigned>(kAgentRowsShown);

unsigned height_for(std::size_t rows) {
  const unsigned n = static_cast<unsigned>(
      std::min<std::size_t>(std::max<std::size_t>(rows, 1), kAgentRowsShown));
  return std::clamp(kChrome + kRowH * n, kMinH, kMaxH);
}

// The one line of result a row shows. Bounded in **bytes** and cut with
// `clip_utf8`, never by hand: 220 bytes is about 73 Japanese characters, and a
// previous spoken summary was cut mid-character roughly two times in three
// because a word-boundary fallback assumed spaces. A worker's result is model
// output of unbounded length arriving on a worker thread, so this is the one
// place it is allowed to be long, and it is not this one.
constexpr std::size_t kPreviewBytes = 160;

std::string preview_of(const std::string& text) {
  // The first line, not the first 160 bytes of a paragraph: a result that opens
  // with a heading and then explains itself reads as the heading, and a result
  // that is one sentence is unaffected.
  std::string s = text.substr(0, text.find('\n'));
  // Newlines are gone by construction; a stray tab or carriage return would
  // still draw as a box in ImGui.
  for (char& c : s)
    if (static_cast<unsigned char>(c) < ' ') c = ' ';
  return clip_utf8(std::move(s), kPreviewBytes);
}

// The same line, trimmed until it actually *fits* the row — because a byte cap
// is not a width. At 160 bytes a result ran off the right edge and was cut
// mid-word against the child's border, which reads as a broken window rather
// than as an abbreviation; the byte cap above is the cheap bound that keeps
// this loop short, and this is the one the eye sees.
//
// Every cut goes through `clip_utf8`, never `resize()`: 220 bytes is about 73
// Japanese characters, and a cut that lands mid-character draws a replacement
// box — the exact failure a previous spoken summary hit two times in three.
// Binary search over the byte count so a long line costs ~8 measurements and
// not one per byte.
std::string fit_utf8(const std::string& s, float max_w) {
  if (s.empty() || ImGui::CalcTextSize(s.c_str()).x <= max_w) return s;
  const float ell = ImGui::CalcTextSize("...").x;
  std::size_t lo = 0, hi = s.size();
  std::string best;
  while (lo <= hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    std::string t = clip_utf8(s, mid);
    if (ImGui::CalcTextSize(t.c_str()).x + ell <= max_w) {
      best = std::move(t);
      lo = mid + 1;
    } else {
      if (mid == 0) break;
      hi = mid - 1;
    }
  }
  return best + "...";
}

ImVec4 state_ink(WorkerPool::State s) {
  // The panel's palette (`avatar_ui.cpp:worker_color`), so a finished agent is
  // the same colour here as it was in the chat before it moved.
  switch (s) {
    case WorkerPool::State::Failed: return ui_color(0.96f, 0.46f, 0.40f);
    case WorkerPool::State::Paused: return ui_color(0.62f, 0.65f, 0.72f);
    default: return ui_color(0.78f, 0.62f, 0.96f);  // Done
  }
}

struct AgentMenuInput {
  std::vector<std::pair<int, bool>> buttons;
  float wheel = 0.0f;
};

LRESULT CALLBACK agentMenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* in = reinterpret_cast<AgentMenuInput*>(ref);
  if (in) {
    switch (msg) {
      case WM_LBUTTONDOWN: in->buttons.emplace_back(0, true); break;
      case WM_LBUTTONUP: in->buttons.emplace_back(0, false); break;
      case WM_MOUSEWHEEL: in->wheel += GET_WHEEL_DELTA_WPARAM(wp) / 120.0f; break;
      case WM_MOUSEACTIVATE:
        // The strip's and the approval window's measurement, and the same
        // reason: WS_EX_NOACTIVATE alone is not enough, because SDL's own
        // procedure answers this message before the ex-style is consulted. The
        // click still lands.
        return MA_NOACTIVATE;
      case WM_CLOSE:
        // Swallowed. There is no close box — this window is borderless — but
        // the SDL backend maps every quit path onto one identityless
        // `CloseRequested` that the frame loop quits the whole application on,
        // so a WM_CLOSE arriving here by any route must not reach it. The ways
        // off this window are its own Close button and the hamburger.
        return 0;
      default: break;
    }
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

struct AgentMenuWindow::Impl {
  std::unique_ptr<platform::PresentationTarget> target;
  std::unique_ptr<gpu::Swapchain> swapchain;
  std::unique_ptr<gpu::FrameRenderer> renderer;
  std::unique_ptr<ImGuiLayer> ui;
  std::unique_ptr<AgentMenuInput> input;
  HWND hwnd = nullptr;
  HWND prev_foreground = nullptr;
  unsigned w = kWinW;
  unsigned h = kMinH;
  int x = 0, y = 0;
  int anchor_x = 0;
  int anchor_y = 0;
  bool shown = false;
  bool subclassed = false;
};

std::unique_ptr<AgentMenuWindow> AgentMenuWindow::create(platform::IPlatformBackend& backend,
                                                         gpu::Instance& instance,
                                                         gpu::Device& device, float font_px,
                                                         int anchor_x, int anchor_y,
                                                         std::string* error) {
  const auto fail = [&](std::string msg) -> std::unique_ptr<AgentMenuWindow> {
    if (error) *error = std::move(msg);
    return nullptr;
  };
  auto self = std::unique_ptr<AgentMenuWindow>(new AgentMenuWindow());
  self->p_ = std::make_unique<Impl>();
  Impl& s = *self->p_;
  // Before the window exists, because createTarget is about to take the
  // foreground away from whoever has it. See the show block in draw().
  s.prev_foreground = GetForegroundWindow();
  s.anchor_x = anchor_x;
  s.anchor_y = anchor_y;

  // Borderless: see the header. A title bar is a title-bar drag and a
  // title-bar drag is a Windows modal loop on the thread that pumps this app's
  // frames.
  auto t = backend.createTarget({
      .style = platform::WindowStyle::Borderless,
      .size = {s.w, s.h},
      .title = "AIInterface - finished agents",
      .vulkan = false,
  });
  if (!t) return fail("createTarget: " + t.error().message);
  s.target = std::move(t).value();
  s.hwnd = static_cast<HWND>(backend.nativeWindowHandle(*s.target));
  if (!s.hwnd) return fail("no HWND for the agent menu");

  // createTarget ends in SDL_ShowWindow, which *activates*. A style added
  // afterwards cannot undo an activation that already happened, so it is
  // hidden, restyled, and shown again with SW_SHOWNOACTIVATE from draw() once
  // it is where it belongs — the sidebar's sequence, for the sidebar's
  // measured reason.
  ShowWindow(s.hwnd, SW_HIDE);
  const LONG_PTR ex = GetWindowLongPtrW(s.hwnd, GWL_EXSTYLE);
  SetWindowLongPtrW(s.hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);

  auto sc = gpu::Swapchain::create(instance, device,
                                   {
                                       .nativeSurface = s.hwnd,
                                       .width = s.w,
                                       .height = s.h,
                                       .transparent = false,
                                       .vsync = false,
                                   });
  if (!sc) return fail("swapchain: " + sc.error().message);
  s.swapchain = std::move(sc).value();

  auto fr = gpu::FrameRenderer::create(device, *s.swapchain);
  if (!fr) return fail("frame renderer: " + fr.error().message);
  s.renderer = std::move(fr).value();
  {
    const ImVec4 bg = ui_color(0.086f, 0.094f, 0.118f, 1.0f);
    s.renderer->setClearColor(bg.x, bg.y, bg.z, bg.w);
  }

  std::string err;
  s.ui = ImGuiLayer::create(device, s.swapchain->imageFormat(), font_px, &err);
  if (!s.ui) return fail("imgui: " + err);
  ImGuiLayer* layer = s.ui.get();
  s.renderer->setOverlayRecorder([layer](gpu::CommandContext& cmd) { layer->end_frame(cmd); });

  s.input = std::make_unique<AgentMenuInput>();
  s.subclassed = SetWindowSubclass(s.hwnd, agentMenuProc, 1,
                                   reinterpret_cast<DWORD_PTR>(s.input.get())) != FALSE;
  if (!s.subclassed) {
    // Fatal for the reason it is fatal in every other window here: without the
    // subclass a close reaches SDL, and SDL's close is identityless and quits
    // the application. A menu that can kill the app is worse than no menu.
    return fail("could not subclass the agent menu for input");
  }
  log::info("agent menu: up, hwnd {:p}", static_cast<void*>(s.hwnd));
  return self;
}

AgentMenuWindow::~AgentMenuWindow() {
  if (!p_) return;
  Impl& s = *p_;
  if (s.hwnd && s.subclassed) RemoveWindowSubclass(s.hwnd, agentMenuProc, 1);
  if (s.renderer) {
    s.renderer->waitIdle();
    s.renderer->setOverlayRecorder(nullptr);
    s.renderer->setFramePasses({});
  }
  s.ui.reset();
  s.renderer.reset();
  s.swapchain.reset();
  s.target.reset();
  log::info("agent menu: torn down");
}

HWND AgentMenuWindow::hwnd() const { return p_->hwnd; }
unsigned AgentMenuWindow::width() const { return p_->w; }
unsigned AgentMenuWindow::height() const { return p_->h; }

void AgentMenuWindow::set_anchor(int anchor_x, int anchor_y) {
  p_->anchor_x = anchor_x;
  p_->anchor_y = anchor_y;
}

void AgentMenuWindow::draw(float dt, const std::vector<AgentMenuRow>& rows, AgentMenuResult* out) {
  Impl& s = *p_;
  if (!s.ui || !out) return;

  // The list decides the height. Through setSize(), never a raw SetWindowPos
  // size — see the header.
  const unsigned want = height_for(rows.size());
  if (want != s.h) {
    s.h = want;
    s.target->setSize({s.w, s.h});
    s.renderer->resize(s.w, s.h);
  }
  // Grows up and left from the anchor, so the bottom-right corner stays put as
  // agents finish: a window whose *top* was fixed would walk its own buttons
  // down the screen under the user's cursor every time one came back.
  s.x = s.anchor_x - static_cast<int>(s.w);
  s.y = s.anchor_y - static_cast<int>(s.h);
  SetWindowPos(s.hwnd, HWND_TOPMOST, s.x, s.y, static_cast<int>(s.w), static_cast<int>(s.h),
               SWP_NOACTIVATE);
  if (!s.shown) {
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    s.shown = true;
    // And hand the foreground back, because none of the above is enough on its
    // own: the activation SDL performed at creation outlives every later "do
    // not activate". Measured by the sidebar.
    if (s.prev_foreground && GetForegroundWindow() == s.hwnd)
      SetForegroundWindow(s.prev_foreground);
  }

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

  const ImVec4 dim = ui_color(0.62f, 0.65f, 0.72f);
  const ImVec4 bright = ui_color(0.91f, 0.92f, 0.94f);
  const ImVec4 accent = ui_color(0.42f, 0.72f, 0.96f);

  s.ui->begin_frame(s.w, s.h, dt);
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(s.w), static_cast<float>(s.h)));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_color(0.086f, 0.094f, 0.118f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::Begin("##agent_menu", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  // Nothing draws this window's frame but us, so we draw one: a borderless
  // window with no edge reads as a hole in the desktop rather than as a thing.
  {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRect(ImVec2(0.5f, 0.5f),
                ImVec2(static_cast<float>(s.w) - 0.5f, static_cast<float>(s.h) - 0.5f),
                ImGui::GetColorU32(accent), 0.0f, 0, 1.0f);
  }

  // ---- the head -------------------------------------------------------
  ImGui::PushStyleColor(ImGuiCol_Text, bright);
  ImGui::TextUnformatted("Finished agents");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  // Hard right, so the close sits where a close sits even though this window
  // has no title bar to put one in.
  {
    const float bw = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(static_cast<float>(s.w) - 14.0f - bw);
    if (ImGui::SmallButton("Close")) out->close = true;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, dim);
  ImGui::TextUnformatted(rows.size() == 1 ? "1 agent has finished.  Click one to open it."
                                          : "Click one to open it.");
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // ---- the list -------------------------------------------------------
  ImGui::BeginChild("##agent_menu_rows",
                    ImVec2(0.0f, -(ImGui::GetFrameHeightWithSpacing() + 6.0f)));
  for (const AgentMenuRow& r : rows) {
    ImGui::PushID(r.name.c_str());
    // The whole row is the click target, not a button at the end of it: the
    // user's gesture is "click the agent", and a 360 px row with a 60 px hit
    // box in it is a row that mostly does not work.
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(
        "##row", ImVec2(ImGui::GetContentRegionAvail().x, static_cast<float>(kRowH) - 8.0f));
    const bool hovered = ImGui::IsItemHovered();
    if (clicked) out->open = r.name;
    if (hovered || r.window_open) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 bot = ImGui::GetItemRectMax();
      dl->AddRectFilled(top, bot,
                        ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button),
                        4.0f);
      if (r.window_open)
        dl->AddRectFilled(top, ImVec2(top.x + 2.0f, bot.y), ImGui::GetColorU32(accent));
    }
    // Painted over the button, which is why the cursor is put back rather than
    // the text being drawn first: an InvisibleButton must own the region.
    ImGui::SetCursorScreenPos(ImVec2(top.x + 8.0f, top.y + 3.0f));
    ImGui::TextColored(state_ink(r.state), "%s", r.name.c_str());
    ImGui::SameLine();
    ImGui::TextColored(dim, "%s%s", worker_state_name(r.state),
                       r.tool_calls ? ("  x" + std::to_string(r.tool_calls)).c_str() : "");
    ImGui::SetCursorScreenPos(ImVec2(top.x + 8.0f, top.y + 3.0f + ImGui::GetTextLineHeight() + 2.0f));
    {
      // The result if there is one, the task if there is not: a `Failed` agent
      // often has no result, and a row that then said nothing at all would be
      // the one row the user most needs to read.
      const std::string line =
          preview_of(!r.result.empty() ? r.result
                                       : (r.task.empty() ? std::string("(nothing came back)")
                                                         : "task: " + r.task));
      // The width left between this row's text origin and the child's right
      // edge, which is what the line has to fit in — not the window's width.
      const float room = ImGui::GetContentRegionAvail().x - 8.0f;
      ImGui::TextColored(dim, "%s", fit_utf8(line, room).c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + static_cast<float>(kRowH) - 6.0f));
    ImGui::PopID();
  }
  ImGui::EndChild();

  // ---- the footer -----------------------------------------------------
  ImGui::Separator();
  // Said rather than implied, the approval window's rule: an agent leaving the
  // chat's live list looks like its work was thrown away, so the window says
  // what actually happened to it. Nothing here was lost.
  ImGui::TextColored(dim, "%d finished - the chat keeps what they said",
                     static_cast<int>(rows.size()));

  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (auto r = s.renderer->waitFrameSlot(); !r) {
    log::error("agent menu wait: {}", r.error().message);
    return;
  }
  if (auto d = s.renderer->drawFrame(nullptr); !d)
    log::error("agent menu draw: {}", d.error().message);
}

}  // namespace aii
