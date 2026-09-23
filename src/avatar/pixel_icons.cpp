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

// M5.1: the prompt inspector. A page with lines of text on it, drawn as a
// solid block with the lines cut *out* of it rather than as an outline with
// lines inside — a 1-cell outline is 2 px at this scale and reads as a smudge
// beside the cog's chunky teeth, while negative space is the idiom the folder
// already uses and stays legible.
constexpr IconRows kIconPrompts = {
    ".............",
    ".###########.",
    ".###########.",
    ".#.........#.",
    ".###########.",
    ".#.........#.",
    ".###########.",
    ".#.....#####.",
    ".###########.",
    ".#.........#.",
    ".###########.",
    ".###########.",
    ".............",
};

// M9.1: the workers button. A figure — round head, shoulders, arms out — drawn
// as a solid block in the same negative-space idiom the folder and the page
// use, because a 1-cell outline is 2 px at this scale and reads as a smudge
// beside the cog's chunky teeth. The arms are part of the silhouette rather
// than detail inside it: at 26 px the outline is the whole of what the eye
// gets, and a figure with its arms in is a bust, not a worker.
constexpr IconRows kIconWorkers = {
    ".............",
    "....#####....",
    "...#######...",
    "...##...##...",
    "...#######...",
    "....#####....",
    ".....###.....",
    "..#########..",
    ".###########.",
    "###..###..###",
    "##...###...##",
    ".....###.....",
    "....##.##....",
};

// M33: a registered `run=` button. A play mark — the idiom this app already
// has for "runs something" (the transport row's own play triangle) — in the
// same negative-space-free solid-block style as the folder, because a
// 1-cell outline is 2 px at this scale and reads as a smudge.
constexpr IconRows kIconScript = {
    ".............",
    "..#..........",
    "..##.........",
    "..###........",
    "..####.......",
    "..#####......",
    "..######.....",
    "..#####......",
    "..####.......",
    "..###........",
    "..##.........",
    "..#..........",
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
    case ButtonGlyph::Prompts: return kIconPrompts;
    case ButtonGlyph::Workers: return kIconWorkers;
    case ButtonGlyph::Script: return kIconScript;
    default: return nullptr;
  }
}

}  // namespace aii
