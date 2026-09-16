#pragma once
// The loading animation (M1.3): four greyscale cubes sliding through one
// another on a shared axis, drawn as a full-window overlay on top of the
// panel while VoiceSession is still bringing its engines up.
//
// The cubes are not geometry. This app draws no scene batch, so the engine
// never attaches a depth buffer (PassContext::depthAttached) and instanced
// cube meshes would have no way to resolve bodies that genuinely pass
// through each other — the later instance would simply paint over the
// earlier one. So shaders/loader.hlsl intersects four analytic boxes per
// pixel and keeps the nearest hit: exact interpenetration with no depth
// attachment, and a full-screen triangle that covers the whole window the
// overlay wants anyway.
//
// The scrim and the caption are ImGui instead, drawn on the foreground draw
// list so they land after the panel; the cubes then draw after
// ImGuiLayer::end_frame(), which is what puts them on top of everything.
#include <cstdint>
#include <string>

namespace aii {

// Push block for shaders/loader.hlsl. The engine's root signature has room
// for 16 dwords (D3D12DescriptorTable::kPushDwords) and silently truncates
// past it, so this must stay at or under 64 bytes.
struct LoaderPush {
  float res[2];     // viewport size in pixels
  float extent;     // cube half-size, world units
  float opacity;    // whole-loader fade (M1.5 handoff); 1 while loading
  float centre[4];  // each cube's position along the slide axis
  float level[4];   // each cube's greyscale level
};
static_assert(sizeof(LoaderPush) == 48);

// The animation state at time `t` (seconds), for a viewport of w x h pixels.
// `opacity` fades the cubes out for the handoff to the avatar; it rides the
// push block because the overlay recorder must not bind a descriptor table.
LoaderPush loader_push(double t, std::uint32_t w, std::uint32_t h, float opacity = 1.0f);

// The scrim, the stage name and the progress bar, on ImGui's foreground draw
// list. Call between begin_frame() and end_frame(), after the panel: the
// scrim recesses the panel, the caption sits on top of the scrim, and the
// cubes go over all of it in the overlay recorder.
//
// `opacity` scales all three together. The loading screen has to leave as one
// thing — a scrim that outlives its caption, or a bar that outlives the cubes,
// is a pop in the middle of the crossfade.
void draw_loader_backdrop(const std::string& stage, float progress, std::uint32_t w,
                          std::uint32_t h, float opacity = 1.0f);

}  // namespace aii
