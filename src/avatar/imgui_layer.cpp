#include "imgui_layer.h"

#include "rend/core/log.h"
#include "rend/gpu/command_context.h"
#include "rend/gpu/device.h"
#include "rend/gpu/frame_renderer.h"

#include <windows.h>
#include <d3d12.h>

#include "imgui.h"
#include "imgui_impl_dx12.h"

#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

using namespace rend;

namespace aii {
namespace {

constexpr int kSrvDescriptors = 16;  // the font atlas, plus room to grow

// sRGB transfer function: the inverse of what the render target applies.
float to_linear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

DXGI_FORMAT dxgi_format(gpu::Format f) {
  switch (f) {
    case gpu::Format::B8G8R8A8Srgb: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case gpu::Format::B8G8R8A8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case gpu::Format::R8G8B8A8Srgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case gpu::Format::R8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

int to_imgui_button(platform::MouseButton b) {
  switch (b) {
    case platform::MouseButton::Left: return 0;
    case platform::MouseButton::Right: return 1;
    case platform::MouseButton::Middle: return 2;
    default: return -1;
  }
}

bool file_exists(const std::string& path) {
  return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string fonts_dir() {
  char buf[MAX_PATH]{};
  if (GetWindowsDirectoryA(buf, MAX_PATH)) return std::string(buf) + "\\Fonts\\";
  return "C:\\Windows\\Fonts\\";
}

// Latin text comes from Segoe UI and Japanese is merged in from whichever CJK
// face this install has, so a reply in either language renders as text rather
// than as the missing-glyph box. Falls back to ImGui's built-in ASCII font.
void load_fonts(float px) {
  ImGuiIO& io = ImGui::GetIO();
  const std::string dir = fonts_dir();

  ImFont* latin = nullptr;
  for (const char* face : {"segoeui.ttf", "tahoma.ttf", "arial.ttf"}) {
    const std::string path = dir + face;
    if (!file_exists(path)) continue;
    latin = io.Fonts->AddFontFromFileTTF(path.c_str(), px);
    if (latin) break;
  }
  if (!latin) {
    io.Fonts->AddFontDefault();
    log::warn("imgui: no system font found; falling back to the built-in ASCII font");
    return;
  }

  // Merged into the same font so no call site has to switch fonts. The
  // Japanese range is ~3000 glyphs, which is why this is a merge onto a Latin
  // face rather than one CJK font doing everything.
  ImFontConfig cfg;
  cfg.MergeMode = true;
  for (const char* face : {"YuGothM.ttc", "YuGothR.ttc", "meiryo.ttc", "msgothic.ttc"}) {
    const std::string path = dir + face;
    if (!file_exists(path)) continue;
    if (io.Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, io.Fonts->GetGlyphRangesJapanese()))
      return;
  }
  log::warn("imgui: no Japanese font found; Japanese text will show as boxes");
}

}  // namespace

ImVec4 ui_color(float r, float g, float b, float a) {
  return ImVec4(to_linear(r), to_linear(g), to_linear(b), a);
}

struct ImGuiLayer::State {
  ID3D12DescriptorHeap* srv_heap = nullptr;
  UINT srv_stride = 0;
  std::vector<int> free_slots;
  ImGuiContext* ctx = nullptr;
  bool backend_up = false;
  bool frame_active = false;
  float mouse_x = -FLT_MAX, mouse_y = -FLT_MAX;
};

namespace {
// The backend allocates SRV descriptors through plain function pointers; one
// static is enough because the avatar runs exactly one layer.
ImGuiLayer::State* g_state = nullptr;
}  // namespace

std::unique_ptr<ImGuiLayer> ImGuiLayer::create(const gpu::Device& device,
                                               gpu::Format color_format, float font_px,
                                               std::string* error) {
  const auto fail = [&](std::string msg) -> std::unique_ptr<ImGuiLayer> {
    if (error) *error = std::move(msg);
    return nullptr;
  };
  if (device.api() != gpu::Api::D3D12)
    return fail("the ImGui layer is D3D12 only (this build ran with --vulkan)");
  const DXGI_FORMAT rtv = dxgi_format(color_format);
  if (rtv == DXGI_FORMAT_UNKNOWN)
    return fail(std::string("unsupported swapchain format ") + gpu::formatName(color_format));

  auto* d3d = static_cast<ID3D12Device*>(device.nativeHandle());
  auto* queue = static_cast<ID3D12CommandQueue*>(device.nativeGraphicsQueue());
  if (!d3d || !queue) return fail("the device exposed no D3D12 handles");

  auto layer = std::unique_ptr<ImGuiLayer>(new ImGuiLayer());
  layer->s_ = std::make_unique<State>();
  State& s = *layer->s_;

  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = kSrvDescriptors;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(d3d->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s.srv_heap))))
    return fail("could not create the ImGui descriptor heap");
  s.srv_stride = d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (int i = kSrvDescriptors - 1; i >= 0; --i) s.free_slots.push_back(i);

  IMGUI_CHECKVERSION();
  s.ctx = ImGui::CreateContext();
  // B2, and it cost an access violation to find: ImGui::CreateContext()
  // *restores the previously current context* before returning (imgui.cpp,
  // "Restore previous context if any, else keep new one"). With one window
  // there is no previous context and the new one is left current, so this line
  // was never needed; with two, everything below — the fonts, the style, and
  // ImGui_ImplDX12_Init — silently configured the *first* window's context a
  // second time and left the second one empty. The crash then landed a frame
  // later inside ImGui_ImplDX12_NewFrame, dereferencing the null backend data
  // of a context nothing had initialised (its IM_ASSERT is compiled out in
  // Release). Nothing about it says "second window".
  ImGui::SetCurrentContext(s.ctx);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // a corner widget has no layout worth persisting
  io.LogFilename = nullptr;
  load_fonts(font_px);

  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.WindowBorderSize = 0.0f;
  style.ScrollbarSize = 10.0f;
  style.WindowPadding = ImVec2(8.0f, 8.0f);
  // The window is sized *from* the panel, so the panel must not be sized from
  // the window. ImGui caps an auto-resizing window at the viewport less twice
  // this padding (`CalcWindowAutoFitSize`), and here the viewport is the OS
  // window we just resized to the panel's own height — leaving the default 3
  // px, each frame trimmed six more pixels off the panel, asked for a shorter
  // window, and the widget collapsed to a sliver within a second. Zero makes
  // "window == panel" the equilibrium it has to be.
  style.DisplaySafeAreaPadding = ImVec2(0.0f, 0.0f);
  style.ItemSpacing = ImVec2(6.0f, 5.0f);
  // Surfaces stay opaque: anything drawn over the avatar area would otherwise
  // blend with the desktop instead of with the panel behind it.
  style.Colors[ImGuiCol_PopupBg].w = 1.0f;
  style.Colors[ImGuiCol_WindowBg].w = 1.0f;
  style.Colors[ImGuiCol_ChildBg].w = 1.0f;
  // Every style colour is authored in sRGB and the target encodes: linearise.
  for (int i = 0; i < ImGuiCol_COUNT; ++i) {
    const ImVec4 c = style.Colors[i];
    style.Colors[i] = ui_color(c.x, c.y, c.z, c.w);
  }

  g_state = &s;
  ImGui_ImplDX12_InitInfo info{};
  info.Device = d3d;
  info.CommandQueue = queue;
  info.NumFramesInFlight = static_cast<int>(gpu::FrameRenderer::kFramesInFlight);
  info.RTVFormat = rtv;
  info.DSVFormat = DXGI_FORMAT_UNKNOWN;  // the overlay pass has no depth
  info.SrvDescriptorHeap = s.srv_heap;
  info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                 D3D12_GPU_DESCRIPTOR_HANDLE* gpu_handle) {
    IM_ASSERT(g_state && !g_state->free_slots.empty());
    const int slot = g_state->free_slots.back();
    g_state->free_slots.pop_back();
    *cpu = g_state->srv_heap->GetCPUDescriptorHandleForHeapStart();
    *gpu_handle = g_state->srv_heap->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += static_cast<SIZE_T>(slot) * g_state->srv_stride;
    gpu_handle->ptr += static_cast<UINT64>(slot) * g_state->srv_stride;
  };
  info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                D3D12_GPU_DESCRIPTOR_HANDLE) {
    if (!g_state) return;
    const SIZE_T base = g_state->srv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    g_state->free_slots.push_back(static_cast<int>((cpu.ptr - base) / g_state->srv_stride));
  };
  if (!ImGui_ImplDX12_Init(&info)) {
    g_state = nullptr;
    ImGui::DestroyContext(s.ctx);
    s.ctx = nullptr;
    s.srv_heap->Release();
    s.srv_heap = nullptr;
    return fail("ImGui_ImplDX12_Init failed");
  }
  s.backend_up = true;
  return layer;
}

ImGuiLayer::~ImGuiLayer() {
  if (!s_) return;
  // The backend's shutdown reads the context it was initialised against, and
  // frees its font descriptor through g_state's allocator: both have to point
  // at *this* layer, not at whichever one happened to draw last.
  make_current();
  if (s_->backend_up) ImGui_ImplDX12_Shutdown();
  if (s_->ctx) ImGui::DestroyContext(s_->ctx);
  if (s_->srv_heap) s_->srv_heap->Release();
  if (g_state == s_.get()) g_state = nullptr;
}

// B2 spike: ImGui's API works on one ambient "current context", and the D3D12
// backend keeps its own state inside that context. With a second window there
// are two, so every entry point into a layer has to say which one it is about
// before touching ImGui at all. `g_state` follows for the same reason: the SRV
// allocator the D3D12 backend calls is a plain function pointer with no user
// data, so it has to find the heap belonging to whichever layer is current.
void ImGuiLayer::make_current() {
  if (!s_) return;
  ImGui::SetCurrentContext(s_->ctx);
  g_state = s_.get();
}

bool ImGuiLayer::handle_event(const platform::Event& event) {
  make_current();
  ImGuiIO& io = ImGui::GetIO();
  switch (event.type) {
    case platform::Event::Type::MouseMoved:
      s_->mouse_x = event.mouseX;
      s_->mouse_y = event.mouseY;
      io.AddMousePosEvent(event.mouseX, event.mouseY);
      return io.WantCaptureMouse;
    case platform::Event::Type::MouseButtonDown:
    case platform::Event::Type::MouseButtonUp: {
      const int button = to_imgui_button(event.button);
      if (button < 0) return false;
      // The click carries its own position: a window that never takes focus
      // can deliver the press before any motion event has arrived.
      if (event.mouseX != s_->mouse_x || event.mouseY != s_->mouse_y) {
        s_->mouse_x = event.mouseX;
        s_->mouse_y = event.mouseY;
        io.AddMousePosEvent(event.mouseX, event.mouseY);
      }
      io.AddMouseButtonEvent(button, event.type == platform::Event::Type::MouseButtonDown);
      return io.WantCaptureMouse;
    }
    case platform::Event::Type::MouseWheel:
      io.AddMouseWheelEvent(0.0f, event.wheelDelta);
      return io.WantCaptureMouse;
    // Keys are deliberately absent: WinTextInput's HWND subclass is the single
    // path into ImGui for them (B1). Forwarding the sixteen the engine knows
    // about as well would deliver each of those twice — a doubled Backspace
    // eats two characters — and the other ninety-odd would still be missing.
    default:
      return false;
  }
}

void ImGuiLayer::sync_pointer(void* hwnd) {
  if (!hwnd) return;
  make_current();
  POINT p{};
  RECT client{};
  if (!GetCursorPos(&p)) return;
  auto* wnd = static_cast<HWND>(hwnd);
  if (!GetClientRect(wnd, &client)) return;
  if (!ScreenToClient(wnd, &p)) return;
  const bool inside = p.x >= client.left && p.x < client.right && p.y >= client.top &&
                      p.y < client.bottom;
  // Only while the pointer is over the window, or while it is dragging one of
  // our widgets. Outside both, the event path already said where it went and
  // inventing a position here would hover widgets the pointer has left.
  if (!inside && !ImGui::IsAnyItemActive()) return;
  const float x = static_cast<float>(p.x);
  const float y = static_cast<float>(p.y);
  if (x == s_->mouse_x && y == s_->mouse_y) return;
  s_->mouse_x = x;
  s_->mouse_y = y;
  ImGui::GetIO().AddMousePosEvent(x, y);
}

bool ImGuiLayer::wants_keyboard() const {
  if (!s_) return false;
  ImGui::SetCurrentContext(s_->ctx);
  const ImGuiIO& io = ImGui::GetIO();
  return io.WantCaptureKeyboard || io.WantTextInput;
}

void ImGuiLayer::begin_frame(std::uint32_t width, std::uint32_t height, float dt) {
  make_current();
  // A frame the renderer never got to (a skipped or failed drawFrame) would
  // otherwise trip NewFrame's "forgot to call Render" assert.
  if (s_->frame_active) ImGui::EndFrame();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
  io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
  ImGui_ImplDX12_NewFrame();
  ImGui::NewFrame();
  s_->frame_active = true;
}

void ImGuiLayer::end_frame(gpu::CommandContext& cmd) {
  make_current();
  if (!s_->frame_active) return;
  s_->frame_active = false;
  ImGui::Render();
  auto* list = static_cast<ID3D12GraphicsCommandList*>(cmd.nativeHandle());
  if (list) ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
}

}  // namespace aii
