#pragma once
// Famicom-style pixel icons, shared by the panel's transport row and the
// sidebar strip (M4.3).
//
// The idiom was set by the transport row (94ee2a7) and the user asked for it
// by name: an icon is a 13x13 ASCII grid with two inks, drawn as filled cells
// at a **whole-number scale** through ImDrawList. '.' is transparent, '#' is
// the shape, 'o' is a mark laid across it — every icon that says "not this"
// needs its negation to read over the thing it negates.
//
// M4's plan said to merge an icon font (Lucide or Font Awesome) instead. That
// was written before this app had an icon idiom; it has one now, and a font
// here would give the strip a different visual language from the row 40 px to
// its right. Nothing about sidebar size forced the question either way: the
// strip's buttons are 40 px, which is *larger* than the transport row's 30 px,
// so the same 13-cell grid draws at 2x with room to spare.
//
// The grids are string literals in the .cpp rather than files under assets/,
// like the transport row's and unlike the avatar's clips: the avatar is a
// themeable, hot-reloadable character, and these are window chrome. Chrome
// that could fail to load is chrome that can leave a button blank.
#include "core/button_registry.h"
#include "imgui.h"

namespace aii {

// Every icon is this many cells square. The scale is an integer and only an
// integer: at 2.3x the cells would alternate 2 and 3 px wide and the icon
// would read as mush, which is the one thing an 8-bit icon may not do. The
// button is sized from the icon, never the icon fitted to the button.
constexpr int kIconCells = 13;
constexpr float kIconScale = 2.0f;
constexpr float kIconPx = kIconCells * kIconScale;

using IconRows = const char* [kIconCells];

// One cell per filled character, snapped to whole pixels. `p` is the icon's
// top-left in screen space and is floored for the same reason the scale is an
// integer: half a pixel of origin undoes every bit of the alignment above.
void draw_icon(ImDrawList* dl, const char* const* rows, ImVec2 p, ImU32 ink, ImU32 mark,
               float scale = kIconScale);

// The grid a registry glyph names, or null for ButtonGlyph::Label — which has
// no picture by construction, because a registered button is text.
const char* const* icon_for_glyph(ButtonGlyph glyph);

}  // namespace aii
