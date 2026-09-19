#include "approval_window.h"

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

using namespace rend;

namespace aii {
namespace {

// Wide enough for a sentence of description and three buttons on one line,
// narrow enough to sit beside the widget on a laptop.
constexpr unsigned kWinW = 420;
// The shell: title, the question, the footer rule and the all-buttons.
constexpr unsigned kChrome = 138;
constexpr unsigned kRowH = 68;
// Past this the list scrolls rather than the window growing. A notification
// that filled the screen because the model had a productive turn would be the
// same failure as a stack of windows, in one frame instead of six.
constexpr unsigned kRowsShown = 4;
constexpr unsigned kMinH = kChrome + kRowH;
constexpr unsigned kMaxH = kChrome + kRowH * kRowsShown;

unsigned height_for(std::size_t rows) {
  const unsigned n = static_cast<unsigned>(std::min<std::size_t>(std::max<std::size_t>(rows, 1),
                                                                 kRowsShown));
  return std::clamp(kChrome + kRowH * n, kMinH, kMaxH);
}


struct ApprovalInput {
  std::vector<std::pair<int, bool>> buttons;
  float wheel = 0.0f;
};

LRESULT CALLBACK approvalProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
  auto* in = reinterpret_cast<ApprovalInput*>(ref);
  if (in) {
    switch (msg) {
      case WM_LBUTTONDOWN: in->buttons.emplace_back(0, true); break;
      case WM_LBUTTONUP: in->buttons.emplace_back(0, false); break;
      case WM_MOUSEWHEEL: in->wheel += GET_WHEEL_DELTA_WPARAM(wp) / 120.0f; break;
      case WM_MOUSEACTIVATE:
        // The strip's measurement, and the same reason: WS_EX_NOACTIVATE alone
        // is not enough because SDL's own procedure answers this message
        // before the ex-style is consulted. The click still lands.
        return MA_NOACTIVATE;
      case WM_CLOSE:
        // Swallowed. There is no close box to press — this window is
        // borderless — but the SDL backend maps both quit paths onto one
        // identityless CloseRequested that the frame loop quits the whole
        // application on, so a WM_CLOSE arriving here by any route (a shell
        // "close window", Alt+F4 while it somehow has focus) must not reach
        // it. The only ways off this window are Confirm and Dismiss.
        return 0;
      default: break;
    }
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

struct ApprovalWindow::Impl {
  std::unique_ptr<platform::PresentationTarget> target;
  std::unique_ptr<gpu::Swapchain> swapchain;
  std::unique_ptr<gpu::FrameRenderer> renderer;
  std::unique_ptr<ImGuiLayer> ui;
  std::unique_ptr<ApprovalInput> input;
  HWND hwnd = nullptr;
  HWND prev_foreground = nullptr;
  unsigned w = kWinW;
  unsigned h = kMinH;
  int x = 0, y = 0;   // window rect top-left
  int anchor_x = 0;   // the bottom-right this grows up and left from
  int anchor_y = 0;
  bool shown = false;
  bool subclassed = false;
};

std::unique_ptr<ApprovalWindow> ApprovalWindow::create(platform::IPlatformBackend& backend,
                                                       gpu::Instance& instance,
                                                       gpu::Device& device, float font_px,
                                                       int anchor_x, int anchor_y,
                                                       std::string* error) {
  const auto fail = [&](std::string msg) -> std::unique_ptr<ApprovalWindow> {
    if (error) *error = std::move(msg);
    return nullptr;
  };
  auto self = std::unique_ptr<ApprovalWindow>(new ApprovalWindow());
  self->p_ = std::make_unique<Impl>();
  Impl& s = *self->p_;
  // Before the window exists, because createTarget is about to take the
  // foreground away from whoever has it. See the show block below.
  s.prev_foreground = GetForegroundWindow();
  s.anchor_x = anchor_x;
  s.anchor_y = anchor_y;

  // **Borderless, not Decorated.** The header explains it: a title bar is a
  // title-bar drag and a title-bar drag is a Windows modal loop on the thread
  // that pumps this app's frames. This window therefore cannot be moved, and
  // that is the trade — a notification the user cannot drag, against a
  // notification that can freeze the app while they drag it.
  auto t = backend.createTarget({
      .style = platform::WindowStyle::Borderless,
      .size = {s.w, s.h},
      .title = "AIInterface - new script",
      .vulkan = false,
  });
  if (!t) return fail("createTarget: " + t.error().message);
  s.target = std::move(t).value();
  s.hwnd = static_cast<HWND>(backend.nativeWindowHandle(*s.target));
  if (!s.hwnd) return fail("no HWND for the approval window");

  // createTarget ends in SDL_ShowWindow, which *activates*. A style added
  // afterwards cannot undo an activation that already happened, so it is
  // hidden, restyled, and shown again with SW_SHOWNOACTIVATE once it is where
  // it belongs — the sidebar's sequence, for the sidebar's measured reason.
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

  s.input = std::make_unique<ApprovalInput>();
  s.subclassed = SetWindowSubclass(s.hwnd, approvalProc, 1,
                                   reinterpret_cast<DWORD_PTR>(s.input.get())) != FALSE;
  if (!s.subclassed) {
    // Fatal for the same reason it is fatal everywhere else here: without the
    // subclass a close reaches SDL, and SDL's close is identityless and quits
    // the application. A notification that can kill the app is worse than no
    // notification — the Scripts row still arms.
    return fail("could not subclass the approval window for input");
  }
  log::info("approval window: up, hwnd {:p}", static_cast<void*>(s.hwnd));
  return self;
}

ApprovalWindow::~ApprovalWindow() {
  if (!p_) return;
  Impl& s = *p_;
  if (s.hwnd && s.subclassed) RemoveWindowSubclass(s.hwnd, approvalProc, 1);
  if (s.renderer) {
    s.renderer->waitIdle();
    s.renderer->setOverlayRecorder(nullptr);
    s.renderer->setFramePasses({});
  }
  s.ui.reset();
  s.renderer.reset();
  s.swapchain.reset();
  s.target.reset();
  log::info("approval window: torn down");
}

HWND ApprovalWindow::hwnd() const { return p_->hwnd; }

void ApprovalWindow::draw(float dt, const std::vector<ApprovalRow>& rows, ApprovalResult* out) {
  Impl& s = *p_;
  if (!s.ui || !out) return;

  // The queue decides the height. Through setSize(), never a raw SetWindowPos
  // size — see the header.
  const unsigned want = height_for(rows.size());
  const bool resize = want != s.h;
  if (resize) {
    s.h = want;
    s.target->setSize({s.w, s.h});
    s.renderer->resize(s.w, s.h);
  }
  // Grows up and left from the anchor, so the bottom-right corner stays put as
  // rows arrive: a window whose *top* is fixed would push its own buttons down
  // the screen under the user's cursor as the model wrote a second file.
  s.x = s.anchor_x - static_cast<int>(s.w);
  s.y = s.anchor_y - static_cast<int>(s.h);
  SetWindowPos(s.hwnd, HWND_TOPMOST, s.x, s.y, static_cast<int>(s.w), static_cast<int>(s.h),
               SWP_NOACTIVATE);
  if (!s.shown) {
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    s.shown = true;
    // And hand the foreground back, because none of the above is enough on its
    // own: the activation SDL performed at creation outlives every later "do
    // not activate". Measured by the sidebar, and it matters more here — this
    // window appears *while the user is talking*, which is exactly when taking
    // their caret would be worst.
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
  ImGui::Begin("##approval", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  // Nothing draws this window's frame but us, so we draw one: a borderless
  // window with no edge reads as a hole in the desktop rather than as a thing.
  {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRect(ImVec2(0.5f, 0.5f), ImVec2(static_cast<float>(s.w) - 0.5f,
                                           static_cast<float>(s.h) - 0.5f),
                ImGui::GetColorU32(accent), 0.0f, 0, 1.0f);
  }

  // ---- the question, in the user's own framing -------------------------
  ImGui::PushStyleColor(ImGuiCol_Text, bright);
  const bool many = rows.size() > 1;
  ImGui::TextUnformatted(many ? "New scripts have been created" : "A new script has been created");
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Text, dim);
  // The second line agrees with the first. It used to be fixed, so two queued
  // scripts read "New scripts have been created / Would you like to see it?".
  // The singular keeps the user's own wording ("we have created a new script.
  // Would you like to see it?") to the letter; the plural changes the one word
  // English makes it change.
  ImGui::TextWrapped(many ? "Would you like to see them? Nothing runs until you confirm."
                          : "Would you like to see it? Nothing runs until you confirm.");
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::BeginChild("##approval_rows",
                    ImVec2(0.0f, -(ImGui::GetFrameHeightWithSpacing() + 8.0f)));
  for (const ApprovalRow& r : rows) {
    ImGui::PushID(r.name.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, bright);
    ImGui::TextUnformatted(r.name.c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
    ImGui::TextWrapped("%s", r.description.empty() ? "(no description)" : r.description.c_str());
    ImGui::PopStyleColor();
    // "Show me" is Explorer with the file selected, which is the user's own
    // "navigates you to the directory of the script and you can open it": they
    // are taken to it and they open it, rather than an editor of our choosing
    // being launched at them.
    if (ImGui::SmallButton("Show me")) out->reveal = r.path;
    ImGui::SameLine();
    if (ImGui::SmallButton("Confirm")) out->arm.push_back(r.name);
    ImGui::SameLine();
    if (ImGui::SmallButton("Dismiss")) out->dismiss.push_back(r.name);
    ImGui::Spacing();
    ImGui::PopID();
  }
  ImGui::EndChild();

  if (many) {
    ImGui::Separator();
    if (ImGui::SmallButton("Confirm all")) out->arm_all = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Dismiss all")) out->dismiss_all = true;
    ImGui::SameLine();
    ImGui::TextColored(dim, "%d waiting", static_cast<int>(rows.size()));
  } else {
    ImGui::Separator();
    // Said rather than implied. "Dismiss" on a thing the AI just made sounds
    // like a delete, and a user who reads it that way will not press it — so
    // the window says what it actually does, which is nothing irreversible.
    ImGui::TextColored(dim, "Dismiss keeps it, unarmed, under Scripts.");
  }

  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (auto r = s.renderer->waitFrameSlot(); !r) {
    log::error("approval window wait: {}", r.error().message);
    return;
  }
  if (auto d = s.renderer->drawFrame(nullptr); !d)
    log::error("approval window draw: {}", d.error().message);
}

}  // namespace aii
