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
//     --clip      which clip to play, instead of the definition's default.
//                 Nothing maps state to clips yet (M2.4); until it does this
//                 is how a clip gets exercised at all.
//     --sprite    force an accessory on, repeatable, "all" for every one.
//                 Same reason: M2.4 decides when a thought bubble belongs on
//                 screen, so until then this is the only way to see one.
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
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "avatar_def.h"
#include "avatar_renderer.h"
#include "avatar_ui.h"
#include "core/config.h"
#include "imgui_layer.h"
#include "loader_anim.h"
#include "voice_session.h"
#include "win_text_input.h"

using namespace rend;

namespace {

// ---- layout (pixels) ----
constexpr std::uint32_t kWindowW = 360;
// Starting height, and also the **ceiling** the panel may never exceed.
// Transparency here comes from a DirectComposition visual whose content is the
// swapchain, and the engine commits that visual once, at creation. Growing the
// swapchain afterwards works — it logs the new size — but DWM keeps compositing
// the visual at the size it was committed at, so everything below the original
// height is simply never painted: the window shows the desktop there and the
// bottom row of controls is cut in half. Shrinking is unaffected, which is why
// this never showed until the message field pushed the chat-open layout past
// the old 640. Fixing it properly means re-committing in the engine; until
// then the window is created at its largest and only ever shrinks from here,
// with enough headroom for the tallest layout (avatar band + open chat + a
// four-line message field) and some to spare.
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

// Places the window in the bottom-right of the primary monitor's work area.
// The anchor is the bottom edge, so a height change grows or shrinks the
// window upward and the corner it sits in never moves.
void placeInCorner(HWND hwnd, int width, int height) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(hwnd, HWND_TOPMOST, work.right - width - kCornerMargin,
                 work.bottom - height - kCornerMargin, width, height, SWP_NOACTIVATE);
}

// Keeps the window above every other one and off the taskbar, then places it.
void pinToCorner(HWND hwnd, int width, int height) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
    placeInCorner(hwnd, width, height);
}

std::string utf8FromWide(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
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
    std::string clipName;
    std::vector<std::string> spriteNames;
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
            else if (a == L"--avatar" && i + 1 < wargc) avatarName = utf8FromWide(wargv[++i]);
            else if (a == L"--clip" && i + 1 < wargc) clipName = utf8FromWide(wargv[++i]);
            else if (a == L"--sprite" && i + 1 < wargc)
                spriteNames.push_back(utf8FromWide(wargv[++i]));
        }
        if (wargv) LocalFree(wargv);
    }
    const bool transparent = !opaque;
    const gpu::Api api = vulkan ? gpu::Api::Vulkan : gpu::Api::D3D12;

    // The voice loop loads its engines in the background while the window comes up.
    std::unique_ptr<aii::VoiceSession> session;
    if (voiceEnabled) session = std::make_unique<aii::VoiceSession>(aii::Config::from_env());

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
        pinToCorner(hwnd, kWindowW, kWindowH);
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
    {
        const std::string override = aii::env_or("AII_AVATAR_DIR", "");
        const std::filesystem::path dir = override.empty()
                                              ? aii::seed_avatar_definition(avatarName)
                                              : std::filesystem::path(override);
        avatarSource.open(dir, clipName, spriteNames);
    }
    aii::AvatarGrid grid;

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

    log::info("avatar live: {}x{} {} {}", extent.width, extent.height,
              transparent ? "transparent" : "opaque", gpu::apiName(api));

    const auto start = std::chrono::steady_clock::now();
    double lastT = 0.0;
    std::uint32_t windowH = kWindowH;  // what the OS window was last set to
    // Resizing the window from inside the frame does not come back as a
    // Resized event, so the swapchain has to be told separately — and only
    // between frames, never after the panel has been built for the old size.
    std::uint32_t pendingH = 0;
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
    while (running) {
        // Which half of the input owns the keyboard this frame. The panel now
        // has a text field, so the hotkeys below have to stand down while it
        // has focus — otherwise typing a space toggles the microphone and a
        // typed "q" quits the app. Read from the last completed frame, which
        // is what ImGui's own backends do.
        const bool uiHasKeyboard = ui && ui->wants_keyboard();
        for (const auto& event : backend->pumpEvents()) {
            // The mouse only. Keys reach ImGui through the subclass alone, so
            // forwarding them here as well would deliver each one twice.
            if (ui) ui->handle_event(event);
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
                else if (session && event.key == platform::Key::S) session->silence();
                else if (session && event.key == platform::Key::E) session->pause();
                break;
            case platform::Event::Type::KeyUp:
                if (event.key == platform::Key::Space) releaseSpace(true);
                break;
            case platform::Event::Type::Resized:
                width = event.size.width;
                height = event.size.height;
                renderer->resize(width, height);
                break;
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

        // Clip playback and the hot-reload poll. It runs before the early-out
        // paths below so that a definition fixed while the window is hidden
        // is picked up all the same.
        avatarSource.update(dt);
        if (avatarSource.take_status_change()) {
            if (avatarSource.status_ok()) log::info("{}", avatarSource.status());
            else log::warn("{}", avatarSource.status());
        }
        // The same band the grid is drawn into below: the stage is sized to
        // it, so a mismatch would lay the art out for a window we do not have.
        avatarSource.compose(grid, width, kAvatarH);

        // The height the panel asked for last frame. Everything that sizes
        // itself from the window reads `height`, the loading overlay included,
        // so the swapchain has to follow the window rather than be cropped by
        // it — the loader centres itself, and a stale height puts it low
        // enough to land on the panel.
        if (pendingH != 0) {
            height = pendingH;
            pendingH = 0;
            // The OS window is moved here too, immediately before the
            // swapchain that has to match it. Doing it where the panel asked
            // (mid-frame) left one present in between, in which DWM showed the
            // old surface pinned to the top of a window that had already grown
            // — the panel jumping to the ceiling with bare desktop under it.
            // Invisible at one resize per chat click, obvious now that "shown
            // when talking" resizes on every turn.
            if (transparent && hwnd) placeInCorner(hwnd, static_cast<int>(kWindowW),
                                                   static_cast<int>(height));
            renderer->resize(width, height);
        }

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
        // window out from under it. The resize itself goes through pendingH
        // below — never from inside the frame (M1.4).
        const std::uint32_t band = (loading || visFade > 0.0f) ? kAvatarH : 0;
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
            ui->begin_frame(width, height, dt);
            const bool submit = textInput && textInput->take_submit();
            const aii::AvatarUiResult r =
                aii::draw_avatar_ui(uiState, snap, session != nullptr,
                                    session && session->mic_open(), kWindowW, band, submit);
            if (session) {
                if (r.talk_pressed) session->talk_pressed();
                if (r.talk_released) session->talk_released(r.talk_over_button, r.talk_held);
                if (r.silence) session->silence();
                if (r.pause) session->pause();
                // Only reaches here once the panel has satisfied itself the
                // session can take it; say() refuses the rest anyway.
                if (!r.send_text.empty()) session->say(r.send_text);
            }
            // The scrim and the caption belong to the loader, not the panel:
            // the loading screen has to look the same whether or not there is
            // anything underneath it. The foreground draw list puts them over
            // the panel; the cubes then go over them in the overlay recorder.
            if (loading) {
                aii::draw_loader_backdrop(loaderStage, loaderProgress, width, height, loaderAlpha);
            }
            // Follow the panel's own height. Only the borderless window gets
            // resized: the decorated fallback has a frame to account for and
            // exists for debugging, where a fixed size is easier to reason about.
            // Clamped to the committed composition height: a window taller
            // than that would claim space DWM never paints, which reads as the
            // bottom of the panel having been cut off.
            if (transparent && hwnd && r.desired_height != 0) {
                const std::uint32_t want = std::min(r.desired_height, kWindowH);
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
