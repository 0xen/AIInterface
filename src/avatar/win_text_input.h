#pragma once
// Keyboard text entry for the avatar window (blocker B1).
//
// `rend::platform::Event` carries a sixteen-entry Key enum and no character
// event at all, so nothing fed from the engine's event pump can ever spell a
// word. Rather than grow the user's engine for one app, this subclasses the
// window we already own the HWND for and feeds WM_CHAR / WM_KEYDOWN / WM_KEYUP
// straight into ImGui. Windows-only, which this whole app already is.
//
// Ownership, so no key is delivered twice: **the subclass owns every key ImGui
// sees**. ImGuiLayer::handle_event therefore no longer forwards KeyDown/KeyUp —
// it still owns the mouse, which arrives only through the platform events. The
// engine's own event pump keeps working untouched (every message is passed on
// to DefSubclassProc), and main.cpp keeps routing SPACE / S / E / Esc / Q from
// platform events — but only while ImGui does not want the keyboard, so a space
// typed into the message field no longer toggles the microphone.
//
// English only (user, 16 Sep 2026): no WM_IME_* path, no composition string, no
// candidate window. Japanese still *renders* through the merged font; it simply
// cannot be retyped, only pasted — which is why Ctrl+V has to work.
#include <memory>

namespace aii {

class WinTextInput {
 public:
  struct State;  // public only so the subclass procedure can see it

  // Null if the window could not be subclassed. Requires a live ImGui context.
  static std::unique_ptr<WinTextInput> install(void* hwnd);
  ~WinTextInput();
  WinTextInput(const WinTextInput&) = delete;
  WinTextInput& operator=(const WinTextInput&) = delete;

  // True once per press of Enter **without Shift** while a text widget has the
  // keyboard. That key never reaches ImGui: the message field is a multiline
  // widget, so an Enter ImGui saw would insert a newline instead of sending.
  // Shift+Enter is forwarded normally and is what makes the newline.
  bool take_submit();

 private:
  WinTextInput() = default;
  std::unique_ptr<State> s_;
};

}  // namespace aii
