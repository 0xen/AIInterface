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
//     --script    run this Python file (M2.6), repeatable, ahead of the user's
//                 own. The viewer's flag, and for the same use: a harness has
//                 to be up before a script that never returns starts. With no
//                 --script and an empty %APPDATA%\AIInterface\scripts, no
//                 Python DLL is loaded at all.
//     --no-scripts  discover nothing, whatever is in scripts\.
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "avatar_controller.h"
#include "avatar_def.h"
#include "avatar_renderer.h"
#include "avatar_ui.h"
#include "bus_bindings.h"
#include "core/app_bus.h"
#include "script_host.h"
#include "core/button_registry.h"
#include "core/config.h"
#include "core/worker_pool.h"
#include "imgui_layer.h"
#include "loader_anim.h"
#include "settings.h"
#include "sidebar_window.h"
#include "voice_session.h"
#include "watermark.h"
#include "win_text_input.h"

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
// M1.5: the loading screen hands over to the avatar across this many seconds,
// the loader fading out as the avatar fades in. Long enough to read as the
// widget settling, short enough that it is not a dissolve you wait through.
constexpr float kHandoffSeconds = 0.42f;
// M1.6: how long the avatar takes to fade in or out when the visibility mode
// changes, or when "shown when talking" follows the session in and out of a
// turn. Much shorter than the handoff on purpose — this is an answer to
// something that just happened, not an opening — but long enough that the
// listening → thinking → speaking sequence reads as a fade and not a blink.
constexpr float kVisibilitySeconds = 0.16f;

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
    std::string sayText;
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
    {
        // Wide command line so Japanese survives (argv is ANSI-mangled).
        int wargc = 0;
        wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        for (int i = 1; wargv && i < wargc; ++i) {
            const std::wstring a = wargv[i];
            if (a == L"--opaque") opaque = true;
            else if (a == L"--vulkan") vulkan = true;
            else if (a == L"--no-voice") voiceEnabled = false;
            else if (a == L"--seconds" && i + 1 < wargc) seconds = _wtof(wargv[++i]);
            else if (a == L"--say" && i + 1 < wargc) sayText = utf8FromWide(wargv[++i]);
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
    bool running = true;
    bool saidOnce = sayText.empty();
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
    // M1.6 visibility fade, 0..1. Negative means "no first frame yet": it then
    // snaps to whatever the mode already asks for, so the default mode does
    // not fade the avatar band in from nothing behind the loading screen.
    float visFade = -1.0f;

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
        // is anchored to the bottom-right corner, so growing it moves its
        // *top* edge: opening the chat lifts the panel 265 px and the strip
        // has to arrive with it. A dock done anywhere later in the frame would
        // leave the strip one present behind, which on a 265 px jump is not a
        // subtlety — it is a visible slide.
        //
        // Every frame, not only on a resize: the widget can also move because
        // the work area changed or the watermark margin was recomputed, and
        // SidebarWindow::dock() is a no-op when nothing has actually moved.
        if (hwnd) GetWindowRect(hwnd, &widgetRect);
        if (sidebar) sidebar->dock(widgetRect, band);

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
        if (width == 0 || height == 0) continue;

        // ---- voice loop tick ----
        aii::VoiceSession::Snapshot snap;
        if (session) {
            session->update();
            snap = session->snapshot();
            if (!saidOnce && snap.state == aii::VoiceSession::State::Idle) {
                saidOnce = true;
                session->say(sayText);
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

        // ---- the loading overlay, and the handoff out of it ----
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

        // M1.6: the same alpha the handoff drives, now also carrying the
        // visibility mode. Multiplying rather than choosing is what makes the
        // end of loading right in every mode: in a mode that does not want the
        // avatar yet, `vis` is already 0, so the handoff fades the loader out
        // to nothing instead of crossfading into an avatar that then vanishes.
        const bool avatarWanted = aii::avatar_visible(uiState.avatar_mode, snap.state);
        if (visFade < 0.0f) visFade = avatarWanted ? 1.0f : 0.0f;
        visFade = std::clamp(visFade + (avatarWanted ? dt : -dt) / kVisibilitySeconds, 0.0f, 1.0f);
        const float vis = smoothstep(0.0f, 1.0f, visFade);
        avatarAlpha = smoothstep(0.30f, 1.00f, handoff) * vis;
        // The band is reserved while anything might still draw in it, and
        // always while the loading screen is up: that overlay covers the whole
        // window and is centred in it, so a mode that hides the avatar gives
        // the 260 px back when the loader leaves rather than shrinking the
        // window out from under it. The resize itself goes through pendingH and
        // lands at the top of the next frame — never from inside this one (M1.4).
        //
        // What the band *will* be. It is not used until the top of the next
        // frame, where it is applied together with the window height that goes
        // with it — see `band` above the event pump. The band moves every
        // control in the panel by 260 px, so laying the panel out against a new
        // band while the window still has the old height puts every control
        // that far from where it is on screen; that mismatch is what made a
        // click on Talk read as hold-to-dictate (see the note at `band`).
        nextBand = (loading || visFade > 0.0f) ? kAvatarH : 0;
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
            const aii::AvatarUiResult r =
                aii::draw_avatar_ui(uiState, snap, avatarOptions, session != nullptr,
                                    session && session->mic_open(),
                                    session && session->mic_hold(), kWindowW, band, submit);
            if (session) {
                if (r.talk_pressed) session->talk_pressed();
                if (r.talk_released) session->talk_released(r.talk_over_button, r.talk_held);
                if (r.stop) session->stop();
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
                // Only reaches here once the panel has satisfied itself the
                // session can take it; say() refuses the rest anyway.
                if (!r.send_text.empty()) session->say(r.send_text);
            }
            // Mirrored out of the panel state every frame rather than from the
            // buttons that write it: the setters are no-ops when the value
            // already matches, so "on change" is decided in one place instead
            // of every control that touches a persisted field remembering to
            // say so. The write itself is debounced inside Settings.
            settings.set_bool("panel", "chat_open", uiState.chat_open);
            settings.set_bool("panel", "muted", uiState.muted);
            settings.set_enum("panel", "avatar_mode", aii::kAvatarVisibilityNames,
                              aii::kAvatarVisibilityCount,
                              static_cast<int>(uiState.avatar_mode));
            // M8.3. One string, "en" / "ja" / "en,ja", rather than two
            // booleans: a file can then never hold the state the app has no
            // answer for, because there is no spelling of "neither".
            settings.set_string("language", "enabled",
                                aii::language_spec({uiState.lang_english, uiState.lang_japanese}));
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
    sidebar.reset();
    // Python next, and before anything the scripts can still reach. The
    // engine's host stops the interpreter and joins its thread; the app's own
    // teardown must not be racing a script that is still posting. Nothing is
    // FreeLibrary'd — CPython does not survive being unloaded.
    scripts.stop();
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
