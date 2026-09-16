#include "pixel_icons.h"

#include <cmath>

namespace aii {
namespace {

// The settings gear. Four teeth and a square hole, which is as much gear as
// 13 cells will carry: a six-toothed one at this size loses the gaps between
// the teeth on the diagonals and reads as a blob.
constexpr IconRows kIconCog = {
    ".....###.....",
    ".....###.....",
    "....#####....",
    "...#######...",
    "..#########..",
    "#####...#####",
    "#####...#####",
    "#####...#####",
    "..#########..",
    "...#######...",
    "....#####....",
    ".....###.....",
    ".....###.....",
};

// The working directory. A folder with its tab, the same shape the panel's
// vector glyph drew, now on the grid.
constexpr IconRows kIconFolder = {
    ".............",
    ".............",
    ".#####.......",
    ".######......",
    ".###########.",
    ".###########.",
    ".###########.",
    ".###########.",
    ".###########.",
    ".###########.",
    ".###########.",
    ".............",
    ".............",
};

// The avatar's own art directory. Deliberately *not* a second folder: the
// strip would then carry two identical pictures whose meanings were only in
// their tooltips. It is the slime, because the slime is what is in there.
constexpr IconRows kIconAvatar = {
    ".............",
    ".............",
    ".....###.....",
    "....#####....",
    "...#######...",
    "..#########..",
    "..#########..",
    ".###.###.###.",
    ".###.###.###.",
    ".###########.",
    ".###########.",
    "..#########..",
    ".............",
};

}  // namespace

void draw_icon(ImDrawList* dl, const char* const* rows, ImVec2 p, ImU32 ink, ImU32 mark,
               float scale) {
  const float x0 = std::floor(p.x), y0 = std::floor(p.y);
  for (int r = 0; r < kIconCells; ++r) {
    for (int c = 0; rows[r][c]; ++c) {
      const char ch = rows[r][c];
      if (ch == '.') continue;
      const float x = x0 + c * scale, y = y0 + r * scale;
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + scale, y + scale), ch == 'o' ? mark : ink);
    }
  }
}

const char* const* icon_for_glyph(ButtonGlyph glyph) {
  switch (glyph) {
    case ButtonGlyph::Cog: return kIconCog;
    case ButtonGlyph::Folder: return kIconFolder;
    case ButtonGlyph::Avatar: return kIconAvatar;
    default: return nullptr;
  }
}

}  // namespace aii
