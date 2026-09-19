// avatar: the corner window of AIInterface. A small borderless, transparent,
// always-on-top window on the rend engine (D3D12 backend: on this AMD GPU
// only D3D12's composition swapchain gives per-pixel alpha; Vulkan's WSI
// composites opaque). The top shows the 2D pixel avatar (M2.1: a cell grid
// drawn by AvatarRenderer); below it an ImGui panel carries the usage status bar, the collapsible chat
// and the Talk / Silence / Pause buttons. The voice loop itself lives in
// VoiceSession (engines from aii_core).
//
// The panel auto-sizes to its content and the window follows it, so closing
// the chat shrinks the whole widget back into the corner instead of leaving a
// transparent rectangle that still swallows clicks meant for the desktop.
//
//   avatar [--opaque] [--seconds S] [--vulkan] [--say "text"] [--no-voice]
//          [--avatar NAME] [--clip NAME]
//     --opaque    decorated opaque window (fallback / debugging)
//     --seconds   quit automatically after S seconds (scripted runs)
//     --vulkan    use the Vulkan backend (no transparency, and no UI: the
//                 ImGui layer is D3D12 only)
//     --say       send this text as the first user turn once the engines are up
//     --say-on-report  send this text as a user turn the first frame a finished
//                 worker report is waiting to be spoken (M2c.2): the harness
//                 for a report riding out on the tail of an answer
//     --no-voice  window only, no engines (layout work)
//     --avatar    which definition under %APPDATA%\AIInterface\avatars to load
//     --theme     one of the definition's named palettes, or "custom"
//     --colour    #rrggbb: pick the derived theme and the body colour it is
//                 derived from, for this run only. Like --theme it is not
//                 written back, so looking at a palette never changes the one
//                 the user comes back to.
//     --clip      pin one clip, instead of letting AvatarController choose.
//                 Either of --clip and --sprite switches the controller off
//                 entirely: they exist to look at one piece of art, and a
//                 policy quietly overwriting the thing you asked to look at
//                 is worse than no policy at all.
//     --sprite    force an accessory on, repeatable, "all" for every one.
//     --bus-in    a file tailed for JSON-line bus commands (M2.5): how the bus
//                 is driven with no Python. See bus_bindings.h.
//     --bus-out   a file every published bus event is appended to.
//     --bus-text  also publish turn text. Opt-in on purpose: the transcript
//                 does not leave the process because something connected.
//     --schedule  create a one-shot timer S seconds from startup (M2b.1),
//                 repeatable, `S` or `S:label`; the fire is logged with the
//                 thread it arrived on and how late it was
//     --schedule-report  the same at the Phrased grade (M2b.4): `S:<what
//                 happened>`, which is injected as a turn and reported in
//                 Claude's own words rather than spoken verbatim
//     --script   run this Python file (M2.6), repeatable, ahead of the user's
//                 own. The viewer's flag, and for the same use: a harness has
//                 to be up before a script that never returns starts. With no
//                 --script and an empty %APPDATA%\AIInterface\scripts, no
//                 Python DLL is loaded at all.
//     --no-scripts  discover nothing, whatever is in scripts\.
//     --settings  open the settings surface at startup, and
//     --settings-timing  the same, held scrolled to the Timing section, which
//                 is below the fold of a surface that scrolls (M1f.2).
//     --settings-startup  the same, held scrolled to the Startup section
//                 (M1f.5), which is below the fold as well.
//     --settings-tools  the same, held scrolled to the Tools section (M3.8),
//                 which is likewise below the fold.
//     --settings-model  the same, held scrolled to the Model section (M3.11),
//                 which sits just above Tools.
//     --listen-timeout-at S:V  at S seconds, write V seconds into the
//                 auto-listen control's own fields, exactly as a hand on it
//                 would; V of 0 is "never". How "a change reaches a
//                 microphone that is already latched" is measured.
//     --model-at S:key  at S seconds, move the Model picker to that row, and
//     --tool-at S:key:0|1  at S seconds, tick that tool group on or off —
//                 both written into the panel's own fields, as a hand would.
//                 M3.12: a change to either restarts the `claude` child at
//                 once and throws the conversation away, so these are how the
//                 restart, the settle window and the wait for a reply in
//                 flight are driven from outside. AII_REPLY_LOG=1 logs what
//                 came back, which is what such a run is read against.
//
//   SPACE / Talk    click (or tap) toggles the mic: conversation mode. While
//                   the mic is on, a pause in speech sends that utterance and
//                   the mic reopens after the reply, so one click carries a
//                   whole conversation. Press and hold instead to dictate: it
//                   records only while held and the transcript lands in the
//                   message field unsent, for editing. Either gesture barges
//                   in on a reply in flight.
//   S / Silence     stop the audio, keep the text
//   E / Pause       cancel the reply in flight
//   Esc / Q         quit
#include "rend/core/log.h"
#include "rend/core/paths.h"
#include "rend/gpu/command_context.h"
#include "rend/gpu/descriptor_table.h"
#include "rend/gpu/device.h"
#include "rend/gpu/feature_set.h"
#include "rend/gpu/frame_renderer.h"
#include "rend/gpu/instance.h"
#include "rend/gpu/pipeline.h"
#include "rend/gpu/shader.h"
#include "rend/gpu/swapchain.h"
#include "rend/platform/backend.h"

#include <windows.h>
#include <shellapi.h>


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "avatar_appearance.h"
#include "avatar_controller.h"
#include "avatar_def.h"
#include "avatar_renderer.h"
#include "avatar_ui.h"
#include "bus_bindings.h"
#include "core/app_bus.h"
#include "script_host.h"
#include "core/button_registry.h"
#include "core/config.h"
#include "core/model_choice.h"
#include "core/schedule.h"
#include "core/worker_pool.h"
#include "imgui_layer.h"
#include "inspector_window.h"
#include "loader_anim.h"
#include "settings.h"
#include "sidebar_window.h"
#include "voice_session.h"
#include "watermark.h"
#include "win_text_input.h"
#include "worker_strip_window.h"
#include "worker_window.h"

using namespace rend;

namespace {

// ---- layout (pixels) ----
constexpr std::uint32_t kWindowW = 360;
// Starting height, and also the **ceiling** the panel may never exceed.
//
// This used to be a hard limit: the window could shrink but never grow past the
// size it was created at, so it was created at its largest. That is fixed — the
// cause was a raw SetWindowPos leaving the *client* rect (the part DWM
// composites) pinned to the size SDL knew, and the size now goes through
// `PresentationTarget::setSize` in placeInCorner. A window here can grow.
//
// The ceiling is kept anyway, as a plain layout bound rather than a workaround:
// 780 has enough headroom for the tallest layout (avatar band + open chat + a
// four-line message field) and some to spare, and creating at the maximum means
// the window only ever shrinks, which is the direction this loop has always
// been exercised in. Starting smaller and growing on demand is a behaviour
// change in the geometry path f713297 fixed, and it buys nothing today; it
// belongs with M5's resizable inspector, which needs growth for real.
constexpr std::uint32_t kWindowH = 780;
constexpr std::uint32_t kAvatarH = 260;   // avatar band at the top
constexpr float kFontPx = 15.0f;
constexpr int kCornerMargin = 16;
// M1.5: the loading screen leaves across this many seconds. Long enough to
// read as the widget settling, short enough that it is not a dissolve you wait
// through.
//
// M7.2 narrowed what this covers. It used to be a crossfade — the loader out
// as the avatar came in — and the avatar's half of it is gone: the band is now
// summoned by AvatarAppearance once the loader has no opacity left, because
// the entrance clip has to be seen and it was not. The scrim, the cubes and
// the caption still leave on this clock, and the panel is still released on
// its first frame.
constexpr float kHandoffSeconds = 0.42f;

// Hermite ramp from 0 at `a` to 1 at `b`. Both ends of the handoff need to
// start and stop without an edge, and the two fades run over different
// sub-ranges of it, so a bare t*t*(3-2t) is not enough.
float smoothstep(float a, float b, float x) {
    const float s = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return s * s * (3.0f - 2.0f * s);
}

// The bottom margin actually used, which is kCornerMargin on an activated
// Windows and more when the widget is dodging the activation watermark (M1d.1,
// aii::corner_bottom_margin). It is settled once at startup — it needs the
// HWND for the window's DPI and the override for the mode — and read from
// there on, because placeInCorner is also called from the deferred resize path
// and both callers have to agree on where the bottom edge is.
int gBottomMargin = kCornerMargin;

// Places the window in the bottom-right of the primary monitor's work area.
// The anchor is the bottom edge, so a height change grows or shrinks the
// window upward and the corner it sits in never moves.
// The size goes through the target first and the position through Win32. A
// raw SetWindowPos alone grows the window rect but not the *client* rect:
// SDL answers WM_NCCALCSIZE for a borderless, non-resizable window with the
// size it knows, and DWM composites the client area — which is why the window
// could shrink but never grow past the size it was created at. `target` may
// be null only where there is no window to place.
void placeInCorner(platform::PresentationTarget* target, HWND hwnd, int width, int height) {
    if (target) target->setSize({static_cast<std::uint32_t>(width),
                                 static_cast<std::uint32_t>(height)});
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(hwnd, HWND_TOPMOST, work.right - width - kCornerMargin,
                 work.bottom - height - gBottomMargin, width, height, SWP_NOACTIVATE);
}

// Keeps the window above every other one and off the taskbar, then places it.
void pinToCorner(platform::PresentationTarget* target, HWND hwnd, int width, int height) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
    placeInCorner(target, hwnd, width, height);
}

std::string utf8FromWide(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

// M1c.5. The picked body colour crosses three boundaries in three shapes: hex
// on the command line and in settings.json (because that is what a person
// reads and what an art tool puts on the clipboard), packed RGBA inside the
// avatar (because that is what a cell is), and three floats in the panel
// (because that is what ImGui's colour widgets work in). These are the only
// two conversions, so there is one place that can be wrong about any of it.
//
// A colour that does not parse yields 0 and the caller leaves the setting
// alone, which is the same contract every other value in settings.json has:
// a bad hand edit costs that key, never the file.
bool colourFromHex(const std::string& text, std::uint32_t& out) {
    std::string s = text;
    if (!s.empty() && s.front() == '#') s.erase(s.begin());
    if (s.size() != 6) return false;
    std::uint32_t v[6]{};
    for (std::size_t i = 0; i < 6; ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') v[i] = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v[i] = static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v[i] = static_cast<std::uint32_t>(c - 'A' + 10);
        else return false;
    }
    out = aii::avatar_rgba(static_cast<std::uint8_t>(v[0] * 16 + v[1]),
                           static_cast<std::uint8_t>(v[2] * 16 + v[3]),
                           static_cast<std::uint8_t>(v[4] * 16 + v[5]), 255);
    return true;
}

std::string colourToHex(std::uint32_t c) {
    static const char* kDigits = "0123456789abcdef";
    const std::uint8_t ch[3] = {aii::avatar_r(c), aii::avatar_g(c), aii::avatar_b(c)};
    std::string s = "#";
    for (const std::uint8_t b : ch) {
        s += kDigits[b >> 4];
        s += kDigits[b & 0xF];
    }
    return s;
}

std::uint32_t colourFromFloats(const float* f) {
    auto q = [](float v) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return aii::avatar_rgba(q(f[0]), q(f[1]), q(f[2]), 255);
}

void colourToFloats(std::uint32_t c, float* f) {
    f[0] = static_cast<float>(aii::avatar_r(c)) / 255.0f;
    f[1] = static_cast<float>(aii::avatar_g(c)) / 255.0f;
    f[2] = static_cast<float>(aii::avatar_b(c)) / 255.0f;
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    SetConsoleOutputCP(CP_UTF8);
    bool opaque = false;
    bool vulkan = false;
    bool voiceEnabled = true;
    double seconds = -1.0;
    // M2b.5. Repeatable, like `voiceloop --say`, and for the reason HANDOFF
    // already gives for that one: anything that is *per session* is otherwise
    // unscriptable. A pending list only exists on the second turn, so a harness
    // that can take one turn cannot see this feature at all. Each is sent as a
    // fresh user turn once the session is back to Idle, so the turns are
    // consecutive in one conversation and schedules created by the first one
    // are live, and firing, while the second is composed.
    std::vector<std::string> sayTexts;
    // M2c.2. `--say-on-report TEXT`: send TEXT as a user turn the first frame a
    // finished worker report is waiting to be spoken. See the frame loop.
    std::string sayOnReport;
    std::string avatarName = "default";
    // M1c.4. Empty means "whatever the settings file says", which in turn
    // falls back to the definition's own default_theme. A name given here
    // outranks the stored one and is *not* written back: --theme is for
    // looking at a palette (the candidate captures were taken with it), and a
    // look should not change what the user comes back to.
    std::string themeName;
    // M1c.5. `--colour #rrggbb` picks the derived theme and the body colour it
    // is derived from, without touching what the user has stored — the same
    // contract --theme has, and for the same reason: this is how an awkward
    // pick gets captured for review without resetting the colour they chose.
    std::string colourArg;
    bool avatarFromArgs = false;
    bool themeFromArgs = false;
    std::string clipName;
    std::vector<std::string> spriteNames;
    std::string buttonsFile;
    // M2.5's escape hatch, in the spirit of --buttons.
    std::string busIn, busOut;
    bool busText = false;
    // M2.6.
    std::vector<std::string> scriptArgs;
    bool scriptsEnabled = true;
    // M2b.1.
    std::vector<std::string> scheduleArgs;
    std::vector<std::string> schedulePhrasedArgs;
    std::vector<std::string> scheduleWorkerArgs;
    bool micLatch = false;
    double micHoldSeconds = 0.0;  // M1f.1 harness: --mic-hold S
    std::chrono::steady_clock::time_point micHoldBegan{};
    // M1f.2's harness, and the pose flags the auto-listen control needs.
    //
    // `--settings` / `--settings-timing` exist for the reason --message does,
    // stated in its own comment above: a state that cannot be posed is a state
    // nobody looks at, and the settings surface scrolls, so the Timing section
    // is below the fold and could not be screenshotted at all.
    //
    // `--listen-timeout-at S:V` writes V seconds into the *panel's* fields at
    // S seconds in, which is exactly what a hand on the control writes and
    // nothing more — the same two fields, pushed down and persisted by the
    // same frame-loop lines. It is the only way to test the claim that matters
    // here: that changing the value reaches a microphone that is already
    // latched. `V` of 0 is never, matching the file and the mechanism.
    bool settingsOpen = false;
    bool settingsScrollTiming = false;
    // M3.8's pose flag, and the copy of the tool policy the child was launched
    // with. `toolsInForce` is written once, beside the tick boxes it is
    // compared against, and never again — it is what "in force" means.
    bool settingsScrollTools = false;
    bool settingsScrollStartup = false;
    // M1f.5. The armed auto-listen latch and the copy of the setting this run
    // started under. `autoListenPending` is a one-shot: it is cleared by the
    // frame that latches, so nothing can re-latch a microphone the user has
    // since closed. Seeded from the panel state further down, once the file
    // has been read.
    bool autoListenPending = false;
    bool autoListenInForce = false;
    aii::ToolPolicy toolsInForce;
    // M3.11's pair of the same, for the model picker: the pose flag and the
    // `--model` argument the child was actually launched with ("" = no flag).
    bool settingsScrollModel = false;
    std::string modelInForce;
    // ---- M3.12: a model or tool change restarts the child, and when --------
    //
    // (user, 19 Sep 2026: "restart immediately, lose context".)
    //
    // The *doing* is VoiceSession::apply_llm_settings(); the deciding is here,
    // because only the frame loop has the clock, the panel and the turn state
    // in one place. Three rules, all of them visible in the log:
    //
    //   1. **Armed by a change to the control, not by a disagreement.** A run
    //      started with AII_MODEL naming a dated id comes up with the picker
    //      already differing from what is in force, permanently and through no
    //      gesture of the user's. Arming on the difference would restart the
    //      child one second after launch, on a choice nobody made. So what is
    //      watched is the panel moving.
    //   2. **It settles before it fires.** A hand running down the three tick
    //      boxes is three changes in half a second and must be one restart,
    //      not three: the timer restarts on every change and the restart goes
    //      when it has been still for `kLlmSettleSeconds`. Closing the
    //      settings surface fires it at once — the gesture is finished by
    //      definition — which is also what makes a change arriving from
    //      anywhere but the open surface prompt rather than delayed.
    //   3. **A reply in flight is finished first.** Reset interrupts one
    //      because interrupting is what Reset means. Nudging a tick box does
    //      not mean "cut this off half-spoken", and the wait costs seconds
    //      against a conversation that is about to end anyway. The section
    //      says it is waiting, so nothing looks stuck.
    //
    // `llmPanelModel`/`llmPanelTools` are the panel's values as of the last
    // frame that looked, and `llmSettleFrom` is negative when nothing is
    // armed.
    std::string llmPanelModel;
    aii::ToolPolicy llmPanelTools;
    double llmSettleFrom = -1.0;
    // How still the controls have to be. Long enough for a second tick box,
    // short enough that "immediately" is still the honest word for it.
    constexpr double kLlmSettleSeconds = 0.75;
    // The harness: `--model-at S:key` and `--tool-at S:key:0|1` write the
    // panel's own fields at S seconds in, which is exactly what a hand on the
    // control writes and nothing more. There is no other way to drive this
    // from outside — the picker is a combo inside a scrolling region — and the
    // claim under test is precisely that the child that comes up afterwards is
    // a different child running on the new flags.
    std::vector<std::pair<double, std::string>> modelAt;
    std::vector<std::pair<double, std::string>> toolAt;
    // --inspector: open the prompt inspector on the first frame, as if the
    // sidebar button had been clicked. Nothing else about it differs.
    bool inspectorOpen = false;
    // M9: `--workers` opens the worker strip on the first frame, and
    // `--watch-worker <name>` opens that worker's chat window the moment a
    // worker by that name appears in the pool. Same hatch as --inspector, for
    // the same stated reason: the strip is otherwise reachable only by a click
    // on a 48 px window whose position depends on the panel's height, and a
    // worker's window only by a second click on a column that does not exist
    // until a worker is running — three things to arrange where one screenshot
    // was wanted. Neither flag fakes anything: --workers sets the same latch
    // the button's callback sets, and --watch-worker sets the same latch the
    // icon's click sets, once the real worker is really there.
    bool workersOpen = false;
    std::vector<std::string> watchWorkers;
    std::vector<std::pair<double, int>> listenTimeoutAt;
    int cancelRaceReps = 0;
    // M2b.5. Seconds of Idle between one --say and the next. Two is enough to
    // read as a conversation; a longer one is how a harness arranges for a
    // schedule to come due *between* two turns, which is otherwise only
    // reachable by guessing at how long a reply will take.
    double sayWait = 2.0;
    int resetAfterSay = -1;   // --reset-after-say <n>; negative = never
    double resetAt = -1.0;    // --reset-at <seconds of uptime>; negative = never
    // The message field's own escape hatch, and the reason this bug survived
    // three rounds of testing. Everything the panel draws could be put on
    // screen from the command line except the one thing the user actually
    // looks at while typing: text *sitting in the field*. A harness could type
    // into it with real WM_CHAR and could watch the message that came out the
    // other end, but nothing could ask for "a screenshot of the field holding
    // this sentence" — so nobody ever took one, and the field rendered its
    // contents off the right-hand edge in every run without a single assertion
    // noticing. `--message` is the same hatch --clip, --sprite and --buttons
    // are, for the same stated reason: a state that cannot be posed cannot be
    // looked at.
    std::string messageArg;
    // Diagnostic only: one log line per interesting frame across an appearance,
    // naming every quantity that could put the band and the alpha on different
    // frames, plus the window and client rects DWM actually composites.
    bool traceBand = false;
    {
        // Wide command line so Japanese survives (argv is ANSI-mangled).
        int wargc = 0;
        wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        for (int i = 1; wargv && i < wargc; ++i) {
            const std::wstring a = wargv[i];
            if (a == L"--opaque") opaque = true;
            else if (a == L"--trace-band") traceBand = true;
            else if (a == L"--vulkan") vulkan = true;
            else if (a == L"--no-voice") voiceEnabled = false;
            else if (a == L"--seconds" && i + 1 < wargc) seconds = _wtof(wargv[++i]);
            else if (a == L"--say" && i + 1 < wargc) sayTexts.push_back(utf8FromWide(wargv[++i]));
            else if (a == L"--say-on-report" && i + 1 < wargc)
                sayOnReport = utf8FromWide(wargv[++i]);
            else if (a == L"--avatar" && i + 1 < wargc) {
                avatarName = utf8FromWide(wargv[++i]);
                avatarFromArgs = true;
            }
            else if (a == L"--theme" && i + 1 < wargc) {
                themeName = utf8FromWide(wargv[++i]);
                themeFromArgs = true;
            }
            else if ((a == L"--colour" || a == L"--color") && i + 1 < wargc) {
                colourArg = utf8FromWide(wargv[++i]);
                themeName = "custom";
                themeFromArgs = true;
            }
            else if (a == L"--clip" && i + 1 < wargc) clipName = utf8FromWide(wargv[++i]);
            else if (a == L"--sprite" && i + 1 < wargc)
                spriteNames.push_back(utf8FromWide(wargv[++i]));
            else if (a == L"--buttons" && i + 1 < wargc) buttonsFile = utf8FromWide(wargv[++i]);
            else if (a == L"--bus-in" && i + 1 < wargc) busIn = utf8FromWide(wargv[++i]);
            else if (a == L"--bus-out" && i + 1 < wargc) busOut = utf8FromWide(wargv[++i]);
            else if (a == L"--bus-text") busText = true;
            else if (a == L"--script" && i + 1 < wargc)
                scriptArgs.push_back(utf8FromWide(wargv[++i]));
            else if (a == L"--no-scripts") scriptsEnabled = false;
            // M2b.1. `--schedule S` (repeatable, `S:label` optional): create a
            // one-shot timer S seconds after startup. A primitive whose only
            // door is a live turn cannot be measured without spending the
            // subscription, which is exactly why --buttons and --bus-in exist.
            // It is also the demonstration that firing lands on the frame loop
            // of the *real* app and not only of a harness.
            else if (a == L"--schedule" && i + 1 < wargc)
                scheduleArgs.push_back(utf8FromWide(wargv[++i]));
            // M2b.4. `--schedule-report S:<what happened>`: the same, at the
            // Phrased grade. It exists for the same reason --schedule does —
            // the only other door to the phrased path is a live turn that then
            // has to run a real worker for minutes, which makes the one thing
            // here that is a race (taking the floor for an injected turn)
            // unrepeatable. Everything after the first colon is the text, so a
            // Windows path inside it is safe.
            else if (a == L"--schedule-report" && i + 1 < wargc)
                schedulePhrasedArgs.push_back(utf8FromWide(wargv[++i]));
            // M2b.4. `--schedule-worker S|name|cwd|task`: a deferred worker
            // without a live turn to create it. Pipe-separated because a cwd
            // has a colon in it on this platform. The case it exists for is
            // the one that cannot be arranged any other way — a deferred
            // worker whose spawn *fails*, ten minutes after the user was
            // promised it, which is the failure this milestone had to decide
            // the wording for.
            else if (a == L"--schedule-worker" && i + 1 < wargc)
                scheduleWorkerArgs.push_back(utf8FromWide(wargv[++i]));
            // M2b.4. Latch the microphone on as soon as the session is idle,
            // as a short Talk click does. The harness cannot hold a button,
            // and "a schedule fires with the microphone open" is one of the
            // two interleavings this task had to get right.
            else if (a == L"--mic-latch") micLatch = true;
            // M1f.1's harness. The other half of the Talk gesture, which
            // --mic-latch could not reach: hold the microphone for S seconds
            // and then release it as a dictation. It exists because "hold-to-
            // dictate never times out" is a claim about a gesture nothing
            // could previously drive, and a claim of that shape is worth
            // exactly as much as the test that can fail it.
            else if (a == L"--mic-hold" && i + 1 < wargc) micHoldSeconds = _wtof(wargv[++i]);
            // M1f.2's pose and drive flags; see the declarations above.
            // M5.2's harness. The inspector is otherwise reachable only by a
            // click on a 48 px strip whose position depends on the panel, and
            // "find the button, then look at the window" is two tests where
            // one was wanted. Same shape as --settings above: it poses the UI,
            // it does not fake anything inside it.
            else if (a == L"--inspector") inspectorOpen = true;
            else if (a == L"--workers") workersOpen = true;
            else if (a == L"--watch-worker" && i + 1 < wargc) {
                workersOpen = true;  // the strip is where the window is opened from
                watchWorkers.push_back(utf8FromWide(wargv[++i]));
            }
            else if (a == L"--settings") settingsOpen = true;
            else if (a == L"--settings-timing") { settingsOpen = true; settingsScrollTiming = true; }
            else if (a == L"--settings-tools") { settingsOpen = true; settingsScrollTools = true; }
            // M1f.5's, same reason again: Startup is below the fold too.
            else if (a == L"--settings-startup") { settingsOpen = true; settingsScrollStartup = true; }
            else if (a == L"--settings-model") { settingsOpen = true; settingsScrollModel = true; }
            else if (a == L"--listen-timeout-at" && i + 1 < wargc) {
                const std::wstring spec = wargv[++i];
                const size_t colon = spec.find(L':');
                if (colon != std::wstring::npos)
                    listenTimeoutAt.emplace_back(_wtof(spec.substr(0, colon).c_str()),
                                                 _wtoi(spec.substr(colon + 1).c_str()));
            }
            // M2b.5. `--cancel-race <reps>`: drive the cancel verb's own path
            // against the tick, from another thread, with the cancel jittered
            // across the frame the schedule is due on. This project has twice
            // declared a race fixed on one sample and been wrong twice, so the
            // thing that has to be repeatable is exactly the thing a live turn
            // cannot repeat: a turn takes seconds and lands where it lands.
            // The thread stands in for the turn thread, calls the same
            // request_cancel() run_commands() calls, and the log is the
            // evidence — each id must appear as fired or as cancelled, and
            // never as both or as neither.
            else if (a == L"--cancel-race" && i + 1 < wargc)
                cancelRaceReps = _wtoi(wargv[++i]);
            else if (a == L"--say-wait" && i + 1 < wargc)
                sayWait = _wtof(wargv[++i]);
            // Reset's harness: press the transport row's reset button once,
            // after `n` of the `--say` turns have been sent. Keyed off the say
            // count rather than off a wall-clock offset because the whole
            // claim being tested is an *ordering* one — this turn was heard,
            // this one was not — and a turn that took a second longer than
            // expected would otherwise move the reset to the wrong side of it.
            //
            // It calls VoiceSession::reset() directly, which is what the
            // second press of the confirm reaches; the confirm itself is a
            // panel gesture and is verified by looking at the row.
            else if (a == L"--reset-after-say" && i + 1 < wargc)
                resetAfterSay = _wtoi(wargv[++i]);
            // The other question about reset, which the one above cannot ask:
            // what it does to a microphone that is already open. With
            // `startup.auto_listen` on that is the ordinary case, and it has no
            // relationship to any turn — the latch is up from the first frame
            // after the fade, before anything has been said. So this one is
            // keyed to the clock, like `--listen-timeout-at`, and is meant to
            // be set a few seconds after the engines are up.
            else if (a == L"--reset-at" && i + 1 < wargc)
                resetAt = _wtof(wargv[++i]);
            // M3.12's harness. `--model-at 20:sonnet` and
            // `--tool-at 20:web:0` move the panel's own control at 20 seconds
            // in; everything after that — the settle, the wait for a reply,
            // the restart, the file — is the ordinary path.
            else if (a == L"--model-at" && i + 1 < wargc) {
                const std::wstring spec = wargv[++i];
                const size_t colon = spec.find(L':');
                if (colon != std::wstring::npos)
                    modelAt.emplace_back(_wtof(spec.substr(0, colon).c_str()),
                                         utf8FromWide(spec.substr(colon + 1).c_str()));
            }
            else if (a == L"--tool-at" && i + 1 < wargc) {
                const std::wstring spec = wargv[++i];
                const size_t colon = spec.find(L':');
                if (colon != std::wstring::npos)
                    toolAt.emplace_back(_wtof(spec.substr(0, colon).c_str()),
                                        utf8FromWide(spec.substr(colon + 1).c_str()));
            }
            else if (a == L"--message" && i + 1 < wargc)
                messageArg = utf8FromWide(wargv[++i]);
        }
        if (wargv) LocalFree(wargv);
    }
    const bool transparent = !opaque;
    const gpu::Api api = vulkan ? gpu::Api::Vulkan : gpu::Api::D3D12;

    // --buttons applies a file of `aii` command lines at startup, exactly as if
    // the assistant had ended a reply with them. The toolbar registry's
    // validation and caps (M1c.2) are the paths that most need exercising and
    // the ones a live turn can least be made to produce on demand, so they get
    // the same kind of escape hatch AII_SETTINGS_FILE and AII_AVATAR_DIR give
    // the settings and avatar loaders. It is also the shape M2.5's bus will
    // use: one block of text, the same single entry point, no second channel.
    if (!buttonsFile.empty()) {
        std::ifstream f(buttonsFile, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        aii::parse_commands("```aii\n" + body + "\n```");
    }

    // M1b.5: the panel's user state comes back the way it was left, and M1d.1
    // reads its override from the same file. Loaded here, before the window is
    // created, because the watermark dodge decides where the window is *first*
    // placed — settling it afterwards would put the widget on screen in one
    // place and move it in the next frame. The panel's own fields are read
    // further down, where they are used; this is only the file.
    aii::Settings settings;
    settings.load(aii::settings_file_path());
    if (settings.take_status_change()) log::warn("{}", settings.status());

    // M8.3. Read here rather than with the rest of the panel's state further
    // down, because this one is not a preference about how the widget looks —
    // it decides which engines are built, and the session below starts
    // building them the moment it is constructed. Read a frame later and
    // VOICEVOX would already be loading, which is the second of startup this
    // setting exists to save.
    //
    // The pair is read through one spec string rather than two booleans so
    // that the invariant lives in one place: `language_selection_from_spec`
    // repairs a hand-edited file that switched both off, the same way it
    // repairs an `AII_LANGS` that names nothing.
    aii::Config voiceCfg = aii::Config::from_env();
    voiceCfg.langs = aii::language_selection_from_spec(
        settings.get_string("language", "enabled", aii::language_spec(voiceCfg.langs)));
    // M1f.2, taking the seam M1f.1 left here. The auto-listen timeout arrives
    // exactly the way the language selection above does, and the precedence is
    // the same as that line's: **settings.json wins over AII_LISTEN_TIMEOUT**,
    // which is the default used when the key is absent — i.e. on the first run
    // and after the user deletes the file. That is the right way round because
    // the file is what the control writes: an environment variable that beat
    // it would make the control appear to do nothing for whoever set one, and
    // a setting that cannot be seen to change is the worst failure this
    // feature has. The variable keeps its whole job of choosing the starting
    // value, and it is still the only way to ask for a timeout below the
    // control's floor (the harness's five seconds).
    //
    // Read here rather than with the panel's state further down for a weaker
    // reason than the language line's — the session does not build anything
    // out of it — but the same one: the value is part of `Config`, `Config` is
    // what the session is constructed from, and the constructor pushes it
    // straight into the mechanism. Read later and the first seconds of a run
    // would use a number the file disagrees with.
    const float defaultListenTimeout = voiceCfg.listen_timeout;
    voiceCfg.listen_timeout =
        settings.get_float("timing", "listen_timeout", voiceCfg.listen_timeout);
    // M3.8. The Tools toggles, read here for the strongest version of the
    // language line's reason: this one *is* built into the session. The list
    // becomes `--tools`/`--allowedTools` on the `claude` child's command line
    // in build_llm, so it has to be settled before the session is constructed
    // — there is no later.
    //
    // One key per group, defaulting to the group's own default, so a file that
    // has never seen this section and a file with one key hand-deleted both
    // land on the same place, and a key a future version adds is a non-event.
    for (int i = 0; i < aii::kToolGroupCount; ++i) {
        const aii::ToolGroup& g = aii::tool_group(i);
        voiceCfg.tools.on[i] = settings.get_bool("tools", g.key, voiceCfg.tools.on[i]);
    }
    // M3.11. The base model, read here for the same reason and in the same
    // breath: `--model` is on the same command line and is settled at the same
    // moment. The stored value is a *key* from the table in
    // `core/model_choice.h`, not a model string, and this is the one place
    // that translation happens.
    //
    // Two things this must never do. It must never put a string the CLI does
    // not accept on the command line — a stale or mistyped key would start a
    // child that fails every turn, with nothing on screen to say why — so an
    // unknown key falls back to the CLI's own default and is logged as having
    // done so. And it must not throw away an `AII_MODEL` naming something the
    // picker cannot produce (a dated id, pinned by hand for a test): the
    // environment override wins for that run, and the settings surface says
    // which model is actually in force rather than showing the picker's row
    // as though it were.
    if (voiceCfg.model_override.empty()) {
        const std::string key = settings.get_string("model", "name",
                                                    aii::model_choice(aii::kModelChoiceDefault).key);
        const int idx = aii::model_choice_for_key(key);
        if (idx < 0)
            log::warn("[model] settings.json names `{}`, which this build does not know - "
                      "falling back to the CLI's default", key);
        voiceCfg.model_override = aii::model_choice(idx < 0 ? aii::kModelChoiceDefault : idx).arg;
    } else {
        log::info("[model] AII_MODEL is set, so settings.json is not applied this run");
    }
    log::info("[model] conversational instance: {} ({})",
              aii::model_label(voiceCfg.model_override),
              voiceCfg.model_override.empty() ? std::string("no --model flag")
                                              : "--model " + voiceCfg.model_override);
    log::info("[tools] conversational instance: {} ({})",
              aii::tool_summary(voiceCfg.tools),
              aii::tool_list(voiceCfg.tools).empty() ? std::string("--tools \"\"")
                                                     : aii::tool_list(voiceCfg.tools));

    // The voice loop loads its engines in the background while the window comes up.
    std::unique_ptr<aii::VoiceSession> session;
    if (voiceEnabled) session = std::make_unique<aii::VoiceSession>(voiceCfg);

    auto backendResult = platform::createBackend(platform::BackendKind::SDL3);
    if (!backendResult) {
        log::error("backend: {}", backendResult.error().message);
        return 1;
    }
    auto backend = std::move(backendResult).value();
    if (auto r = backend->initialize(); !r) {
        log::error("backend init: {}", r.error().message);
        return 1;
    }

    auto instanceResult = gpu::Instance::create(api, {
        .appName = "AIInterface avatar",
        .enableValidation = false,
        .extraExtensions = backend->requiredVulkanInstanceExtensions(),
    });
    if (!instanceResult) {
        log::error("instance: {}", instanceResult.error().message);
        return 1;
    }
    auto instance = std::move(instanceResult).value();

    auto features = gpu::FeatureSet::gpuDriven();
    features.requiredExtensions.push_back(gpu::kSwapchainExtension);
    auto deviceResult = gpu::Device::create(*instance, features);
    if (!deviceResult) {
        log::error("device: {}", deviceResult.error().message);
        return 1;
    }
    auto device = std::move(deviceResult).value();
    log::info("Adapter: {} ({})", device->adapterName(), gpu::apiName(api));

    auto targetResult = backend->createTarget({
        .style = transparent ? platform::WindowStyle::BorderlessTransparent
                             : platform::WindowStyle::Decorated,
        .size = {kWindowW, kWindowH},
        .title = "AIInterface",
        .vulkan = api == gpu::Api::Vulkan,
    });
    if (!targetResult) {
        log::error("window: {}", targetResult.error().message);
        return 1;
    }
    auto target = std::move(targetResult).value();

    HWND hwnd = static_cast<HWND>(backend->nativeWindowHandle(*target));
    if (hwnd && transparent) {
        // M1d.1: the activation watermark is composited by DWM over topmost
        // windows, so the only way past it is to not be under it. The mode
        // comes from the settings file (so M1c.3's surface can expose it
        // later) with AII_DODGE_WATERMARK overruling it, which is the order
        // every other override in this app uses: the file is the preference,
        // the environment is this run.
        const auto stored = static_cast<aii::WatermarkDodge>(
            settings.get_enum("window", "dodge_watermark", aii::kWatermarkDodgeNames,
                              aii::kWatermarkDodgeCount,
                              static_cast<int>(aii::WatermarkDodge::Auto)));
        const aii::WatermarkDodge dodge =
            aii::parse_watermark_dodge(aii::env_or("AII_DODGE_WATERMARK", ""), stored);
        gBottomMargin = aii::corner_bottom_margin(hwnd, kCornerMargin, dodge);
        // Logged, never shown: the console says what the placement did, and
        // the window itself says nothing about activation anywhere.
        log::info("corner bottom margin {} ({} dodge)", gBottomMargin,
                  aii::kWatermarkDodgeNames[static_cast<int>(dodge)]);
        pinToCorner(target.get(), hwnd, kWindowW, kWindowH);
    }

    void* nativeSurface = nullptr;
    if (api == gpu::Api::Vulkan) {
        auto s = backend->createVulkanSurface(static_cast<VkInstance>(instance->nativeHandle()),
                                              *target);
        if (!s) {
            log::error("surface: {}", s.error().message);
            return 1;
        }
        nativeSurface = s.value();
    } else {
        nativeSurface = hwnd;
        if (!nativeSurface) {
            log::error("no HWND for the D3D12 swapchain");
            return 1;
        }
    }

    const auto extent = target->sizeInPixels();
    auto swapchainResult = gpu::Swapchain::create(*instance, *device, {
        .nativeSurface = nativeSurface,
        .width = extent.width,
        .height = extent.height,
        .transparent = transparent,
        .vsync = true,
    });
    if (!swapchainResult) {
        log::error("swapchain: {}", swapchainResult.error().message);
        return 1;
    }
    auto swapchain = std::move(swapchainResult).value();

    auto rendererResult = gpu::FrameRenderer::create(*device, *swapchain);
    if (!rendererResult) {
        log::error("frame renderer: {}", rendererResult.error().message);
        return 1;
    }
    auto renderer = std::move(rendererResult).value();
    // Premultiplied (0,0,0,0): the desktop shows through wherever nothing is drawn.
    renderer->setClearColor(0.0f, 0.0f, 0.0f, transparent ? 0.0f : 1.0f);

    // ---- the UI ----
    // D3D12 only. Under --vulkan the window still runs, just without a panel;
    // that path exists to compare backends, and transparency is already gone
    // there anyway.
    std::string uiError;
    auto ui = aii::ImGuiLayer::create(*device, swapchain->imageFormat(), kFontPx, &uiError);
    if (!ui) log::warn("no UI this run: {}", uiError);

    // B1: the engine's event set has no character event, so typing arrives
    // through an HWND subclass instead. It owns every key ImGui sees; the
    // engine's own pump keeps delivering the app's hotkeys below, and every
    // message is passed on so nothing downstream loses anything.
    std::unique_ptr<aii::WinTextInput> textInput;
    if (ui && hwnd) {
        textInput = aii::WinTextInput::install(hwnd);
        if (!textInput) log::warn("no keyboard this run: the window could not be subclassed");
    }

    // ---- GPU resources ----
    // One table for both passes here: the avatar's grid buffer lives at user
    // storage binding 0, and the loader shares the table only because the
    // D3D12 backend keeps the root signature in it.
    auto tableResult = gpu::DescriptorTable::create(
        *device, gpu::DescriptorTableDesc{.maxTextures = 1, .userStorageBuffers = 1});
    if (!tableResult) {
        log::error("descriptor table: {}", tableResult.error().message);
        return 1;
    }
    auto table = std::move(tableResult).value();

    const auto shaderDir = executableDirectory() / "data" / "shaders";
    auto loadShader = [&](const char* file) -> std::unique_ptr<gpu::Shader> {
        auto r = gpu::Shader::createFromFile(*device, shaderDir / file);
        if (!r) {
            log::error("shader {}: {}", file, r.error().message);
            return nullptr;
        }
        return std::move(r).value();
    };

    std::string avatarError;
    auto avatarRenderer = aii::AvatarRenderer::create(*device, swapchain->imageFormat(), *table, 0,
                                                      shaderDir, &avatarError);
    if (!avatarRenderer) {
        log::error("avatar renderer: {}", avatarError);
        return 1;
    }
    // The art is data (M2.2): a definition directory under %APPDATA%, seeded
    // from assets/avatars on first run and watched for edits while we run.
    // AII_AVATAR_DIR points the loader straight at a directory instead, which
    // is how the failure paths get tested without touching the user's copy.
    aii::AvatarSource avatarSource;
    const std::string avatarDirOverride = aii::env_or("AII_AVATAR_DIR", "");
    {
        // M1c.3/M1c.4: which avatar, and which of its themes, are stored by
        // name in the same settings file the panel's own state comes from.
        // Read here rather than with the panel's fields further down, because
        // this is where the definition is opened: reading them later would
        // load the default art and the default palette and then swap both on
        // the first frame, which on a window whose height follows its content
        // is a visible flash and a resize.
        if (!avatarFromArgs) avatarName = settings.get_string("avatar", "name", avatarName);
        if (!themeFromArgs) themeName = settings.get_string("avatar", "theme", themeName);
        // M1c.5. The picked colour, read here for the same reason the theme is:
        // pushed in before open(), it is resolved by the load itself, so the
        // first frame is already the user's colour instead of the default one
        // flashing past. An unparseable value simply leaves the colour unset
        // and the derived theme starts from the theme it would have had.
        if (std::uint32_t picked = 0;
            colourFromHex(colourArg.empty() ? settings.get_string("avatar", "colour", "") : colourArg,
                          picked)) {
            avatarSource.set_custom_colour(picked);
        }
        const std::filesystem::path dir = avatarDirOverride.empty()
                                              ? aii::seed_avatar_definition(avatarName)
                                              : std::filesystem::path(avatarDirOverride);
        // Before open(), not after: with nothing loaded yet this only records
        // the name, and the load that follows resolves it — so the avatar's
        // very first frame is already in the user's colours.
        avatarSource.set_theme(themeName);
        avatarSource.open(dir, clipName, spriteNames);
    }
    aii::AvatarGrid grid;

    // M2.4: the policy that decides which clip and which accessory, from the
    // snapshot the frame loop already takes. It is off whenever the command
    // line pinned something, so --clip and --sprite stay a straight look at
    // the art.
    aii::AvatarTuning tuning;
    if (const std::string s = aii::env_or("AII_SLEEPY_SEC", ""); !s.empty()) {
        // Only so the sleepy path can be exercised without sitting through
        // two minutes of idle. Nothing else here is tunable from outside;
        // when M2.5's bus exists it is the place for this.
        tuning.sleepy_seconds = std::max(1.0f, static_cast<float>(atof(s.c_str())));
    }
    aii::AvatarController controller(tuning);
    const bool controllerOwnsAvatar = clipName.empty() && spriteNames.empty();
    if (controllerOwnsAvatar && avatarSource.loaded())
        controller.note_definition(avatarSource.definition());
    std::string lastClipLogged;

    // The loader's full-screen triangle. It shares the descriptor table only
    // because the D3D12 backend's root signature lives there; the shader
    // reads nothing from it, so the pass never binds it — ImGui has just set
    // its own descriptor heap on this command list and re-pointing the root
    // table into a heap that is no longer bound would be worse than leaving
    // a parameter the shader does not touch unset.
    auto loaderVs = loadShader("loader.vert.spv");
    auto loaderPs = loadShader("loader.frag.spv");
    if (!loaderVs || !loaderPs) return 1;

    gpu::GraphicsPipelineDesc loaderDesc{};
    loaderDesc.vertexShader = loaderVs.get();
    loaderDesc.fragmentShader = loaderPs.get();
    loaderDesc.colorFormat = swapchain->imageFormat();
    loaderDesc.pushConstantBytes = sizeof(aii::LoaderPush);
    loaderDesc.descriptorTable = table.get();
    // The window is premultiplied-transparent and the cubes are antialiased
    // by coverage, so their silhouette has to blend over the scrim and the
    // panel rather than overwrite them.
    loaderDesc.alphaBlend = true;
    auto loaderPipelineResult = gpu::Pipeline::createGraphics(*device, loaderDesc);
    if (!loaderPipelineResult) {
        log::error("loader pipeline: {}", loaderPipelineResult.error().message);
        return 1;
    }
    auto loaderPipeline = std::move(loaderPipelineResult).value();

    std::uint32_t width = extent.width;
    std::uint32_t height = extent.height;
    // The loader's frame state. Both recorders below read it: the loading
    // screen covers the whole window, so it also decides whether the avatar
    // placeholder draws at all. During the M1.5 handoff the two overlap —
    // `loading` stays true while the loader still has any opacity left, and
    // `avatarAlpha` rises from 0 over the same frames.
    bool loading = false;
    float avatarAlpha = 0.0f;
    aii::LoaderPush loaderPush{};

    renderer->setFramePasses({
        gpu::FramePass{
            .point = gpu::PassPoint::InScene,
            .name = "avatar-grid",
            .record =
                [&](gpu::CommandContext& cmd, const gpu::PassContext& ctx) {
                    // Withheld outright until the handoff starts: while the
                    // loading screen owns the window, the avatar sitting
                    // behind the scrim is just noise. From the first frame of
                    // the fade it draws at a rising alpha instead (the fade
                    // in this slot's header). The frame renderer re-records
                    // this pass every frame, so skipping the record is enough.
                    if (avatarAlpha <= 0.0f) return;
                    avatarRenderer->record(cmd, ctx.slot, ctx.width, kAvatarH);
                },
        },
    });

    // The panel rides the engine's overlay pass: its own command buffer after
    // the scene, inside a rendering pass on the swapchain image. The loading
    // cubes are recorded after the panel, over the whole window with no
    // scissor — that ordering is the whole reason they live here and not in
    // the InScene pass, which runs before the overlay and is clipped to the
    // avatar band.
    if (ui) {
        renderer->setOverlayRecorder([&](gpu::CommandContext& cmd) {
            ui->end_frame(cmd);
            if (!loading) return;
            cmd.setViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
            cmd.setScissor(0, 0, width, height);
            cmd.bindPipeline(*loaderPipeline);
            cmd.pushConstants(*loaderPipeline, &loaderPush, sizeof(loaderPush));
            cmd.draw(3, 1);
        });
    }

    aii::AvatarUiState uiState;

    // M1b.5: the panel's user state comes back the way it was left. Read here,
    // before the loop and so before the first frame is built, because reading
    // it any later would draw the window in its default state and correct it
    // afterwards — a visible flip on every start, and on a window whose height
    // follows the panel, a resize with it. The file itself was loaded before
    // the window (M1d.1 needs it to place the window at all); a load failure
    // was already reported there, and every key here answers with its default.
    uiState.chat_open = settings.get_bool("panel", "chat_open", uiState.chat_open);
    uiState.muted = settings.get_bool("panel", "muted", uiState.muted);
    uiState.avatar_mode = static_cast<aii::AvatarVisibility>(
        settings.get_enum("panel", "avatar_mode", aii::kAvatarVisibilityNames,
                          aii::kAvatarVisibilityCount, static_cast<int>(uiState.avatar_mode)));
    // The pickers show what is actually in force, not what was asked for: the
    // definition has already been opened above, so a stored theme that the art
    // no longer declares shows as the fallback it really got. Seeded from the
    // source rather than from the settings file for exactly that reason.
    uiState.avatar_name = avatarName;
    uiState.theme = avatarSource.theme();
    // M8.3. The same selection the session was built from, so the checkboxes
    // show what is actually in force rather than re-reading the file and
    // possibly disagreeing with the engines that are already loading.
    uiState.lang_english = voiceCfg.langs.english;
    uiState.lang_japanese = voiceCfg.langs.japanese;
    // M1f.2. The same idea, and the same reason it is seeded from the value
    // the session was actually built with rather than re-read from the file:
    // the control has to show what is in force.
    //
    // Not clamped to the control's range. A five-second timeout from
    // AII_LISTEN_TIMEOUT is a real, running setting and the surface says so in
    // its own words (see listen_timeout_section); clamping it here would push
    // 15 back down into the session and the file on the next frame, and the
    // harness value the user asked for would be gone before the first frame
    // was drawn.
    //
    // The number shown while the box is unticked comes from the default rather
    // than from the stored 0, so a file that says "never" still offers
    // something sensible to turn back on. `std::max` covers the case where the
    // default is itself a "never" (AII_LISTEN_TIMEOUT=0 on a first run): there
    // is no number to show then, so the floor is the honest starting point.
    // M1f.5. The one setting in this block that is not a level: it is read
    // here, armed once below, and never pushed anywhere. The default is
    // `kAutoListenDefault` (true) and lives in avatar_ui.h, so a fresh install
    // with no settings.json takes exactly the same path as a file that has no
    // `startup` section — which is the case the user asked about.
    uiState.auto_listen = settings.get_bool("startup", "auto_listen", uiState.auto_listen);
    uiState.listen_timeout_on = voiceCfg.listen_timeout > 0.0f;
    uiState.listen_timeout_sec =
        uiState.listen_timeout_on
            // At least one second when the box is on, because the panel holds
            // whole seconds and a configured 0.4 would round to a zero that
            // the rest of the feature spells "never" — a ticked box pushing
            // "never" down is precisely the setting-that-lies this task is
            // about.
            ? std::max(1, (int)std::lround(voiceCfg.listen_timeout))
            : std::max(aii::kListenTimeoutUserFloorSec, (int)std::lround(defaultListenTimeout));
    // M1f.2's pose flags. `settings_open` is deliberately not persisted (see
    // AvatarUiState), so this is the only way a scripted run can be looking at
    // the surface at all.
    if (settingsOpen) uiState.settings_open = true;
    uiState.settings_scroll_timing = settingsScrollTiming;
    uiState.settings_scroll_tools = settingsScrollTools;
    uiState.settings_scroll_startup = settingsScrollStartup;
    // M3.8. Same idea again, and here it is the whole feature: the tick boxes
    // are seeded from the value the session was actually built with, and
    // `toolsInForce` keeps a copy of it that nothing ever writes to. The
    // section draws the difference between the two, which is the only honest
    // thing it can do until a toggle can reach a running child (M3.6).
    uiState.tools = voiceCfg.tools;
    toolsInForce = voiceCfg.tools;
    // M1f.5. Armed here, from the panel state, and read by exactly one place in
    // the frame loop. `autoListenInForce` is the copy the settings surface
    // compares the live tick box against; nothing writes it again.
    //
    // `--no-voice` is not a special case in the arming: there is no session, so
    // the condition below can never be true and the flag simply never fires.
    // The surface is told separately, because a tick box that silently did
    // nothing all run is the thing this project keeps refusing to ship.
    autoListenInForce = uiState.auto_listen;
    autoListenPending = uiState.auto_listen;
    log::info("[auto-listen] start listening: {}", uiState.auto_listen ? "on" : "off");
    // M3.11. The same shape for the model. `model_choice_for_arg` is -1 when
    // AII_MODEL named something off the list, and the picker then shows the
    // default row while `modelInForce` holds the real value — which is exactly
    // the disagreement the section's amber line exists to name.
    uiState.settings_scroll_model = settingsScrollModel;
    uiState.model = std::max(0, aii::model_choice_for_arg(voiceCfg.model_override));
    modelInForce = voiceCfg.model_override;
    // M3.12. The panel as it stands before anyone has touched it. Seeded from
    // the panel rather than from `voiceCfg`, because the case that matters is
    // exactly the one where they differ (AII_MODEL naming something off the
    // table): nothing was changed, so nothing is armed, and the first restart
    // this run does has to be one the user asked for.
    llmPanelModel = aii::model_choice(uiState.model).arg;
    llmPanelTools = uiState.tools;
    log::info("[listen-timeout] setting: {} ({:.1f} s configured, default {:.1f} s)",
              uiState.listen_timeout_on ? std::to_string(uiState.listen_timeout_sec) + " s"
                                        : std::string("never"),
              voiceCfg.listen_timeout, defaultListenTimeout);
    // --message: put text in the field before the first frame, exactly as if it
    // had been typed. It goes through the same buffer a keystroke lands in, so
    // what is captured is the field doing its own job and not a special case.
    if (!messageArg.empty())
        std::snprintf(uiState.message, sizeof(uiState.message), "%s", messageArg.c_str());
    // M1c.5. The picker's own value, and the last value this side pushed into
    // it. The pair is what keeps a drag one-way: while the user is moving the
    // control the panel is the authority and the source follows, and on every
    // other frame the source is the authority and the panel follows. Mirroring
    // unconditionally would write the 8-bit round-trip of the colour back into
    // the picker sixty times a second, which is how a colour picker loses its
    // hue the moment the user drags the saturation to zero.
    std::uint32_t uiColour = avatarSource.custom_colour();
    colourToFloats(uiColour, uiState.custom_colour);
    // The avatar picker's list. Re-read whenever the surface is opened rather
    // than every frame — it is a directory scan, and the answer only changes
    // when the user puts a folder somewhere, which they cannot do while
    // looking at this window.
    std::vector<std::string> avatarNames = aii::avatar_definition_names();
    bool settingsWasOpen = false;

    // ---- M2.5: the app bus ----
    // Installed here, after uiState exists, because the inbound families write
    // their wishes into it: `avatar.load` and `theme.set` go through the same
    // fields the settings pickers write, so a scripted change and a clicked one
    // are one code path, persist alike, and correct themselves alike when the
    // art does not have what was asked for.
    aii::BusBindings bus;
    {
        aii::BusBindings::Context bc;
        bc.source = &avatarSource;
        bc.controller = &controller;
        bc.ui = &uiState;
        bc.avatar_pinned = !controllerOwnsAvatar;
        bc.dir_override = !avatarDirOverride.empty();
        bc.publish_text = busText;
        // M2b.2. The schedule family cancels and lists through the session,
        // because a schedule that has already fired is a running worker and
        // only the session knows about those. Null on a --no-voice run, which
        // the handler answers from the book alone.
        bc.session = session.get();
        bus.install(bc);
    }
    aii::BusFileHatch busFiles;
    if (std::string err; !busFiles.open(busIn, busOut, &err)) log::warn("[bus] {}", err);
    if (!busIn.empty()) log::info("[bus] tailing {}", busIn);
    if (!busOut.empty()) log::info("[bus] publishing to {}", busOut);

    // ---- M2.6: Python, only if there is a script ----
    // After the families are registered, because a message posted from a
    // script before its family exists would be applied against nobody. Before
    // the loop, because the interesting half of scripting is the loading
    // sequence — a script sees session.state go Loading → Idle like anything
    // else on the bus.
    //
    // **The trigger is a script to run, and nothing else.** No script, no
    // `LoadLibrary`, no `python313.dll`, no interpreter, and no stage added to
    // the loading screen. See script_host.h for what "a script to run" means
    // and why the shipped example does not count as one.
    aii::ScriptHost scripts;
    bool scripting = false;
    if (scriptsEnabled) {
        const std::vector<std::string> found = aii::ScriptHost::discover(scriptArgs);
        if (!found.empty()) {
            for (const std::string& s : found) log::info("[py] script {}", s);
            scripting = scripts.start(aii::AppBus::instance(), found);
            if (!scripting) log::warn("[py] {}", scripts.status());
        }
    }
    // The same field the scripts' own `aii.status()` writes to: from the
    // user's side "my script isn't working" has one answer, not two places to
    // look for one.
    if (!scripts.status().empty()) bus.set_script_status(scripts.status(), scripts.status_ok());
    // Both of these drain the *same* outbound queue, so a run with a script
    // and a --bus-out file splits the event stream between them. That is fine
    // for what --bus-out is (a debugging hatch, and how M2.5 was tested with
    // no Python at all) and confusing for anything else, so it is said once
    // here rather than discovered from a capture with half the lines missing.
    if (!busOut.empty() && scripting)
        log::warn("[bus] --bus-out and a script share one event queue; each sees only "
                  "what the other has not drained");

    log::info("avatar live: {}x{} {} {}", extent.width, extent.height,
              transparent ? "transparent" : "opaque", gpu::apiName(api));

    const auto start = std::chrono::steady_clock::now();
    // M2b.1. The thread that owns delivery. Recorded rather than assumed: the
    // whole point of the primitive is that a schedule fires *here*, and a claim
    // nobody can check is a claim that quietly stops being true.
    const std::thread::id frameThread = std::this_thread::get_id();
    for (const std::string& spec : scheduleArgs) {
        const std::size_t colon = spec.find(':');
        const double secs = atof(spec.substr(0, colon).c_str());
        aii::ScheduleAction action;
        action.kind = "timer";
        action.label = colon == std::string::npos ? (spec + " second timer")
                                                  : spec.substr(colon + 1);
        action.report = "Your " + action.label + " is done.";
        // Captured now, never re-resolved at fire time: a deferred worker runs
        // with bypassed permissions in the directory it was promised, and the
        // user may be away from the desk when it fires.
        action.cwd = std::filesystem::current_path().string();
        std::string err;
        const std::uint64_t id = aii::ScheduleBook::instance().create(
            std::chrono::duration<double>(secs), action, aii::ReportGrade::Fixed, &err);
        if (id == 0)
            log::warn("[schedule] refused: {}", err);
        else
            log::info("[schedule] created id={} kind=timer in {:.3f}s grade=fixed label=\"{}\"", id,
                      secs, action.label);
    }
    for (const std::string& spec : schedulePhrasedArgs) {
        const std::size_t colon = spec.find(':');
        const double secs = atof(spec.substr(0, colon).c_str());
        aii::ScheduleAction action;
        action.kind = "timer";
        action.report = colon == std::string::npos ? std::string("that thing you asked about")
                                                   : spec.substr(colon + 1);
        action.label = action.report;
        action.cwd = std::filesystem::current_path().string();
        std::string err;
        const std::uint64_t id = aii::ScheduleBook::instance().create(
            std::chrono::duration<double>(secs), action, aii::ReportGrade::Phrased, &err);
        if (id == 0)
            log::warn("[schedule] refused: {}", err);
        else
            log::info("[schedule] created id={} kind=timer in {:.3f}s grade=phrased report=\"{}\"",
                      id, secs, action.report);
    }
    for (const std::string& spec : scheduleWorkerArgs) {
        std::vector<std::string> f;
        for (std::size_t b = 0;;) {
            const std::size_t p = spec.find('|', b);
            f.push_back(spec.substr(b, p == std::string::npos ? p : p - b));
            if (p == std::string::npos) break;
            b = p + 1;
        }
        if (f.size() < 4) {
            log::warn("[schedule] --schedule-worker wants S|name|cwd|task");
            continue;
        }
        aii::ScheduleAction action;
        action.kind = "worker";
        action.name = f[1];
        action.cwd = f[2];
        action.task = f[3];
        action.label = f[1];
        action.report = f[1];
        std::string err;
        const std::uint64_t id = aii::ScheduleBook::instance().create(
            std::chrono::duration<double>(atof(f[0].c_str())), action, aii::ReportGrade::Phrased,
            &err);
        if (id == 0)
            log::warn("[schedule] refused: {}", err);
        else
            log::info("[schedule] created id={} kind=worker in {}s grade=phrased name={} cwd=\"{}\"",
                      id, f[0], f[1], f[2]);
    }
    // M2b.5. The cancel-versus-fire race, driven from a thread that is not the
    // frame loop, which is the only property of the turn thread that matters
    // here. Each rep creates a timer a few frames out and asks for it to be
    // cancelled at a moment jittered across the frame it is due on, so the
    // sample lands on both sides of the tick and on the tick itself.
    //
    // It asserts nothing in-process on purpose: the evidence is the log, which
    // is what a reader can check afterwards and what the two previously
    // mis-declared races were missing. Every id must show up exactly once as
    // `fired` or exactly once as `cancelled`, never both and never neither.
    std::thread raceThread;
    std::atomic<bool> raceStop{false};
    if (cancelRaceReps > 0 && session) {
        raceThread = std::thread([&raceStop, reps = cancelRaceReps, s = session.get()] {
            std::mt19937 rng(20260917u);
            // Due ~4 frames out; the cancel lands anywhere from well before to
            // well after, so roughly a third of the reps are genuinely in the
            // window where the outcome is decided by which side of one tick
            // the two calls fall.
            std::uniform_int_distribution<int> jitter(15, 115);
            for (int i = 0; i < reps && !raceStop; ++i) {
                aii::ScheduleAction action;
                action.kind = "timer";
                action.label = "race " + std::to_string(i);
                // Run this with mute on (AII_SETTINGS_FILE with panel.muted):
                // a fired timer still reaches the transcript and the log, which
                // is where the evidence is, and nothing is synthesised, which
                // is what makes hundreds of reps take a minute instead of an
                // afternoon. That is mute's existing behaviour, not a special
                // case for the harness.
                action.report = action.label;
                action.cwd = std::filesystem::current_path().string();
                std::string err;
                const std::uint64_t id = aii::ScheduleBook::instance().create(
                    std::chrono::duration<double>(0.066), action, aii::ReportGrade::Fixed, &err);
                if (id == 0) {
                    log::warn("[race] refused: {}", err);
                    break;
                }
                log::info("[race] rep={} id={}", i, id);
                std::this_thread::sleep_for(std::chrono::milliseconds(jitter(rng)));
                s->request_cancel({id});
                std::this_thread::sleep_for(std::chrono::milliseconds(90));
            }
            log::info("[race] done");
        });
    }
    double lastT = 0.0;
    std::uint32_t windowH = kWindowH;  // what the OS window was last set to
    // Resizing the window from inside the frame does not come back as a
    // Resized event, so the swapchain has to be told separately — and only
    // between frames, never after the panel has been built for the old size.
    std::uint32_t pendingH = 0;
    // The avatar band the panel is being drawn with, and the one the policy
    // decided this frame. They are one frame apart on purpose: a new band is
    // only ever applied at the top of a frame, in the same breath as the window
    // height it needs, and before any of that frame's input is read.
    std::uint32_t band = 0;
    std::uint32_t nextBand = 0;
    // The panel's own height, i.e. what it last asked for less the band it was
    // drawn with. Adding a band to it is what the window has to become.
    std::uint32_t panelH = 0;
    std::uint64_t frameNo = 0;
    float lastTracedAlpha = -1.0f;
    std::uint32_t lastTracedBand = 9999;
    std::string lastTracedClip;
    int lastTracedWanted = -1;
    bool running = true;
    std::size_t nextSay = 0;
    // A settle before the next turn goes in: Idle is reached the moment the
    // audio drains, and a schedule that fired during the reply has not
    // necessarily been flushed yet. The gap is what makes the transcript read
    // as a conversation rather than as a burst.
    std::chrono::steady_clock::time_point lastSayDone{};
    // SPACE is the keyboard half of the Talk gesture, and carries both of its
    // meanings (M1b.3): tap it for the latch, hold it to dictate. SDL repeats
    // KeyDown while a key is held, so only the first one is the press — the
    // same edge guard that already stopped a leaned-on SPACE flapping the
    // latch is what makes auto-repeat harmless here.
    bool spaceDown = false;
    std::chrono::steady_clock::time_point spaceDownAt{};
    // Releases a SPACE gesture, wherever the release is noticed. `over` is
    // false only where the key-up itself went missing — the panel taking the
    // keyboard mid-hold — which is the same abandoned press as dragging off
    // the button, and ends the utterance in the field rather than sending it.
    auto releaseSpace = [&](bool over) {
        if (!spaceDown) return;
        spaceDown = false;
        if (!session) return;
        const float heldFor = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - spaceDownAt).count();
        session->talk_released(over, heldFor >= aii::kTalkHoldSeconds);
    };
    // M1.5 handoff: 0 while the engines are still loading, then driven to 1
    // over kHandoffSeconds. The last stage name and the bar are frozen at the
    // moment loading ended, so the caption does not blank out mid-fade — the
    // session clears them the instant it goes idle.
    float handoff = 0.0f;
    std::string loaderStage;
    float loaderProgress = 0.0f;
    // M7.2, replacing M1.6's visibility fade: the avatar's arrivals and
    // departures, from every path, in one object. It needs no "first frame"
    // special case — the loading screen is up on frame one and it will not
    // let the avatar appear underneath it.
    aii::AvatarAppearance appearance;
    // And the policy that feeds it: whether the user is engaged, which in
    // `when_talking` is now the whole of "is the avatar wanted" (user,
    // 19 Sep 2026). It holds the grace period and nothing else.
    aii::AvatarPresence presence;

    // ---- M4: the sidebar, a real second window ----
    //
    // Created here, last, because it is the piece that is allowed to fail: a
    // strip that could not be made is survivable (the registry hands its
    // buttons back to the panel's toolbar row) and nothing above it is.
    //
    // Only in the transparent D3D12 build, which is the only one that has a
    // widget to dock against: --opaque is a decorated debugging window and
    // --vulkan has no ImGui layer at all.
    //
    // The strip keeps no list of its own (M4.4): a button reaches it by being
    // registered with ButtonSurface::Sidebar, from anywhere, and neither the
    // strip nor the panel is edited when one is. M5's inspector and M6's
    // editor will do exactly that, with an Invoke action instead of a path.
    // Register such a button *outside* the `if` below, whether or not the
    // strip is ever made: it is a button, not a piece of the strip, and the
    // fallback surface draws it exactly as it draws the cog.
    // ---- M5.1: the prompt inspector, a real third window ----
    //
    // Its button is registered here, *before* the strip is made and outside the
    // `if` that makes it, for the two reasons the block above states: a button
    // is not a piece of the strip (without one the registry hands it to the
    // panel's toolbar row, which dispatches Invoke exactly as the strip does),
    // and the strip sizes itself at create() from the buttons it is going to
    // draw — registering afterwards would have it born one button short and
    // resize on its first frame.
    //
    // The click only *asks*. Creating or destroying a window inside the strip's
    // ImGui frame would tear down GPU objects and switch ImGui contexts with a
    // half-built frame on the stack, so the callback sets a flag and the frame
    // loop answers it between frames — which is also what makes a second click
    // close the window rather than open a second one.
    bool inspectorToggle = inspectorOpen;  // --inspector: the same latch the click sets
    std::unique_ptr<aii::InspectorWindow> inspector;
    // The remembered geometry, read once here and written back whenever it
    // changes. `placed` is a stored flag rather than a sentinel coordinate
    // because 0,0 is a real position and a multi-monitor desktop has real
    // negative ones; false simply means "never placed", which is first run.
    // Whether the rect still lands on a monitor is not decided here — see
    // fit_to_desktop() in inspector_window.cpp, which re-checks it against the
    // desktop as it is at the moment the window is made.
    aii::InspectorGeometry inspectorGeom;
    inspectorGeom.placed = settings.get_bool("inspector", "placed", false);
    inspectorGeom.x = static_cast<int>(settings.get_float("inspector", "x", 0.0f));
    inspectorGeom.y = static_cast<int>(settings.get_float("inspector", "y", 0.0f));
    inspectorGeom.w = static_cast<unsigned>(
        std::max(0.0f, settings.get_float("inspector", "w", static_cast<float>(inspectorGeom.w))));
    inspectorGeom.h = static_cast<unsigned>(
        std::max(0.0f, settings.get_float("inspector", "h", static_cast<float>(inspectorGeom.h))));
    // Every setter is a no-op when the value already matches, so this can be
    // called every frame and "on change" stays a decision the store makes.
    const auto rememberInspector = [&] {
        settings.set_bool("inspector", "placed", inspectorGeom.placed);
        settings.set_float("inspector", "x", static_cast<float>(inspectorGeom.x));
        settings.set_float("inspector", "y", static_cast<float>(inspectorGeom.y));
        settings.set_float("inspector", "w", static_cast<float>(inspectorGeom.w));
        settings.set_float("inspector", "h", static_cast<float>(inspectorGeom.h));
    };
    {
        aii::ButtonAction open;
        open.kind = aii::ButtonActionKind::Invoke;
        open.callback = [&inspectorToggle] { inspectorToggle = true; };
        std::string buttonError;
        if (!aii::ButtonRegistry::instance().add_app_button(
                "inspector", aii::ButtonGlyph::Prompts, "Prompt inspector",
                aii::ButtonSurface::Sidebar, std::move(open), &buttonError))
            log::warn("no inspector button: {}", buttonError);
    }

    // ---- M9: the workers button, the worker strip, and N worker windows ----
    //
    // Three layers, and each one only *asks* the layer below it:
    //
    //  1. A sidebar button, registered here exactly as the inspector's is and
    //     for the same two reasons — a button is not a piece of the strip (with
    //     no strip the registry hands it to the panel's toolbar row, which
    //     dispatches Invoke identically), and the strip sizes itself at
    //     create() from the buttons it is about to draw.
    //  2. The **worker strip**: a second 48 px column, left of the first, one
    //     icon per worker. Created and destroyed by the click, like the
    //     inspector, which is why a second click closes it.
    //  3. A **worker window** per watched worker, opened from an icon in that
    //     column and placed to the left of the widget.
    //
    // Every one of those creations and destructions happens in the frame loop
    // between windows, never inside anyone's ImGui frame: building GPU objects
    // and an ImGui context with a half-built frame on the stack is the one
    // mistake in this area that produces a bare access violation somewhere
    // unrelated.
    //
    // **Nothing here opens unbidden and nothing here is persisted.** Not the
    // strip's open state, not which windows were open, not where they were.
    // That is a standing instruction, and it is also the only honest default:
    // a worker window restored at startup would be a window about a worker that
    // no longer exists.
    bool workersToggle = workersOpen;  // --workers: the same latch the click sets
    std::unique_ptr<aii::WorkerStripWindow> workerStrip;
    // The rows the strip was sized and docked with at the top of *this* frame,
    // rebuilt from the session's snapshot at the bottom of it. One frame of lag
    // on a worker appearing, deliberately: the snapshot is taken well after the
    // geometry block, and drawing a row the window was not sized for clips it.
    std::vector<aii::WorkerStripRow> stripRows;

    // One window per worker, and the same window for a second click on the same
    // icon — that is what makes the icon a toggle rather than a window factory.
    // `slot` is the column it was given, kept across a live worker's whole life
    // so that a window closed and reopened comes back where it was.
    struct OpenWorker {
        std::string name;
        int slot = 0;
        std::unique_ptr<aii::WorkerWindow> window;
    };
    std::vector<OpenWorker> workerWindows;
    // The cap, and it is not a layout bound: each ImGui context carries its own
    // font atlas (Segoe UI plus ~3000 Japanese glyphs), so every one of these
    // windows is a real texture and a real descriptor heap. Four side by side
    // is already 1472 px of screen; past that they would have to overlap, which
    // is the arrangement this rule exists to avoid.
    constexpr int kWorkerWindowsMax = 4;
    constexpr int kWorkerWinW = 360;  // the widget's own width: a sibling, not a cousin
    constexpr int kWorkerWinH = 520;
    constexpr int kWorkerWinGap = 8;
    // Where the strips end and the worker windows begin, recomputed each frame
    // in the geometry block below.
    int dockEdge = 0;
    {
        aii::ButtonAction open;
        open.kind = aii::ButtonActionKind::Invoke;
        open.callback = [&workersToggle] { workersToggle = true; };
        std::string buttonError;
        if (!aii::ButtonRegistry::instance().add_app_button(
                "workers", aii::ButtonGlyph::Workers, "Active workers",
                aii::ButtonSurface::Sidebar, std::move(open), &buttonError))
            log::warn("no workers button: {}", buttonError);
    }

    std::unique_ptr<aii::SidebarWindow> sidebar;
    if (transparent && hwnd && ui) {
        // Set *before* create(), not after: the strip sizes itself from the
        // buttons it is going to draw, and asking the registry for them while
        // it still believes there is no sidebar returns none — which had it
        // born 48x40 and resized on its first frame.
        aii::ButtonRegistry::instance().set_sidebar_available(true);
        std::string sidebarError;
        sidebar = aii::SidebarWindow::create(*backend, *instance, *device, kFontPx, &sidebarError);
        if (!sidebar) {
            log::warn("no sidebar this run: {}", sidebarError);
            // Which hands its buttons straight back to the panel's toolbar row.
            aii::ButtonRegistry::instance().set_sidebar_available(false);
        }
    }
    // The widget's rect, kept from the geometry block at the top of each frame
    // so the strip and the tooltip it hands back both read the same numbers.
    RECT widgetRect{};
    if (hwnd) GetWindowRect(hwnd, &widgetRect);

    // M9. Worker windows tile **leftward** from whatever is left of the widget,
    // one slot each, so two of them never stack exactly on top of each other.
    // The slot is chosen when the window opens and the window may then be
    // dragged anywhere; nothing drags it back. Declared here, after widgetRect,
    // because that is what it measures from.
    const auto workerSlotGeometry = [&](int slot) {
        aii::WorkerGeometry g;
        g.w = static_cast<unsigned>(kWorkerWinW);
        g.h = static_cast<unsigned>(kWorkerWinH);
        g.x = dockEdge - (slot + 1) * (kWorkerWinW + kWorkerWinGap);
        // Bottom-aligned with the widget, which is itself pinned to the bottom
        // right: a row of windows sharing one baseline reads as a set, and a
        // row sharing a *top* edge would not, because the widget's top moves
        // every time the chat opens.
        g.y = widgetRect.bottom - kWorkerWinH;
        g.placed = true;
        return g;
    };

    while (running) {
        // ---- the window's geometry, before a single one of this frame's
        // events is read ----
        //
        // The height the panel asked for last frame, and the avatar band that
        // goes with it. Everything that sizes itself from the window reads
        // `height`, the loading overlay included, so the swapchain has to
        // follow the window rather than be cropped by it — the loader centres
        // itself, and a stale height puts it low enough to land on the panel.
        //
        // Both are applied *here*, together, ahead of the event pump, and that
        // ordering is load-bearing (fixed 16 Sep 2026):
        //
        //  - Together, because the band is what places every control in the
        //    panel. Taking the new band while the window still has the old
        //    height drew the whole panel 260 px below where it actually was on
        //    screen, for as long as the resize took to land.
        //  - Ahead of the pump, because a mouse event carries the client
        //    coordinates the window had when the OS generated it. Moving the
        //    window after those events have been read makes them point at the
        //    wrong place in the layout they are about to be used in.
        //
        // Either way round, a release landed off the button the pointer was
        // still sitting on, and an abandoned release means "dictate" — which is
        // exactly how the bug showed itself: with the avatar minimized, a click
        // on Talk opened the band, moved the panel, and then read as a hold. A
        // 55-68 ms click failed every time; with the avatar already shown, where
        // nothing moves, the same click was always right.
        // ...and not at all while a gesture is in flight.
        //
        // Applying the band and the height together, before the pump, made the
        // panel's own layout self-consistent, and 4837a94 made the pointer's
        // position survive the move. Neither is enough, because the *hover*
        // verdict does not come from this frame's layout at all: ImGui decides
        // which window the pointer is over inside NewFrame, before a single
        // window has been submitted, so it tests against each window's rect as
        // of the previous frame. On the one frame the widget grows 260 px, that
        // rect is the old short one, and a pointer sitting on the transport row
        // — whose position is now correctly reported in the *new* client space
        // — falls outside it. IsItemHovered() then says no about a button the
        // pointer never left. Measured as rect=1 clip=1 win=0 in the [gesture]
        // line, which is that disagreement written down. It is a one-frame
        // race, so it failed about one click in four rather than every time,
        // and a matrix with one sample per cell could not see it.
        //
        // Chasing that with more coordinate repair means winning a race every
        // time. Not moving is strictly better: a resize that waits for the
        // user's finger to come up costs a few tens of milliseconds nobody can
        // see, and removes the whole class — the microphone, the chat arrow,
        // the cog and anything M5 adds, for every reason the window resizes,
        // not just this one.
        //
        // The one visible consequence, stated rather than hidden: hold-to-
        // dictate holds the avatar's appearance back for the length of the
        // hold, so in "shown when talking" the avatar arrives when the user
        // lets go rather than when they press. That is a second or two, it
        // applies only to the hold gesture, and an avatar popping up in the
        // middle of a deliberate hold was never the point of the mode.
        //
        // Both halves of gesture_in_flight() matter and neither is a timeout:
        // it clears when ImGui sees the release, which is the same event the
        // deferral exists to protect, so there is no way for it to latch on.
        const bool gestureInFlight = ui && ui->gesture_in_flight();
        band = gestureInFlight ? band : nextBand;
        if (pendingH != 0 && !gestureInFlight) {
            height = pendingH;
            pendingH = 0;
            // The OS window is moved here too, immediately before the
            // swapchain that has to match it. Doing it where the panel asked
            // (mid-frame) left one present in between, in which DWM showed the
            // old surface pinned to the top of a window that had already grown
            // — the panel jumping to the ceiling with bare desktop under it.
            // Invisible at one resize per chat click, obvious now that "shown
            // when talking" resizes on every turn.
            if (transparent && hwnd)
                placeInCorner(target.get(), hwnd, static_cast<int>(kWindowW),
                              static_cast<int>(height));
            renderer->resize(width, height);
        }
        // M4.2: the strip is part of this block, not a follower of it. It is
        // docked here — in the same breath as the widget's own height and
        // avatar band, before this frame's input is read — because the widget
        // is anchored to the bottom-right corner and a strip docked later in
        // the frame arrives one present behind whatever the widget just did.
        //
        // Both strips are anchored to the widget's *bottom* edge and grow
        // upward (the user's own rule: overflowing up is fine, overflowing
        // down puts buttons underneath the widget). That edge is the one the
        // widget never moves, so opening the chat — which lifts the panel
        // 265 px — no longer moves either strip at all; what still moves them
        // is the work area changing or the watermark margin being recomputed,
        // which is why this runs every frame. dock() is a no-op when nothing
        // has actually moved.
        if (hwnd) GetWindowRect(hwnd, &widgetRect);
        dockEdge = widgetRect.left;
        if (sidebar) {
            sidebar->dock(widgetRect);
            dockEdge = sidebar->left();
        }
        // M9: the worker strip is part of this same block, and for the same
        // reason.
        //
        // Sized and docked from the rows the *previous* frame's snapshot built
        // (see stripRows). The snapshot is taken well below this point, and
        // drawing a row the window has not been sized for clips it against a
        // client rect that is still a button short — so the strip is one frame
        // late on a worker appearing, which is 16 ms, rather than one frame
        // wrong, which is a half-drawn icon.
        if (workerStrip) {
            workerStrip->set_rows(stripRows);
            workerStrip->dock(widgetRect, dockEdge);
            dockEdge = workerStrip->left();
        }

        // Which half of the input owns the keyboard this frame. The panel now
        // has a text field, so the hotkeys below have to stand down while it
        // has focus — otherwise typing a space toggles the microphone and a
        // typed "q" quits the app. Read from the last completed frame, which
        // is what ImGui's own backends do.
        const bool uiHasKeyboard = ui && ui->wants_keyboard();
        for (const auto& event : backend->pumpEvents()) {
            // The mouse only. Keys reach ImGui through the subclass alone, so
            // forwarding them here as well would deliver each one twice.
            // The widget's HWND goes with it: the pointer's position is taken
            // from the live cursor in this window's client space rather than
            // from the coordinates the message was stamped with, because this
            // window moves itself and a queued message remembers where it used
            // to be. See ImGuiLayer::handle_event.
            if (ui) ui->handle_event(event, hwnd);
            if (uiHasKeyboard && (event.type == platform::Event::Type::KeyDown ||
                                  event.type == platform::Event::Type::KeyUp)) {
                // Held keys still have to be released, or a SPACE leaned on as
                // the field took focus would latch `spaceDown` forever — and
                // now would leave the microphone open with it.
                if (event.type == platform::Event::Type::KeyUp &&
                    event.key == platform::Key::Space)
                    releaseSpace(false);
                continue;
            }
            switch (event.type) {
            case platform::Event::Type::CloseRequested:
                running = false;
                break;
            case platform::Event::Type::KeyDown:
                if (event.key == platform::Key::Escape || event.key == platform::Key::Q) running = false;
                else if (event.key == platform::Key::Space) {
                    if (!spaceDown) {
                        spaceDown = true;
                        spaceDownAt = std::chrono::steady_clock::now();
                        if (session) session->talk_pressed();
                    }
                }
                // S is mute, E is stop — the same two keys, following the two
                // buttons they have always shadowed as those buttons changed
                // meaning (16 Sep 2026). S toggles the panel's own stored flag
                // rather than calling into the session, because the panel is
                // the owner of record for it and the mirror below is what
                // carries it into the session and into settings.json; calling
                // the session here would leave the button and the file behind.
                else if (event.key == platform::Key::S) uiState.muted = !uiState.muted;
                else if (session && event.key == platform::Key::E) session->stop();
                break;
            case platform::Event::Type::KeyUp:
                if (event.key == platform::Key::Space) releaseSpace(true);
                break;
            case platform::Event::Type::Resized: {
                // B2: `Event` carries no window identity and `pumpEvents()` is
                // backend-global, so this event may well belong to the *other*
                // window. Observed, the first time two windows ran: the second
                // window's 256x256 birth resize recreated the widget's own
                // swapchain at 256x256. Asking our own target for its size,
                // instead of believing the event's payload, is immune to that
                // — the event is then only a hint that something resized.
                const auto now = target->sizeInPixels();
                width = now.width;
                height = now.height;
                renderer->resize(width, height);
                break;
            }
            default:
                break;
            }
        }
        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const float dt = static_cast<float>(t - lastT);
        lastT = t;
        if (seconds >= 0.0 && t >= seconds) running = false;
        // M1f.2's harness: a change to the auto-listen setting, made where a
        // hand would make it. Erased from the list once applied so it happens
        // once; the log line is what a run is read back against.
        for (auto it = listenTimeoutAt.begin(); it != listenTimeoutAt.end();) {
            if (t < it->first) { ++it; continue; }
            uiState.listen_timeout_on = it->second > 0;
            if (uiState.listen_timeout_on) uiState.listen_timeout_sec = it->second;
            log::info("[harness] listen timeout set to {} at t={:.1f}s",
                      uiState.listen_timeout_on ? std::to_string(it->second) + " s"
                                                : std::string("never"),
                      t);
            it = listenTimeoutAt.erase(it);
        }
        if (width == 0 || height == 0) continue;

        // M3.12's harness, beside M1f.2's and for the same reason: written
        // into the panel's own fields, where a hand would write them.
        for (auto it = modelAt.begin(); it != modelAt.end();) {
            if (t < it->first) { ++it; continue; }
            const int id = aii::model_choice_for_key(it->second);
            if (id < 0) log::warn("[harness] no such model key: {}", it->second);
            else {
                uiState.model = id;
                log::info("[harness] model picker set to {} at t={:.1f}s",
                          aii::model_choice(id).label, t);
            }
            it = modelAt.erase(it);
        }
        for (auto it = toolAt.begin(); it != toolAt.end();) {
            if (t < it->first) { ++it; continue; }
            const size_t colon = it->second.find(':');
            const std::string key = it->second.substr(0, colon);
            const bool on = colon != std::string::npos && it->second.substr(colon + 1) != "0";
            int group = -1;
            for (int i = 0; i < aii::kToolGroupCount; ++i)
                if (key == aii::tool_group(i).key) group = i;
            if (group < 0) log::warn("[harness] no such tool group: {}", key);
            else {
                uiState.tools.on[group] = on;
                log::info("[harness] tool '{}' ticked {} at t={:.1f}s", key, on ? "on" : "off", t);
            }
            it = toolAt.erase(it);
        }

        // ---- voice loop tick ----
        aii::VoiceSession::Snapshot snap;
        // M3.12. What the settings sections are told about the restart this
        // frame; see the declarations above for what decides them.
        bool llmRestartPending = false;
        bool llmRestartWaitingTurn = false;
        if (session) {
            // M2c.2's harness, and it has to be **before** update(). The thing
            // under test is what happens when a user turn starts while a
            // worker's report is sitting finished and unspoken — the case a
            // person reaches by still talking when the report lands. From
            // outside the process that window is one frame wide: update() is
            // where a waiting report takes the floor on its own, so a check
            // made after it would only ever see a report that had already been
            // delivered the old way.
            if (!sayOnReport.empty() && session->reports_waiting()) {
                log::info("[harness] a report is waiting; sending a user turn on top of it");
                session->say(sayOnReport);
                sayOnReport.clear();
            }
            session->update();
            snap = session->snapshot();
            // ---- M3.12: take a changed model or tool grant up now ----
            //
            // What is in force is the session's answer, not a variable kept in
            // step here: it moves when the new child is up, so the surface
            // never claims a model is running that nothing is running on.
            modelInForce = snap.model_in_force;
            toolsInForce = snap.tools_in_force;
            const std::string modelWanted = aii::model_choice(uiState.model).arg;
            // The `api` backend has no tools at all — build_llm composes
            // against an empty policy there whatever the boxes say — so a box
            // ticked on that backend is not a reason to restart anything: the
            // conversation would be thrown away to change nothing. The section
            // already says the row does not apply.
            const aii::ToolPolicy toolsWanted =
                voiceCfg.backend == "api" ? toolsInForce : uiState.tools;
            if (modelWanted != llmPanelModel || toolsWanted != llmPanelTools) {
                llmPanelModel = modelWanted;
                llmPanelTools = toolsWanted;
                llmSettleFrom = t;  // rule 2: every change restarts the timer
                log::info("[restart] a setting moved at t={:.1f}s (model {}, tools {})", t,
                          aii::model_label(modelWanted), aii::tool_summary(toolsWanted));
            }
            // Moved back to what is already running — a box ticked and
            // unticked again — so there is nothing to apply and nothing to
            // warn about. This is also how the armed flag is cleared after a
            // restart has landed.
            if (modelWanted == modelInForce && toolsWanted == toolsInForce) llmSettleFrom = -1.0;
            // Not while the engines are still coming up, and not after they
            // have failed: there is no child to replace in either case. The
            // change stays armed and goes when there is one — or never, which
            // is the same answer the rest of the window is giving.
            if (llmSettleFrom >= 0.0 && snap.state != aii::VoiceSession::State::Loading &&
                snap.state != aii::VoiceSession::State::Failed) {
                const bool inFlight = snap.state == aii::VoiceSession::State::Thinking ||
                                      snap.state == aii::VoiceSession::State::Speaking;
                llmRestartPending = true;
                llmRestartWaitingTurn = inFlight;  // rule 3
                // Closing the surface is the gesture finishing, so it does not
                // wait out the rest of the settle window.
                const bool settled = !uiState.settings_open || t - llmSettleFrom >= kLlmSettleSeconds;
                if (settled && !inFlight && !session->resetting()) {
                    if (session->apply_llm_settings(toolsWanted, modelWanted))
                        log::info("[restart] applying at t={:.1f}s: model {}, tools {}", t,
                                  aii::model_label(modelWanted), aii::tool_summary(toolsWanted));
                }
            }
            // The reset harness, before the next say is considered: it fires
            // on the first Idle frame after the nth turn has finished, so the
            // turn after it is the first one the new child ever sees.
            // Listening counts as well as Idle, and only when the latch is on:
            // with `startup.auto_listen` the app comes up latched and never
            // sits at Idle for a whole frame, so an Idle-only test could not
            // reach the one case this harness most needs to drive — a reset
            // pressed with the microphone already open. It cannot fire
            // mid-turn either way, because Thinking and Speaking are neither.
            const bool resetMoment =
                snap.state == aii::VoiceSession::State::Idle ||
                (snap.state == aii::VoiceSession::State::Listening && session->mic_open());
            if (resetAfterSay >= 0 && static_cast<int>(nextSay) >= resetAfterSay && resetMoment &&
                !session->resetting()) {
                resetAfterSay = -1;
                log::info("[harness] pressing reset");
                session->reset();
                lastSayDone = {};
            }
            if (resetAt >= 0.0 && t >= resetAt && !session->resetting()) {
                resetAt = -1.0;
                log::info("[harness] pressing reset at {:.2f}s (mic_open={})", t,
                          session->mic_open() ? 1 : 0);
                session->reset();
                lastSayDone = {};
            }
            // `resetting()` is part of the guard because the session reports
            // itself Idle throughout a reset — it genuinely is — and a say
            // handed over in that second would be refused by start_turn() and
            // lost, with `nextSay` already past it.
            if (nextSay < sayTexts.size() && snap.state == aii::VoiceSession::State::Idle &&
                !session->resetting()) {
                const auto nowSay = std::chrono::steady_clock::now();
                if (lastSayDone.time_since_epoch().count() == 0) lastSayDone = nowSay;
                if (std::chrono::duration<double>(nowSay - lastSayDone).count() >= sayWait) {
                    session->say(sayTexts[nextSay++]);
                    lastSayDone = {};
                }
            } else if (session && snap.state != aii::VoiceSession::State::Idle) {
                lastSayDone = {};
            }
            // M2b.4's harness: the same call a short Talk click makes, once,
            // as soon as there is a session to make it on.
            if (micLatch && snap.state == aii::VoiceSession::State::Idle && !session->mic_open()) {
                micLatch = false;
                session->toggle_mic();
                log::info("[harness] microphone latched");
            }
            // M1f.1's harness: press once the engines are up, hold for the
            // requested seconds, release as a dictation. The verdict the
            // session prints for the release (AII_TALK_DEBUG) is what proves
            // the gesture was still alive at the end of the hold.
            if (micHoldSeconds > 0.0 && micHoldBegan.time_since_epoch().count() == 0 &&
                snap.state == aii::VoiceSession::State::Idle && !session->mic_open()) {
                micHoldBegan = std::chrono::steady_clock::now();
                session->talk_pressed();
                log::info("[harness] talk held for {:.1f} s", micHoldSeconds);
            } else if (micHoldSeconds > 0.0 && micHoldBegan.time_since_epoch().count() != 0 &&
                       std::chrono::duration<double>(std::chrono::steady_clock::now() - micHoldBegan)
                               .count() >= micHoldSeconds) {
                micHoldSeconds = 0.0;
                log::info("[harness] talk released after hold (mic_hold={})",
                          session->mic_hold() ? 1 : 0);
                session->talk_released(true, true);
            }
        }

        // The settings debounce runs on the same frame clock. A failed write
        // is reported once, the same way a failed avatar reload is.
        settings.tick(dt);
        if (settings.take_status_change()) log::warn("{}", settings.status());
        // A toolbar button the agent asked for and did not get. Refusals happen
        // on the turn thread and are deliberately never spoken (config.cpp), so
        // this line is the only record that one was turned away.
        for (const std::string& note : aii::ButtonRegistry::instance().take_status())
            log::warn("[button] {}", note);

        // ---- M2.5: the bus, applied at one point in the frame ----
        // **This is the defined point.** Inbound messages arrive from any
        // thread and are queued; they are applied here, after the session tick
        // and before the avatar policy, the theme reconciliation and compose()
        // below — so a command lands at a frame boundary with everything else
        // that decides what this frame looks like, and a `avatar.cells` cannot
        // land halfway through a composed frame.
        busFiles.poll(dt);
        aii::AppBus::instance().apply_pending();
        bus.tick(dt);
        for (const std::string& note : aii::AppBus::instance().take_status())
            log::warn("[bus] {}", note);
        // M2b.1: the schedule book, ticked from the same defined point and for
        // the same reason. There is no timer thread anywhere in this feature —
        // a thread would still have to hand the event back to this loop, so it
        // would add a join, a kernel object and a cancel/callback race and buy
        // no promptness the loop does not already have at 60 Hz.
        //
        // M2b.4: the fired schedule is handed to the session, which is what
        // owns the floor — announce()'s queue for a fixed line, an injected
        // turn for a phrased one, and neither ever speaks from here. The log
        // line stays: it is the evidence that a schedule fired *on this
        // thread*, which is the property the whole design rests on, and a
        // report that is correctly waiting for a gap looks exactly like one
        // that never arrived without it.
        {
            const auto fireNow = std::chrono::steady_clock::now();
            for (const aii::Schedule& s : aii::ScheduleBook::instance().tick(fireNow)) {
                const bool onLoop = std::this_thread::get_id() == frameThread;
                log::info(
                    "[schedule] fired id={} kind={} grade={} late={:.1f}ms frame-loop={} "
                    "cwd=\"{}\" report=\"{}\"",
                    s.id, s.action.kind, aii::to_string(s.grade),
                    std::chrono::duration<double, std::milli>(fireNow - s.due).count(),
                    onLoop ? "yes" : "NO", s.action.cwd, s.action.report);
                // M2b.2. Published before it is delivered, and published
                // whether or not anything is listening, like every other event
                // on the bus. A script that scheduled something has no other
                // way to know it happened: the report is spoken at the user,
                // not sent back to the program that asked for it.
                aii::AppBus::instance().publish(aii::BusLine("schedule.fired")
                                                    .num("id", static_cast<double>(s.id), 0)
                                                    .str("kind", s.action.kind)
                                                    .str("grade", aii::to_string(s.grade))
                                                    .str("label", s.action.label)
                                                    .done());
                if (session) session->deliver_schedule(s);
                else log::warn("[schedule] no voice session: nothing to report through");
            }
            for (const std::string& note : aii::ScheduleBook::instance().take_status())
                log::warn("[schedule] {}", note);
            // M2b.5: cancels the AI asked for, applied here and nowhere else.
            // The position is the argument, not a convenience — immediately
            // after the tick and after every schedule that came due has been
            // delivered, so a cancel can never land in the gap between a
            // schedule leaving the book and the work it started being on
            // record. "Cancelled" and "fired" were exclusive inside the book
            // already; this is what keeps them exclusive outside it.
            if (session) session->apply_cancels();
        }
        // M2.6. Empties the engine queue the Python host is wired to — nothing
        // here consumes it, so a script that called into the `rend` module
        // would otherwise grow it for the rest of the run — and passes on
        // whatever a script asked to have logged.
        scripts.tick();
        for (const std::string& line : bus.take_script_log()) log::info("[py] {}", line);
        // Published from the snapshot the frame loop already took, rather than
        // from inside the session: the turn thread and the worker poll keep
        // queueing and the frame loop keeps acting, which is announce()'s rule
        // and the reason M2.5 changes no line of voice_session.cpp.
        bus.publish(snap, dt);

        // Clip playback and the hot-reload poll, then the policy, then the
        // composition — in that order, and after the session tick and the
        // resize above, because all three feed it: the reload is what the
        // policy re-measures its clip lengths from, the snapshot is what it
        // decides on, and `width` is what the stage is laid out against. The
        // same `width` reaches write_slot below, so the art is never laid out
        // for a band the frame does not have.
        // M1c.3: the pickers wrote a wish into uiState last frame; this is
        // where it becomes true, before update() and so before this frame's
        // status is read below — which is what puts a failed choice's reason
        // on screen on the same frame the choice was made.
        //
        // Both are applied by *name* through one call each, and neither the
        // panel nor this block knows what a palette is. That is the shape
        // M2.5's bus needs: setting a theme from a message is this same
        // set_theme() at a different call site, not a second mechanism.
        if (avatarDirOverride.empty() && !uiState.avatar_name.empty() &&
            uiState.avatar_name != avatarName) {
            avatarName = uiState.avatar_name;
            // Seeded on the way in, like the first one: an avatar the user has
            // never opened before has no copy under %APPDATA% yet, and this is
            // the call that makes one (and refreshes a stale one).
            avatarSource.open(aii::seed_avatar_definition(avatarName), clipName, spriteNames);
        }
        if (uiState.theme != avatarSource.theme()) {
            const std::string wanted = uiState.theme;
            if (avatarSource.set_theme(wanted) && wanted != aii::AvatarSource::custom_theme()) {
                // M1c.5: picking a named preset seeds the colour picker with
                // that preset's own body, so "start from ember and nudge it"
                // is a click and a drag rather than matching a hex by eye. The
                // named themes are not touched by this — it is the derived
                // theme's starting point that moves.
                if (const std::uint32_t body = avatarSource.theme_body_colour(wanted); body != 0)
                    avatarSource.set_custom_colour(body);
            }
        }
        // The drag. Everything it costs is inside set_custom_colour: a pass
        // over the frames' ink bytes, no file touched and no clip restarted.
        if (uiState.custom_colour_changed) {
            avatarSource.set_custom_colour(colourFromFloats(uiState.custom_colour));
            uiColour = avatarSource.custom_colour();
        } else if (avatarSource.custom_colour() != uiColour) {
            // Something other than the picker moved it — a preset seeding it
            // above, or the value the settings file came back with.
            uiColour = avatarSource.custom_colour();
            colourToFloats(uiColour, uiState.custom_colour);
        }
        // Written back from what is actually loaded, every frame. A theme the
        // art no longer declares was refused above and the picker corrects
        // itself here rather than showing a setting that is not in force; and
        // because only the truth is ever stored, the settings file cannot
        // accumulate a name that stopped meaning anything.
        uiState.avatar_name = avatarName;
        uiState.theme = avatarSource.theme();
        if (avatarDirOverride.empty()) settings.set_string("avatar", "name", avatarName);
        if (!themeFromArgs) {
            settings.set_string("avatar", "theme", avatarSource.theme());
            // Only while the derived theme is actually in force, which keeps
            // the rule the rest of this file already keeps: the file holds what
            // loaded, so it cannot accumulate a value nothing is using. The key
            // therefore never appears until the user has had the derived theme
            // on, and once it exists it stays as the seed they come back to.
            if (avatarSource.theme() == aii::AvatarSource::custom_theme())
                settings.set_string("avatar", "colour", colourToHex(avatarSource.custom_colour()));
        }

        // ---- the loading overlay's clock, and the avatar's appearance ----
        //
        // Both are settled *here*, above the controller, and that ordering is
        // load-bearing (fixed 18 Sep 2026). It used to be below: `appear()`
        // was called after `controller.update()` had already run for the
        // frame, so the entrance was consumed a frame late and the first
        // frame the avatar was drawn on showed **the clip it was playing when
        // it was last on screen** — a whole, ordinary-looking slime — before
        // the entrance took over on the next one. Measured: at the summon
        // frame the trace read `alpha=0.11 clip=think`, and one frame later
        // `alpha=0.40 clip=wake`. That single normal-looking frame is what the
        // user reported as the avatar "coming back for one frame" as it
        // flickered, and it happened on every appearance, in both modes.
        //
        // With the order reversed the appearance edge reaches the controller
        // in the same frame it is raised, so the entrance's own first frame is
        // the first frame with any alpha on it, and nothing else is ever seen.
        //
        // With --no-voice there is no session and the snapshot stays Loading,
        // which is the long window the animation is tuned in.
        if (snap.state == aii::VoiceSession::State::Loading) {
            handoff = 0.0f;
            // Only while there is one: the session clears the stage name as
            // the last stage completes, a frame or two before it leaves
            // Loading, so taking it unconditionally would freeze an empty
            // caption and the loading screen would lose its label first.
            if (!snap.load_stage.empty()) loaderStage = snap.load_stage;
            loaderProgress = snap.load_progress;
        } else if (handoff < 1.0f) {
            // The bar runs to full as the loader leaves rather than stopping
            // wherever the last stage left it; the caption holds that stage.
            loaderProgress = 1.0f;
            handoff = std::min(1.0f, handoff + dt / kHandoffSeconds);
        }
        // Staggered rather than strictly complementary. An even crossfade puts
        // the loader's cubes and the avatar both at half strength through the
        // middle, which reads as two overlaid images rather than one handing
        // over; the loader is most of the way out before the avatar has any
        // real presence, and the two still coexist across the middle third.
        const float loaderAlpha = 1.0f - smoothstep(0.00f, 0.62f, handoff);
        loading = loaderAlpha > 0.0f;

        // ---- M1f.5: the app latches its own microphone on, once ----
        //
        // **This line, and this position in the frame, is the whole decision.**
        //
        // The moment is "the first frame on which the loading screen is
        // finally gone", and it is chosen over the two obvious alternatives:
        //
        //  - *As soon as the session exists* is wrong twice over. The session
        //    exists while it is still loading its engines, and
        //    `set_mic_open()` refuses in `Loading` by dropping the latch back
        //    to false — so an early call does not crash, it does something
        //    worse: it leaves a microphone that is reported shut and a setting
        //    that appears not to work.
        //  - *As soon as the session reaches Idle* would work mechanically —
        //    the recogniser is up and `begin_listening()` starts the capture
        //    device synchronously — but it is up to 0.26 s before the loader
        //    has faded, and for that whole stretch the panel is still clamped
        //    to `Loading` further down and the avatar is still suppressed by
        //    `AvatarPresence`. The microphone would be live while all three
        //    surfaces that report it said the app was still starting. A live
        //    microphone nobody has been told about is the one thing this
        //    feature must never produce.
        //
        // Latching here instead costs a quarter of a second of a fade nobody
        // is talking over, and buys the property that matters: the first frame
        // on which the panel tells the truth is the first frame on which the
        // microphone is open. Icon, status line and avatar all change on the
        // same frame, and `begin_listening()` calls `mic_->discard()` before
        // `mic_->start()`, so nothing said before it is half-captured either.
        //
        // It is *above* the engagement gather below on purpose, so the frame
        // that latches is also the frame `engagement.mic_on` is true on: the
        // avatar is summoned into the space the loader left by the ordinary
        // startup path (cb38eef) rather than a second time, a frame later, by
        // a latch the presence rule found afterwards. There is one `summoned`
        // edge in the program and this does not add another.
        //
        // One shot, cleared by the frame that fires. A microphone the user
        // closes afterwards stays closed; the setting had its one say.
        // `resetting()` is part of the guard and not an optimisation: the
        // session refuses to open the microphone while the child is being
        // replaced (VoiceSession::set_mic_open), so a one-shot spent in that
        // second would be spent on a call that did nothing and the setting
        // would silently not work — the same failure the comment above
        // `!loading` describes, arriving by the other door. Nothing is lost by
        // waiting: the flag is still pending on the far side.
        if (autoListenPending && session && !loading && !session->resetting()) {
            if (snap.state == aii::VoiceSession::State::Failed) {
                // Nothing to listen with, and never will be this run. Cleared
                // so the condition stops being asked, and said once, because
                // "the setting did nothing" is otherwise indistinguishable
                // from a bug in it.
                autoListenPending = false;
                log::warn("[auto-listen] engines failed; the microphone is not being opened");
            } else if (!session->mic_open()) {
                autoListenPending = false;
                // The same call a short click on the microphone makes, and
                // deliberately that call and not a private one: latched, not
                // hold-to-dictate, and there is exactly one path into the
                // latch for both of them.
                session->toggle_mic();
                log::info("[auto-listen] microphone latched at {:.2f}s (mic_open={})", t,
                          session->mic_open() ? 1 : 0);
            } else {
                // Something else already opened it this frame — `--mic-latch`,
                // or a hand fast enough to click during the fade. Either way
                // the app is listening, which is what the setting asked for.
                autoListenPending = false;
            }
        }

        // M7.2: the one place the avatar appears. Every path that can put it
        // on screen — the loader handing over at startup, the mode switched in
        // settings, a turn starting in "shown when talking", hold-to-dictate
        // letting go — is `avatarWanted` by the time it reaches here, and none
        // of them is named below.
        //
        // The state is clamped to Loading for as long as the loader still has
        // opacity, exactly as the panel's copy is further down: while that
        // overlay owns the window no mode wants the avatar, and the two gates
        // then say the same thing. It is written out here rather than taken
        // from `snap` because `snap` is deliberately *not* clamped until after
        // the controller has read it.
        //
        // The M1.5 handoff no longer multiplies into this. That product was
        // the bug: it fed the avatar in across the loader's own 0.42 s
        // dissolve, so the entrance M2.4 fired on the same frame played under
        // a scrim and was finished before the band was opaque. Now the loader
        // leaves first and the avatar is summoned into the space it left.
        // What the user is doing, gathered here and judged in one place
        // (avatar_ui.cpp). Every field is a level read off something that
        // already exists — the session's two microphone answers and the
        // panel's message buffer — so there is no fourth owner of "is anyone
        // there" to fall out of step with the three that were already right.
        aii::AvatarEngagement engagement;
        engagement.state = loading ? aii::VoiceSession::State::Loading : snap.state;
        engagement.mic_on = session && session->mic_open();
        engagement.mic_hold = session && session->mic_hold();
        engagement.composing = uiState.message[0] != '\0';
        const bool avatarWanted = presence.update(uiState.avatar_mode, engagement, dt);
        const aii::AvatarAppearance::Frame appeared =
            appearance.update(avatarWanted, loading, dt);
        avatarAlpha = appeared.alpha;
        // The whole of the summon: an arrival plays an entrance, and this is
        // the only line in the program that starts one. It is deliberately
        // outside the `controllerOwnsAvatar` test's block but inside its
        // condition — a run pinned to `--clip` has no policy to ask.
        if (appeared.summoned && controllerOwnsAvatar) {
            // `engagement.mic_on` and not `snap`: the snapshot was taken at the
            // top of this frame and the auto-listen latch above opened the
            // microphone after it, so on the boot path the snapshot still says
            // Idle while the thing that summoned the avatar is the microphone.
            // The entrance has to know, or it pre-empts itself on the next
            // frame -- see AvatarController::appear().
            controller.appear(engagement.mic_on);
            log::info("avatar: summoned at {:.2f}s (pop {:.0f} ms)", t,
                      aii::AvatarAppearance::kPopSeconds * 1000.0f);
        }
        // M2.3c, and the mirror of it: a departure plays an exit, and the alpha
        // is held at full for exactly as long as that takes. The controller is
        // asked how long rather than told, because only the definition knows
        // -- an avatar with no exit art answers 0 and the departure goes back
        // to M1.6's dissolve, which is what every avatar did before this line.
        //
        // It sits *above* the controller for the same reason the summon does,
        // and the two edges are now symmetric in their placement as well as in
        // their contract. The asymmetry is inside the controller, not here:
        // `appear()` only arms a pending entrance that `update()` consumes,
        // while `depart()` starts its one-shot outright and returns its length
        // in the same call. Below the controller that made no difference to the
        // *band* -- which M2.3c held through `present()` -- but it did put the
        // exit's own first frame a frame late, exactly as the entrance was,
        // and it left `depart()`'s suspension of the dwell floor to be spent by
        // the *next* frame's update() rather than by the frame that started the
        // exit. Raised here, the departure edge reaches the controller in the
        // frame it is raised: update() runs afterwards with the exit one-shot
        // already current, Yield::Exit keeps it there, and the floor is
        // suspended for the frame it was written for.
        if (appeared.dismissed) {
            const float exit_len = controllerOwnsAvatar ? controller.depart() : 0.0f;
            appearance.hold_exit(exit_len);
            log::info("avatar: dismissed at {:.2f}s (exit {:.0f} ms, fade {:.0f} ms)", t,
                      exit_len * 1000.0f,
                      (exit_len > 0.0f ? aii::AvatarAppearance::kLeaveAfterExitSeconds
                                       : aii::AvatarAppearance::kLeaveSeconds) *
                          1000.0f);
        }
        // ---- the band's height follows the *mode*, not the moment ----
        //
        // (user, 18 Sep 2026: "stop resizing the window when the AI is no
        // longer visible. Keep it fully sized, but just don't show the avatar.
        // Just show an empty region.")
        //
        // This is the flicker fix, and it is a fix rather than a preference.
        // The band used to be reserved only while the avatar had any alpha, so
        // an appearance was also a *resize*: the window grew 260 px as the
        // alpha started rising and shrank again as it reached zero. On a
        // borderless window DWM composites with per-pixel alpha, a geometry
        // change and an alpha change a frame apart is a visible flash, and in
        // `when_talking` it happened four times a turn — the avatar is wanted
        // for Listening, not for the Thinking pause, and wanted again for
        // Speaking, so a single exchange grew, shrank, grew and shrank the
        // window inside a couple of seconds.
        //
        // Reserving it by mode removes the whole class: in `always` and
        // `when_talking` the band is 260 px of window for as long as that mode
        // is selected and the avatar simply fades in and out of an empty
        // region that was already there. In `hidden` it is 0 px, permanently —
        // the mode is the user saying they do not want the space, and the only
        // thing that changes it is them changing the mode, which is a
        // deliberate act and not a frame anyone is watching for a flash.
        //
        // The loading screen still forces it: that overlay covers the whole
        // window and is centred in it, so `hidden` gives the 260 px back when
        // the loader leaves rather than shrinking the window out from under
        // it. That is one resize, at startup, and it is unchanged.
        //
        // This subsumes M2.3c's reason for widening `present()`. That was the
        // band's only defence against being taken away mid-exit -- the clip
        // needs the 260 px for its whole length and a height that followed the
        // alpha would have started shrinking as the departure began. Reserving
        // by mode makes the defence unnecessary rather than removing it: in
        // `always` and `when_talking` the band the exit plays in was never
        // going anywhere, and in `hidden` there is no avatar to depart. The
        // hold itself is unchanged and still lives where it belongs, in
        // AvatarAppearance's alpha -- `present()` is simply no longer what
        // this line reads.
        //
        // The resize itself, when a mode change does cause one, still goes
        // through pendingH and lands at the top of the next frame — never from
        // inside this one (M1.4) — and through `PresentationTarget::setSize`
        // in placeInCorner, never a raw SetWindowPos.
        nextBand =
            (loading || uiState.avatar_mode != aii::AvatarVisibility::Hidden) ? kAvatarH : 0;

        avatarSource.update(dt);
        if (avatarSource.take_status_change()) {
            if (avatarSource.status_ok()) log::info("{}", avatarSource.status());
            else log::warn("{}", avatarSource.status());
            if (controllerOwnsAvatar && avatarSource.loaded())
                controller.note_definition(avatarSource.definition());
        }
        if (controllerOwnsAvatar) {
            // Muted with the chat shut is the one case where the panel cannot
            // say so by itself — the Mute button is on screen, but the widget
            // in the corner is a 360 px strip a user is not looking at, and
            // the point of the bubble is that the avatar carries the news. It
            // outranks the clip's own accessory (see avatar_apply).
            //
            // M2.5 puts a second producer on this channel: a script's sprite.
            // The muted bubble still wins — it is the only sign of a condition
            // the user cannot otherwise see with the chat shut, and a script's
            // accessory is a decoration. Below it, a script's sprite outranks
            // the clip's own for the same reason a status sprite always has.
            const char* statusSprite = (uiState.muted && !uiState.chat_open)
                                           ? "muted"
                                           : bus.script_sprite();
            aii::avatar_apply(controller.update(snap, dt), avatarSource, statusSprite);
            if (controller.clip() != lastClipLogged) {
                lastClipLogged = controller.clip();
                log::info("avatar clip: {} ({}) after {:.2f}s", lastClipLogged,
                          controller.reason(), controller.last_dwell());
            }
        }
        avatarSource.compose(grid, width, kAvatarH);
        // M2.5's direct cell control, stamped over the composed frame and
        // before it is written to the GPU below. After compose() rather than
        // inside AvatarSource because a script's cells are not part of what the
        // avatar *is*: they are an overlay with a lease, they survive no reload
        // and they cannot corrupt the art.
        bus.stamp_cells(grid);

        // ---- the loading overlay's own drawing ----
        // The handoff's clock, `loading`, the avatar's alpha and the band are
        // all settled above the controller now; see the block there. What is
        // left here is the loader's push constants and the snapshot the *panel*
        // is given, both of which have to happen after the controller has read
        // the unclamped snapshot.
        //
        // From here down the snapshot says Loading for as long as the loader is
        // still on screen, which is what avatar_ui.h already documents the panel
        // being given: the panel's loading layout is keyed off snap.state, but
        // the session leaves Loading a whole crossfade before the loader does.
        // Left unheld, the chat is released kHandoffSeconds before the avatar
        // band is given back, and a stored "chat open, avatar hidden" (M1b.5)
        // starts by growing to the full chat-plus-band height and then shrinking
        // again — two resizes under the loading screen where there should be
        // one, on the frame the loader leaves. Nothing above this line is
        // affected: the controller, the loader's own progress and the first
        // --say all read the snapshot before it.
        if (loading) snap.state = aii::VoiceSession::State::Loading;

        if (loading) loaderPush = aii::loader_push(t, width, height, loaderAlpha);
        // The panel is released on the first frame of the handoff, not held for
        // it: its one discrete change (the usage row, the state line, the live
        // buttons) then lands under the scrim at full strength, and everything
        // after it is opacity alone. Holding it instead put that change a
        // tenth of a second past the loader's last frame, on a window where
        // nothing else was moving, which read as a glitch rather than a settle.
        // With the chat closed by default the panel is the same height loading
        // or idle, so nothing resizes across the fade and there is no window
        // growth left to place — except in a mode that hides the avatar, where
        // the band is given back on the frame the loader finally leaves.

        // ---- the panel ----
        if (ui) {
            // The strip's own frame, first — before the widget's, so a hover
            // picked up here is lettered by the widget on this frame rather
            // than the next one. It runs in its own ImGui context and presents
            // its own swapchain; everything ImGui after this line has to make
            // the widget's context current again, which begin_frame() and
            // sync_pointer() both do for themselves.
            if (sidebar) {
                const aii::SidebarResult sb = sidebar->draw(dt, loading);
                // The cog is a toggle on a region of the *panel*, so this is
                // the one thing the strip cannot do for itself.
                if (sb.open_settings) uiState.settings_open = !uiState.settings_open;
                if (!sb.refusal.empty()) {
                    // Same reserved row the toolbar's folder uses when its path
                    // has gone away: the answer to a click belongs beside the
                    // click, not in a log the user is not reading.
                    uiState.refusal = sb.refusal;
                    uiState.refusal_left = 2.5f;
                }
            }
            // ---- M9: the worker strip and the worker windows ----
            //
            // All three layers' creations and destructions are answered here,
            // between the strip's frame and the widget's, which is the only
            // place in this loop where a GPU object may be built or torn down
            // and an ImGui context switched — nobody's frame is open across
            // this block.
            //
            // **N ImGui contexts.** There are now four window types and up to
            // four worker windows, so between five and eight contexts can be
            // live at once. That is not, by itself, the hazard: the hazard is
            // *assuming* which one is current, because CreateContext() restores
            // the previously current context before it returns. Every entry
            // point below makes its own context current first (ImGuiLayer does
            // it in make_current(), which moves the D3D12 backend's `g_state`
            // with it), and creating a window here leaves that new context
            // current — which is harmless precisely because the next thing to
            // touch ImGui, whichever window it belongs to, says so first.
            //
            // `--watch-worker` is answered first, and only once the named
            // worker is really in the pool: it sets the same latch the icon's
            // click sets and then forgets the name, so it opens one window and
            // not one per frame.
            std::string toggleWorker;
            if (!watchWorkers.empty()) {
                for (auto it = watchWorkers.begin(); it != watchWorkers.end(); ++it) {
                    const bool present = std::any_of(
                        snap.workers.begin(), snap.workers.end(),
                        [&](const aii::WorkerPool::Snapshot& w) { return w.name == *it; });
                    if (!present) continue;
                    toggleWorker = *it;
                    watchWorkers.erase(it);
                    break;
                }
            }
            if (workersToggle) {
                workersToggle = false;
                if (workerStrip) {
                    // A second press closes the column. The worker windows it
                    // opened stay: the strip is the index, and each window has
                    // its own close box and its own reason to be on screen.
                    // Closing the index is not a statement about the documents.
                    workerStrip.reset();
                } else {
                    std::string stripError;
                    workerStrip = aii::WorkerStripWindow::create(*backend, *instance, *device,
                                                                 kFontPx, &stripError);
                    // Survivable: the button stays and the next click tries
                    // again. It is shown by the next frame's dock(), which is
                    // also the first thing that knows where it belongs — one
                    // frame hidden rather than one frame in SDL's corner.
                    if (!workerStrip) log::warn("no worker strip: {}", stripError);
                }
            }
            if (workerStrip) {
                const aii::WorkerStripResult ws = workerStrip->draw(dt);
                if (!ws.toggled.empty()) toggleWorker = ws.toggled;
            }
            if (!toggleWorker.empty()) {
                const auto at = std::find_if(
                    workerWindows.begin(), workerWindows.end(),
                    [&](const OpenWorker& o) { return o.name == toggleWorker; });
                if (at != workerWindows.end()) {
                    // **One window per worker.** A second click on the same
                    // icon closes the window it opened rather than making
                    // another one, which is the inspector's rule and the only
                    // one that makes an icon read as a toggle.
                    workerWindows.erase(at);
                } else if (static_cast<int>(workerWindows.size()) >= kWorkerWindowsMax) {
                    // The same reserved row the toolbar's folder uses when its
                    // path has gone away: the answer to a click belongs beside
                    // the click, not in a log nobody is reading.
                    uiState.refusal = "Four worker windows at once is the limit";
                    uiState.refusal_left = 2.5f;
                } else {
                    // The lowest free column, so closing the middle of three
                    // windows and opening a fourth fills the gap rather than
                    // starting a fifth column off the left of the screen.
                    int slot = 0;
                    while (std::any_of(workerWindows.begin(), workerWindows.end(),
                                       [&](const OpenWorker& o) { return o.slot == slot; }))
                        ++slot;
                    std::string workerError;
                    auto win = aii::WorkerWindow::create(*backend, *instance, *device, kFontPx,
                                                         toggleWorker, workerSlotGeometry(slot),
                                                         &workerError);
                    if (!win)
                        log::warn("no window for worker {}: {}", toggleWorker, workerError);
                    else
                        workerWindows.push_back({toggleWorker, slot, std::move(win)});
                }
            }
            // Each open window's own frame, each handed *this frame's* row out
            // of the snapshot the session already took under the pool's mutex.
            // Handed, never reached for: WorkerPool is written from each
            // worker's own thread, and a window that read it directly would be
            // reading it mid-write. A null row means the worker has left the
            // pool — the window is told, and decides for itself what to say.
            for (std::size_t i = 0; i < workerWindows.size();) {
                const aii::WorkerPool::Snapshot* live = nullptr;
                for (const aii::WorkerPool::Snapshot& w : snap.workers)
                    if (w.name == workerWindows[i].name) {
                        live = &w;
                        break;
                    }
                if (workerWindows[i].window->draw(dt, live))
                    ++i;
                else
                    workerWindows.erase(workerWindows.begin() + static_cast<std::ptrdiff_t>(i));
            }
            // What the strip will be sized and docked with at the top of the
            // next frame. Built here, at the bottom of this one, because this
            // is the first point at which both halves of a row are known: the
            // pool's state, and whether its window is open.
            stripRows.clear();
            for (const aii::WorkerPool::Snapshot& w : snap.workers) {
                aii::WorkerStripRow row;
                row.name = w.name;
                row.state = w.state;
                row.activity = w.activity;
                row.window_open = std::any_of(
                    workerWindows.begin(), workerWindows.end(),
                    [&](const OpenWorker& o) { return o.name == w.name; });
                stripRows.push_back(std::move(row));
            }

            // M5.1: the inspector's own life, answered here — between the
            // strip's frame and the widget's, which is the only place a window
            // may be created or destroyed. The toggle was set by the button's
            // callback, from either surface; a close box sets nothing and is
            // reported by draw() returning false instead, because that message
            // arrives while the widget's pumpEvents is running.
            if (inspectorToggle) {
                inspectorToggle = false;
                if (inspector) {
                    inspectorGeom = inspector->geometry();
                    inspector.reset();
                } else {
                    std::string inspectorError;
                    inspector = aii::InspectorWindow::create(*backend, *instance, *device, kFontPx,
                                                             inspectorGeom, &inspectorError);
                    // Survivable: the button stays and the next click tries
                    // again. Nothing above the inspector depends on it.
                    if (!inspector) log::warn("no inspector window: {}", inspectorError);
                }
            }
            if (inspector) {
                // Its own ImGui context and its own present, exactly as the
                // strip's, and like the strip everything after this line makes
                // the widget's context current again for itself.
                // M5.2. Copied here, every frame, from the session — which is
                // where the store and the injector actually live and which
                // took its own lock to hand this over. The window never
                // touches either, and because the copy is taken per frame the
                // list is live: a prompt injected by the turn running right
                // now is in the next frame's copy.
                //
                // Default-constructed when there is no session (`--no-voice`):
                // `ready` is false, and the window says the store has not been
                // read rather than that there is nothing in it.
                aii::PromptInventory inventory;
                if (session) inventory = session->prompt_inventory();
                const bool stillOpen = inspector->draw(dt, inventory);
                inspectorGeom = inspector->geometry();
                rememberInspector();
                if (!stillOpen) inspector.reset();
            }
            // After the resize above, never before it: the pointer's client
            // coordinates are meaningless until the window it is measured
            // against has stopped moving this frame.
            ui->sync_pointer(hwnd);
            ui->begin_frame(width, height, dt);
            const bool submit = textInput && textInput->take_submit();
            // Rescanned on the edge of the surface opening, not every frame.
            if (uiState.settings_open && !settingsWasOpen) avatarNames = aii::avatar_definition_names();
            settingsWasOpen = uiState.settings_open;
            aii::AvatarOptions avatarOptions;
            avatarOptions.avatars = avatarNames;
            avatarOptions.themes = avatarSource.themes();
            avatarOptions.script_status = bus.script_status();
            avatarOptions.script_status_ok = bus.script_status_ok();
            avatarOptions.art_status = avatarSource.status();
            avatarOptions.art_status_ok = avatarSource.status_ok();
            avatarOptions.derived = avatarSource.derived();
            avatarOptions.custom_theme = avatarSource.theme() == aii::AvatarSource::custom_theme();
            // M8.3. What the session is really doing, which is not always what
            // the checkboxes say — see VoiceSession::effective_langs.
            avatarOptions.japanese_voice = snap.japanese_voice;
            avatarOptions.japanese_voice_error = snap.japanese_voice_error;
            avatarOptions.stt_language = aii::stt_language_for(snap.effective_langs);
            // M3.8. What the child was launched with, beside what the boxes
            // say. Unlike every other pair above it these cannot be reconciled
            // mid-run — `--allowedTools` is fixed at process start — so the
            // surface's job is to show the gap and name the restart.
            avatarOptions.tools_in_force = toolsInForce;
            avatarOptions.tools_supported = voiceCfg.backend != "api";
            // M3.12. The three states of the restart a change now causes, and
            // whether there is a child here to restart at all: a --no-voice
            // run, a session still loading and one that has failed all have to
            // read as "at the next start" rather than as a promise.
            avatarOptions.llm_restart_live = session != nullptr && !loading &&
                                             snap.state != aii::VoiceSession::State::Failed;
            avatarOptions.llm_restart_pending = llmRestartPending;
            avatarOptions.llm_restart_waiting_turn = llmRestartWaitingTurn;
            avatarOptions.llm_restart_running = snap.resetting;
            // M1f.5. What this run started under, beside what the box says.
            avatarOptions.auto_listen_in_force = autoListenInForce;
            avatarOptions.voice_enabled = session != nullptr;
            // M3.11. Same pair, same reason: `--model` is fixed at process
            // start too, so the picker and this are drawn against each other.
            avatarOptions.model_in_force = modelInForce;
            const aii::AvatarUiResult r =
                aii::draw_avatar_ui(uiState, snap, avatarOptions, session != nullptr,
                                    session && session->mic_open(),
                                    session && session->mic_hold(), kWindowW, band, submit);
            if (session) {
                if (r.talk_pressed) session->talk_pressed();
                if (r.talk_released) session->talk_released(r.talk_over_button, r.talk_held);
                if (r.stop) session->stop();
                // The second press of the transport row's confirm, never the
                // first: the panel does the arming and only tells us when the
                // user has said yes twice. reset() returns straight away and
                // does the teardown on its own thread, so this does not stall
                // the frame.
                if (r.reset) session->reset();
                // Mute is a level, not an event: the button and the S key both
                // write the panel's flag and this pushes it down, so there is
                // one place that decides what "muted" is. set_muted() is a
                // no-op unless it changed, and the cut-what-is-playing part of
                // it happens on the edge inside the session.
                session->set_muted(uiState.muted);
                // M8.3, the same level-not-edge mirror as mute, and for the
                // same reason: the panel owns the flags, this pushes them
                // down, and set_languages() is a no-op unless they changed.
                session->set_languages({uiState.lang_english, uiState.lang_japanese});
                // M1f.2, and the whole reason M1f.1 made this a level rather
                // than an edge. Pushed every frame, from the panel state the
                // control writes, so a number changed while the microphone is
                // already latched is honoured by *that* latch — there is no
                // "takes effect next time you click Talk", and no restart.
                // set_listen_timeout() is a no-op unless the value changed.
                session->set_listen_timeout(aii::listen_timeout_seconds(uiState));
                // Only reaches here once the panel has satisfied itself the
                // session can take it; say() refuses the rest anyway.
                if (!r.send_text.empty()) {
                    // What the field actually sent, which is now a question a
                    // screenshot cannot answer: the panel wraps the text it
                    // shows, and the message must leave without those breaks
                    // in it. A capture shows a field of wrapped lines whether
                    // the wrap is the panel's or the user's; this line is the
                    // only place the difference is visible.
                    log::info("send: {}", r.send_text);
                    session->say(r.send_text);
                }
            }
            // Mirrored out of the panel state every frame rather than from the
            // buttons that write it: the setters are no-ops when the value
            // already matches, so "on change" is decided in one place instead
            // of every control that touches a persisted field remembering to
            // say so. The write itself is debounced inside Settings.
            settings.set_bool("panel", "chat_open", uiState.chat_open);
            settings.set_bool("panel", "muted", uiState.muted);
            // M1f.5. Its own section rather than `panel`, because it is not a
            // property of the panel's state: it is what the app does to itself
            // at launch, and the next things to join it there (which window to
            // come up in, whether to open the chat) belong beside it rather
            // than among the toggles.
            settings.set_bool("startup", "auto_listen", uiState.auto_listen);
            settings.set_enum("panel", "avatar_mode", aii::kAvatarVisibilityNames,
                              aii::kAvatarVisibilityCount,
                              static_cast<int>(uiState.avatar_mode));
            // M8.3. One string, "en" / "ja" / "en,ja", rather than two
            // booleans: a file can then never hold the state the app has no
            // answer for, because there is no spelling of "neither".
            settings.set_string("language", "enabled",
                                aii::language_spec({uiState.lang_english, uiState.lang_japanese}));
            // M1f.2. One number and no companion flag: 0 is never, which is
            // the spelling the mechanism, the config and the file all share.
            // The same expression that was pushed into the session above, so
            // what is remembered and what is running cannot diverge.
            settings.set_float("timing", "listen_timeout", aii::listen_timeout_seconds(uiState));
            // M3.8. One boolean per group, under "tools". Written every frame
            // like the rest and debounced like the rest; the file is the only
            // place a change to these can go, since the running child cannot
            // take one.
            for (int i = 0; i < aii::kToolGroupCount; ++i)
                settings.set_bool("tools", aii::tool_group(i).key, uiState.tools.on[i]);
            // M3.11. The picked model's *key*, under "model". A key and not a
            // model string, so that a value written today still resolves
            // through the table tomorrow when the alias behind it has moved on
            // — and so that an entry dropped from the table degrades to the
            // CLI's default at the next read rather than onto a command line.
            settings.set_string("model", "name", aii::model_choice(uiState.model).key);
            // The scrim and the caption belong to the loader, not the panel:
            // the loading screen has to look the same whether or not there is
            // anything underneath it. The foreground draw list puts them over
            // the panel; the cubes then go over them in the overlay recorder.
            if (loading) {
                aii::draw_loader_backdrop(loaderStage, loaderProgress, width, height, loaderAlpha);
            }
            // M4.3: the strip's hover label, drawn in the *widget's* frame. A
            // 48 px window cannot hold a tooltip — an ImGui tooltip is a
            // floating window clamped to its own viewport, and "Open the
            // working directory" would be silently cut off at the strip's
            // edge, which is the trap the settings surface avoided by being a
            // region rather than a popup. Here it lands immediately right of
            // the icon it names, which is where a flyout label belongs.
            if (sidebar) sidebar->draw_tooltip_into_widget(widgetRect);
            // M9: the worker strip's hover label, by the same mechanism and for
            // the same reason — it is 48 px wide and an ImGui tooltip is a
            // floating window clamped to its own viewport. It lands in the
            // widget rather than beside the icon that raised it, which is two
            // columns to the right; that is further than the primary strip's
            // label travels, and still the only place in this app wide enough
            // to letter "scout [Working] / Read(notes.txt)" without cutting it
            // off. Only one of the two strips can be hovered at a time, so the
            // two labels cannot collide.
            if (workerStrip) workerStrip->draw_tooltip_into_widget(widgetRect);
            // Follow the panel's own height. Only the borderless window gets
            // resized: the decorated fallback has a frame to account for and
            // exists for debugging, where a fixed size is easier to reason about.
            // Clamped to kWindowH, which is now a layout bound rather than the
            // hard composition ceiling it used to be (see kWindowH).
            if (r.desired_height > band) panelH = r.desired_height - band;
            if (transparent && hwnd && r.desired_height != 0) {
                // The panel's own height plus the band the *next* frame will be
                // drawn with, so the window is already that tall when it is.
                const std::uint32_t want = std::min(panelH + nextBand, kWindowH);
                if (want != windowH) {
                    windowH = want;
                    pendingH = windowH;
                }
            }
        }

        if (auto r = renderer->waitFrameSlot(); !r) {
            log::error("wait: {}", r.error().message);
            break;
        }
        const std::uint32_t slot = renderer->frameSlot();

        // The avatar's grid and the fade that gates it, into this slot's
        // region — after the slot has been waited on, so the GPU is no longer
        // reading it. The band is what the scale and the centring are worked
        // out against, so it is passed rather than assumed.
        if (traceBand) {
            RECT wr{}, cr{};
            if (hwnd) { GetWindowRect(hwnd, &wr); GetClientRect(hwnd, &cr); }
            const bool interesting = avatarAlpha != lastTracedAlpha || band != lastTracedBand ||
                                     pendingH != 0 || nextBand != band ||
                                     (avatarWanted ? 1 : 0) != lastTracedWanted ||
                                     controller.clip() != lastTracedClip;
            if (interesting) {
                // `mic` and `draft` are the engagement inputs the user's rule
                // is written in; without them a trace can say the avatar was
                // wanted but not why anyone thought so.
                log::info("[band] f={} t={:.3f} dt={:.4f} state={} wanted={} mic={}{} draft={} "
                          "loading={} "
                          "alpha={:.4f} clip={} why={} band={} next={} h={} pendingH={} winH={} "
                          "win={}x{} client={}x{} winTop={}",
                          frameNo, t, dt, static_cast<int>(snap.state), avatarWanted ? 1 : 0,
                          engagement.mic_on ? 1 : 0, engagement.mic_hold ? "h" : "",
                          engagement.composing ? 1 : 0,
                          loading ? 1 : 0, avatarAlpha, controller.clip(), controller.reason(),
                          band, nextBand, height, pendingH, windowH,
                          static_cast<int>(wr.right - wr.left), static_cast<int>(wr.bottom - wr.top),
                          static_cast<int>(cr.right - cr.left), static_cast<int>(cr.bottom - cr.top),
                          static_cast<int>(wr.top));
            }
            lastTracedClip = controller.clip();
            lastTracedWanted = avatarWanted ? 1 : 0;
            lastTracedAlpha = avatarAlpha;
            lastTracedBand = band;
        }
        ++frameNo;

        avatarRenderer->write_slot(slot, grid, width, kAvatarH, avatarAlpha);

        if (auto r = renderer->drawFrame(nullptr); !r) {
            log::error("draw: {}", r.error().message);
            break;
        }
    }
    // The strip goes first, and unconditionally: it is a real always-on-top
    // window on the user's desktop, and an orphan left behind by an exit —
    // normal, Esc, a close, or a `break` out of the loop above — is the worst
    // outcome this feature has. It is destroyed here rather than left to the
    // unique_ptr's own scope so that it happens before the device it draws on
    // is touched by anything else in this teardown.
    // M2b.5's harness thread, before anything it touches goes: it holds a raw
    // pointer to the session and calls into the schedule book.
    if (raceThread.joinable()) {
        raceStop = true;
        raceThread.join();
    }
    sidebar.reset();
    // M9's windows with it, and before the device they draw on is touched by
    // anything else in this teardown. The worker windows go first: each one is
    // a real window on the user's desktop, and an orphan left behind by an exit
    // — normal, Esc, a close, or a `break` out of the loop above — is the worst
    // outcome this feature has, multiplied by four. Nothing is written down on
    // the way out, deliberately: no open-state and no geometry is persisted, so
    // a fresh run starts with the desktop it started with.
    workerWindows.clear();
    workerStrip.reset();
    // And the inspector with it, for the same reason and with one addition:
    // where it was is written down first. The frame loop already mirrors that
    // every frame, so this only catches a window moved on the very last one.
    if (inspector) {
        inspectorGeom = inspector->geometry();
        rememberInspector();
        inspector.reset();
    }
    // Python next, and before anything the scripts can still reach. The
    // engine's host stops the interpreter and joins its thread; the app's own
    // teardown must not be racing a script that is still posting. Nothing is
    // FreeLibrary'd — CPython does not survive being unloaded.
    scripts.stop();
    // M2b.1: schedules are not persistent, by the user's decision — but they
    // must not vanish *silently*, which the plan calls out as a thing to
    // design rather than accept. The book hands back exactly what is being
    // dropped, with enough of each payload to describe it. Today that is a log
    // line; M2b.4 adds one transcript line beside it and deliberately no
    // speech — see VoiceSession::drop_schedules() for why speaking here is
    // impossible rather than merely unwanted.
    {
        const auto dropped = aii::ScheduleBook::instance().take_pending();
        if (!dropped.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (session) session->drop_schedules(dropped);
            log::warn("[schedule] {} pending schedule{} dropped at shutdown (not persistent):",
                      dropped.size(), dropped.size() == 1 ? "" : "s");
            for (const aii::Schedule& s : dropped)
                log::warn("[schedule]   id={} kind={} label=\"{}\" due in {:.1f}s", s.id,
                          s.action.kind, s.action.label, s.seconds_until(now));
        }
    }
    // Anything still inside the debounce window goes now: a mode clicked and
    // then Esc pressed half a second later is still a change the user made.
    settings.flush();
    if (settings.take_status_change()) log::warn("{}", settings.status());

    const auto stats = renderer->takeStats();
    log::info("avatar exit: {} frames", stats.frames);
    renderer->waitIdle();
    // The overlay recorder captures `ui`, so drop it from the renderer first.
    renderer->setOverlayRecorder(nullptr);
    // Off the window before the ImGui context it feeds, and before the window
    // itself goes: a subclass left installed would outlive both.
    textInput.reset();
    ui.reset();
    session.reset();
    return 0;
}
