#pragma once
// The 2D pixel avatar's renderer (M2.1): a cell grid the CPU composes and a
// quad over the avatar band that reads it back out, integer-scaled and
// centred.
//
// The grid travels to the GPU the way the panel's pixels do — a HostVisible
// storage buffer with one region per frame slot, written after
// waitFrameSlot() — because the engine's sampled images are not
// update-after-bind and a per-frame texture upload would need a waitIdle
// around it (HANDOFF). So there is no texture in this path at all, which is
// also why the result is nearest-neighbour by construction: shaders/
// avatar_grid.hlsl floors a band pixel into a cell with an integer division,
// and nothing on the way in or out can interpolate.
//
// Two layers, composited in the shader: `Base` is the character and
// `Overlay` is accessories and particles (M2.4), so an accessory never
// destroys the body art underneath it and the two can be driven apart.
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "rend/gpu/format.h"

namespace rend::gpu {
class Buffer;
class CommandContext;
class DescriptorTable;
class Device;
class Pipeline;
class Shader;
}  // namespace rend::gpu

namespace aii {

// The grid may be any size up to this on each edge, chosen once so a
// user-configured size never reallocates: at 64x64 the whole buffer is 16 KB
// per layer per frame slot, which is nothing next to never having to
// rebuild a binding.
inline constexpr std::uint32_t kAvatarMaxGrid = 64;
inline constexpr std::uint32_t kAvatarMaxCells = kAvatarMaxGrid * kAvatarMaxGrid;

enum class AvatarLayer : std::uint32_t { Base = 0, Overlay = 1 };

// One cell. Bytes are sRGB-encoded as authored and alpha is straight, not
// premultiplied; the shader premultiplies, in the encoded domain, because
// that is the domain the desktop compositor blends in.
constexpr std::uint32_t avatar_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                    std::uint8_t a) {
  return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
         (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(a) << 24);
}

// The composed frame: what the avatar looks like right now, in cells. Rows
// are packed at the live `width`, not at the maximum, so the shader's index
// is the obvious one and the CPU copies only what the grid actually uses.
//
// Since M2.2's sub-sprites the grid is a *stage* rather than the character:
// it covers the band in cells at the chosen scale, with the character
// composed into it at a fixed position and accessories composed at their
// anchors, so everything stays one buffer, one draw and one composite.
struct AvatarGrid {
  std::uint32_t width = 16;   // the default the milestone specifies
  std::uint32_t height = 16;
  // Cells per pixel, or 0 to derive one that fits the grid to the band (the
  // M2.1 rule, still what the placeholder wants). A stage has to state its
  // own scale: it is sized *from* a scale chosen for the character, so
  // deriving one back from the stage size would not generally agree.
  std::uint32_t scale = 0;
  std::array<std::uint32_t, kAvatarMaxCells> base{};
  std::array<std::uint32_t, kAvatarMaxCells> overlay{};

  // Clears both layers: a resize renumbers every cell, so keeping the old
  // contents would scramble the art rather than preserve it.
  void resize(std::uint32_t w, std::uint32_t h);
  void clear();
  void set(AvatarLayer layer, std::uint32_t x, std::uint32_t y, std::uint32_t rgba);
};

// Stand-in art until M2.2 defines the file format and M2.3 authors the real
// slime: a 16x16 black-and-white blob with a face, plus one overlay cell so
// the two-layer composite is exercised. A blob rather than a test pattern so
// the window does not look broken in the meantime, and so scale, centring
// and the fades are all obviously right or obviously wrong.
void avatar_placeholder_blob(AvatarGrid& grid);

class AvatarRenderer {
 public:
  // `table` supplies the root signature (the D3D12 backend keeps it there)
  // and carries the grid buffer at user storage binding `storage_index`.
  static std::unique_ptr<AvatarRenderer> create(const rend::gpu::Device& device,
                                                rend::gpu::Format color_format,
                                                rend::gpu::DescriptorTable& table,
                                                std::uint32_t storage_index,
                                                const std::filesystem::path& shader_dir,
                                                std::string* error);
  ~AvatarRenderer();

  AvatarRenderer(const AvatarRenderer&) = delete;
  AvatarRenderer& operator=(const AvatarRenderer&) = delete;

  // Writes this slot's region: the header (grid size, the integer scale and
  // origin worked out for this band, and `alpha`) then both layers. Call
  // after waitFrameSlot(), when the GPU has finished reading the slot.
  //
  // `alpha` is the M1.5 handoff times the M1.6 visibility fade; the shader
  // premultiplies by it.
  void write_slot(std::uint32_t slot, const AvatarGrid& grid, std::uint32_t band_w,
                  std::uint32_t band_h, float alpha);

  // Records the quad over the band. Sets its own viewport and scissor, so
  // the band is all it can touch.
  void record(rend::gpu::CommandContext& cmd, std::uint32_t slot, std::uint32_t band_w,
              std::uint32_t band_h) const;

 private:
  AvatarRenderer() = default;

  std::unique_ptr<rend::gpu::Shader> vs_;
  std::unique_ptr<rend::gpu::Shader> ps_;
  std::unique_ptr<rend::gpu::Pipeline> pipeline_;
  std::unique_ptr<rend::gpu::Buffer> buffer_;
  const rend::gpu::DescriptorTable* table_ = nullptr;
};

}  // namespace aii
