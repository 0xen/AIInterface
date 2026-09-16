#include "watermark.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

namespace aii {

namespace {

// ---- the watermark rectangle, measured on this machine ----
//
// Measured rather than remembered: a black topmost window was placed over the
// bottom-right corner of the primary display and the screen captured, which
// makes every watermark pixel separable in one pass (DWM composites it over
// topmost windows, which is the whole problem, and here it is the tool). On
// this display — 3840x2160 at 150%, Windows 11 26200 — the text occupied
// physical x 3322..3663, y 1926..1985: 342x60 physical pixels, 174 above the
// bottom of the screen and 176 in from its right edge.
//
// Stored in DIPs (the physical numbers divided by the 1.5 scale factor) and
// scaled back by the window's own DPI at runtime, so the value is not specific
// to 150%. The width is not needed: the widget is anchored to the same corner
// and the two overlap horizontally at every sane widget width, so only the
// vertical extent decides the margin.
constexpr int kMarkHeightDip = 40;  // 60 physical / 1.5
constexpr int kMarkBottomDip = 116;  // 174 physical / 1.5, above the screen bottom
// A little air, so the widget's border does not sit on the text. Small on
// purpose: every pixel of it is corner space the widget gives up.
constexpr int kClearanceDip = 8;

// Software Licensing API, loaded by hand. slwga.dll's SLIsGenuineLocal is the
// cheapest honest answer to "is this activated": one call, no COM — which the
// WMI route (SoftwareLicensingProduct.LicenseStatus) would have dragged in for
// a single boolean this app needs once at startup, in a process that
// initialises no apartment of its own. Shelling out to slmgr was ruled out.
//
// Bound at runtime rather than linked: there is no import library for slwga in
// the SDK, and a machine without the DLL must degrade to "activated" rather
// than fail to start.
using SLIsGenuineLocalFn = HRESULT(WINAPI*)(const GUID*, DWORD*, void*);

// SL_GEN_STATE_IS_GENUINE. Everything else (invalid licence, tampered,
// offline) is either a watermark or a state we cannot read, and the state we
// cannot read is handled by the HRESULT check, not by this.
constexpr DWORD kGenuine = 0;

bool query_not_genuine() {
    // The Windows product itself, the application id the licensing API keys
    // every Windows SKU under.
    static const GUID kWindowsAppId = {
        0x55c92734, 0xd682, 0x4d71, {0x98, 0x3e, 0xd6, 0xec, 0x3f, 0x16, 0x05, 0x9f}};

    // Never freed: the answer is cached by the caller, so this runs once, and
    // unloading a licensing DLL on the way out buys nothing.
    HMODULE dll = LoadLibraryW(L"slwga.dll");
    if (!dll) return false;  // no DLL, no opinion -> activated
    auto is_genuine =
        reinterpret_cast<SLIsGenuineLocalFn>(reinterpret_cast<void*>(
            GetProcAddress(dll, "SLIsGenuineLocal")));
    if (!is_genuine) return false;

    DWORD state = kGenuine;
    // The third argument is the non-genuine UI options. It is null on purpose:
    // with it non-null this call is allowed to *show* something, and this
    // feature says nothing to the user anywhere.
    const HRESULT hr = is_genuine(&kWindowsAppId, &state, nullptr);
    if (FAILED(hr)) return false;  // could not tell -> assume activated
    return state != kGenuine;
}

int scale_dip(int dip, UINT dpi) {
    return MulDiv(dip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

}  // namespace

const char* const kWatermarkDodgeNames[kWatermarkDodgeCount] = {"auto", "always", "never"};

WatermarkDodge parse_watermark_dodge(const std::string& text, WatermarkDodge def) {
    std::string s;
    s.reserve(text.size());
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (s.empty()) return def;
    for (int i = 0; i < kWatermarkDodgeCount; ++i) {
        if (s == kWatermarkDodgeNames[i]) return static_cast<WatermarkDodge>(i);
    }
    if (s == "1" || s == "on" || s == "true" || s == "yes") return WatermarkDodge::Always;
    if (s == "0" || s == "off" || s == "false" || s == "no") return WatermarkDodge::Never;
    return def;
}

bool windows_watermark_expected() {
    static const bool expected = query_not_genuine();
    return expected;
}

int corner_bottom_margin(void* hwnd, int base_margin, WatermarkDodge mode) {
    const bool dodge = mode == WatermarkDodge::Always ||
                       (mode == WatermarkDodge::Auto && windows_watermark_expected());
    if (!dodge) return base_margin;

    // The process is per-monitor DPI aware (measured: the 360-wide window is
    // 360 *physical* pixels), so SPI_GETWORKAREA and SetWindowPos both speak
    // physical pixels and the DIP constants above have to be scaled into them.
    // GetDpiForWindow is the right source either way: a DPI-unaware process
    // would get 96 back and its virtualised coordinates would already be DIPs.
    const UINT dpi = hwnd ? GetDpiForWindow(static_cast<HWND>(hwnd)) : 0;
    const UINT eff = dpi != 0 ? dpi : USER_DEFAULT_SCREEN_DPI;

    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) return base_margin;
    const int screen_bottom = GetSystemMetrics(SM_CYSCREEN);
    if (screen_bottom <= 0) return base_margin;

    // The margin is worked out from the watermark's *top* rather than from its
    // height alone, because the two are not the same lift: the watermark
    // floats well clear of the taskbar (103 px above the work area here), so
    // raising the widget by the text's height would still leave it overlapping.
    // This puts the widget's bottom edge exactly `clearance` above the text.
    const int mark_top = screen_bottom - scale_dip(kMarkBottomDip + kMarkHeightDip, eff);
    const int margin = work.bottom - mark_top + scale_dip(kClearanceDip, eff);
    // Never *lower* the widget: if the watermark somehow sits below where the
    // widget already is, today's placement is already clear of it.
    return std::max(base_margin, margin);
}

}  // namespace aii
