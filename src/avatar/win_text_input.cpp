#include "win_text_input.h"

#include <windows.h>
#include <commctrl.h>

#include "imgui.h"

namespace aii {
namespace {

constexpr UINT_PTR kSubclassId = 1;

// Virtual key → ImGuiKey. The three doubled modifiers and the keypad Enter
// cannot be told apart from the virtual key alone, so lParam's scancode and
// extended bit resolve them the way Windows itself does.
ImGuiKey vk_to_imgui(WPARAM vk, LPARAM lparam) {
  const bool extended = (HIWORD(lparam) & KF_EXTENDED) != 0;
  const UINT scancode = (HIWORD(lparam) & 0xFF);
  switch (vk) {
    case VK_SHIFT:
      return scancode == MapVirtualKeyW(VK_RSHIFT, MAPVK_VK_TO_VSC) ? ImGuiKey_RightShift
                                                                    : ImGuiKey_LeftShift;
    case VK_CONTROL: return extended ? ImGuiKey_RightCtrl : ImGuiKey_LeftCtrl;
    case VK_MENU: return extended ? ImGuiKey_RightAlt : ImGuiKey_LeftAlt;
    case VK_RETURN: return extended ? ImGuiKey_KeypadEnter : ImGuiKey_Enter;
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_LWIN: return ImGuiKey_LeftSuper;
    case VK_RWIN: return ImGuiKey_RightSuper;
    case VK_APPS: return ImGuiKey_Menu;
    case VK_CAPITAL: return ImGuiKey_CapsLock;
    case VK_SCROLL: return ImGuiKey_ScrollLock;
    case VK_NUMLOCK: return ImGuiKey_NumLock;
    case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
    case VK_PAUSE: return ImGuiKey_Pause;
    case VK_OEM_1: return ImGuiKey_Semicolon;
    case VK_OEM_PLUS: return ImGuiKey_Equal;
    case VK_OEM_COMMA: return ImGuiKey_Comma;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_2: return ImGuiKey_Slash;
    case VK_OEM_3: return ImGuiKey_GraveAccent;
    case VK_OEM_4: return ImGuiKey_LeftBracket;
    case VK_OEM_5: return ImGuiKey_Backslash;
    case VK_OEM_6: return ImGuiKey_RightBracket;
    case VK_OEM_7: return ImGuiKey_Apostrophe;
    default: break;
  }
  if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (int)(vk - '0'));
  if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (int)(vk - 'A'));
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
    return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (int)(vk - VK_NUMPAD0));
  if (vk >= VK_F1 && vk <= VK_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (int)(vk - VK_F1));
  switch (vk) {
    case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case VK_ADD: return ImGuiKey_KeypadAdd;
    case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
    case VK_DIVIDE: return ImGuiKey_KeypadDivide;
    default: return ImGuiKey_None;
  }
}

// ImGui tracks the modifiers as keys of their own *and* as a combined state.
// Polling rather than deducing them from the key stream is what keeps Ctrl+V
// working when the window was focused with Ctrl already held down.
void sync_modifiers(ImGuiIO& io) {
  io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
  io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
  io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
  io.AddKeyEvent(ImGuiMod_Super,
                 ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0);
}

}  // namespace

struct WinTextInput::State {
  HWND hwnd = nullptr;
  // The ImGui context this window's keys belong to, captured at install().
  // Until M30 the procedure fed whichever context was *current* when the
  // message arrived, which with one window was always the right one and with
  // seven -- the widget plus six script windows -- was almost always the
  // widget's. A script window's text field could never receive a keystroke,
  // and imnodes never saw a Delete.
  ImGuiContext* ctx = nullptr;
  bool submit = false;
  // Whether the last Enter press was handed to ImGui. The release has to
  // follow the press: a key-up for a key ImGui never saw go down leaves its
  // repeat bookkeeping describing a keyboard that does not exist.
  bool enter_forwarded = false;
};

namespace {

LRESULT CALLBACK subclass_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                               UINT_PTR /*id*/, DWORD_PTR ref) {
  auto* s = reinterpret_cast<WinTextInput::State*>(ref);
  // Messages can arrive before the first frame and after the context is gone.
  // The context is switched for the duration of the feed and put back, so a
  // key for a script window never lands in the widget's io and the frame loop
  // finds the ambient context exactly as it left it.
  ImGuiContext* const prev = ImGui::GetCurrentContext();
  if (s && s->ctx) ImGui::SetCurrentContext(s->ctx);
  struct Restore {
    ImGuiContext* prev;
    ~Restore() { ImGui::SetCurrentContext(prev); }
  } restore{prev};
  if (s && ImGui::GetCurrentContext()) {
    ImGuiIO& io = ImGui::GetIO();
    switch (msg) {
      case WM_CHAR:
      case WM_SYSCHAR:
        // UTF-16 so a pasted or dead-key composed character that lives outside
        // the BMP still arrives as one code point. Control characters are
        // dropped: Ctrl+V reaches here as 0x16, and Enter as 0x0D, and neither
        // is text — both are handled as keys below.
        if (wparam >= 32 && wparam != 127)
          io.AddInputCharacterUTF16(static_cast<unsigned short>(wparam));
        break;
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN: {
        sync_modifiers(io);
        const ImGuiKey key = vk_to_imgui(wparam, lparam);
        if (key == ImGuiKey_Enter || key == ImGuiKey_KeypadEnter) {
          // Plain Enter is withheld from ImGui and reported as a submit. The
          // message field is multiline (there is no other way to get a newline
          // out of an ImGui text widget), so an Enter ImGui saw would insert
          // one instead of sending. Shift+Enter is passed through and is
          // exactly what makes the newline.
          const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
          s->enter_forwarded = shift;
          if (!shift) {
            if (io.WantTextInput) s->submit = true;
            break;
          }
        }
        if (key != ImGuiKey_None) io.AddKeyEvent(key, true);
        break;
      }
      case WM_KEYUP:
      case WM_SYSKEYUP: {
        sync_modifiers(io);
        const ImGuiKey key = vk_to_imgui(wparam, lparam);
        if ((key == ImGuiKey_Enter || key == ImGuiKey_KeypadEnter) && !s->enter_forwarded) break;
        if (key != ImGuiKey_None) io.AddKeyEvent(key, false);
        break;
      }
      case WM_SETFOCUS:
      case WM_KILLFOCUS:
        // The window is created SWP_NOACTIVATE and is focused by a click, so it
        // loses focus routinely. Telling ImGui lets it drop the held keys it
        // will never see released.
        io.AddFocusEvent(msg == WM_SETFOCUS);
        break;
      default:
        break;
    }
  }
  // Always passed on: SDL3 owns this window and drains its messages, and the
  // app's own SPACE / S / E / Esc / Q still arrive as platform events.
  return DefSubclassProc(hwnd, msg, wparam, lparam);
}

}  // namespace

std::unique_ptr<WinTextInput> WinTextInput::install(void* hwnd) {
  if (!hwnd) return nullptr;
  auto in = std::unique_ptr<WinTextInput>(new WinTextInput());
  in->s_ = std::make_unique<State>();
  in->s_->hwnd = static_cast<HWND>(hwnd);
  in->s_->ctx = ImGui::GetCurrentContext();
  if (!SetWindowSubclass(in->s_->hwnd, subclass_proc, kSubclassId,
                         reinterpret_cast<DWORD_PTR>(in->s_.get())))
    return nullptr;
  return in;
}

WinTextInput::~WinTextInput() {
  if (s_ && s_->hwnd) RemoveWindowSubclass(s_->hwnd, subclass_proc, kSubclassId);
}

bool WinTextInput::take_submit() {
  if (!s_ || !s_->submit) return false;
  s_->submit = false;
  return true;
}

}  // namespace aii
