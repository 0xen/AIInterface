#include "loader_anim.h"

#include "imgui.h"
#include "imgui_layer.h"

#include <cmath>

namespace aii {
namespace {

constexpr int kCubes = 4;
constexpr double kPeriod = 1.6;  // seconds for one full out-and-back
// Sized against shaders/loader.hlsl's kScale, which maps 1.32 world units
// onto half the window's 360 px width: a 0.155 half-extent is a ~58 px cube
// and the amplitude carries it ~70 px either side of centre, so four cubes
// and the gaps between them fit the corner window with margin to spare.
constexpr float kAmplitude = 0.60f;
constexpr float kExtent = 0.155f;
// A small fixed fan along the same axis, on top of the shared slide. Without
// it the quarter-period offsets make cubes 0/1 and 2/3 exactly coincident
// four times a cycle — one body swallowing another whole, which reads as a
// cube blinking out. Offset, the same moment is a deep overlap with a
// visible intersection, which is the point of the animation.
constexpr float kFan = 0.16f;
constexpr float kTau = 6.28318530718f;

// Four brightness levels, the black-and-white register M2's avatar is heading
// for. They are what tells the cubes apart where they interpenetrate, so the
// steps are wide, but the floor is well clear of the scrim: a level dark
// enough to read as "dark grey" on paper simply disappears into it. They are
// linear values, because the swapchain view is sRGB.
constexpr float kLevels[kCubes] = {1.00f, 0.74f, 0.50f, 0.30f};

// A sine flattened near its extremes: the same seamless loop, but the cubes
// dwell at the ends of the slide and cross quickly, which reads as deliberate
// rather than as a pendulum. Range is untouched (+-1 at s = +-1).
float eased(float s) { return s * (1.35f - 0.35f * s * s); }

ImU32 u32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

}  // namespace

LoaderPush loader_push(double t, std::uint32_t w, std::uint32_t h, float opacity) {
  LoaderPush p{};
  p.res[0] = static_cast<float>(w);
  p.res[1] = static_cast<float>(h);
  p.extent = kExtent;
  p.opacity = opacity;
  const float phase = static_cast<float>(t / kPeriod) * kTau;
  for (int i = 0; i < kCubes; ++i) {
    // A quarter period apart on one shared axis, so two of them meet in the
    // middle every 0.4 s. They pass straight through each other: the shader
    // keeps the nearest surface, so a crossing resolves exactly instead of
    // one cube popping in front of the other.
    p.centre[i] = (static_cast<float>(i) - 1.5f) * kFan +
                  kAmplitude * eased(std::sin(phase + static_cast<float>(i) * (kTau * 0.25f)));
    p.level[i] = kLevels[i];
  }
  return p;
}

void draw_loader_backdrop(const std::string& stage, float progress, std::uint32_t w,
                          std::uint32_t h, float opacity) {
  const float fw = static_cast<float>(w);
  const float fh = static_cast<float>(h);
  // Every alpha below is scaled by this, so the scrim, the caption and the bar
  // leave together with the cubes.
  const float o = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
  // The foreground list draws after every window, so the scrim lands over the
  // panel however the panel is laid out, and the caption lands over the scrim.
  ImDrawList* dl = ImGui::GetForegroundDrawList();

  // Near-black rather than black, and translucent: the panel underneath has
  // to stay readable through it. The window is transparent and this covers
  // all of it, so the alpha is also what the desktop is seen through.
  dl->AddRectFilled({0.0f, 0.0f}, {fw, fh}, u32(ui_color(0.04f, 0.04f, 0.06f, 0.62f * o)));

  const float cx = fw * 0.5f;
  const float cy = fh * 0.5f;
  // Clear of the cubes, which loader.hlsl's kLift raises to make room for
  // exactly this: the caption and the bar are what sit in the lower half.
  const float captionY = cy + fw * 0.12f;
  if (!stage.empty()) {
    const char* text = stage.c_str();
    const ImVec2 size = ImGui::CalcTextSize(text);
    dl->AddText({cx - size.x * 0.5f, captionY}, u32(ui_color(0.88f, 0.89f, 0.92f, o)), text);
  }

  const float barW = fw * 0.44f;
  const float barY = captionY + ImGui::GetTextLineHeight() + 10.0f;
  const ImVec2 a{cx - barW * 0.5f, barY};
  const ImVec2 b{cx + barW * 0.5f, barY + 3.0f};
  dl->AddRectFilled(a, b, u32(ui_color(1.0f, 1.0f, 1.0f, 0.16f * o)));
  const float filled = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
  if (filled > 0.0f) {
    dl->AddRectFilled(a, {a.x + barW * filled, b.y}, u32(ui_color(0.93f, 0.94f, 0.96f, 0.90f * o)));
  }
}

}  // namespace aii
