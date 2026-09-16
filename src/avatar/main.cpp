// avatar: the corner window of AIInterface. A small borderless, transparent,
// always-on-top window on the rend engine (D3D12 backend: on this AMD GPU
// only D3D12's composition swapchain gives per-pixel alpha; Vulkan's WSI
// composites opaque). For now it draws a spinning cube as the placeholder
// avatar; the voice loop's transcript, usage readout and controls come next.
//
//   avatar [--opaque] [--size N] [--seconds S] [--vulkan]
//     --opaque   decorated opaque window (fallback / debugging)
//     --size N   window edge in pixels (default 320)
//     --seconds  quit automatically after S seconds (scripted runs)
//     --vulkan   use the Vulkan backend (transparency will not work here)
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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace rend;

namespace {

// Mirrors CubeFrame in shaders/cube.hlsl.
struct CubeFrame {
    math::Mat4 mvp;
    math::Mat4 model;
    float cameraPos[4];
    float tint[4];
};
static_assert(sizeof(CubeFrame) == 160);

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

} // namespace

int main(int argc, char** argv) {
    bool opaque = false;
    bool vulkan = false;
    int size = 320;
    double seconds = -1.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--opaque") == 0) {
            opaque = true;
        } else if (std::strcmp(argv[i], "--vulkan") == 0) {
            vulkan = true;
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            size = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        }
    }
    const bool transparent = !opaque;
    const gpu::Api api = vulkan ? gpu::Api::Vulkan : gpu::Api::D3D12;

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
        .size = {static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size)},
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
        pinToCorner(hwnd, size, size);
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
    // Premultiplied (0,0,0,0): the desktop shows through wherever the cube is not.
    renderer->setClearColor(0.0f, 0.0f, 0.0f, transparent ? 0.0f : 1.0f);

    // ---- cube pass: one per-slot storage buffer at user binding 40 ----
    auto tableResult = gpu::DescriptorTable::create(
        *device, gpu::DescriptorTableDesc{.maxTextures = 1, .userStorageBuffers = 1});
    if (!tableResult) {
        log::error("descriptor table: {}", tableResult.error().message);
        return 1;
    }
    auto table = std::move(tableResult).value();

    auto bufferResult = gpu::Buffer::create(*device, {
        .size = std::uint64_t{kSlots} * sizeof(CubeFrame),
        .usage = gpu::kUsageStorage,
        .location = gpu::MemoryLocation::HostVisible,
    });
    if (!bufferResult) {
        log::error("frame buffer: {}", bufferResult.error().message);
        return 1;
    }
    auto frameBuffer = std::move(bufferResult).value();
    std::memset(frameBuffer->mapped(), 0, static_cast<std::size_t>(frameBuffer->size()));
    table->writeStorageBuffer(table->userStorageBinding(0), *frameBuffer);

    const auto shaderDir = executableDirectory() / "data" / "shaders";
    auto vs = gpu::Shader::createFromFile(*device, shaderDir / "cube.vert.spv");
    auto ps = gpu::Shader::createFromFile(*device, shaderDir / "cube.frag.spv");
    if (!vs || !ps) {
        log::error("shaders in {}: {}", shaderDir.string(),
                   !vs ? vs.error().message : ps.error().message);
        return 1;
    }
    auto vertexShader = std::move(vs).value();
    auto fragmentShader = std::move(ps).value();

    gpu::GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = vertexShader.get();
    pipelineDesc.fragmentShader = fragmentShader.get();
    pipelineDesc.colorFormat = swapchain->imageFormat();
    pipelineDesc.pushConstantBytes = sizeof(Push);
    pipelineDesc.descriptorTable = table.get();
    auto pipelineResult = gpu::Pipeline::createGraphics(*device, pipelineDesc);
    if (!pipelineResult) {
        log::error("pipeline: {}", pipelineResult.error().message);
        return 1;
    }
    auto pipeline = std::move(pipelineResult).value();

    renderer->setFramePasses({gpu::FramePass{
        .point = gpu::PassPoint::InScene,
        .name = "avatar-cube",
        .record =
            [&](gpu::CommandContext& cmd, const gpu::PassContext& ctx) {
                cmd.bindPipeline(*pipeline);
                cmd.bindDescriptorTable(*pipeline, *table);
                const Push push{.slot = ctx.slot, .pad = 0};
                cmd.pushConstants(*pipeline, &push, sizeof(push));
                cmd.draw(36);
            },
    }});

    log::info("avatar live: {}x{} {} {}", extent.width, extent.height,
              transparent ? "transparent" : "opaque", gpu::apiName(api));

    const auto start = std::chrono::steady_clock::now();
    std::uint32_t width = extent.width;
    std::uint32_t height = extent.height;
    bool running = true;
    while (running) {
        for (const auto& event : backend->pumpEvents()) {
            switch (event.type) {
            case platform::Event::Type::CloseRequested:
                running = false;
                break;
            case platform::Event::Type::KeyDown:
                if (event.key == platform::Key::Escape || event.key == platform::Key::Q) {
                    running = false;
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
        if (seconds >= 0.0 && t >= seconds) {
            running = false;
        }
        if (width == 0 || height == 0) {
            continue;
        }

        if (auto r = renderer->waitFrameSlot(); !r) {
            log::error("wait: {}", r.error().message);
            break;
        }
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
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
        auto* slots = static_cast<CubeFrame*>(frameBuffer->mapped());
        slots[renderer->frameSlot()] = frame;

        if (auto r = renderer->drawFrame(nullptr); !r) {
            log::error("draw: {}", r.error().message);
            break;
        }
    }
    const auto stats = renderer->takeStats();
    log::info("avatar exit: {} frames", stats.frames);
    renderer->waitIdle();
    return 0;
}
