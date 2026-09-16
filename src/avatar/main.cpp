// avatar: the corner window of AIInterface. A small borderless, transparent,
// always-on-top window on the rend engine (D3D12 backend: on this AMD GPU
// only D3D12's composition swapchain gives per-pixel alpha; Vulkan's WSI
// composites opaque). The top shows the placeholder avatar (a spinning cube);
// below it a GDI-rendered panel carries the usage readout, the transcript
// and the Talk / Silence / Pause buttons. The voice loop itself lives in
// VoiceSession (engines from aii_core).
//
//   avatar [--opaque] [--seconds S] [--vulkan] [--say "text"] [--no-voice]
//     --opaque    decorated opaque window (fallback / debugging)
//     --seconds   quit automatically after S seconds (scripted runs)
//     --vulkan    use the Vulkan backend (transparency will not work here)
//     --say       send this text as the first user turn once the engines are up
//     --no-voice  window only, no engines (layout work)
//
//   SPACE / Talk    start listening; again to stop and send (barge-in while speaking)
//   S / Silence     stop the audio, keep the text
//   E / Pause       cancel the reply in flight
//   Esc / Q         quit
#include "rend/core/log.h"
#include "rend/core/math.h"
#include "rend/core/paths.h"
#include "rend/gpu/buffer.h"
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

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "ui/gdi_canvas.h"
#include "voice_session.h"

using namespace rend;

namespace {

// ---- layout (pixels) ----
constexpr std::uint32_t kWindowW = 360;
constexpr std::uint32_t kWindowH = 640;
constexpr std::uint32_t kCubeH = 260;                      // avatar area at the top
constexpr std::uint32_t kPanelY = kCubeH;                  // panel below it
constexpr std::uint32_t kPanelH = kWindowH - kPanelY;
constexpr float kPanelAlpha = 0.96f;
constexpr int kButtonH = 40;
constexpr int kPad = 8;

// Mirrors CubeFrame in shaders/cube.hlsl.
struct CubeFrame {
    math::Mat4 mvp;
    math::Mat4 model;
    float cameraPos[4];
    float tint[4];
};
static_assert(sizeof(CubeFrame) == 160);

// Mirrors the header in shaders/panel.hlsl.
struct PanelHeader {
    std::uint32_t winW, winH, x, y, w, h;
    float alpha;
    std::uint32_t stride; // bytes per slot region (read from slot 0 by the shader)
};
static_assert(sizeof(PanelHeader) == 32);

struct Push {
    std::uint32_t slot;
    std::uint32_t pad;
};

constexpr std::uint32_t kSlots = gpu::FrameRenderer::kFramesInFlight;
constexpr float kPi = 3.14159265358979f;

math::Mat4 rotationY(float a) {
    math::Mat4 m{};
    const float c = std::cos(a), s = std::sin(a);
    m[0] = c;
    m[2] = -s;
    m[5] = 1.0f;
    m[8] = s;
    m[10] = c;
    m[15] = 1.0f;
    return m;
}

math::Mat4 rotationX(float a) {
    math::Mat4 m{};
    const float c = std::cos(a), s = std::sin(a);
    m[0] = 1.0f;
    m[5] = c;
    m[6] = s;
    m[9] = -s;
    m[10] = c;
    m[15] = 1.0f;
    return m;
}

// Pins the window to the bottom-right of the primary monitor's work area,
// keeps it above every other window and off the taskbar.
void pinToCorner(HWND hwnd, int width, int height) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int margin = 16;
    const int x = work.right - width - margin;
    const int y = work.bottom - height - margin;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

std::string utf8FromWide(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

struct Button {
    const char* label;
    int x, y, w, h;
    bool hit(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// Draws the whole panel (usage, status, transcript, buttons) into the canvas.
// Coordinates are panel-local; the shader places the panel at kPanelY.
void drawPanel(aii::GdiCanvas& canvas, const aii::VoiceSession::Snapshot& snap,
               const std::array<Button, 3>& buttons, bool voiceEnabled) {
    using aii::Color;
    const Color bg{22, 24, 30};
    const Color dim{150, 155, 170};
    const Color fg{232, 234, 240};
    const Color user{255, 196, 120};
    const Color claude{150, 205, 255};
    const Color accent{214, 84, 74};
    const Color button{54, 58, 72};
    const int w = canvas.width();
    canvas.clear(bg);

    int y = kPad;
    // Usage readout under the avatar.
    const std::string usage = snap.usage.empty() ? "usage: (after the first reply)" : snap.usage;
    y += canvas.text(kPad, y, w - 2 * kPad, 36, usage, 13, dim, false, DT_WORD_ELLIPSIS) + 4;
    // State + status line.
    std::string status = voiceEnabled ? std::string("[") + aii::VoiceSession::state_name(snap.state) + "] " + snap.status
                                      : "(no voice: --no-voice)";
    canvas.text(kPad, y, w - 2 * kPad, 18, status, 13,
                snap.state == aii::VoiceSession::State::Listening ? accent : fg, true,
                DT_END_ELLIPSIS | DT_SINGLELINE);
    y += 24;

    // Worker strip: one line per background instance, with what it is doing.
    if (!snap.workers.empty()) {
        const Color barBg{34, 38, 48};
        const int rows = static_cast<int>(snap.workers.size());
        const int stripH = rows * 17 + 8;
        canvas.fill_rect(kPad, y, w - 2 * kPad, stripH, barBg);
        int wy = y + 4;
        for (const auto& k : snap.workers) {
            Color tone = dim;
            switch (k.state) {
                case aii::WorkerPool::State::Working: tone = Color{140, 225, 170}; break;
                case aii::WorkerPool::State::Done: tone = Color{150, 205, 255}; break;
                case aii::WorkerPool::State::Failed: tone = accent; break;
                default: break;
            }
            const std::string line =
                k.name + " [" + aii::worker_state_name(k.state) + "] " + k.activity +
                (k.tool_calls ? "  x" + std::to_string(k.tool_calls) : std::string());
            canvas.text(kPad + 6, wy, w - 2 * kPad - 12, 16, line, 12, tone, false,
                        DT_END_ELLIPSIS | DT_SINGLELINE);
            wy += 17;
        }
        y += stripH + 6;
    }

    // Transcript: newest at the bottom, filling upward.
    const int transcriptTop = y;
    const int transcriptBottom = buttons[0].y - kPad;
    const int textW = w - 2 * kPad;
    int cursor = transcriptBottom;
    if (!snap.partial.empty()) {
        const std::string line = "You: " + snap.partial;
        const int h = canvas.measure(textW, line, 14);
        cursor -= h;
        canvas.text(kPad, cursor, textW, h, line, 14, accent);
        cursor -= 4;
    }
    for (auto it = snap.lines.rbegin(); it != snap.lines.rend() && cursor > transcriptTop; ++it) {
        if (it->text.empty()) continue;
        const std::string line = (it->user ? "You: " : "Claude: ") + it->text;
        const int h = canvas.measure(textW, line, 14);
        cursor -= h;
        if (cursor < transcriptTop) {
            // Clip the oldest visible line at the top rather than skipping it.
            canvas.text(kPad, transcriptTop, textW, cursor + h - transcriptTop, line, 14,
                        it->user ? user : claude);
            break;
        }
        canvas.text(kPad, cursor, textW, h, line, 14, it->user ? user : claude);
        cursor -= 6;
    }

    // Buttons.
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        const Button& b = buttons[i];
        Color fill = button;
        if (i == 0 && snap.state == aii::VoiceSession::State::Listening) fill = accent;
        canvas.fill_rect(b.x, b.y, b.w, b.h, fill);
        canvas.frame_rect(b.x, b.y, b.w, b.h, Color{90, 95, 115});
        canvas.text(b.x, b.y + (b.h - 18) / 2, b.w, 18, b.label, 14, fg, true, DT_CENTER | DT_SINGLELINE);
    }
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    SetConsoleOutputCP(CP_UTF8);
    bool opaque = false;
    bool vulkan = false;
    bool voiceEnabled = true;
    double seconds = -1.0;
    std::string sayText;
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

    // ---- GPU resources: cube frames (binding 40) and the panel pixels (binding 41) ----
    auto tableResult = gpu::DescriptorTable::create(
        *device, gpu::DescriptorTableDesc{.maxTextures = 1, .userStorageBuffers = 2});
    if (!tableResult) {
        log::error("descriptor table: {}", tableResult.error().message);
        return 1;
    }
    auto table = std::move(tableResult).value();

    auto makeHostBuffer = [&](std::uint64_t size, const char* what) -> std::unique_ptr<gpu::Buffer> {
        auto r = gpu::Buffer::create(*device, {.size = size,
                                               .usage = gpu::kUsageStorage,
                                               .location = gpu::MemoryLocation::HostVisible});
        if (!r) {
            log::error("{} buffer: {}", what, r.error().message);
            return nullptr;
        }
        std::memset(r.value()->mapped(), 0, static_cast<std::size_t>(size));
        return std::move(r).value();
    };
    auto cubeBuffer = makeHostBuffer(std::uint64_t{kSlots} * sizeof(CubeFrame), "cube");
    const std::uint32_t panelStride = static_cast<std::uint32_t>(sizeof(PanelHeader)) + kWindowW * kPanelH * 4;
    auto panelBuffer = makeHostBuffer(std::uint64_t{kSlots} * panelStride, "panel");
    if (!cubeBuffer || !panelBuffer) return 1;
    table->writeStorageBuffer(table->userStorageBinding(0), *cubeBuffer);
    table->writeStorageBuffer(table->userStorageBinding(1), *panelBuffer);

    const auto shaderDir = executableDirectory() / "data" / "shaders";
    auto loadShader = [&](const char* file) -> std::unique_ptr<gpu::Shader> {
        auto r = gpu::Shader::createFromFile(*device, shaderDir / file);
        if (!r) {
            log::error("shader {}: {}", file, r.error().message);
            return nullptr;
        }
        return std::move(r).value();
    };
    auto cubeVs = loadShader("cube.vert.spv");
    auto cubePs = loadShader("cube.frag.spv");
    auto panelVs = loadShader("panel.vert.spv");
    auto panelPs = loadShader("panel.frag.spv");
    if (!cubeVs || !cubePs || !panelVs || !panelPs) return 1;

    auto makePipeline = [&](gpu::Shader& vs, gpu::Shader& ps, const char* what) -> std::unique_ptr<gpu::Pipeline> {
        gpu::GraphicsPipelineDesc desc{};
        desc.vertexShader = &vs;
        desc.fragmentShader = &ps;
        desc.colorFormat = swapchain->imageFormat();
        desc.pushConstantBytes = sizeof(Push);
        desc.descriptorTable = table.get();
        auto r = gpu::Pipeline::createGraphics(*device, desc);
        if (!r) {
            log::error("{} pipeline: {}", what, r.error().message);
            return nullptr;
        }
        return std::move(r).value();
    };
    auto cubePipeline = makePipeline(*cubeVs, *cubePs, "cube");
    auto panelPipeline = makePipeline(*panelVs, *panelPs, "panel");
    if (!cubePipeline || !panelPipeline) return 1;

    renderer->setFramePasses({
        gpu::FramePass{
            .point = gpu::PassPoint::InScene,
            .name = "avatar-cube",
            .record =
                [&](gpu::CommandContext& cmd, const gpu::PassContext& ctx) {
                    cmd.setViewport(0.0f, 0.0f, static_cast<float>(ctx.width), static_cast<float>(kCubeH));
                    cmd.setScissor(0, 0, ctx.width, kCubeH);
                    cmd.bindPipeline(*cubePipeline);
                    cmd.bindDescriptorTable(*cubePipeline, *table);
                    const Push push{.slot = ctx.slot, .pad = 0};
                    cmd.pushConstants(*cubePipeline, &push, sizeof(push));
                    cmd.draw(36);
                },
        },
        gpu::FramePass{
            .point = gpu::PassPoint::InScene,
            .name = "avatar-panel",
            .record =
                [&](gpu::CommandContext& cmd, const gpu::PassContext& ctx) {
                    cmd.setViewport(0.0f, 0.0f, static_cast<float>(ctx.width), static_cast<float>(ctx.height));
                    cmd.setScissor(0, 0, ctx.width, ctx.height);
                    cmd.bindPipeline(*panelPipeline);
                    cmd.bindDescriptorTable(*panelPipeline, *table);
                    const Push push{.slot = ctx.slot, .pad = 0};
                    cmd.pushConstants(*panelPipeline, &push, sizeof(push));
                    cmd.draw(6);
                },
        },
    });

    // ---- the panel canvas and its buttons (panel-local coordinates) ----
    aii::GdiCanvas canvas(static_cast<int>(kWindowW), static_cast<int>(kPanelH));
    const int buttonW = (static_cast<int>(kWindowW) - 4 * kPad) / 3;
    const int buttonY = static_cast<int>(kPanelH) - kButtonH - kPad;
    const std::array<Button, 3> buttons{{
        {"Talk", kPad, buttonY, buttonW, kButtonH},
        {"Silence", 2 * kPad + buttonW, buttonY, buttonW, kButtonH},
        {"Pause", 3 * kPad + 2 * buttonW, buttonY, buttonW, kButtonH},
    }};
    std::array<std::uint64_t, kSlots> slotPanelVersion{};
    std::uint64_t panelVersion = 1;
    std::string lastPanelKey;

    log::info("avatar live: {}x{} {} {}", extent.width, extent.height,
              transparent ? "transparent" : "opaque", gpu::apiName(api));

    const auto start = std::chrono::steady_clock::now();
    std::uint32_t width = extent.width;
    std::uint32_t height = extent.height;
    bool running = true;
    bool saidOnce = sayText.empty();
    while (running) {
        for (const auto& event : backend->pumpEvents()) {
            switch (event.type) {
            case platform::Event::Type::CloseRequested:
                running = false;
                break;
            case platform::Event::Type::KeyDown:
                if (event.key == platform::Key::Escape || event.key == platform::Key::Q) running = false;
                else if (session && event.key == platform::Key::Space) session->toggle_talk();
                else if (session && event.key == platform::Key::S) session->silence();
                else if (session && event.key == platform::Key::E) session->pause();
                break;
            case platform::Event::Type::MouseButtonDown:
                if (session && event.button == platform::MouseButton::Left) {
                    const float px = event.mouseX;
                    const float py = event.mouseY - static_cast<float>(kPanelY);
                    if (buttons[0].hit(px, py)) session->toggle_talk();
                    else if (buttons[1].hit(px, py)) session->silence();
                    else if (buttons[2].hit(px, py)) session->pause();
                }
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
        if (seconds >= 0.0 && t >= seconds) running = false;
        if (width == 0 || height == 0) continue;

        // ---- voice loop tick + panel redraw when anything changed ----
        aii::VoiceSession::Snapshot snap;
        if (session) {
            session->update();
            snap = session->snapshot();
            if (!saidOnce && snap.state == aii::VoiceSession::State::Idle) {
                saidOnce = true;
                session->say(sayText);
            }
        }
        std::string panelKey = aii::VoiceSession::state_name(snap.state);
        panelKey += '|' + snap.status + '|' + snap.usage + '|' + snap.partial;
        for (const auto& l : snap.lines) panelKey += (l.user ? "\nU:" : "\nC:") + l.text;
        for (const auto& k : snap.workers) {
            panelKey += "\nW:" + k.name + aii::worker_state_name(k.state) + k.activity +
                        std::to_string(k.tool_calls);
        }
        if (panelKey != lastPanelKey) {
            lastPanelKey = std::move(panelKey);
            drawPanel(canvas, snap, buttons, session != nullptr);
            ++panelVersion;
        }

        if (auto r = renderer->waitFrameSlot(); !r) {
            log::error("wait: {}", r.error().message);
            break;
        }
        const std::uint32_t slot = renderer->frameSlot();

        // Cube: this slot's frame record.
        const float aspect = static_cast<float>(width) / static_cast<float>(kCubeH);
        const math::Vec3 eye{0.0f, 0.0f, 3.2f};
        const math::Mat4 model = math::mul(rotationY(static_cast<float>(t) * 0.9f),
                                           rotationX(static_cast<float>(t) * 0.55f));
        const math::Mat4 view = math::lookAt(eye, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
        const math::Mat4 proj = math::perspective(40.0f * kPi / 180.0f, aspect, 0.1f, 10.0f);
        CubeFrame frame{};
        frame.mvp = math::mul(math::mul(proj, view), model);
        frame.model = model;
        frame.cameraPos[0] = eye.x;
        frame.cameraPos[1] = eye.y;
        frame.cameraPos[2] = eye.z;
        frame.tint[0] = frame.tint[1] = frame.tint[2] = 1.0f;
        if (snap.state == aii::VoiceSession::State::Listening) {
            frame.tint[0] = 1.0f; frame.tint[1] = 0.55f; frame.tint[2] = 0.5f;  // reddish while listening
        } else if (snap.state == aii::VoiceSession::State::Thinking) {
            const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(t) * 6.0f);
            frame.tint[0] = frame.tint[1] = frame.tint[2] = pulse;
        }
        static_cast<CubeFrame*>(cubeBuffer->mapped())[slot] = frame;

        // Panel: copy the canvas into this slot's region when it is stale.
        if (slotPanelVersion[slot] != panelVersion) {
            slotPanelVersion[slot] = panelVersion;
            auto* region = static_cast<std::uint8_t*>(panelBuffer->mapped()) + std::size_t{slot} * panelStride;
            PanelHeader header{.winW = width, .winH = height, .x = 0, .y = kPanelY,
                               .w = kWindowW, .h = kPanelH, .alpha = transparent ? kPanelAlpha : 1.0f,
                               .stride = panelStride};
            std::memcpy(region, &header, sizeof(header));
            std::memcpy(region + sizeof(header), canvas.pixels(), canvas.bytes());
        }

        if (auto r = renderer->drawFrame(nullptr); !r) {
            log::error("draw: {}", r.error().message);
            break;
        }
    }
    const auto stats = renderer->takeStats();
    log::info("avatar exit: {} frames", stats.frames);
    renderer->waitIdle();
    session.reset();
    return 0;
}
