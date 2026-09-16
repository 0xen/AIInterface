#pragma once
// Dodging the Windows activation watermark (M1d.1).
//
// On an unactivated Windows, DWM draws "Activate Windows / Go to Settings to
// activate Windows" near the bottom-right of the primary display. It is
// composited *over* topmost windows, so the corner widget cannot win by being
// topmost and cannot repaint over it — and it has already cost this project
// time, landing across the avatar band in capture after capture. The answer is
// purely placement: when the watermark is there, sit above it.
//
// Two things are deliberately not attempted here. Nothing touches activation
// itself, and nothing is ever said about it in the UI: the window simply sits
// somewhere sensible and the user is not nagged by their own widget.
#include <string>

namespace aii {

// Three states rather than a boolean, because detection of this kind is
// exactly what is wrong on somebody else's machine: `auto` trusts the
// licensing API, and the other two are the user overruling it in either
// direction (a watermark we failed to detect, or a placement they do not want
// on a machine we wrongly think is unactivated).
enum class WatermarkDodge { Auto = 0, Always, Never };
inline constexpr int kWatermarkDodgeCount = 3;
// On-disk and on-command-line spelling, in declaration order. These are the
// settings file's format, so they must stay stable.
extern const char* const kWatermarkDodgeNames[kWatermarkDodgeCount];

// Accepts the names above, and also on/off/true/false/1/0 — an env var that
// is a switch invites being set to `1`. Anything else yields `def`, so a typo
// leaves the behaviour the settings file asked for rather than silently
// meaning "never".
WatermarkDodge parse_watermark_dodge(const std::string& text, WatermarkDodge def);

// True when Windows reports itself as not genuinely licensed, i.e. when the
// watermark is expected to be on screen. Asked once and cached: it cannot
// change under a running process in any way that matters here.
//
// A failed or unavailable check answers **false** — today's placement is the
// safe default, and an activated machine must never have its widget shoved up
// the screen because a licensing call misfired.
bool windows_watermark_expected();

// The bottom margin `placeInCorner` should use, given the margin it would use
// on an activated machine. Returns `base_margin` unchanged unless the dodge is
// on, so the placement that already works is byte-identical in that case.
//
// The watermark cannot be found as a window — DWM draws it, so FindWindow and
// enumeration turn up nothing — so its rectangle is a measured offset from the
// bottom-right of the primary display, in DIPs, scaled here by the window's
// DPI. See the constants in the .cpp for how it was measured.
int corner_bottom_margin(void* hwnd, int base_margin, WatermarkDodge mode);

}  // namespace aii
