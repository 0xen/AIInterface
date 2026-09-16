#include "avatar_renderer.h"

#include "rend/gpu/buffer.h"
#include "rend/gpu/command_context.h"
#include "rend/gpu/descriptor_table.h"
#include "rend/gpu/device.h"
#include "rend/gpu/frame_renderer.h"
#include "rend/gpu/pipeline.h"
#include "rend/gpu/shader.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace aii {

namespace gpu = rend::gpu;

namespace {

// The twin of the layout constants in shaders/avatar_grid.hlsl. One region
// per frame slot: a header, then both layers at their maximum size, so the
// live grid size only ever changes what the header says.
constexpr std::uint32_t kSlots = gpu::FrameRenderer::kFramesInFlight;
constexpr std::size_t kLayerBytes = std::size_t{kAvatarMaxCells} * sizeof(std::uint32_t);
constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kSlotBytes = kHeaderBytes + 2 * kLayerBytes;

// The header, byte for byte as the shader's two Load4 calls read it.
struct SlotHeader {
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t scale;
  std::int32_t origin_x;
  std::int32_t origin_y;
  float alpha;
  std::uint32_t pad[2];
};
static_assert(sizeof(SlotHeader) == kHeaderBytes);

// The push block. Only the frame slot rides it; everything else lives in the
// slot's own region, so a recording of this pass stays valid however the grid
// and the fade change.
struct AvatarPush {
  std::uint32_t slot;
  std::uint32_t pad;
};

// Stand-in art (M2.1). '#' is the body, 'o' the face, '.' transparent.
// Replaced wholesale by the authored slime in M2.3.
constexpr const char* kBlob[16] = {
    "................",
    ".....######.....",
    "...##########...",
    "..############..",
    ".##############.",
    ".##############.",
    ".###oo####oo###.",
    ".###oo####oo###.",
    ".##############.",
    ".##############.",
    ".####o####o####.",
    ".#####oooo#####.",
    ".##############.",
    "################",
    "################",
    "................",
};

}  // namespace

void AvatarGrid::resize(std::uint32_t w, std::uint32_t h) {
  width = std::clamp(w, 1u, kAvatarMaxGrid);
  height = std::clamp(h, 1u, kAvatarMaxGrid);
  clear();
}

void AvatarGrid::clear() {
  base.fill(0);
  overlay.fill(0);
}

void AvatarGrid::set(AvatarLayer layer, std::uint32_t x, std::uint32_t y, std::uint32_t rgba) {
  if (x >= width || y >= height) return;
  auto& cells = layer == AvatarLayer::Base ? base : overlay;
  cells[y * width + x] = rgba;
}

void avatar_placeholder_blob(AvatarGrid& grid) {
  grid.resize(16, 16);
  // The placeholder is a character, not a stage: it wants the derived scale
  // that fills the band, and it has to say so in case a stage ran before it.
  grid.scale = 0;
  constexpr std::uint32_t kBody = avatar_rgba(0, 0, 0, 255);
  constexpr std::uint32_t kFace = avatar_rgba(255, 255, 255, 255);
  for (std::uint32_t y = 0; y < 16; ++y) {
    for (std::uint32_t x = 0; x < 16; ++x) {
      const char c = kBlob[y][x];
      if (c == '#') grid.set(AvatarLayer::Base, x, y, kBody);
      else if (c == 'o') grid.set(AvatarLayer::Base, x, y, kFace);
    }
  }
  // A shine in the overlay layer rather than painted into the body: this is
  // the accessory layer M2.4 drives, and having something in it from the
  // start means the composite is exercised rather than assumed.
  for (std::uint32_t y = 3; y <= 4; ++y) {
    for (std::uint32_t x = 4; x <= 5; ++x) grid.set(AvatarLayer::Overlay, x, y, kFace);
  }
}

AvatarRenderer::~AvatarRenderer() = default;

std::unique_ptr<AvatarRenderer> AvatarRenderer::create(const gpu::Device& device,
                                                       gpu::Format color_format,
                                                       gpu::DescriptorTable& table,
                                                       std::uint32_t storage_index,
                                                       const std::filesystem::path& shader_dir,
                                                       std::string* error) {
  auto fail = [&](std::string message) -> std::unique_ptr<AvatarRenderer> {
    if (error) *error = std::move(message);
    return nullptr;
  };
  std::unique_ptr<AvatarRenderer> self(new AvatarRenderer());

  // createFromFile takes the .spv name and the D3D12 backend swaps in .dxil.
  auto vs = gpu::Shader::createFromFile(device, shader_dir / "avatar_grid.vert.spv");
  if (!vs) return fail("avatar_grid.vert: " + vs.error().message);
  auto ps = gpu::Shader::createFromFile(device, shader_dir / "avatar_grid.frag.spv");
  if (!ps) return fail("avatar_grid.frag: " + ps.error().message);
  self->vs_ = std::move(vs).value();
  self->ps_ = std::move(ps).value();

  auto buffer = gpu::Buffer::create(device, {.size = std::uint64_t{kSlots} * kSlotBytes,
                                             .usage = gpu::kUsageStorage,
                                             .location = gpu::MemoryLocation::HostVisible});
  if (!buffer) return fail("grid buffer: " + buffer.error().message);
  self->buffer_ = std::move(buffer).value();
  // A slot that is drawn before it has been written would otherwise read
  // whatever the allocation came with, at a scale of zero.
  std::memset(self->buffer_->mapped(), 0, kSlots * kSlotBytes);
  table.writeStorageBuffer(table.userStorageBinding(storage_index), *self->buffer_);
  self->table_ = &table;

  gpu::GraphicsPipelineDesc desc{};
  desc.vertexShader = self->vs_.get();
  desc.fragmentShader = self->ps_.get();
  desc.colorFormat = color_format;
  desc.pushConstantBytes = sizeof(AvatarPush);
  // Not optional even though the shader reads the grid through it: on D3D12
  // the table owns the root signature.
  desc.descriptorTable = &table;
  auto pipeline = gpu::Pipeline::createGraphics(device, desc);
  if (!pipeline) return fail("grid pipeline: " + pipeline.error().message);
  self->pipeline_ = std::move(pipeline).value();
  return self;
}

void AvatarRenderer::write_slot(std::uint32_t slot, const AvatarGrid& grid, std::uint32_t band_w,
                                std::uint32_t band_h, float alpha) {
  const std::uint32_t gw = std::clamp(grid.width, 1u, kAvatarMaxGrid);
  const std::uint32_t gh = std::clamp(grid.height, 1u, kAvatarMaxGrid);
  // Integer scale, and the leftover split by an integer division: a cell has
  // to cover a whole number of whole pixels and start on one. A fractional
  // scale or a half-pixel origin is the one way this design can still come
  // out blurred, so neither is ever computed. At the default 16x16 in the
  // 360x260 band that is x16 — 256 px — with 52 px and 2 px of margin.
  //
  // A stage states its scale instead (it was sized from one); it is still an
  // integer, so a sprite placed a whole number of cells from the body lands a
  // whole number of pixels from it.
  const std::uint32_t scale = grid.scale != 0
                                  ? std::max(1u, grid.scale)
                                  : std::max(1u, std::min(band_w / gw, band_h / gh));
  SlotHeader header{};
  header.width = gw;
  header.height = gh;
  header.scale = scale;
  header.origin_x = (static_cast<std::int32_t>(band_w) - static_cast<std::int32_t>(gw * scale)) / 2;
  header.origin_y = (static_cast<std::int32_t>(band_h) - static_cast<std::int32_t>(gh * scale)) / 2;
  header.alpha = alpha;

  auto* region = static_cast<std::byte*>(buffer_->mapped()) + std::size_t{slot} * kSlotBytes;
  const std::size_t used = std::size_t{gw} * gh * sizeof(std::uint32_t);
  std::memcpy(region, &header, sizeof(header));
  std::memcpy(region + kHeaderBytes, grid.base.data(), used);
  std::memcpy(region + kHeaderBytes + kLayerBytes, grid.overlay.data(), used);
}

void AvatarRenderer::record(gpu::CommandContext& cmd, std::uint32_t slot, std::uint32_t band_w,
                            std::uint32_t band_h) const {
  cmd.setViewport(0.0f, 0.0f, static_cast<float>(band_w), static_cast<float>(band_h));
  cmd.setScissor(0, 0, band_w, band_h);
  cmd.bindPipeline(*pipeline_);
  cmd.bindDescriptorTable(*pipeline_, *table_);
  const AvatarPush push{.slot = slot, .pad = 0};
  cmd.pushConstants(*pipeline_, &push, sizeof(push));
  cmd.draw(3);
}

}  // namespace aii
