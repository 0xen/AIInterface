#include "avatar_ui.h"

#include "core/button_registry.h"
#include "core/wake_word.h"
#include "imgui.h"
#include "imgui_layer.h"
#include "pixel_icons.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

// GetCursorPos, for the gesture instrumentation below: what Windows says the
// pointer is doing, beside what ImGui believes it is doing.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace aii {
namespace {

constexpr float kChatHeight = 260.0f;  // the scrollback area when open
// How long the reason something did nothing stays on the reserved row under
// the message field. Shared by the send refusals and by a toolbar button whose
// folder has gone away since it was registered.
constexpr float kRefusalSeconds = 2.5f;

// Palette, written as ordinary sRGB literals; ui_color() converts.
ImVec4 dim() { return ui_color(0.59f, 0.61f, 0.67f); }
ImVec4 fg() { return ui_color(0.91f, 0.92f, 0.94f); }
ImVec4 user_color() { return ui_color(1.00f, 0.77f, 0.47f); }
ImVec4 claude_color() { return ui_color(0.59f, 0.80f, 1.00f); }
ImVec4 accent() { return ui_color(0.84f, 0.33f, 0.29f); }
ImVec4 good() { return ui_color(0.55f, 0.88f, 0.67f); }
ImVec4 warn() { return ui_color(1.00f, 0.75f, 0.35f); }
ImVec4 bad() { return ui_color(0.93f, 0.45f, 0.42f); }
// The "mic is live" red, and the shades it takes under the pointer.
ImVec4 red() { return ui_color(0.80f, 0.16f, 0.16f); }
ImVec4 red_hot() { return ui_color(0.91f, 0.25f, 0.24f); }
ImVec4 red_deep() { return ui_color(0.62f, 0.10f, 0.10f); }

// ------------------------------------------------------- Famicom-style icons
//
// The transport row lost its text labels (user, 16 Sep 2026) and gained pixel
// icons. They are authored the way the avatar's art is: an ASCII grid with a
// palette, read top-left to bottom-right, '.' transparent. Two inks rather
// than one, because every icon that says "not this" needs its negation to read
// *over* the shape it negates: '#' is the icon, 'o' is the mark laid across it.
//
// They are string literals here rather than .txt files under assets/, unlike
// the avatar's clips. The avatar's art is a themeable, hot-reloadable
// character; these are window chrome, in the same category as the cog and the
// folder on the toolbar and the arrows beside them, which are already drawn in
// code. Chrome that could fail to load is chrome that can leave a button blank.
//
// The grid, the integer scale and the draw call itself now live in
// pixel_icons.h, because the sidebar strip (M4.3) draws its icons the same
// way: same 13 cells, same whole-number scale, same two inks. The transport
// row's own grids stay here — they are this panel's chrome — while the
// sidebar's live with the shared code, since the strip will not be the only
// surface that ever wants a cog.
constexpr float kTransportButton = 30.0f;  // kIconPx plus 2 px of air all round

// The microphone, six ways. Idle is the bare capsule-and-cradle; everything
// else is that same shape with something added, so the button never changes
// what it *is*, only what it is doing — the shape is the noun and the addition
// is the verb.
constexpr IconRows kIconMic = {
    ".....###.....",
    ".....###.....",
    ".....###.....",
    ".....###.....",
    ".....###.....",
    "...#.###.#...",
    "...#.###.#...",
    "...#.....#...",
    "....#####....",
    "......#......",
    "......#......",
    "...#######...",
    ".............",
};
// Hearing you: arcs either side.
constexpr IconRows kIconMicLive = {
    ".....###.....",
    ".#...###...#.",
    "#.#..###..#.#",
    "#.#..###..#.#",
    ".#...###...#.",
    "...#.###.#...",
    "...#.###.#...",
    "...#.....#...",
    "....#####....",
    "......#......",
    "......#......",
    "...#######...",
    ".............",
};
// Holding to dictate: the words are going into the box below, not to Claude.
constexpr IconRows kIconMicHold = {
    ".....###.....",
    ".....###.....",
    ".....###.....",
    ".....###.....",
    ".....###.....",
    "...#.###.#...",
    "...#.###.#...",
    "...#.....#...",
    "....#####....",
    "......#......",
    "...#######...",
    ".............",
    "..#..#..#....",
};
// Latched on but not listening right now — the stretch where Claude is
// replying and the session deliberately shuts the microphone so the speakers
// are not transcribed back in as the user.
constexpr IconRows kIconMicShut = {
    ".....###....o",
    ".....###...o.",
    ".....###..o..",
    ".....###.o...",
    ".....###o....",
    "...#.##o.#...",
    "...#.#o#.#...",
    "...#.o...#...",
    "....o####....",
    "...o..#......",
    "..o...#......",
    ".o.#######...",
    "o............",
};

// M1f.3. Shut, and shut *by itself*: the latch timed out on a silent room.
//
// This is a sixth face rather than a reuse of one of the five, and the reason
// is the failure it exists to prevent. Without it the button falls back to
// `Idle` the instant the timeout fires -- the same bare capsule as a
// microphone nobody has ever clicked -- so the one channel on the button that
// could say what happened would be saying nothing happened. It is the same
// argument the status line's wording is making, on the other surface.
//
// The mark is a `z`, matching the slime's `zzz` overlay a hundred pixels
// above it, so the two read as one sentence rather than two coincidences. It
// sits in the corner the capsule does not use, and the capsule itself is
// untouched: the shape is still the noun, the addition is still the verb.
constexpr IconRows kIconMicDozed = {
    ".....###.oooo",
    ".....###...o.",
    ".....###..o..",
    ".....###.oooo",
    ".....###.....",
    "...#.###.#...",
    "...#.###.#...",
    "...#.....#...",
    "....#####....",
    "......#......",
    "......#......",
    "...#######...",
    ".............",
};

// M12.2. Open, and listening for one word only.
//
// This is the face that has to be impossible to mistake for a shut microphone,
// because it is drawn while the capture device is genuinely running: the user
// chose always-on local matching, and the deal they made is that the app is
// honest about it. So the capsule is drawn **whole and unmarked** — nothing
// crossed out, nothing dimmed, no `z` — and what is added is two arcs at the
// right, the ear/ripple that means "hearing". The same ripples the `Listening`
// face does not have, because that one does not need them: it is the loudest
// plate on the row.
//
// Read against its neighbours: `Listening` is a red plate with a bare capsule
// (open, and everything goes to Claude); this is a cool plate with a capsule
// and ripples (open, and nothing goes anywhere); `Latched` and `Dozed` are the
// same capsule struck through or asleep. Four different glyphs, four different
// plates — the house rule, kept.
constexpr IconRows kIconMicWake = {
    ".....###.....",
    ".....###..o..",
    ".....###.o.o.",
    ".....###.o.o.",
    ".....###.o.o.",
    "...#.###.o.o.",
    "...#.###.o.o.",
    "...#.....o.o.",
    "....#####o.o.",
    "......#..o.o.",
    "......#...o..",
    "...#######...",
    ".............",
};

// The speaker, for the mute button. Same trick: one shape, one mark.
constexpr IconRows kIconSpeaker = {
    ".............",
    "........#....",
    ".......##....",
    "......###...#",
    ".....####...#",
    ".########.#.#",
    ".########.#.#",
    ".########.#.#",
    ".....####...#",
    "......###...#",
    ".......##....",
    "........#....",
    ".............",
};
constexpr IconRows kIconSpeakerMuted = {
    ".............",
    "........#....",
    ".......##....",
    "......###....",
    ".....####o..o",
    ".########.oo.",
    ".########.oo.",
    ".########o..o",
    ".....####....",
    "......###....",
    ".......##....",
    "........#....",
    ".............",
};

// Reset, two faces (user, 19 Sep 2026). A loop with an arrowhead is the one
// glyph everybody already reads as "start again", and it is drawn here the way
// every other icon in this row is: a ring in the same 2-cell stroke as the
// speaker's cone and the microphone's cradle, broken at the top right so the
// head has somewhere to come from. The head points down the right-hand arm,
// which is the direction a refresh arrow has always travelled.
//
// The stroke is two cells and not three, which was the first attempt and read
// as a blob at 26 px in a capture: a thick ring leaves a hole five cells wide,
// and nothing legible fits in five cells. Two cells leave seven, which is what
// the armed face's exclamation mark needs — and the gap between that mark's
// bar and its dot is two cells rather than one, because one cell is two pixels
// and two pixels of gap is not a gap, it is an artefact.
constexpr IconRows kIconReset = {
    ".............",
    "....###.#####",
    "..###....###.",
    "..##......#..",
    ".##.......##.",
    ".##.......##.",
    ".##.......##.",
    ".##.......##.",
    ".##.......##.",
    "..##.....##..",
    "..###...###..",
    "....#####....",
    ".............",
};
// Armed: the same loop with an exclamation mark inside it, in the mark ink.
// The idiom the microphone set — the shape is the noun and the addition is the
// verb — and the rule the user set with it: colour *and* a change of glyph,
// never colour alone. The plate goes red underneath this, but the button still
// says "careful" on a monitor, in a capture and to a colour-blind eye.
constexpr IconRows kIconResetArmed = {
    ".............",
    "....###.#####",
    "..###....###.",
    "..##.ooo..#..",
    ".##..ooo..##.",
    ".##..ooo..##.",
    ".##.......##.",
    ".##.......##.",
    ".##..ooo..##.",
    "..##.ooo.##..",
    "..###...###..",
    "....#####....",
    ".............",
};

// Close, two faces (user, 19 Sep 2026), built to the same rule Reset's pair
// is: the armed face changes the *glyph* as well as the plate colour, so the
// warning survives a greyscale monitor, a capture and a colour-blind eye.
//
// An X is the one glyph that means "close this" without a caption, which
// matters at 13 cells. It is drawn to the corners rather than tucked in,
// because the armed face needs somewhere to go and pulling *inward* is a
// motion the eye reads as something closing on it.
constexpr IconRows kIconClose = {
    ".............",
    ".#.........#.",
    ".##.......##.",
    "..##.....##..",
    "...##...##...",
    "....##.##....",
    ".....###.....",
    "....##.##....",
    "...##...##...",
    "..##.....##..",
    ".##.......##.",
    ".#.........#.",
    ".............",
};
// Armed: the X has drawn in by one cell and four brackets have closed around
// it. The plate goes red underneath, but the shape alone already says it.
constexpr IconRows kIconCloseArmed = {
    "ooo.......ooo",
    "o...........o",
    "o.#.......#.o",
    "..##.....##..",
    "...##...##...",
    "....##.##....",
    ".....###.....",
    "....##.##....",
    "...##...##...",
    "..##.....##..",
    "o.#.......#.o",
    "o...........o",
    "ooo.......ooo",
};

// Stop. A square is the one transport glyph that needs no explaining, and at
// 13 cells there is no room for anything that does.
constexpr IconRows kIconStop = {
    ".............",
    ".............",
    "..#########..",
    "..#########..",
    "..#########..",
    "..#########..",
    "..#########..",
    "..#########..",
    "..#########..",
    "..#########..",
    "..#########..",
    ".............",
    ".............",
};


// Same grading the shell status line uses: green below `amber`, amber up to
// `red`, red from there. `pct` is 0..100.
ImVec4 grade(double pct, double amber, double red) {
  if (pct >= red) return bad();
  if (pct >= amber) return warn();
  return good();
}

// How long until a quota window rolls over, as the label for that window:
// "2h14m", "3d 5h", "<1m". Empty when the CLI has not reported a reset time,
// which is also what happens before the first turn of a session.
std::string until(long long reset_epoch) {
  if (reset_epoch <= 0) return {};
  const long long now = static_cast<long long>(std::time(nullptr));
  long long left = reset_epoch - now;
  if (left <= 0) return "now";
  char buf[32];
  if (left >= 24 * 3600)
    std::snprintf(buf, sizeof(buf), "%lldd %lldh", left / (24 * 3600), (left % (24 * 3600)) / 3600);
  else if (left >= 3600)
    std::snprintf(buf, sizeof(buf), "%lldh%02lldm", left / 3600, (left % 3600) / 60);
  else if (left >= 60)
    std::snprintf(buf, sizeof(buf), "%lldm", left / 60);
  else
    std::snprintf(buf, sizeof(buf), "<1m");
  return buf;
}

// The label a segment draws: "Session", or "Session (2h14m)" when there is a
// reported reset time and there is room on the row for it. Shared with
// segment_width() so the measurement and the drawing can never disagree.
std::string segment_label(const char* label, long long reset_epoch, bool countdown) {
  std::string text = label;
  if (!countdown) return text;
  if (const std::string left = until(reset_epoch); !left.empty()) text += " (" + left + ")";
  return text;
}

// One "12% CTX" segment. A negative fraction means the CLI has not said yet.
// `reset_epoch` > 0 turns the label into "Session (2h14m)" — the real time
// left in that window rather than its nominal length. `countdown` is the
// row's verdict on whether the bracket fits; the hover tooltip reports the
// reset time either way, so a dropped bracket costs convenience and not
// information.
void segment(double fraction, const char* label, double amber, double red,
             long long reset_epoch = 0, bool countdown = true) {
  const std::string text = segment_label(label, reset_epoch, countdown);
  char buf[64];
  if (fraction < 0.0) {
    std::snprintf(buf, sizeof(buf), "--%% %s", text.c_str());
    ImGui::TextColored(dim(), "%s", buf);
    return;
  }
  const double pct = fraction * 100.0;
  std::snprintf(buf, sizeof(buf), "%.0f%%", pct);
  ImGui::TextColored(grade(pct, amber, red), "%s", buf);
  ImGui::SameLine(0.0f, 4.0f);
  ImGui::TextColored(dim(), "%s", text.c_str());
  if (ImGui::IsItemHovered() && reset_epoch > 0) {
    const std::time_t t = static_cast<std::time_t>(reset_epoch);
    std::tm local{};
    char when[64] = "?";
    if (localtime_s(&local, &t) == 0) std::strftime(when, sizeof(when), "%a %d %b %H:%M", &local);
    ImGui::SetTooltip("%s resets at %s", label, when);
  }
}

// What segment() above will take, to the pixel. It mirrors the two branches
// of the draw exactly — the placeholder is one string, a real reading is the
// percentage, four pixels and the label — because a measurement that drifts
// from its drawing is worse than no measurement at all.
float segment_width(double fraction, const char* label, long long reset_epoch, bool countdown) {
  const std::string text = segment_label(label, reset_epoch, countdown);
  if (fraction < 0.0) return ImGui::CalcTextSize(("--% " + text).c_str()).x;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.0f%%", fraction * 100.0);
  return ImGui::CalcTextSize(buf).x + 4.0f + ImGui::CalcTextSize(text.c_str()).x;
}

const char* visibility_name(AvatarVisibility mode) {
  switch (mode) {
    case AvatarVisibility::Always: return "always shown";
    case AvatarVisibility::WhenTalking: return "shown when talking";
    default: return "hidden";
  }
}

// The avatar visibility cycle, immediately left of the chat arrow and the
// same size. Drawn rather than lettered: this font is Segoe UI with the
// Japanese ranges merged and carries no geometric symbols, and merging an
// icon font belongs to M4 — so the three modes are a filled disc, a half disc
// and a struck-through ring, which is the same vector register as the arrow
// beside it. The tooltip is what actually names the mode.
void visibility_button(AvatarUiState& state, float size) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  if (ImGui::InvisibleButton("##avatar_vis", ImVec2(size, size)))
    state.avatar_mode = static_cast<AvatarVisibility>(
        (static_cast<int>(state.avatar_mode) + 1) % 3);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImGuiCol bg_col = ImGui::IsItemActive()    ? ImGuiCol_ButtonActive
                          : ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered
                                                   : ImGuiCol_Button;
  dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ImGui::GetColorU32(bg_col),
                    ImGui::GetStyle().FrameRounding);

  const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
  const float r = size * 0.26f;
  switch (state.avatar_mode) {
    case AvatarVisibility::Always:
      dl->AddCircleFilled(c, r, ImGui::GetColorU32(fg()), 20);
      break;
    case AvatarVisibility::WhenTalking:
      // Half of a disc: there some of the time.
      dl->PathArcTo(c, r, 3.14159265f * 0.5f, 3.14159265f * 1.5f, 14);
      dl->PathFillConvex(ImGui::GetColorU32(fg()));
      dl->AddCircle(c, r, ImGui::GetColorU32(fg()), 20, 1.4f);
      break;
    default:
      dl->AddCircle(c, r, ImGui::GetColorU32(dim()), 20, 1.4f);
      dl->AddLine(ImVec2(c.x - r * 0.8f, c.y + r * 0.8f), ImVec2(c.x + r * 0.8f, c.y - r * 0.8f),
                  ImGui::GetColorU32(dim()), 1.6f);
      break;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Avatar: %s", visibility_name(state.avatar_mode));
}

// ------------------------------------------------------------ the button bar
//
// M1c.1: a row of small buttons at the top of the panel, above the status
// line. Every entry comes from ButtonRegistry — the settings cog and the
// working-directory folder are simply its first two — so adding a button, from
// here or from an ```aii``` block or later from the bus, never means editing
// this layout.
//
// The two glyphs are drawn by hand over an InvisibleButton, the same pattern
// and the same frame-height size as the avatar-mode disc and the chat arrow on
// the row below. There is no icon font in this build and merging one is M4's
// job; until then a cog and a folder are a handful of lines each and keep the
// bar in the same vector register as the rest of the widget.

// One bar button. Returns true on a click. Glyph buttons are square and match
// the controls on the status row; a label button is as wide as its (already
// capped) text. `avail` is what is left of the row: a button that would not fit
// is not drawn at all, which is the second guard on the layout contract — the
// registry's caps are sized for this font, and this holds even if it is not.
bool bar_button(const ToolbarButton& b, float size, float& avail, bool& first) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float w = b.glyph == ButtonGlyph::Label
                      ? ImGui::CalcTextSize(b.label.c_str()).x + 2.0f * style.FramePadding.x
                      : size;
  const float gap = first ? 0.0f : style.ItemSpacing.x;
  if (w + gap > avail) return false;
  avail -= w + gap;
  if (!first) ImGui::SameLine(0.0f, style.ItemSpacing.x);
  first = false;

  bool clicked = false;
  if (b.glyph == ButtonGlyph::Label) {
    clicked = ImGui::Button(b.label.c_str(), ImVec2(w, size));
  } else {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    clicked = ImGui::InvisibleButton(("##bar_" + b.id).c_str(), ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiCol bg = ImGui::IsItemActive()    ? ImGuiCol_ButtonActive
                        : ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered
                                                 : ImGuiCol_Button;
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ImGui::GetColorU32(bg),
                      style.FrameRounding);
    // The same 13x13 grids the strip draws, at 1x rather than 2x: this button
    // is one frame height (~21 px) and 26 px would not fit in it. One icon,
    // two sizes, no second drawing of a cog to keep in step with the first.
    if (const char* const* rows = icon_for_glyph(b.glyph)) {
      const ImU32 col = ImGui::GetColorU32(fg());
      draw_icon(dl, rows, ImVec2(p.x + (size - kIconCells) * 0.5f,
                                 p.y + (size - kIconCells) * 0.5f),
                col, col, 1.0f);
    }
  }
  if (ImGui::IsItemHovered() && !b.tooltip.empty()) ImGui::SetTooltip("%s", b.tooltip.c_str());
  return clicked;
}

// Draws the whole bar and acts on whatever was clicked. Live while loading,
// like the avatar-mode button beside it: none of these route into an engine,
// and the row has to occupy its height from the first frame or the window
// would change size the moment loading ended.
void button_bar(AvatarUiState& state, bool loading, float width) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float size = ImGui::GetFrameHeight();
  float avail = width - 2.0f * style.WindowPadding.x;
  bool first = true;
  bool open_settings = false;

  // Only this surface's buttons. With the sidebar up that is the registered
  // (text) ones alone — the cog and the folder moved to the strip, which is
  // where an icon button belongs — and the row is empty until an agent
  // registers something, costing the widget the ~27 px it used to spend on it.
  // Without a strip the registry hands them back here (snapshot_for), so the
  // settings surface is never unreachable.
  for (const ToolbarButton& b : ButtonRegistry::instance().snapshot_for(ButtonSurface::Toolbar)) {
    // The cog alone stands down while the loading screen is up, for the same
    // reason the chat arrow does: the region it opens is withheld until the
    // engines are up, so the click would do nothing visible. The folder is
    // unaffected — opening Explorer works from the first frame.
    const bool off = loading && b.action.kind == ButtonActionKind::OpenSettings;
    ImGui::BeginDisabled(off);
    const bool hit = bar_button(b, size, avail, first);
    ImGui::EndDisabled();
    if (!hit) continue;
    switch (b.action.kind) {
      case ButtonActionKind::OpenSettings:
        open_settings = true;
        break;
      case ButtonActionKind::OpenPath:
        // A failure here is the user's answer to their own click, so it goes
        // on the same reserved row that says why an Enter did nothing rather
        // than into a log they are not reading. The path was checked when the
        // button was registered, so this is the rare case of it having gone
        // away since.
        if (std::string err; !open_directory(b.action.path, &err)) {
          state.refusal = err;
          state.refusal_left = kRefusalSeconds;
        }
        break;
      case ButtonActionKind::Invoke:
        // M4.4: a window that registered itself. Only reachable here in the
        // fallback (no strip), and it does exactly what the strip would do.
        if (b.action.callback) b.action.callback();
        break;
    }
  }
  // M1c.3: the cog is a toggle on a region of the panel, not a popup.
  //
  // The placeholder it replaces *was* a popup, and a popup is wrong here for a
  // reason worth writing down: an ImGui popup is a floating window, and this
  // window is 360 px wide and as short as 168 px tall, with a transparent
  // DirectComposition surface that nothing may be drawn outside of. A popup
  // tall enough to hold voices, paths and timings would simply be cut off at
  // the window's edge — silently, since ImGui has no idea the viewport is the
  // whole of the app. A region inside the panel cannot be clipped by anything
  // but itself.
  if (open_settings) state.settings_open = !state.settings_open;
}

// ---------------------------------------------------------- the settings surface
//
// M1c.3. Built to grow, which here means three properties rather than three
// controls:
//
//  - It is a **scrolling child of a fixed height**, so adding a setting costs
//    scrollback and never costs window height. The window's height is the one
//    quantity in this app that is expensive to change: it moves every control
//    on screen, it goes through main.cpp's deferred `pendingH` path, and it is
//    capped by a DirectComposition ceiling the engine commits once. A surface
//    that grew with its contents would put every future setting back in front
//    of that.
//  - It is **sections of rows**, not a bespoke layout. A row is a label and
//    one control at a fixed split, so the next setting is one call.
//  - It reports choices by **name**, into `AvatarUiState`, and knows nothing
//    about how they are applied. The avatar picker does not know what an
//    avatar directory is and the theme picker does not know what a palette is.

// The label column. Wide enough for "Theme" and the words that are coming
// (Voice, Language, Endpoint), narrow enough to leave a usable combo in 360 px.
constexpr float kSettingsLabel = 96.0f;

void settings_heading(const char* text) {
  ImGui::Spacing();
  ImGui::TextColored(dim(), "%s", text);
  ImGui::Separator();
}

// One "label: control" row. Returns with the cursor on the control, already
// sized to the rest of the row, so the caller submits exactly one widget.
void settings_row(const char* label) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(kSettingsLabel);
  ImGui::SetNextItemWidth(-FLT_MIN);
}

// A picker over names the app discovered at runtime. `current` is both the
// value shown and where a new choice is written; it is left alone if nothing
// was clicked. Names come from the art, so the list can be empty (no avatar
// directory at all) and the current value can be absent from it (a theme
// deleted from the file since it was chosen) — both are shown as they are
// rather than corrected, because the correction is somebody else's job and a
// picker that quietly changed the setting it was showing would hide it.
bool name_picker(const char* id, const std::vector<std::string>& names, std::string& current) {
  bool changed = false;
  const char* preview = current.empty() ? "(none)" : current.c_str();
  if (ImGui::BeginCombo(id, preview)) {
    for (const std::string& name : names) {
      const bool selected = name == current;
      if (ImGui::Selectable(name.c_str(), selected) && !selected) {
        current = name;
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

// ---- M1c.5: the colour picker and what it derives --------------------------
//
// One control the user drives — the body — and three swatches they do not.
// The derivation itself is not here: it is avatar_derive_palette(), the same
// function that colours the avatar, so the swatches cannot drift from the
// character on screen the way a second implementation would.
//
// The picker is `ColorPicker3`, drawn **inline**, not `ColorEdit3`, which
// opens its picker in a popup. An ImGui popup is a floating window, and
// nothing in this app may be drawn outside the one composition surface the
// window owns — a popup here is silently cut off at the window's edge. Inline
// costs vertical space inside a child that scrolls, which is the surface's
// whole design; a popup would have cost correctness.
//
// `NoOptions` is not cosmetic either: without it a right-click on the hue bar
// opens a context menu, which is a popup by another name.

ImVec4 rgba_to_vec(std::uint32_t c) {
  return ImVec4(static_cast<float>(avatar_r(c)) / 255.0f, static_cast<float>(avatar_g(c)) / 255.0f,
                static_cast<float>(avatar_b(c)) / 255.0f,
                static_cast<float>(avatar_a(c)) / 255.0f);
}

void custom_colour_section(AvatarUiState& state, const AvatarOptions& options) {
  const AvatarDerivedPalette& d = options.derived;

  // A plain label rather than settings_heading(): the surface is 260 px tall
  // and scrolls, and every separator spent here is a row of the picker pushed
  // under the fold. What has to be above the fold is the control itself and
  // the numbers that say what it did — the prose can be scrolled to.
  ImGui::Spacing();
  ImGui::TextColored(dim(), "Body colour");

  // Sized rather than stretched: ColorPicker3 draws a square of whatever width
  // it is given, and -FLT_MIN in a 360 px panel would make a 330 px square
  // with no room beside it for anything that explains the result.
  ImGui::SetNextItemWidth(150.0f);
  const bool changed = ImGui::ColorPicker3(
      "##custom_body", state.custom_colour,
      ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoOptions |
          ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoLabel |
          ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueBar);
  // The edge, not the level: main.cpp pushes the colour down and persists it
  // off this, and a drag that reported "changed" on every frame it was merely
  // held still would turn one settings write into sixty.
  state.custom_colour_changed = changed;

  // The honest readout, beside the control rather than under it. The hue is
  // never moved off the true complement, so what these numbers report is how
  // far the *lightness* had to travel to keep the features apart from the
  // body — and, where it could not travel far enough, that it could not,
  // rather than a hue quietly swapped for one that was easier to read.
  ImGui::SameLine();
  ImGui::BeginGroup();
  // Both swatches on one row, and no swatch for the glint. Every row spent
  // here is a row of readout pushed under the fold of a 260 px surface, and
  // the glint is the features' own colour at the art's own alpha — derived
  // from the square beside it by construction, so a third square would say
  // nothing the second one does not.
  const float sw = ImGui::GetTextLineHeight();
  ImGui::ColorButton("##sw_body", rgba_to_vec(d.body),
                     ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                     ImVec2(sw, sw));
  ImGui::SameLine(0.0f, 4.0f);
  ImGui::ColorButton("##sw_feat", rgba_to_vec(d.feature),
                     ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                     ImVec2(sw, sw));
  ImGui::SameLine(0.0f, 6.0f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(dim(), "body/ink");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The body you picked, and the ink derived from it:\nthe eyes, the mouth and "
                      "the gloss on the dome.\nThe one translucent ink (the glint in think.txt)\n"
                      "takes the second colour at its own alpha.");
  ImGui::PushStyleColor(ImGuiCol_Text,
                        d.contrast_short ? warn() : (d.contrast >= 4.5f ? good() : dim()));
  ImGui::Text("%.1f:1 contrast", static_cast<double>(d.contrast));
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Features against body, the WCAG ratio.\nThe rule stops lifting at %.1f:1.",
                      static_cast<double>(kAvatarMinContrast));
  ImGui::TextColored(dim(), "hue %.0f to %.0f", static_cast<double>(d.hue),
                     static_cast<double>(d.feature_hue));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The features take the exact opposite hue on the\nwheel. This never moves: "
                      "only the lightness below\nis adjusted, and only as far as it must be.");
  ImGui::TextColored(dim(), "light %.0f%% to %.0f%%", static_cast<double>(d.body_l * 100.0f),
                     static_cast<double>(d.feature_l * 100.0f));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The one adjustment. The features are lifted to the\nfirst lightness that "
                      "keeps them apart from the body -\nthe first, because every step up is a "
                      "step toward\nwhite and away from the complement's colour.");

  // The short forms live here, in the column, so a pick that went wrong says
  // so without the user having to scroll for it; the sentence that explains
  // why is a hover away and again in full below.
  if (d.contrast_short) {
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("%.1f:1 is all", static_cast<double>(d.contrast));
    ImGui::PopStyleColor();
  }
  if (d.achromatic) {
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("grey: hue nominal");
    ImGui::PopStyleColor();
  }
  if (d.gloss_inverted) {
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("gloss inverted");
    ImGui::PopStyleColor();
  }
  if (d.body_faint_dark || d.body_faint_light) {
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("zzz / ? / steam faint");
    ImGui::PopStyleColor();
  }
  ImGui::EndGroup();

  if (d.contrast_short) {
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("Nothing on this hue's opposite reaches %.1f:1 against this body - %.1f:1 "
                       "is the most there is, so the eyes and the gloss will be faint at 16 px. "
                       "The hue is still the true complement; it has not been swapped for one "
                       "that reads more easily.",
                       static_cast<double>(kAvatarMinContrast), static_cast<double>(d.contrast));
    ImGui::PopStyleColor();
  }
  if (d.gloss_inverted) {
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("This complement carries less light than the body, so the highlight on the "
                       "top-left of the dome reads as a scuff rather than a gloss. Lifting it "
                       "until it did not would mean taking it to near-white and losing the "
                       "complement's colour, so it has been left as the wheel says.");
    ImGui::PopStyleColor();
  }
  if (d.achromatic) {
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("A grey body has no hue to be opposite, so this complement is nominal: the "
                       "features carry the minimum tint rather than a second grey.");
    ImGui::PopStyleColor();
  }
  if (d.body_faint_dark || d.body_faint_light) {
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("zzz, the question mark and the steam puff are drawn in body ink alone - "
                       "they have no second ink to rescue them. At this %s body they are exactly "
                       "as easy to see on the desktop as the body is, and no easier.",
                       d.body_faint_dark ? "dark" : "pale");
    ImGui::PopStyleColor();
  }
  ImGui::TextColored(dim(), "Picking a named theme above seeds this from it. The colour is stored "
                            "in settings.json and never in avatar.json, so hand-editing the art "
                            "and this control cannot overwrite each other.");
}

// ---- M8.3: the language section ---------------------------------------------
//
// Two checkboxes with one rule: at least one is always on. The rule is
// enforced by **disabling the last enabled box**, not by rejecting the click
// afterwards. A control that can be clicked and then springs back is read as a
// broken app; a control that cannot be clicked, and says why on hover, is read
// as a rule. `AllowWhenDisabled` on the hover test is what makes the second
// half of that possible — ImGui suppresses tooltips on disabled items by
// default, and the tooltip is the entire explanation.
//
// A language that is off stops both halves of the loop, which is the user's
// own decision and the reason this is one control and not two: the recogniser
// is pinned away from it and Claude is told not to reply in it.

// One language row. `other` is the other language's flag, which is what decides
// whether this one is the last one standing.
void language_row(const char* label, const char* id, bool& value, bool other,
                  const char* on_tip, const char* off_tip) {
  const bool locked = value && !other;
  settings_row(label);
  ImGui::BeginDisabled(locked);
  ImGui::Checkbox(id, &value);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (locked)
      ImGui::SetTooltip("This is the only language left on.\nSwitch the other one on first - the "
                        "app has to be\nable to hear and speak in something.");
    else
      ImGui::SetTooltip("%s", value ? on_tip : off_tip);
  }
}

void language_section(AvatarUiState& state, const AvatarOptions& options) {
  settings_heading("Language");
  language_row("English", "##lang_en", state.lang_english, state.lang_japanese,
               "On: heard and spoken.",
               "Off: Claude replies only in Japanese, and the\nrecogniser is pinned to Japanese.");
  language_row("Japanese", "##lang_ja", state.lang_japanese, state.lang_english,
               "On: heard and spoken (VOICEVOX 四国めたん).",
               "Off: Claude replies only in English, the recogniser\nis pinned to English, and the "
               "Japanese voice is not\nloaded at all - about a second off every start.");

  // What the choice actually did, in the surface that made it. The recogniser
  // line is the one with a measured payoff behind it, so it says the value and
  // the hover says why it matters.
  ImGui::TextColored(dim(), "Recogniser: %s", options.stt_language);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Both languages on means \"auto\", which is inertial:\nit switches English to "
                      "Japanese and never back, and a\nshort Japanese phrase inside an English "
                      "sentence is\nsilently dropped. With one language on it is pinned\nto that "
                      "language instead, and that failure cannot happen.");

  // The on-demand voice, stated rather than announced. Nothing here is ever
  // spoken: a voice that introduced itself when it finished loading would be
  // an announcement the mute button never asked for, and it would arrive
  // while the user was reading the checkbox they had just ticked.
  switch (options.japanese_voice) {
    case VoiceSession::VoiceLoad::Loading:
      ImGui::TextColored(warn(), "Japanese voice: loading...");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Loading now, on its own thread. Until it is ready\nthe app stays in "
                          "English rather than speaking\nJapanese through the English voice.");
      break;
    case VoiceSession::VoiceLoad::Failed:
      ImGui::PushStyleColor(ImGuiCol_Text, warn());
      ImGui::TextWrapped("Japanese voice failed to load, so Japanese stays off however this box is "
                         "set: %s", options.japanese_voice_error.c_str());
      ImGui::PopStyleColor();
      break;
    case VoiceSession::VoiceLoad::Absent:
      if (state.lang_japanese) {
        ImGui::TextColored(dim(), "Japanese voice: loads when the session is up.");
      } else {
        ImGui::TextColored(dim(), "Japanese voice: not loaded.");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Skipped at startup, on purpose. Switching Japanese\non loads it then "
                            "and there - the window keeps\nrunning while it does.");
      }
      break;
    case VoiceSession::VoiceLoad::Ready:
      ImGui::TextColored(dim(), "Japanese voice: ready.");
      break;
  }

  // The honest limit, said once, where the setting is made. Speaking a
  // switched-off language is not detected and not corrected: the recogniser is
  // pinned, so the words come back as whatever English the pin can make of
  // them. Building a detector would mean running `auto` underneath the pin,
  // which is the failure mode this setting exists to remove.
  // TextWrapped, not TextColored: the panel is 360 px wide and a sentence this
  // long is simply cut off at the right edge otherwise, which is how a caveat
  // turns into a truncated fragment.
  ImGui::PushStyleColor(ImGuiCol_Text, dim());
  ImGui::TextWrapped("Speaking a language that is off transcribes as nonsense rather than being "
                     "detected: the recogniser is pinned, not listening for it.");
  ImGui::PopStyleColor();
}

// ---- M1f.2: the auto-listen timeout ------------------------------------------
//
// One setting, two widgets, and the second one is why. A single numeric
// control would have to hold "never" somewhere in its range, and every way of
// doing that is worse than a tick box:
//
//  - A minimum of zero puts 1-14 s inside the control, and the mechanism then
//    rewrites anything under its own floor. The setting would lie about
//    itself, which is the failure this task was told to avoid.
//  - A minimum at the floor makes "never" unreachable, and the user asked for
//    it explicitly.
//  - A sentinel step below the floor displayed as the word "Never" reads well
//    until someone ctrl+clicks it to type a number, at which point ImGui shows
//    them the sentinel integer. A control with a value that only means
//    something to the code behind it is a control that cannot be trusted.
//
// The tick box is therefore the whole of "never", and the number beside it
// starts at the floor and never leaves the legal range. Nothing in between can
// be expressed. **Both are one value in storage** — seconds, 0 for never
// (`listen_timeout_seconds()`), the same spelling `Config::listen_timeout` and
// `VoiceSession::set_listen_timeout()` already use — so the tick box is an
// affordance and not a second piece of state that can contradict the first.
//
// The prose under it is not decoration. This timeout only ever fires when the
// user is not at the desk, so unlike every other control in this panel it can
// never teach itself through use: whatever they understand about it, they
// understand from reading it cold. So the line says what will happen, in
// seconds, in the tense it will happen in — and the word "Never" appears on
// screen when it is off rather than being implied by an empty box.
void listen_timeout_section(AvatarUiState& state) {
  settings_row("Stop listening");
  ImGui::Checkbox("##listen_timeout_on", &state.listen_timeout_on);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("On: a microphone you latched by clicking Talk\ncloses itself once it has "
                      "heard no voice for a\nwhile, and says nothing when it does.\n\nOff: it "
                      "stays open until you close it, which is\nwhat the app did before this "
                      "setting existed.");

  settings_row("After");
  ImGui::BeginDisabled(!state.listen_timeout_on);
  // DragInt rather than SliderInt: a slider in a 360 px panel is about 200 px
  // for 585 values, so landing on a round number is luck, and a click anywhere
  // on a slider's track jumps the value under the pointer. A drag moves only
  // while the pointer does, ctrl+click types an exact number, and neither
  // gesture moves the control itself — this project has shipped one bug
  // already from a widget that moved between the press and the release.
  //
  // AlwaysClamp is what makes the floor a property of the control rather than
  // advice: it binds the typed value as well as the dragged one. It binds only
  // while the user is in the widget, which is deliberate — a value inherited
  // from AII_LISTEN_TIMEOUT or a hand edit is shown as it is, and said to be
  // out of range below, rather than being silently corrected by a control
  // nobody has touched.
  ImGui::DragInt("##listen_timeout_sec", &state.listen_timeout_sec, 1.0f,
                 kListenTimeoutUserFloorSec, kListenTimeoutMaxSec, "%d s",
                 ImGuiSliderFlags_AlwaysClamp);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Drag to change it; ctrl+click to type a number.\n%d to %d seconds.\n\n"
                      "It stops no lower than %d s on purpose: shorter\nthan that and a pause for "
                      "thought closes the\nmicrophone, which reads as the app breaking\nrather "
                      "than as a timeout. Untick for never.",
                      kListenTimeoutUserFloorSec, kListenTimeoutMaxSec,
                      kListenTimeoutUserFloorSec);

  ImGui::PushStyleColor(ImGuiCol_Text, dim());
  if (state.listen_timeout_on) {
    ImGui::TextWrapped("The latch closes after %d s with no voice, silently - by then you are not "
                       "at the desk to be told. Hold-to-talk is never cut off.",
                       state.listen_timeout_sec);
  } else {
    // Trimmed to the same two wrapped lines the ticked variant takes, which is
    // not a style choice. The surface scrolls, this section is near the bottom
    // of it, and a reader who has scrolled to the end is sitting at max
    // scroll: shorten the content under them and ImGui clamps the scroll,
    // which slides the tick box they are about to click a dozen pixels out
    // from under the pointer. This project has already shipped one bug from a
    // control that moved between the press and the release. What the two lines
    // give up — "this is what the app did before the setting existed" — the
    // tick box's own tooltip still says.
    ImGui::TextWrapped("Never: a latched microphone stays open until you close it, however long "
                       "the room stays quiet. Hold-to-talk is unaffected.");
  }
  ImGui::PopStyleColor();

  // A value the control could not have produced: AII_LISTEN_TIMEOUT, or a
  // hand-edited settings.json. It is in force exactly as it stands — the
  // mechanism honours anything down to a second — and the honest thing is to
  // say so rather than to quietly round it up to the floor and leave the user
  // wondering why their five seconds became fifteen.
  if (state.listen_timeout_on && state.listen_timeout_sec < kListenTimeoutUserFloorSec) {
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("%d s came from AII_LISTEN_TIMEOUT or a hand edit, and is in force as it "
                       "stands. The control itself stops at %d s, so touching it will take the "
                       "value up there and it cannot come back down here.",
                       state.listen_timeout_sec, kListenTimeoutUserFloorSec);
    ImGui::PopStyleColor();
  }
}

// ---- M12.2: the wake phrase ---------------------------------------------------
//
// One text box, and everything interesting about it is what it says underneath.
//
// **The box is the whole of the switch.** There is no tick box beside it, for
// the reason the timeout section spends a paragraph arriving at from the other
// direction: an empty phrase is not a broken setting, it is the feature being
// off, and it is the shipped default. A separate on/off would be a second
// piece of state that can disagree with the first — "on, with no phrase" has
// no meaning, and "off, with a phrase" is a value the app is not honouring.
// Clearing the box is switching it off, and the line underneath says so in
// those words.
//
// **The prose is not decoration, and it is not reassurance.** This control
// opens the user's microphone and leaves it open. What is written under it is
// the only place the deal is stated in full before they make it: the mic is
// on, the matching happens here, nothing is sent until the phrase lands. The
// user chose always-on over match-only-while-open, so what they are owed is
// not a warning but an accurate description — and the microphone button's
// seventh face is the same sentence said continuously afterwards.
void wake_phrase_section(AvatarUiState& state) {
  settings_row("Wake word");
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##wake_phrase", "off - type a name to switch it on",
                           state.wake_phrase, sizeof(state.wake_phrase));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("A word or short phrase - a name for the AI, or anything\nyou would not say "
                      "by accident.\n\nCase, spaces and punctuation are ignored, and it is\nfound "
                      "inside a longer sentence, so \"Hey, Aria!\" and\n\"hey aria what time is "
                      "it\" both match \"Aria\".\n\nClear the box to switch it off.");

  const std::string phrase(state.wake_phrase);
  const std::string problem = wake_phrase_problem(phrase);
  if (!problem.empty()) {
    // A refusal, in the one place the user is looking. The phrase is *not*
    // armed while this is showing, and the sentence says why rather than
    // leaving a box that was typed into and does nothing.
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("%s", problem.c_str());
    ImGui::PopStyleColor();
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, dim());
  if (wake_phrase_armed(phrase)) {
    ImGui::TextWrapped("The microphone stays on whenever you are not already talking, and "
                       "everything it hears is matched here, on this machine. Nothing is sent "
                       "anywhere until you say \"%s\" - and what you said to wake it is not sent "
                       "either. The mic button shows a teal ripple the whole time it is on.",
                       phrase.c_str());
  } else {
    ImGui::TextWrapped("Off. The microphone only opens when you click Talk or hold SPACE. Type a "
                       "name here and it will stay on, listening on this machine for that one "
                       "word.");
  }
  ImGui::PopStyleColor();
}

// ---- M3.11: which model the one you talk to runs on --------------------------
//
// The user asked for it in one line: "make a setting to choose what base model
// you would like to use". Three decisions, all of them visible on screen
// rather than only here.
//
// **A short list of aliases, not a text field and not dated ids.** The CLI
// takes `opus`/`sonnet`/`haiku` as well as full model names. A pinned dated id
// rots — the day it is retired the app starts a child that fails every turn —
// and a free-text field lets a typo do the same thing today, with no way for
// the window to say why until the first reply has already failed. So the
// control is a picker over a table whose every entry completed a real turn on
// this machine (`core/model_choice.h` names the probes), and the escape hatch
// for anything else is `AII_MODEL`, which the section says is in force when it
// is.
//
// **Default is a real entry, and it is the default.** Not passing `--model` at
// all is what this app did before the setting existed and is still what it
// does out of the box; it has to stay reachable, so it is the first row rather
// than an empty selection.
//
// **A picker cannot reach the running Claude**, exactly as a tick box cannot
// (M3.8). `--model` is fixed when the child process starts. M3.12 answers that
// the way the user chose — the child is restarted on the new model at once and
// the conversation goes with it — so the line under the picker no longer names
// the next app start. It names the restart that is happening, and the model
// Claude is still holding until it finishes. A setting that appears to do
// nothing is the worst failure a setting has; one that quietly throws a
// conversation away without saying so is the second.
//
// **Workers are not in this.** They are separate `claude` processes with their
// own grant and their own prompt (`WorkerPool::spawn` sets no model at all),
// and the user said "the base model you would like to use", which is the thing
// they talk to. One control that silently changed both would be one control
// meaning two things. Said in a dim line rather than left to be discovered.
// ---- M3.12: the line both sections say it with --------------------------------
//
// Model and Tools have the same fact to state and it is now a four-state one,
// so it is written once. `still` is the section's own clause for what the
// running child is holding *until* the restart lands — the only part of the
// sentence the two sections do not share — and it is always present, because
// "in a moment" is a promise and the user is entitled to know what is true
// meanwhile.
//
// The tense is the point. Before M3.12 the amber line described a state that
// would last until the app was next started; now it describes something that
// finishes in about a second, so it says what is happening rather than what is
// prevented.
void llm_restart_line(const AvatarOptions& options, const std::string& still) {
  ImGui::PushStyleColor(ImGuiCol_Text, warn());
  if (options.llm_restart_running)
    ImGui::TextWrapped("Restarting Claude on it now, and the conversation so far goes with it. %s",
                       still.c_str());
  else if (options.llm_restart_waiting_turn)
    ImGui::TextWrapped("Saved. Claude restarts on it as soon as this reply finishes, and the "
                       "conversation so far goes with it. %s",
                       still.c_str());
  else if (options.llm_restart_pending)
    ImGui::TextWrapped("Saved. Claude is restarting on it, and the conversation so far goes with "
                       "it. %s",
                       still.c_str());
  else if (options.llm_restart_live)
    // Live, and yet nothing is pending: the setting and the running child
    // disagree over something the control was never moved to ask for — an
    // AII_MODEL naming a dated id the picker cannot produce is the case that
    // reaches this. Restarting on it uninvited would throw a conversation away
    // to honour a choice nobody made this run, so it says what a change here
    // would do instead.
    ImGui::TextWrapped("Changing this restarts Claude on it, and the conversation so far goes with "
                       "it. %s",
                       still.c_str());
  else
    // No child to restart: --no-voice, or the engines have not come up. The
    // old promise, and here it is the true one.
    ImGui::TextWrapped("Saved, and it reaches Claude when one is next started. %s", still.c_str());
  ImGui::PopStyleColor();
}

// M3.12. Said before the gesture rather than after it, because after it the
// conversation is already gone. There is no confirm - a modal cannot be drawn
// in this window at all (an ImGui popup is a floating window and would be
// clipped at the widget's edge), and a two-press arm like Reset's belongs to a
// button that means one thing, not to a picker and three tick boxes.
const char kRestartWarning[] =
    "\n\nChanging this restarts Claude straight away: the\n"
    "model and the tool list are fixed when it starts,\n"
    "so the conversation so far is lost. Workers,\n"
    "schedules and anything they have already found\n"
    "are kept.";

void model_section(AvatarUiState& state, const AvatarOptions& options) {
  if (state.settings_scroll_model) ImGui::SetScrollHereY(0.0f);
  settings_heading("Model");
  settings_row("Base model");
  const ModelChoice& picked = model_choice(state.model);
  if (ImGui::BeginCombo("##model_pick", picked.label)) {
    for (int i = 0; i < kModelChoiceCount; ++i) {
      const ModelChoice& c = model_choice(i);
      const bool sel = i == state.model;
      if (ImGui::Selectable(c.label, sel)) state.model = i;
      // The cost is in the tooltip of the row that would incur it, and only
      // when there is something running to lose: on a --no-voice run the
      // sentence would be a threat the app cannot carry out.
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s%s", c.tip, options.llm_restart_live ? kRestartWarning : "");
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s%s", picked.tip, options.llm_restart_live ? kRestartWarning : "");

  ImGui::PushStyleColor(ImGuiCol_Text, dim());
  ImGui::TextWrapped("%s", kModelWorkersNote);
  ImGui::PopStyleColor();

  // The honest part, the same cases the Tools section has. `picked.arg` is
  // what a child started now would be given; `options.model_in_force` is what
  // the one that is running was given. They differ from the instant the picker
  // moves until the restart it causes has a new child up — and permanently
  // when AII_MODEL named something the picker cannot produce, which this
  // covers without a special case, because it names the value rather than
  // assuming it is one of ours.
  if (picked.arg != options.model_in_force) {
    llm_restart_line(options, "Until then Claude is running on " +
                                  model_label(options.model_in_force) + ".");
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, dim());
    ImGui::TextWrapped("In force now: %s.", model_label(options.model_in_force).c_str());
    ImGui::PopStyleColor();
  }
}

// ---- M3.8: what the one you talk to is allowed to do ------------------------
//
// The user asked for this section in so many words: "create a subsection
// called Tools, where the user can toggle tools on and off - file access, web
// searches". Three things had to be decided to build it, and all three are
// visible on screen rather than only in the code.
//
// **The grouping.** The CLI's built-in set is twenty-eight tools (measured,
// not read off `--help`; the list is in core/tool_policy.h). A tick box each
// would be a wall of jargon nobody can have an opinion about, and one switch
// for all of them would put `Bash` behind the same gesture as `WebSearch`. So
// they are grouped as a person groups them — internet, reading my files,
// changing my files — and the rest are withheld and *said* to be withheld,
// because "what else can it do" is the question that started this.
//
// **Writing is offered, and off.** An enabled tool here is *granted*, not
// offered: `--permission-prompts none` means there is no confirmation step,
// because nothing in this app can answer one. Reading without asking is a
// thing a person can weigh. Creating and overwriting files without asking, in
// whatever directory the app was launched from, is a thing they had to be
// asked about first — and they have now answered, by trying to use it. So the
// row is live, still off by default, and the amber line under it is what it
// always said it would be: a description of what ticking the box hands over,
// in the tense it happens in. Being offered is not being recommended.
//
// **A toggle cannot reach the running Claude.** `--allowedTools` is fixed when
// the child process starts. The honest options were: restart it and replay the
// conversation (M3.6, unbuilt), restart it and lose the conversation, or wait
// for the next launch. M3.8 shipped the third and said so; the user has since
// chosen the second ("restart immediately, lose context"), so ticking a box
// here ends the `claude` child and starts another on the new grant, and the
// conversation goes with it. The line under the boxes still exists and still
// never pretends — it now names a restart that is happening rather than one
// that is being waited for, and the cost is in each box's tooltip *before* the
// click, since afterwards there is nothing left to warn about.
void tools_section(AvatarUiState& state, const AvatarOptions& options) {
  // M3.8's pose flag; see AvatarUiState::settings_scroll_tools. Held rather
  // than applied once, exactly as Timing's is, and taken *before* the heading
  // so that the section's own name is in the frame rather than one pixel above
  // it — the point of the flag is a capture in which the thing is identifiable.
  if (state.settings_scroll_tools) ImGui::SetScrollHereY(0.0f);
  settings_heading("Tools");

  for (int i = 0; i < kToolGroupCount; ++i) {
    const ToolGroup& g = tool_group(i);
    settings_row(g.label);
    ImGui::BeginDisabled(!g.offered || !options.tools_supported);
    ImGui::Checkbox((std::string("##tool_") + g.key).c_str(), &state.tools.on[i]);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (!options.tools_supported)
        ImGui::SetTooltip("This run is on the API backend, which is not the\n"
                          "Claude Code CLI and has no tools at all.");
      else if (!g.offered)
        ImGui::SetTooltip("Not offered yet, and off.\n\nA tool switched on here runs without asking "
                          "you\nfirst - there is no confirmation step, because\nnothing in this "
                          "window could answer one. That is\nsurvivable for reading a file and is "
                          "not\nsurvivable for overwriting one, so this stays with\nthe workers "
                          "until you say otherwise.\n\n%s",
                          g.tip_on);
      else
        ImGui::SetTooltip("%s%s", state.tools.on[i] ? g.tip_on : g.tip_off,
                          options.llm_restart_live ? kRestartWarning : "");
    }
    // The warning belongs under the row it is about, not in a footnote: a
    // greyed control with no visible reason reads as a bug.
    if (g.risky) {
      ImGui::PushStyleColor(ImGuiCol_Text, warn());
      ImGui::TextWrapped("A tool switched on here is granted, not offered: with this ticked, "
                         "Claude creates and overwrites files with no confirmation and no undo, "
                         "in whatever folder the app was launched from unless it is given a "
                         "path. Leave it off and writing stays a worker's job.");
      ImGui::PopStyleColor();
    }
  }

  ImGui::PushStyleColor(ImGuiCol_Text, dim());
  ImGui::TextWrapped("%s", kToolsWithheld);
  ImGui::PopStyleColor();

  // The honest part. Two cases, and the surface is never silent about either.
  if (!options.tools_supported) {
    ImGui::PushStyleColor(ImGuiCol_Text, dim());
    ImGui::TextWrapped("Not in use this run: the API backend has no tools.");
    ImGui::PopStyleColor();
  } else if (state.tools != options.tools_in_force) {
    llm_restart_line(options, "Until then Claude still holds what it was launched with: " +
                                  tool_summary(options.tools_in_force) + ".");
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, dim());
    ImGui::TextWrapped("In force now: %s.", tool_summary(state.tools).c_str());
    ImGui::PopStyleColor();
  }
}

// ---- M1f.5: the app starts listening by itself -------------------------------
//
// (user, 19 Sep 2026: "I would like the toggle in the settings to have the AI
// auto start by listening. And I would like this by default to be on.")
//
// **Its own heading, and it is neither Timing nor Voice.** Timing is about
// durations the app spends, Voice is about which voice speaks; this is about
// what state the app comes up in, which is a third thing and the first of its
// kind here — the avatar mode and mute are startup behaviour too, but they are
// *levels* that happen to persist, while this one describes a single act
// performed once per launch. A row that means "do this at startup" filed under
// a heading about endpointing would be findable only by someone who already
// knew where it was.
//
// **It is placed immediately above Timing**, which is not decoration either.
// The two rows that result read as one pair on screen —
//
//     Startup   Start listening  [x]
//     Timing    Stop listening   [x]  After [60 s]
//
// — and that pairing is the whole of the interaction between them. With both
// on, a user who launches the app and walks away starts listening and is
// closed again a minute later; the only place that is discoverable is a
// surface where the two controls are next to each other and can be read in one
// glance. The alternative, Startup at the top of the surface with sixty
// seconds of scrolling between them, hides a real consequence behind a
// scrollbar.
//
// **The line underneath is the honest part, and it is required.** This setting
// is read exactly once, at the moment the loading screen leaves, so changing
// it now cannot do anything now — the failure Tools already documents, in a
// milder form. It therefore never pretends: the prose says what this run did,
// and the moment the box disagrees with what this run did, an amber line names
// the next launch. It does not say "restart to apply", because the microphone
// button beside it will open the microphone this second and that is what a
// user who wants it open *now* should be pointed at.
void startup_section(AvatarUiState& state, const AvatarOptions& options) {
  // The pose flag before the heading, as Tools' is: the point of the flag is a
  // capture in which the section's own name is in the frame.
  if (state.settings_scroll_startup) ImGui::SetScrollHereY(0.0f);
  settings_heading("Startup");

  settings_row("Start listening");
  ImGui::BeginDisabled(!options.voice_enabled);
  ImGui::Checkbox("##auto_listen", &state.auto_listen);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (!options.voice_enabled)
      ImGui::SetTooltip("This run was started with --no-voice, so there\nare no engines and nothing "
                        "to listen with.");
    else
      ImGui::SetTooltip("On: once the engines have finished loading, the\napp latches the "
                        "microphone on by itself - the\nsame state clicking the microphone puts it "
                        "in,\nnot hold-to-talk.\n\nOff: it comes up idle and waits for you to "
                        "click.\n\nEither way this is read once, when the app\nstarts.");
  }

  ImGui::PushStyleColor(ImGuiCol_Text, dim());
  if (state.auto_listen) {
    // Says the whole sequence in the order it happens, because the one second
    // it describes has already gone by for anybody reading this. The timeout
    // is named here rather than left to Timing below: "it starts listening"
    // and "it stops listening on its own" are the same sentence for a user who
    // launches the app and goes to make tea, and this is the surface where
    // that sentence can be finished.
    ImGui::TextWrapped("The microphone latches on as the loading screen leaves, and the app is "
                       "listening before you touch it. Speak and it answers. Stop listening below "
                       "still applies from there.");
  } else {
    ImGui::TextWrapped("The app comes up idle, with the microphone shut, and waits for you to "
                       "click it.");
  }
  ImGui::PopStyleColor();

  if (!options.voice_enabled) {
    ImGui::PushStyleColor(ImGuiCol_Text, dim());
    ImGui::TextWrapped("Not in use this run: --no-voice loads no engines.");
    ImGui::PopStyleColor();
  } else if (state.auto_listen != options.auto_listen_in_force) {
    // The gap, named. Amber rather than dim for the same reason Tools' line
    // is: this is the state in which a control genuinely is not doing the
    // thing it describes, and that has to look different from the state in
    // which it is.
    ImGui::PushStyleColor(ImGuiCol_Text, warn());
    ImGui::TextWrapped("Saved, and it decides what happens the next time you start the app. This "
                       "run started %s, and nothing here changes that now - the microphone button "
                       "at the top of the panel opens and closes it today.",
                       options.auto_listen_in_force ? "listening" : "idle");
    ImGui::PopStyleColor();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, dim());
    ImGui::TextWrapped("This run started %s.", options.auto_listen_in_force ? "listening" : "idle");
    ImGui::PopStyleColor();
  }
}

void settings_surface(AvatarUiState& state, const AvatarOptions& options) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_color(0.055f, 0.063f, 0.082f));
  ImGui::BeginChild("##settings", ImVec2(0.0f, kChatHeight), ImGuiChildFlags_None, 0);

  // The controls are toned to the panel, the same way the message field is:
  // this window is a calm dark strip in the corner of somebody's desktop and
  // ImGui's default frame blue reads as a row of lit controls in it. Pushed
  // before the first widget, so the close button is in the same register as
  // the pickers below it.
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ui_color(0.071f, 0.078f, 0.098f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui_color(0.110f, 0.120f, 0.150f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui_color(0.130f, 0.142f, 0.178f));
  ImGui::PushStyleColor(ImGuiCol_Button, ui_color(0.110f, 0.120f, 0.150f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_color(0.150f, 0.163f, 0.205f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui_color(0.180f, 0.196f, 0.245f));
  // The tick, toned with the rest. ImGui's default check mark is its accent
  // blue, which on this palette reads as a lit control in a panel where every
  // other frame is deliberately dark — the one bright thing in the widget
  // would be a checkbox.
  ImGui::PushStyleColor(ImGuiCol_CheckMark, fg());
  ImGui::PushStyleColor(ImGuiCol_Header, ui_color(0.140f, 0.153f, 0.192f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui_color(0.170f, 0.185f, 0.232f));
  ImGui::PushStyleColor(ImGuiCol_Border, ui_color(0.16f, 0.17f, 0.21f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(fg(), "Settings");
  ImGui::SameLine();
  // A way out that is not the cog. The cog is at the top of the panel and the
  // panel's top moves up when this opens — closing from inside the surface is
  // the one control guaranteed not to have gone anywhere since it appeared.
  ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
  if (ImGui::SmallButton("x##settings_close")) state.settings_open = false;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close settings");

  settings_heading("Appearance");
  settings_row("Avatar");
  name_picker("##avatar_pick", options.avatars, state.avatar_name);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Which definition is drawn.\nOne directory per avatar, under\n%%APPDATA%%\\AIInterface\\avatars.");
  settings_row("Theme");
  name_picker("##theme_pick", options.themes, state.theme);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("A named palette in this avatar's avatar.json,\nor \"custom\" to pick the body colour yourself.\nEvery sprite shares it, so the thought bubble\nfollows the body.");

  if (options.custom_theme) custom_colour_section(state, options);

  if (!options.art_status.empty()) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, options.art_status_ok ? dim() : warn());
    ImGui::TextWrapped("%s", options.art_status.c_str());
    ImGui::PopStyleColor();
  }

  // The rest of the surface, stated rather than implied. These are the
  // settings the milestone plan already names as coming here (voices,
  // endpoint timing, paths); they are listed so that what this surface is for
  // is visible from inside it, and so the next section is an addition to a
  // shape that exists rather than a decision to be taken again.
  language_section(state, options);

  // M2.6. Scripting's only surface in the app, and the only place a script
  // that failed to load says so — the traceback goes to the log, but the log
  // is a console this window does not have.
  settings_heading("Scripts");
  if (options.script_status.empty()) {
    ImGui::TextColored(dim(), "None. Put a .py in scripts\\ to start;");
    ImGui::TextColored(dim(), "scripts\\examples\\ has one to copy.");
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, options.script_status_ok ? dim() : warn());
    ImGui::TextWrapped("%s", options.script_status.c_str());
    ImGui::PopStyleColor();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Python, in this process, over the app bus.\n"
                      "%%APPDATA%%\\AIInterface\\scripts\\*.py runs at startup;\n"
                      "nothing below that directory is scanned.");

  // ---- M10.5: the two consent switches, and the actions themselves -------
  //
  // **This is the only place either switch can be changed.** Both are
  // `NotSettable` in `kSettingKeys`, so the AI can read them and name them but
  // cannot write them: a gate the gated party can open is not a gate.
  settings_row("Allow scripts");
  ImGui::Checkbox("##scripts_authoring", &state.scripts_authoring);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Whether scripts the AI wrote may run at all.\n"
                      "Off by default. This does not stop it writing a file --\n"
                      "with file changes on it can already do that -- it stops\n"
                      "this app from loading and offering what it finds.");

  settings_row("Auto allow new");
  ImGui::Checkbox("##scripts_auto_allow", &state.scripts_auto_allow);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("On: a newly written script is allowed straight away.\n"
                      "Off (the default): a small window asks you first, once\n"
                      "per new script. Either way it is one decision per script,\n"
                      "never one per call.");

  if (!options.scripts.empty()) {
    ImGui::Spacing();
    for (const AvatarOptions::ScriptRow& r : options.scripts) {
      ImGui::PushID(r.name.c_str());
      ImGui::PushStyleColor(ImGuiCol_Text, r.armed ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                                   : warn());
      ImGui::TextUnformatted(r.name.c_str());
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered() && !r.description.empty())
        ImGui::SetTooltip("%s", r.description.c_str());
      // An action the app found but will not run is **listed and refused with
      // its own word**, never hidden: the whole hazard of a thing that is
      // quietly not there is that nobody can say how it went missing.
      if (!r.in_digest) {
        ImGui::SameLine();
        ImGui::TextColored(warn(), "(over the limit)");
      } else if (!r.armed) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Allow")) state.script_arm = r.name;
      }
      ImGui::SameLine();
      // Undo is deleting the file, and this is the whole of it: no versioning,
      // no quarantine, no trash. The AI cannot press this.
      if (ImGui::SmallButton("Delete")) state.script_delete = r.name;
      ImGui::PopID();
    }
  }
  if (ImGui::SmallButton("Open scripts folder")) state.scripts_open_folder = true;

  // M3.8. Above Timing rather than below it: this is the section that decides
  // what Claude can do to the user's machine, and it does not belong under a
  // heading about endpointing.
  // M3.11. Directly above Tools: both are facts about the `claude` child that
  // are fixed when it starts, both carry the same "at the next start" line,
  // and reading them together is how "what am I talking to, and what may it
  // do" is one question rather than two.
  model_section(state, options);
  tools_section(state, options);

  settings_heading("Voice");
  ImGui::TextColored(dim(), "Which voice speaks each language: M8.");
  // M1f.5. Deliberately the section immediately above Timing; startup_section
  // says why at length, and the short version is that "start listening" and
  // "stop listening" are one pair and have to be read as one.
  startup_section(state, options);
  settings_heading("Timing");
  // M1f.2's pose flag; see AvatarUiState::settings_scroll_timing. Held rather
  // than applied once, because a run that is being screenshotted is one where
  // nothing else is going to scroll this surface anyway.
  if (state.settings_scroll_timing) ImGui::SetScrollHereY(0.0f);
  listen_timeout_section(state);
  // M12.2. Directly under "Stop listening", and that placement is the same
  // argument startup_section makes about sitting above it: the three controls
  // the user now has over the microphone are "start it at launch", "stop it
  // when the room goes quiet" and "start it when I say this", and they are one
  // idea read top to bottom. The wake phrase in particular only makes sense
  // next to the timeout — the timeout is what keeps putting the app into the
  // state the wake phrase gets it out of.
  wake_phrase_section(state);
  // Left as it was (eae9061): the rest of Timing is still a stub, and it reads
  // correctly under a control rather than instead of one.
  ImGui::TextColored(dim(), "Endpointing and early speech: M2.8.");
  settings_heading("Paths");
  ImGui::TextColored(dim(), "Models, avatars and the working directory.");

  ImGui::PopStyleVar();
  ImGui::PopStyleColor(10);
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void separator() {
  ImGui::SameLine(0.0f, 6.0f);
  ImGui::TextColored(dim(), "|");
  ImGui::SameLine(0.0f, 6.0f);
}

// Defined with the rest of the close gesture, down among the transport row's
// faces and skins, because that is where its sibling (Reset) lives and the two
// must stay identical. Declared here because this is the row it is drawn on.
// See the definition for why it is on this row at all.
void close_button(AvatarUiState& state, const VoiceSession::Snapshot& snap, float size,
                  AvatarUiResult& out);

// "12% CTX | 40% Session (2h14m) | 53% Week (3d 5h)" plus the chat toggle,
// pinned to the right edge of the same row. The bracketed times count down to
// when each window actually resets, as reported by the CLI.
// While loading none of the three windows has a number yet, so the row would
// read "--% CTX | --% Session | --% Week" — three placeholders that say
// nothing while the overlay's own caption is already saying what is happening.
// The row still draws (empty) so the panel keeps its height and the chat
// toggle keeps its place; the toggle is disabled with the rest of the
// controls, since there is no chat to open until the engines are up.
//
// Three buttons on the right now, not two (user, 19 Sep 2026): the avatar-mode
// disc, the chat arrow, and Close outboard of both. The cursor is set from the
// right edge, so the arithmetic is "three buttons and two gaps" — it was two
// and one — and getting that wrong is how the cluster walks off the panel or
// lands on the usage text.
void status_bar(AvatarUiState& state, const VoiceSession::Snapshot& snap, bool loading,
                float width, AvatarUiResult& out) {
  const UsageStats& usage = snap.usage_stats;
  const float button = ImGui::GetFrameHeight();
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  const float pad = ImGui::GetStyle().WindowPadding.x;

  // The third button costs this row 27 px of text, and at 360 px the row did
  // not have 27 px to spare: "12% CTX | 40% Session (2h14m) | 53% Week (3d 5h)"
  // fitted the two-button cluster to the pixel and ran under the third. Text
  // that disappears under an opaque plate is the worst of the options — it
  // reads as a rendering bug, and "(3d 5h" with no closing bracket is a number
  // the user cannot trust.
  //
  // So the brackets are the part that gives way, and Week's goes first. They
  // are the one thing on the row that is duplicated elsewhere: hovering
  // Session or Week says "Week resets at Thu 25 Sep 14:00" in full, and that
  // tooltip is offered whether or not the bracket was drawn. The percentages,
  // which are the reason anyone looks at this row, are never touched.
  //
  // **Week's first** because it is both the widest ("3d 5h" against "2h14m")
  // and the least worth watching: a seven-day window that rolls over in three
  // days is not a number anyone is counting down. The five-hour window is.
  //
  // Measured, not assumed: this depends on the font, and the font is whichever
  // of Segoe UI / Tahoma / Arial the machine has, merged with whichever CJK
  // face it has. The verdict can change as a countdown ticks from "2h14m" to
  // "2h9m" — that is a layout adapting a handful of times an hour, not a
  // per-frame flicker, and the alternative is dropping the brackets forever on
  // a row where one of them usually fits.
  const float sep = 6.0f + ImGui::CalcTextSize("|").x + 6.0f;
  const float cluster = 3.0f * button + 2.0f * gap;
  const float budget = width - 2.0f * pad - cluster - gap;
  auto row_width = [&](bool session_cd, bool week_cd) {
    return segment_width(usage.ctx, "CTX", 0, false) + sep +
           segment_width(usage.session, "Session", usage.session_reset, session_cd) + sep +
           segment_width(usage.week, "Week", usage.week_reset, week_cd);
  };
  const bool week_cd = row_width(true, true) <= budget;
  const bool session_cd = week_cd || row_width(true, false) <= budget;

  if (loading) {
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
  } else {
    segment(usage.ctx, "CTX", 20.0, 30.0);
    separator();
    segment(usage.session, "Session", 70.0, 90.0, usage.session_reset, session_cd);
    separator();
    segment(usage.week, "Week", 70.0, 90.0, usage.week_reset, week_cd);
  }

  ImGui::SameLine();
  ImGui::SetCursorPosX(width - pad - cluster);
  // Live even while loading, unlike the controls around it: it is a
  // preference about this window, not a control that routes into engines that
  // are not up yet, and setting it during the wait is when it is most natural
  // to set it. Nothing resizes until the loading screen leaves.
  visibility_button(state, button);
  ImGui::SameLine(0.0f, gap);
  ImGui::BeginDisabled(loading);
  if (ImGui::ArrowButton("##chat", state.chat_open ? ImGuiDir_Down : ImGuiDir_Right))
    state.chat_open = !state.chat_open;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", state.chat_open ? "Hide the chat" : "Show the chat");
  ImGui::EndDisabled();
  ImGui::SameLine(0.0f, gap);
  // Outboard of the arrow, hard against the right edge, and **not** inside
  // BeginDisabled: the engines take about five seconds to come up and a user
  // who wants out during that wait should not have to reach for Esc. Closing
  // is the one thing this app is always able to do.
  close_button(state, snap, button, out);
}

ImVec4 worker_color(WorkerPool::State s) {
  switch (s) {
    case WorkerPool::State::Working: return good();
    case WorkerPool::State::Done: return claude_color();
    case WorkerPool::State::Failed: return accent();
    default: return dim();
  }
}

void chat(const VoiceSession::Snapshot& snap) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_color(0.055f, 0.063f, 0.082f));
  ImGui::BeginChild("##chat", ImVec2(0.0f, kChatHeight), ImGuiChildFlags_None, 0);

  // Background instances, above the transcript: what each one is doing.
  //
  // **Live ones only, since M11.1.** A finished worker is never erased from the
  // pool — only the explicit `stop` command erases — so before this filter every
  // agent that had ever run stayed on this list for the rest of the session,
  // pushing the transcript down inside a fixed-height region. They are in the
  // agent menu now (`agent_menu_window.h`), which is what the user asked for
  // and where a thing that has stopped happening belongs. Nothing is lost from
  // this region: what a worker said was written into the transcript below when
  // it reported back, and that is still there.
  int live = 0;
  for (const auto& w : snap.workers) {
    if (agent_finished(w.state)) continue;
    std::string line = w.name + " [" + worker_state_name(w.state) + "] " + w.activity;
    if (w.tool_calls) line += "  x" + std::to_string(w.tool_calls);
    ImGui::TextColored(worker_color(w.state), "%s", line.c_str());
    ++live;
  }
  if (live) ImGui::Separator();

  for (const auto& l : snap.lines) {
    if (l.text.empty()) continue;
    ImGui::PushStyleColor(ImGuiCol_Text, l.user ? user_color() : claude_color());
    const std::string line = (l.user ? "You: " : "Claude: ") + l.text;
    ImGui::TextWrapped("%s", line.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
  // The live partial used to be echoed here as well. It is not any more: it
  // now streams into the message field (M1b.4), which is visible whether or
  // not this region is open, and two places showing the same words while they
  // are still being recognised is one too many (user, 16 Sep 2026).

  // Follow the newest line, but only while the user has not scrolled up.
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);

  ImGui::EndChild();
  ImGui::PopStyleColor();
}

constexpr int kMessageLinesMax = 4;

bool blank(const char* s) {
  for (; *s; ++s)
    if (static_cast<unsigned char>(*s) > ' ') return false;
  return true;
}

// `send_refusal()` used to live here, file-static. It is declared in the
// header now because the message field is no longer the only door into
// `VoiceSession::say()` — M2.9's `session.say` bus verb is the same act and
// has to refuse for the same reasons — and a second copy of that switch is
// exactly the drift `build_schedule()` was factored out to prevent.

// The message field, above the transport row (M1b.2).
//
// Multiline, sized to one line and grown by the newlines in it, because a
// single-line InputText cannot hold a '\n' at all and Shift+Enter has to make
// one. Plain Enter never reaches ImGui — WinTextInput withholds it and reports
// it as `submit` — so the widget's own Enter handling only ever sees the
// shifted one, which is exactly the newline case.
//
// Toned to the window rather than to ImGui's frame blue: the user asked for
// "roughly the same colour as the background of the window", so this is the
// window's own colour a shade darker, with a faint border to say it is a
// field. It should read as a recess in the panel, not as a lit control.
// Writing the field from outside — the clear after a send, and every frame of
// a dictation — has to go through the widget, not the buffer behind it: while
// an InputText is active its own copy of the text takes priority and the user
// buffer is simply overwritten from it again next frame. DeleteChars /
// InsertChars are the supported way in, and only reachable from a callback.
//
// `text` must not point into the widget's own buffer: DeleteChars empties that
// before InsertChars reads anything.
//
// ------------------------------------------------------------- the wrapping
//
// **InputTextMultiline does not wrap, and nothing in ImGui makes it.** Measured
// against the vendored v1.91.8 rather than assumed: `InputTextEx` builds a
// multiline field's layout by counting '\n' and nothing else (`text_size =
// ImVec2(inner_size.x, line_count * g.FontSize)`), and the one thing
// `ImGuiInputTextFlags_NoHorizontalScroll` does is pin `state->Scroll.x` to
// zero. So the flag does not make text flow; it removes the sideways view of
// the text that has run off the edge. The field then sized itself from
// `CalcTextSize(..., wrap_w)` — a *wrapped* measurement of text that was
// rendered unwrapped — so a long sentence produced a four-line-tall box with
// one line in it and the rest invisible past the right border. That is the
// user's bug, and the height and the content disagreeing is the same defect
// seen from the other side.
//
// The panel therefore wraps the text itself, and keeps what the user typed out
// of it: `state.message` is the message and never holds a soft break;
// `state.message_view` is what the widget edits and is the message with breaks
// inserted. Everything downstream — the send, the refusal test, dictation —
// still reads `state.message` and is unchanged.
//
// The alternative was drawing the field by hand, i.e. writing a text editor
// with a caret, a selection, IME and a clipboard. This is the smaller thing.
// What it costs is listed where the reconciliation happens below.

// Wraps `text` at `wrap_w`, returning the wrapped string and, in `soft`, the
// byte offset within it of every '\n' this function inserted.
//
// The break points come from `ImFont::CalcWordWrapPositionA` — the same
// function `CalcTextSize` wraps with — so the field is as tall as the text it
// is actually showing by construction, instead of by two calculations that are
// meant to agree and did not. It is also why **Japanese wraps**: with no blanks
// to break at, a Japanese sentence is one "word" that cannot fit on a line, and
// that function cuts such a run wherever it must. A word-boundary wrap written
// here by hand would have put the user's second language on one endless line
// and reproduced the bug for them specifically.
//
// The break goes *after* any blanks that end the line rather than in place of
// them, so unwrapping is the removal of a '\n' and nothing else. A space the
// wrap had swallowed would have to be put back on the way out — and for
// Japanese, where the cut falls between two characters with no space involved,
// putting one back would corrupt the sentence.
std::string wrap_for_field(const char* text, float wrap_w, std::vector<int>& soft) {
  soft.clear();
  const char* const end = text + std::strlen(text);
  std::string out;
  if (wrap_w <= 1.0f) {
    out.assign(text, end);
    return out;
  }
  out.reserve(static_cast<size_t>(end - text) + 16);
  ImFont* font = ImGui::GetFont();
  const float scale = ImGui::GetFontSize() / font->FontSize;
  const char* s = text;
  while (s < end) {
    // One of the user's own lines at a time. CalcWordWrapPositionA walks
    // straight through a '\n' (it resets its width and carries on), so handed
    // the whole text it reports a break position past the newline.
    const char* line_end =
        static_cast<const char*>(std::memchr(s, '\n', static_cast<size_t>(end - s)));
    if (!line_end) line_end = end;
    while (s < line_end) {
      const char* eol = font->CalcWordWrapPositionA(scale, s, line_end, wrap_w);
      if (eol >= line_end) break;  // what is left fits
      while (eol < line_end && (*eol == ' ' || *eol == '\t')) ++eol;
      if (eol >= line_end) break;  // ... and what is left is blanks
      out.append(s, eol);
      soft.push_back(static_cast<int>(out.size()));
      out.push_back('\n');
      s = eol;
    }
    out.append(s, line_end);
    if (line_end < end) out.push_back('\n');  // the user's own, kept as it is
    s = (line_end < end) ? line_end + 1 : line_end;
  }
  return out;
}

int count_lines(const char* s) {
  int n = 1;
  for (; *s; ++s)
    if (*s == '\n') ++n;
  return n;
}

// The width the text is laid out in is not quite the width of the field.
// `InputTextEx` gives the multiline child a vertical scrollbar the moment its
// content is taller than the box and then does `inner_size.x -=
// draw_window->ScrollbarSizes.x` — so a view wrapped at the full width has its
// longest lines clipped by the scrollbar that view itself brought into
// existence. It shows up only past the line cap, which is the only place the
// field ever scrolls, and it is why the first cut of this fix still lost the
// end of a line in exactly one of the four cases.
//
// Wrapping narrower can only ever produce *more* lines, so a view that was over
// the cap is still over it after the second pass: this is a correction, not the
// first step of an oscillation.
std::string wrap_field_view(const char* text, float wrap_w, std::vector<int>& soft) {
  std::string view = wrap_for_field(text, wrap_w, soft);
  if (count_lines(view.c_str()) > kMessageLinesMax)
    view = wrap_for_field(text, wrap_w - ImGui::GetStyle().ScrollbarSize, soft);
  return view;
}

// The view with the soft breaks taken back out: the message as typed.
std::string unwrap_field(const std::string& view, const std::vector<int>& soft) {
  std::string out;
  out.reserve(view.size());
  size_t k = 0;
  for (size_t i = 0; i < view.size(); ++i) {
    if (k < soft.size() && static_cast<size_t>(soft[k]) == i) {
      ++k;
      continue;
    }
    out.push_back(view[i]);
  }
  return out;
}

// The two index maps between the view and the message. `soft` is sorted, so
// both are a walk over it. A caret sitting exactly on a break maps to the start
// of the next line rather than the end of the previous one, which is where
// typing that caused the break leaves it.
int canon_index(const std::vector<int>& soft, int view_index) {
  int c = view_index;
  for (int k : soft) {
    if (k >= view_index) break;
    --c;
  }
  return c;
}

int view_index(const std::vector<int>& soft, int canon) {
  int v = canon;
  for (int k : soft) {
    if (k <= v) ++v;
  }
  return v;
}

// Back up / forward over one whole UTF-8 code point. Deleting a byte would
// leave a half character behind, and half of a Japanese character is not a
// character at all.
size_t step_back(const std::string& s, size_t i) {
  if (i == 0) return 0;
  --i;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
  return i;
}

size_t step_forward(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  ++i;
  while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
  return i;
}

struct FieldEdit {
  AvatarUiState* st = nullptr;
  const char* text = nullptr;  // non-null: replace the field's contents with this
  float wrap_w = 0.0f;
};

// Runs every frame the field is focused (CallbackAlways), and is the only place
// the widget's buffer may be written: while an InputText is active its own copy
// of the text takes priority and the user buffer is simply overwritten from it
// again next frame. It is also the only place the caret may be moved, which a
// re-wrap has to do.
//
// What this costs, honestly:
//  - ImGui's undo stack sees the re-wrap as an edit of its own, so Ctrl+Z in a
//    wrapped field can step through a break. That was already true of every
//    dictation frame, which has replaced the whole buffer since M1b.4.
//  - Backspace at the start of a wrapped line is handled by reading the key
//    (below), because stb's backspace and its forward delete leave the caret in
//    the same place and the edit alone cannot say which was meant.
//  - A re-wrap only happens when the message or the width actually changed, so
//    nothing moves under a selection drag or a caret walk.
int wrap_and_replace(ImGuiInputTextCallbackData* data) {
  auto* edit = static_cast<FieldEdit*>(data->UserData);
  AvatarUiState& st = *edit->st;

  if (edit->text) {
    // Forced from outside: a dictation frame, the clear after a send, or a
    // re-wrap the panel asked for. `text` is already the wrapped view and
    // `st.message_soft` already describes it.
    data->DeleteChars(0, data->BufTextLen);
    if (*edit->text) data->InsertChars(0, edit->text);
    edit->text = nullptr;
    st.message_view_last.assign(data->Buf, static_cast<size_t>(data->BufTextLen));
    return 0;
  }

  const std::string cur(data->Buf, static_cast<size_t>(data->BufTextLen));
  if (cur == st.message_view_last) return 0;  // a caret walk, a selection, nothing

  // The user's edit is one contiguous replacement — a keystroke, a paste, a
  // delete, a selection typed over — so it is recovered by matching the ends.
  // That is what carries the soft-break offsets across it: a break inside what
  // was replaced is gone with it, and everything after it moves by the change
  // in length.
  const std::string& prev = st.message_view_last;
  size_t pre = 0;
  while (pre < prev.size() && pre < cur.size() && prev[pre] == cur[pre]) ++pre;
  size_t suf = 0;
  while (suf < prev.size() - pre && suf < cur.size() - pre &&
         prev[prev.size() - 1 - suf] == cur[cur.size() - 1 - suf])
    ++suf;
  const size_t cut_begin = pre;
  const size_t cut_end = prev.size() - suf;
  const long long shift =
      static_cast<long long>(cur.size()) - static_cast<long long>(prev.size());

  std::vector<int> soft;
  soft.reserve(st.message_soft.size());
  for (int k : st.message_soft) {
    const size_t p = static_cast<size_t>(k);
    if (p < cut_begin)
      soft.push_back(k);
    else if (p >= cut_end)
      soft.push_back(static_cast<int>(static_cast<long long>(p) + shift));
  }

  std::string canonical = unwrap_field(cur, soft);

  // A soft break is not in the message, so deleting one changes nothing and the
  // re-wrap puts it straight back: Backspace at the start of a wrapped line
  // would appear to do nothing at all. The key that was pressed is the only
  // thing that can say what was meant.
  if (cur.size() < prev.size() && canonical == st.message) {
    size_t ci = static_cast<size_t>(canon_index(soft, static_cast<int>(cut_begin)));
    if (ci > canonical.size()) ci = canonical.size();
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && ci > 0) {
      const size_t b = step_back(canonical, ci);
      canonical.erase(b, ci - b);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, true) && ci < canonical.size()) {
      canonical.erase(ci, step_forward(canonical, ci) - ci);
    }
  }

  // `message` is still the cap on what can be typed, enforced here because the
  // view is the bigger buffer and is what ImGui filled.
  if (canonical.size() >= sizeof(st.message))
    canonical.resize(step_back(canonical, sizeof(st.message) - 1));

  const int caret = canon_index(soft, data->CursorPos);
  std::snprintf(st.message, sizeof(st.message), "%s", canonical.c_str());
  const std::string view = wrap_field_view(st.message, edit->wrap_w, st.message_soft);
  if (view != cur) {
    const int at = std::clamp(view_index(st.message_soft, caret), 0, static_cast<int>(view.size()));
    data->DeleteChars(0, data->BufTextLen);
    if (!view.empty()) data->InsertChars(0, view.c_str());
    data->CursorPos = at;
    data->SelectionStart = data->SelectionEnd = at;
  }
  st.message_view_last = view;
  return 0;
}

// The live partial transcript, streamed into the message field (M1b.4).
// True when the field's text changed and the widget has to be told.
//
// The four edge cases, decided deliberately:
//  - Text already typed when the microphone opens is kept and dictated speech
//    is appended after it. Refusing to write into a dirty field would hide
//    what is being heard, which is the one thing this is for.
//  - A recognition that produced nothing leaves nothing behind: the text is
//    rebuilt from the prefix each frame, so an empty partial *is* the prefix.
//  - The user editing mid-utterance wins outright. Speech notices the text is
//    no longer what it last wrote and stops until the microphone next opens.
//  - Leaving Listening without a turn starting (Pause, Silence, nothing
//    intelligible) leaves the partial in the field to be edited and sent by
//    hand. Only an utterance that was actually sent clears it.
bool dictate_into_field(AvatarUiState& s, const VoiceSession::Snapshot& snap) {
  const bool was_listening = s.prev_state == VoiceSession::State::Listening;
  s.prev_state = snap.state;

  if (snap.state == VoiceSession::State::Listening) {
    if (s.dictation == AvatarUiState::Dictation::Idle) {
      s.dictation_prefix = s.message;
      s.dictation_last = s.message;
      s.dictation = AvatarUiState::Dictation::Writing;
    }
    if (s.dictation != AvatarUiState::Dictation::Writing) return false;
    if (s.dictation_last != s.message) {  // the user took it over
      s.dictation = AvatarUiState::Dictation::Yielded;
      return false;
    }
    std::string next = s.dictation_prefix;
    if (!snap.partial.empty()) {
      if (!next.empty() && next.back() != ' ' && next.back() != '\n') next += ' ';
      next += snap.partial;
    }
    if (next == s.message) return false;
    std::snprintf(s.message, sizeof(s.message), "%s", next.c_str());
    s.dictation_last = s.message;
    return true;
  }

  // A hold-to-dictate release (M1b.3). The session finalised the utterance
  // without sending it and published the finished decode, which is not always
  // what the last partial said, so the field is written from it one last time.
  // An empty one rebuilds the field as the prefix alone: a hold that captured
  // nothing leaves what the user had typed exactly as it was, with no trailing
  // space, because the separator is only ever added in front of real words.
  if (s.dictated_seq != snap.dictated_seq) {
    s.dictated_seq = snap.dictated_seq;
    const bool ours = s.dictation == AvatarUiState::Dictation::Writing;
    s.dictation = AvatarUiState::Dictation::Idle;
    if (ours) {  // Yielded means the user was editing; their text wins outright
      std::string next = s.dictation_prefix;
      if (!snap.partial.empty()) {
        if (!next.empty() && next.back() != ' ' && next.back() != '\n') next += ' ';
        next += snap.partial;
      }
      if (next != s.message) {
        std::snprintf(s.message, sizeof(s.message), "%s", next.c_str());
        s.dictation_last = s.message;
        return true;
      }
    }
    return false;
  }

  if (!was_listening) return false;
  // The utterance was sent the moment the session went to Thinking; that is
  // the only exit that clears, and it clears back to what the user had typed.
  const bool sent = snap.state == VoiceSession::State::Thinking;
  const bool clears = sent && s.dictation == AvatarUiState::Dictation::Writing;
  s.dictation = AvatarUiState::Dictation::Idle;
  if (!clears) return false;
  std::snprintf(s.message, sizeof(s.message), "%s", s.dictation_prefix.c_str());
  s.dictation_last = s.message;
  return true;
}

void message_field(AvatarUiState& state, const VoiceSession::Snapshot& snap, bool voice_enabled,
                   bool loading, bool submit, AvatarUiResult& out) {
  // Resolved before the widget is built so the send, the clear and the field
  // shrinking back to one line all land on the same frame. `pending` is the
  // separate storage the callback needs, since it wipes state.message first.
  std::string pending;
  FieldEdit edit;
  edit.st = &state;
  bool rebuild = dictate_into_field(state, snap);
  if (submit && !loading) {
    if (const char* why = send_refusal(snap, voice_enabled, state.message)) {
      // Refused, never queued and never dropped: the text is left in the field
      // exactly as typed and the reason appears below it.
      state.refusal = why;
      state.refusal_left = kRefusalSeconds;
    } else {
      out.send_text = state.message;
      state.message[0] = '\0';
      rebuild = true;
      // A typed send ends any dictation that was feeding the field, so the
      // prefix does not come back on the next microphone close.
      state.dictation = AvatarUiState::Dictation::Idle;
      state.dictation_prefix.clear();
      state.dictation_last.clear();
      state.refusal_left = 0.0f;
      // HANDOFF follow-up 7: "after Enter sends a message the field loses
      // keyboard focus, so typing again needs a re-click." Asserted here
      // rather than repaired, because on this build the loss **could not be
      // reproduced** — driven with real WM_KEYDOWN/WM_CHAR through the
      // subclass, a typed send followed by two more keystrokes put both of
      // them in the field, with the message both one line and wrapped to two
      // (which resizes the window, the likeliest suspect). Enter never reaches
      // ImGui at all (WinTextInput withholds it), so there is no obvious way
      // for the widget to deactivate on a send.
      //
      // This line costs nothing when focus was never lost — the field is empty
      // and already active, so re-asserting it is a no-op the user cannot see
      // — and states the invariant the report is really about: a typed send
      // leaves the field ready for the next message. If the user still sees
      // it, the cause is outside this function and the next investigation has
      // one fewer explanation to rule out.
      state.refocus_field = true;
    }
  }

  // The field is sized from the lines of the view — the text that will
  // actually be drawn — rather than from a wrapped measurement of text that was
  // drawn unwrapped. The two are now the same count by construction, because
  // the same function decides both. Capped at four lines so a long utterance
  // cannot push the transport row down the window; past the cap the field
  // scrolls instead of growing.
  //
  // A dictation is why this cannot count the newlines the *user* typed: speech
  // arrives as one long unpunctuated run with no '\n' anywhere in it, and a
  // one-line field would leave it scrolled out of sight with nobody having
  // touched a key.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float line_h = ImGui::GetTextLineHeight();
  const float wrap_w = ImGui::GetContentRegionAvail().x - 2.0f * style.FramePadding.x;
  edit.wrap_w = wrap_w;
  // The width is part of the wrap, so a change to it rebuilds the view exactly
  // as a change to the text does.
  if (rebuild || wrap_w != state.message_wrap_w) {
    state.message_wrap_w = wrap_w;
    pending = wrap_field_view(state.message, wrap_w, state.message_soft);
    // Written both ways on purpose. ImGui only runs the callback while the
    // field is focused; when it is not, the widget reads this buffer directly
    // and the callback would never fire at all.
    std::snprintf(state.message_view, sizeof(state.message_view), "%s", pending.c_str());
    state.message_view_last = pending;
    edit.text = pending.c_str();
  }
  const int lines = std::clamp(count_lines(state.message_view), 1, kMessageLinesMax);
  const float h = lines * line_h + 2.0f * style.FramePadding.y;

  ImGui::PushStyleColor(ImGuiCol_FrameBg, ui_color(0.071f, 0.078f, 0.098f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui_color(0.086f, 0.094f, 0.118f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui_color(0.098f, 0.106f, 0.133f));
  ImGui::PushStyleColor(ImGuiCol_Border, ui_color(0.16f, 0.17f, 0.21f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  ImGui::BeginDisabled(loading);
  // Claimed on the frame the send happens, so the clear and the focus land
  // together and there is never a frame in which the field is empty and dead.
  if (state.refocus_field) {
    state.refocus_field = false;
    ImGui::SetKeyboardFocusHere();
  }
  // The widget owns the *view*, never the message. NoHorizontalScroll stays:
  // with the text wrapped there is nothing to the right to scroll to, and the
  // flag is what keeps a caret at the end of a long line from sliding the whole
  // field sideways.
  ImGui::InputTextMultiline("##message", state.message_view, sizeof(state.message_view),
                            ImVec2(-FLT_MIN, h),
                            ImGuiInputTextFlags_NoHorizontalScroll |
                                ImGuiInputTextFlags_CallbackAlways,
                            wrap_and_replace, &edit);
  ImGui::EndDisabled();
  // InputTextWithHint is single-line only, so the placeholder is drawn by hand
  // over the empty field. Not while it is focused: a caret sitting on top of
  // greyed-out words reads as text that will not delete.
  if (state.message[0] == '\0' && !ImGui::IsItemActive()) {
    const ImVec2 p = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(p.x + style.FramePadding.x + 1.0f, p.y + style.FramePadding.y),
        ImGui::GetColorU32(dim()), "Message");
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);

  // Always one row tall, whatever it says. A row that came and went would
  // change the panel's height, and the panel's height is the window's.
  state.refusal_left = std::max(0.0f, state.refusal_left - ImGui::GetIO().DeltaTime);
  if (state.refusal_left > 0.0f)
    ImGui::TextColored(warn(), "%s", state.refusal.c_str());
  else
    ImGui::TextColored(dim(), "Enter sends  -  Shift+Enter starts a line");
}

// ------------------------------------------------------------ the transport
//
// Four fixed slots (reset was the fourth, user 19 Sep 2026). Stop hides itself
// when there is nothing to stop (user,
// 16 Sep 2026) and its slot stays reserved when it does, because the row is
// laid out from an origin and a slot index rather than by flowing one button
// after another. That is not tidiness: the microphone button is the target of
// a press-and-hold gesture, and a control that moves under a pointer mid-press
// turns a release into an abandoned one, which is byte-for-byte the
// hold-to-dictate path. The Talk-click bug of 16 Sep 2026 was exactly that
// failure arriving by a different route (commit f713297), and Stop appears and
// disappears *precisely* when the session starts and stops doing something —
// i.e. at the moments a gesture is most likely to be in flight. So the slot is
// empty, never closed up.
//
// Reset takes slot 3, at the end, and never vacates it: unlike Stop, its
// availability is a steady condition of the session rather than a flicker that
// tracks the AI starting and stopping work, so an empty slot on the end of the
// row would just read as a button that had gone missing. It is drawn dim and
// inert instead, which also gives it somewhere to say *why* it cannot be
// pressed. Either way the invariant is the same one and it is satisfied more
// strictly here than at slot 2: the slot's occupant never changes size and the
// slots to its left were never a function of it.

struct TransportSkin {
  ImVec4 bg, hovered, active, ink, mark;
};

TransportSkin neutral_skin(ImVec4 ink) {
  return {ImGui::GetStyleColorVec4(ImGuiCol_Button),
          ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered),
          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive), ink, ink};
}

// Submits one slot as an InvisibleButton at an absolute position and paints it.
// Nothing is returned: the caller reads ImGui::IsItem* afterwards, which still
// refers to this button because a draw-list call is not an item. The microphone
// needs the press and the release separately, so a click-returning wrapper
// would only have to be unwrapped again.
bool transport_slot(const char* id, ImVec2 pos, const char* const* rows,
                    const TransportSkin& skin) {
  ImGui::SetCursorScreenPos(pos);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(kTransportButton, kTransportButton));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec4 bg = ImGui::IsItemActive()    ? skin.active
                    : ImGui::IsItemHovered() ? skin.hovered
                                             : skin.bg;
  dl->AddRectFilled(pos, ImVec2(pos.x + kTransportButton, pos.y + kTransportButton),
                    ImGui::GetColorU32(bg), ImGui::GetStyle().FrameRounding);
  const float inset = (kTransportButton - kIconPx) * 0.5f;
  draw_icon(dl, rows, ImVec2(pos.x + inset, pos.y + inset), ImGui::GetColorU32(skin.ink),
            ImGui::GetColorU32(skin.mark));
  return clicked;
}

// The seven faces of the microphone button. Every one of them is something the
// session can actually report — there is no "about to listen" or "hearing
// noise" here, because nothing in VoiceSession knows either.
enum class MicFace {
  Unavailable,   // engines still loading, or --no-voice: nothing to press
  Idle,          // shut, and the next press decides what it means
  Dictating,     // a Talk press is down; the words are going into the box
  Listening,     // latched on and hearing the room
  Latched,       // latched on but deliberately shut while Claude replies
  Dozed,         // M1f.3: shut because the latch timed out on a silent room
  // M12.2. **Open, matching the wake phrase, and sending nothing anywhere.**
  //
  // A seventh face and not a reuse of `Idle`, and the reason is the same shape
  // as M1f.3's but with a great deal more riding on it: with a wake phrase set,
  // `Idle` and this are the same session state to everything except the
  // capture device, and the capture device is the entire question. Drawing the
  // ordinary shut capsule here would mean the app holding a microphone open
  // while the one control that reports on the microphone said it was closed.
  // The user picked always-on local matching over the safer option; they did
  // not pick being unable to tell.
  WakeListening,
};
constexpr int kMicFaceCount = 7;

// `dozed` is the panel's latched reading of `Snapshot::listen_timeout_seq` —
// see AvatarUiState::mic_dozed for why it is a level here and an edge there.
// It is tested last of the closed cases and first among them, which is the
// whole of its precedence: it only ever competes with `Idle`, because it only
// exists while the microphone is shut and nothing has happened since.
MicFace mic_face(const VoiceSession::Snapshot& snap, bool voice_enabled, bool loading,
                 bool mic_on, bool mic_hold, bool dozed, bool wake_listening) {
  // The same escape hatch `--clip` and `--sprite` give the avatar's art, and
  // for the same reason: three of these six faces are only reachable by
  // talking into a microphone or by walking away from one, so without this
  // there is no way to *look* at them — and "verified by looking at a capture"
  // is the standard this panel is held to. `AII_MIC_FACE=0..6` pins one;
  // `cycle` walks all seven, two seconds each.
  if (const char* pin = std::getenv("AII_MIC_FACE")) {
    const int n = std::strcmp(pin, "cycle") == 0
                      ? static_cast<int>(ImGui::GetTime() * 0.5) % kMicFaceCount
                      : std::atoi(pin);
    return static_cast<MicFace>(std::clamp(n, 0, kMicFaceCount - 1));
  }
  if (loading || !voice_enabled) return MicFace::Unavailable;
  // A hold is not the latch and never sets it (VoiceSession::talk_pressed),
  // so this order is not a preference between two true things.
  if (mic_hold) return MicFace::Dictating;
  if (!mic_on) {
    // M12.2, and it outranks `Dozed` — which is the whole of its precedence.
    // The two can be true at once and routinely are: the latch times out on a
    // silent room, and the passive matching that was waiting underneath it
    // comes straight back up. `Dozed` would then draw a dimmed, shut capsule
    // over a microphone that is open, which is the one thing this face exists
    // to make unreachable. The news `Dozed` carries is real but it is second:
    // the status line still says the latch closed by itself, and the tooltip
    // here says what is listening now.
    if (wake_listening) return MicFace::WakeListening;
    return dozed ? MicFace::Dozed : MicFace::Idle;
  }
  return snap.state == VoiceSession::State::Listening ? MicFace::Listening : MicFace::Latched;
}

const char* const* mic_icon(MicFace face) {
  switch (face) {
    case MicFace::Dictating: return kIconMicHold;
    case MicFace::Listening: return kIconMicLive;
    case MicFace::Latched: return kIconMicShut;
    case MicFace::Dozed: return kIconMicDozed;
    case MicFace::WakeListening: return kIconMicWake;
    default: return kIconMic;
  }
}

// Colour *and* a change of icon, never colour alone (user, 16 Sep 2026): each
// row below pairs a plate with a different glyph, so the button still reads on
// a monitor, in a capture and to a colour-blind eye. The open-microphone red is
// the one this panel already used; what has gone is the word "Mute" on it,
// which now belongs to the button next door.
TransportSkin mic_skin(MicFace face) {
  switch (face) {
    case MicFace::Listening:
      return {red(), red_hot(), red_deep(), ui_color(1.00f, 0.96f, 0.95f), warn()};
    case MicFace::Latched:
      // Darker than Listening on purpose: the latch is on but the room is not
      // being heard, and the slash across the icon is what says which.
      return {red_deep(), red(), red_deep(), ui_color(0.98f, 0.86f, 0.85f), warn()};
    case MicFace::Dictating:
      return {ui_color(0.62f, 0.42f, 0.10f), ui_color(0.74f, 0.51f, 0.14f),
              ui_color(0.50f, 0.33f, 0.07f), ui_color(1.00f, 0.97f, 0.90f), warn()};
    case MicFace::Dozed: {
      // The plate is the ordinary one and the capsule is dimmed, because the
      // microphone really is off and a lit button would be a lie about that.
      // The `z` is the only thing carrying the news, so it gets the same amber
      // mark ink the latched face's slash uses — one mark colour across the
      // button, so a second colour never has to be learned.
      TransportSkin skin = neutral_skin(dim());
      skin.mark = warn();
      return skin;
    }
    case MicFace::WakeListening: {
      // **Lit, and deliberately not red.** Lit because the microphone is open
      // and a neutral plate here would read as off; not red because red on
      // this row has meant one thing since M1b — what you say is going to
      // Claude — and borrowing it for a state that sends nothing would make
      // the loudest signal on the button the least reliable one. A cool,
      // clearly-on teal is the honest middle: something is happening, and it
      // is not that.
      //
      // The ripples take the same amber `warn()` mark ink the slash and the
      // `z` use, so there is still exactly one mark colour on this button.
      return {ui_color(0.10f, 0.40f, 0.44f), ui_color(0.14f, 0.52f, 0.57f),
              ui_color(0.07f, 0.31f, 0.34f), ui_color(0.92f, 0.99f, 1.00f), warn()};
    }
    case MicFace::Unavailable:
      return neutral_skin(dim());
    default:
      return neutral_skin(fg());
  }
}

// `wake_phrase` is only read for the one face that names it. It is a
// std::string return rather than a literal for that same reason: the wake
// tooltip has the user's own word in it, and quoting it back is most of what
// makes the tooltip answer "what is it listening *for*".
std::string mic_tooltip(MicFace face, const std::string& wake_phrase) {
  if (face == MicFace::WakeListening) {
    // Three sentences, in the order the questions arrive: is the microphone
    // on, what is it doing with what it hears, and how do I stop it. The
    // middle one is the promise the whole design rests on and it is stated as
    // a fact about this machine rather than as reassurance.
    return "The microphone is ON, listening for \"" + wake_phrase +
           "\".\nWhat it hears is matched on this machine and sent nowhere.\n"
           "Say it, or click, to start talking to Claude.\n"
           "(Settings > Listening clears the phrase to switch this off.)";
  }
  switch (face) {
    case MicFace::Unavailable: return "Microphone - not ready yet";
    // The two gestures, still spelled out: they are the whole of what this
    // button does and the label that used to hint at it is gone.
    case MicFace::Idle: return "Click to talk  -  hold to dictate into the box";
    case MicFace::Dictating: return "Recording - release to put it in the box";
    case MicFace::Listening: return "Listening - click to stop  (hold to dictate)";
    // The one face whose tooltip has to explain itself rather than name a
    // gesture: nobody pressed anything to get here, so "what did I do?" is the
    // question, and the second line is the answer to "and will it keep doing
    // that?" — the setting, quoted, since this is the only moment it is worth
    // reading about.
    case MicFace::Dozed:
      return "Stopped listening by itself - no voice was heard.\n"
             "Click to talk.  (Settings > Timing sets how long it waits.)";
    default: return "Conversation mode - the mic reopens when Claude finishes.\nClick to stop.";
  }
}

// Is there anything for Stop to stop? Everything stop() actually does, asked as
// a question: a reply to cancel, an utterance in progress, the latch it drops,
// or a worker it would pause. A button that can do nothing is not drawn.
bool anything_to_stop(const VoiceSession::Snapshot& snap, bool mic_on, bool mic_hold) {
  switch (snap.state) {
    case VoiceSession::State::Listening:
    case VoiceSession::State::Thinking:
    case VoiceSession::State::Speaking:
      return true;
    default:
      break;
  }
  if (mic_on || mic_hold) return true;
  for (const auto& w : snap.workers) {
    if (w.state == WorkerPool::State::Starting || w.state == WorkerPool::State::Working)
      return true;
  }
  return false;
}

// The four faces of the reset button. Same rule as the microphone's six: each
// one is something the session can actually report, and no face means "about
// to" anything.
enum class ResetFace {
  Inert,    // nothing has been said this session, or there is no session
  Ready,    // a conversation exists and one press will arm the confirm
  Armed,    // armed: the next press throws it away
  Working,  // the child is being replaced right now
};

ResetFace reset_face(const VoiceSession::Snapshot& snap, bool voice_enabled, bool loading,
                     bool armed) {
  // The same escape hatch AII_MIC_FACE gives the microphone, and for the same
  // reason: `Working` lasts about a second and `Armed` needs a live
  // conversation to reach, so without this there is no way to *look* at either,
  // and "verified by looking at a capture" is the standard this panel is held
  // to. `AII_RESET_FACE=0..3` pins one; `cycle` walks all four.
  if (const char* pin = std::getenv("AII_RESET_FACE")) {
    const int n = std::strcmp(pin, "cycle") == 0 ? static_cast<int>(ImGui::GetTime() * 0.5) % 4
                                                 : std::atoi(pin);
    return static_cast<ResetFace>(std::clamp(n, 0, 3));
  }
  if (snap.resetting) return ResetFace::Working;
  if (loading || !voice_enabled || !snap.resettable) return ResetFace::Inert;
  return armed ? ResetFace::Armed : ResetFace::Ready;
}

TransportSkin reset_skin(ResetFace face) {
  switch (face) {
    case ResetFace::Armed:
      // The one plate in this row that means "this is about to be
      // irreversible". It is the mute button's red family rather than the
      // microphone's, because the microphone's red means "live" and this means
      // the opposite of live.
      return {ui_color(0.55f, 0.13f, 0.13f), ui_color(0.70f, 0.19f, 0.18f),
              ui_color(0.42f, 0.09f, 0.09f), ui_color(1.00f, 0.93f, 0.92f), warn()};
    case ResetFace::Working: {
      // Dim like Inert, because the button really is unpressable, but in the
      // accent rather than the dim ink: something is happening. That is a
      // colour-only difference from Inert and it is the one place in this row
      // that is allowed one, for the same reason MicFace::Unavailable is —
      // the two are never in play at the same moment, they last for different
      // orders of magnitude of time, and the status line names this one in
      // words while it is on screen.
      TransportSkin skin = neutral_skin(accent());
      skin.hovered = skin.active = skin.bg;
      return skin;
    }
    case ResetFace::Inert: {
      // Flat under the pointer. A slot that lights up and then swallows the
      // click is worse than one that never lit up: it says the button works.
      TransportSkin skin = neutral_skin(dim());
      skin.hovered = skin.active = skin.bg;
      return skin;
    }
    default:
      return neutral_skin(fg());
  }
}

const char* reset_tooltip(ResetFace face) {
  switch (face) {
    // Says why, rather than saying nothing: this is the steady state of a
    // fresh session, so it is the face the button wears most often on a quiet
    // desktop, and "greyed out with no explanation" is the worst answer to
    // "why can I not press this".
    case ResetFace::Inert: return "Reset - nothing has been said yet";
    case ResetFace::Working: return "Starting a new session...";
    // Spells out both halves of what is about to happen, because they are
    // different things and only one of them is visible: the chat clearing is
    // the part the user will see, and Claude forgetting is the part they are
    // actually asking for. And it says it cannot be undone, which is true —
    // the child that held the conversation is gone.
    case ResetFace::Armed:
      return "Press again to clear the conversation.\n"
             "Claude forgets everything said so far and the chat is emptied.\n"
             "This cannot be undone.  (Or wait a moment to cancel.)";
    default:
      return "Reset - start a fresh conversation\n"
             "(asks once more before it does)";
  }
}

// --- the close button's three faces (user, 19 Sep 2026) ---------------------
//
// There is no Inert. Closing is the one thing this app must never refuse: Esc
// and Q already quit from any state, and a quit button that greys itself out
// would be a worse promise than no button at all. `Waiting` is not a refusal
// either — it is a quit that has been accepted and is queued behind the turn
// in flight.
enum class CloseFace { Ready, Armed, Waiting };

TransportSkin close_skin(CloseFace face) {
  switch (face) {
    case CloseFace::Armed:
      // Deliberately the same red plate Reset's armed face wears. Two buttons
      // that are one press from doing something irreversible should look the
      // same while they are armed; teaching the user a second danger colour
      // would only dilute the first.
      return {ui_color(0.55f, 0.13f, 0.13f), ui_color(0.70f, 0.19f, 0.18f),
              ui_color(0.42f, 0.09f, 0.09f), ui_color(1.00f, 0.93f, 0.92f), warn()};
    case CloseFace::Waiting: {
      // Still armed — the glyph is the armed one — but dimmed to the accent,
      // because the user's part is over and the app's has started. Same device
      // ResetFace::Working uses, and for the same reason: something is
      // happening and the button is no longer the thing to press.
      TransportSkin skin = neutral_skin(accent());
      skin.hovered = skin.active = skin.bg;
      return skin;
    }
    default:
      return neutral_skin(fg());
  }
}

// The armed tooltip names the workers by name rather than counting them,
// because "2 workers" is a number and "docs, refactor" is the thing the user
// actually has to decide about. This is the case most likely to cost real
// work: a worker is a separate `claude` process that has been running for
// minutes, and closing cancels it wherever it had got to.
std::string close_tooltip(CloseFace face, const VoiceSession::Snapshot& snap) {
  if (face == CloseFace::Waiting)
    return "Closing as soon as this turn finishes.\n"
           "(Press again to stay open.)";
  if (face != CloseFace::Armed) return "Close - quit AIInterface\n(asks once more before it does)";

  std::string live;
  int n = 0;
  for (const WorkerPool::Snapshot& w : snap.workers) {
    if (w.state != WorkerPool::State::Working && w.state != WorkerPool::State::Starting) continue;
    if (n++) live += ", ";
    live += w.name;
  }
  std::string t = "Press again to close AIInterface.\n";
  if (n > 0)
    t += (n == 1 ? "The worker " : "The workers ") + live +
         (n == 1 ? " is still running and will be stopped;\nwhatever it has not reported yet is "
                   "lost.\n"
                 : " are still running and will be stopped;\nwhatever they have not reported yet "
                   "is lost.\n");
  t += "(Or wait a moment to cancel.)";
  return t;
}

// --- the close button (user, 19 Sep 2026) -----------------------------------
//
// **Why on the status row.** The window is borderless on purpose, so there is
// no OS close button to lean on, and the only ways out were Esc/Q and the
// window's own close message — neither of which is visible. It spent one
// commit as slot 4 of the transport row; the user asked for it beside the chat
// arrow instead, and that is the better place on its own merits. The status
// row is the top of the panel and this cluster is its right-hand end, which is
// where every window on this desktop keeps its close button.
//
// **Outboard of the arrow, not inboard.** Hard right is where the muscle
// memory goes, and it is the only one of the three that has a convention at
// all. The argument the other way is that the cluster is right-aligned, so the
// outermost slot is what a reach for the arrow overshoots onto — but an
// overshoot here only *arms* the button, which turns red, says what it is
// about to do, and forgets about it four seconds later. That is exactly the
// accident the two-press gesture exists to absorb. Inboard would have cost
// more: it would have driven the disc and the arrow apart, and put the one
// destructive control in this window between two harmless toggles, where a
// reach for either could land on it.
//
// **Why two presses.** Closing is at least as destructive as Reset and has
// even less of an undo: Reset loses a conversation, this loses the
// conversation *and* every worker mid-task. So it borrows Reset's gesture
// wholesale — arm, change glyph and plate, fire on the second press, with the
// same kResetArmMinSeconds dwell that stops a double-click walking through the
// confirm, and the same kResetArmSeconds timeout. (That gesture is the house
// style rather than a proven design: it is still on the user's own test list.
// If it turns out to be wrong, it is wrong in one place and both buttons are
// fixed together.)
//
// **Why it never greys out.** See CloseFace: a quit button that refuses is a
// worse promise than no button, and Esc/Q would contradict it anyway. Note
// that the caller draws it outside BeginDisabled for the same reason.
//
// **Why it paints itself instead of calling transport_slot.** These are
// ImGui frame-height buttons, not the transport row's 30 px plates, and
// kIconScale is 2 — a 26 px icon in a 21 px button. The scale is computed from
// the button here and is still a whole number, which is the whole of the rule:
// a 13-cell grid at 1.5x alternates 1 and 2 px cells and reads as mush.
void close_button(AvatarUiState& state, const VoiceSession::Snapshot& snap, float size,
                  AvatarUiResult& out) {
  const double now = ImGui::GetTime();
  if (state.close_armed_at > 0.0) {
    // Deliberately a shorter list than Reset's. Reset disarms when the session
    // runs out of things to reset; nothing can make a quit inapplicable, so
    // only a real gesture elsewhere and the timeout drop it. The transport
    // row's gestures arrive as a counter because it draws after this row —
    // see the tail of transport().
    const bool moved_on = state.transport_gesture_seq != state.close_gesture_seq;
    if (moved_on || now - state.close_armed_at > kResetArmSeconds) state.close_armed_at = 0.0;
  }
  state.close_gesture_seq = state.transport_gesture_seq;

  const CloseFace face = state.close_pending ? CloseFace::Waiting
                         : state.close_armed_at > 0.0 ? CloseFace::Armed
                                                      : CloseFace::Ready;
  const TransportSkin skin = close_skin(face);

  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##close", ImVec2(size, size));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec4 bg = ImGui::IsItemActive()    ? skin.active
                    : ImGui::IsItemHovered() ? skin.hovered
                                             : skin.bg;
  dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ImGui::GetColorU32(bg),
                    ImGui::GetStyle().FrameRounding);
  // The largest whole-number scale that leaves a pixel of air on each side.
  // At this font (15 px, so a 21 px frame) that is 1x: a 13 px glyph with four
  // pixels round it, which is the same air-to-ink the arrow beside it has.
  const float scale = std::max(1.0f, std::floor((size - 2.0f) / kIconCells));
  const float inset = std::floor((size - kIconCells * scale) * 0.5f);
  draw_icon(dl, face == CloseFace::Ready ? kIconClose : kIconCloseArmed,
            ImVec2(p.x + inset, p.y + inset), ImGui::GetColorU32(skin.ink),
            ImGui::GetColorU32(skin.mark), scale);

  if (clicked) {
    if (face == CloseFace::Waiting) {
      // Second thoughts, and the only way back: a queued quit the user can no
      // longer call off would be the same trap as one they never asked for.
      state.close_pending = false;
      state.refusal = "Staying open.";
      state.refusal_left = kRefusalSeconds;
    } else if (face == CloseFace::Ready) {
      state.close_armed_at = now;
    } else if (now - state.close_armed_at >= kResetArmMinSeconds) {
      state.close_armed_at = 0.0;
      // Confirmed. Whether it happens on this frame is quitting_ok()'s
      // decision, not this button's — the app must not tear a turn down
      // half-way, and the reply already half paid for is the user's.
      state.close_pending = true;
    }
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", close_tooltip(face, snap).c_str());

  // The pending quit, resolved here rather than at the click, so it is retried
  // every frame until the session allows it. On the overwhelmingly common path
  // — nothing in flight — `quit_ok` is already true on the very frame the
  // second press lands and this fires immediately, so the wait costs a user
  // who is simply closing an idle app exactly nothing.
  if (state.close_pending && snap.quit_ok) {
    state.close_pending = false;
    out.close = true;
  } else if (state.close_pending) {
    // Said in words as well as in the button's face. This is the reserved row
    // that already answers "why did my Enter do nothing"; "why is the app not
    // closing" is the same question. It is set before that row is drawn now
    // that this button is at the top of the panel, so the words appear on the
    // same frame as the face rather than one behind it.
    state.refusal = "Closing when this turn finishes...";
    state.refusal_left = kRefusalSeconds;
  }
}

void transport(AvatarUiState& state, const VoiceSession::Snapshot& snap, bool voice_enabled,
               bool loading, bool mic_on, bool mic_hold, AvatarUiResult& out) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float gap = style.ItemSpacing.x;
  // One origin, four slot indices. Read off the layout cursor once, before
  // anything is submitted, so nothing that happens inside the row can move a
  // later slot: slot 2 being absent cannot shift slot 0 because slot 0's
  // position was never a function of slot 2.
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  auto slot_pos = [&](int i) {
    return ImVec2(origin.x + i * (kTransportButton + gap), origin.y);
  };

  // The two stretches in which the microphone has nothing to attach itself to:
  // the engines are not up, or the child that would answer is being replaced.
  // Mute is deliberately not included — it stays live through both, on the
  // avatar-visibility disc's precedent, because it is a stored preference
  // about this window rather than a control that routes into a session.
  const bool busy = loading || snap.resetting;
  ImGui::BeginDisabled(busy);

  // The fixed-slot claim, published rather than asserted. Behind the same
  // environment variable the gesture harness already uses, and printed only
  // when something in it changes, so a run produces one line per distinct
  // (microphone rect, Stop present) pair — which is exactly the pair that has
  // to prove independent. Screen coordinates, so it is comparable with the
  // window rect the harness measures from outside.
  if (std::getenv("AII_TALK_DEBUG")) {
    static float last[4] = {-1, -1, -1, -1};
    static int last_stop = -1;
    const bool stop_here = !loading && voice_enabled && anything_to_stop(snap, mic_on, mic_hold);
    const ImVec2 m = slot_pos(0);
    if (m.x != last[0] || m.y != last[1] || last_stop != static_cast<int>(stop_here)) {
      last[0] = m.x;
      last[1] = m.y;
      last[2] = m.x + kTransportButton;
      last[3] = m.y + kTransportButton;
      last_stop = static_cast<int>(stop_here);
      std::printf("  [row] mic_rect=(%.1f,%.1f)-(%.1f,%.1f) stop=%d\n", last[0], last[1], last[2],
                  last[3], last_stop);
      std::fflush(stdout);
    }
  }

  // --- slot 0: the microphone (was "Talk"/"Mute") ---
  //
  // Two gestures on one button (M1b.3), unchanged. A **click** is the latch:
  // one click opens the mic, the next shuts it, and in between the utterances
  // send themselves on a pause. A **press and hold** records only while it is
  // down and puts the transcript in the message field unsent.
  //
  // The press and the release are reported separately rather than taking a
  // single click, because the session opens the microphone on the press —
  // before the gesture's meaning is known — so that neither reading of it loses
  // the words spoken while it was still undecided.
  const MicFace face = mic_face(snap, voice_enabled, busy, mic_on, mic_hold, state.mic_dozed,
                                snap.wake_listening);
  transport_slot("##mic", slot_pos(0), mic_icon(face), mic_skin(face));
  // One line per edge of the gesture, with everything needed to decide *why* a
  // release was judged off the button: the item's rect, where ImGui thinks the
  // pointer is, where Windows thinks it is, and the display the two are
  // measured against. Client coordinates throughout — ImGui's screen space is
  // this window's client space, since there is one viewport at (0,0) — so the
  // harness's window rect is the only thing needed to relate them to a desktop
  // position. Extends the f713297 instrumentation rather than replacing it.
  const auto gesture_line = [&](const char* edge) {
    if (!std::getenv("AII_TALK_DEBUG")) return;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 r0 = ImGui::GetItemRectMin();
    const ImVec2 r1 = ImGui::GetItemRectMax();
    POINT raw{};
    GetCursorPos(&raw);
    // `hovered` is one answer built from three separate facts, and when it
    // disagrees with the position above, which of the three is lying is the
    // whole question. So all three are printed:
    //
    //   rect   - the pointer against this item's rect, unclipped. Position
    //            alone. A 1 here says ImGui's mouse position and the item's
    //            geometry agree, which is what 4837a94 fixed.
    //   clip   - the same test through the window's clip rect, which is set
    //            during Begin() *this* frame.
    //   win    - whether ImGui holds this window to be the hovered one. This is
    //            computed in NewFrame, before any window is submitted, so it
    //            is tested against each window's rect as of the *previous*
    //            frame. On the frame the widget grows, that rect is the old,
    //            shorter one, and a pointer at the button's new client y is
    //            outside it.
    //
    // rect=1 clip=1 win=0 is therefore the signature of a hover verdict and a
    // pointer position evaluated one frame apart.
    const bool rect_hit = ImGui::IsMouseHoveringRect(r0, r1, false);
    const bool clip_hit = ImGui::IsMouseHoveringRect(r0, r1, true);
    const bool win_hit = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    std::printf(
        "  [gesture] %s item=(%.1f,%.1f)-(%.1f,%.1f) imgui_mouse=(%.1f,%.1f) "
        "screen_mouse=(%d,%d) display=%.0fx%.0f hovered=%d rect=%d clip=%d win=%d "
        "active=%d down=%d t=%.3f\n",
        edge, r0.x, r0.y, r1.x, r1.y, io.MousePos.x, io.MousePos.y, static_cast<int>(raw.x),
        static_cast<int>(raw.y), io.DisplaySize.x, io.DisplaySize.y,
        static_cast<int>(ImGui::IsItemHovered()), static_cast<int>(rect_hit),
        static_cast<int>(clip_hit), static_cast<int>(win_hit),
        static_cast<int>(ImGui::IsItemActive()), static_cast<int>(io.MouseDown[0]),
        ImGui::GetTime());
    std::fflush(stdout);
  };
  if (ImGui::IsItemActivated()) {
    state.talk_pressed_at = ImGui::GetTime();
    out.talk_pressed = true;
    gesture_line("press  ");
  }
  if (ImGui::IsItemDeactivated()) {
    out.talk_released = true;
    out.talk_held = ImGui::GetTime() - state.talk_pressed_at >= kTalkHoldSeconds;
    // Judged against where the button is *this* frame, which is only a fair
    // test because main.cpp changes the window's height and the avatar band
    // together, at the top of a frame, before any of that frame's input is
    // read. When it did not, the panel could be laid out 260 px from where it
    // was on screen and this test called a release that never left the button
    // an abandoned press — the Talk-click bug of 16 Sep 2026. The fixed slots
    // above are the same invariant applied within the row.
    out.talk_over_button = ImGui::IsItemHovered();
    gesture_line("release");
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mic_tooltip(face, snap.wake_phrase).c_str());

  // --- slot 1: mute (was "Silence") ---
  //
  // A level, not a one-shot. The old Silence emptied the speech queue, which
  // stopped the sentence that was playing and nothing else — the turn thread
  // went on feeding the queue and the reply carried on a beat later. This is
  // remembered instead, across runs, and the session suppresses at the source
  // while it is on. The reply still arrives as text: muting Claude's voice is
  // not muting Claude.
  //
  // Live while loading, like the avatar-mode disc on the status row and unlike
  // the two buttons either side of it: it is a stored preference about this
  // window rather than a control that routes into engines that are not up yet,
  // and a user who wants the app to come up silent wants to say so during the
  // wait, not after the first reply has started talking.
  ImGui::EndDisabled();
  const TransportSkin mute_skin =
      state.muted ? TransportSkin{ui_color(0.32f, 0.15f, 0.16f), ui_color(0.42f, 0.20f, 0.21f),
                                  ui_color(0.25f, 0.11f, 0.12f), ui_color(0.88f, 0.74f, 0.73f),
                                  bad()}
                  : neutral_skin(fg());
  // Kept rather than consumed, because slot 3 disarms its confirm on any other
  // transport gesture and this is one. `state.muted` alone cannot say it — it
  // is a level that main.cpp also writes.
  const bool mute_clicked = transport_slot(
      "##mute", slot_pos(1), state.muted ? kIconSpeakerMuted : kIconSpeaker, mute_skin);
  if (mute_clicked) state.muted = !state.muted;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", state.muted ? "Claude's voice is muted - click to unmute\n"
                                          "(replies still arrive as text)"
                                        : "Mute Claude's voice\n(replies still arrive as text)");

  // --- slot 2: stop (was "Pause") ---
  //
  // Same behaviour as the old Pause, which was only ever misnamed: nothing here
  // can resume a paused worker turn. Hidden when it would do nothing, and the
  // slot it leaves behind is padding, not a gap that closes.
  if (!loading && voice_enabled && anything_to_stop(snap, mic_on, mic_hold)) {
    if (transport_slot("##stop", slot_pos(2), kIconStop, neutral_skin(accent()))) out.stop = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Stop - cancel the reply, close the mic, pause every worker");
  } else {
    // The slot still exists; it is simply empty. Submitted rather than skipped
    // so the row's content extent does not depend on whether Stop is there.
    ImGui::SetCursorScreenPos(slot_pos(2));
    ImGui::Dummy(ImVec2(kTransportButton, kTransportButton));
  }

  // --- slot 3: reset (user, 19 Sep 2026) ---
  //
  // Throws the conversation away: the `claude` child that holds it is ended
  // and a new one started, so the next thing said starts from nothing. The
  // transcript goes with it — see VoiceSession::run_reset() for why keeping it
  // would be the worse of the two defensible answers.
  //
  // **Two presses, in this one slot.** There is no undo to offer (the context
  // window lived in a process that no longer exists) and no dialog to draw
  // (nothing may be composited outside this window, so an ImGui popup would be
  // silently clipped — the same constraint that made the settings surface a
  // region). So the confirm is the button itself changing, which costs no
  // geometry and cannot be clipped. See kResetArmSeconds.
  const double now = ImGui::GetTime();
  // Anything that shows the user has moved on disarms it. The three transport
  // gestures are checked here rather than at each button because this is where
  // the armed flag lives, and because "the user did something else" is one
  // rule, not three.
  if (state.reset_armed_at > 0.0) {
    const bool moved_on = out.talk_pressed || out.stop || mute_clicked || snap.resetting ||
                          !snap.resettable || loading || !voice_enabled;
    if (moved_on || now - state.reset_armed_at > kResetArmSeconds) state.reset_armed_at = 0.0;
  }
  const ResetFace reset = reset_face(snap, voice_enabled, loading, state.reset_armed_at > 0.0);
  const bool reset_live = reset == ResetFace::Ready || reset == ResetFace::Armed;
  // Drawn in every state, unlike Stop: the slot is never vacated, so there is
  // nothing here that could move and nothing that could look missing. Inert
  // means the click is dropped, not that the button is absent — and the
  // tooltip is still offered, because "why can I not press this" deserves an
  // answer.
  if (transport_slot("##reset", slot_pos(3), reset == ResetFace::Armed ? kIconResetArmed
                                                                      : kIconReset,
                     reset_skin(reset)) &&
      reset_live) {
    if (reset == ResetFace::Ready) {
      state.reset_armed_at = now;
    } else if (now - state.reset_armed_at >= kResetArmMinSeconds) {
      // The second press, and the only frame `out.reset` is ever true. The
      // minimum dwell is what a double-click runs into: without it the second
      // half of an accidental double-click would arm and fire in the same
      // gesture, which is precisely the accident this whole mechanism exists
      // to prevent.
      out.reset = true;
      state.reset_armed_at = 0.0;
    }
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", reset_tooltip(reset));

  // The close button was slot 4 of this row for one commit (0739f64). It now
  // sits beside the chat arrow on the status row (user, 19 Sep 2026), so the
  // row is back to exactly the four slots it had before — and slots 0-3 did
  // not move in either direction, because none of their positions was ever a
  // function of anything but their own index.
  //
  // One thing stays behind. The close button cannot see the gestures that mean
  // the user has moved on and is no longer closing: status_bar() draws before
  // this row, so `out` is still empty when the close button reads it. A
  // counter is the honest way across, and one frame of lag is nothing against
  // a four-second timeout. `out.reset` is on the list because arming Close and
  // then confirming Reset is a user who is plainly no longer closing.
  if (out.talk_pressed || out.stop || out.reset || mute_clicked) ++state.transport_gesture_seq;
}

}  // namespace

// M2.9. One set of words for both doors into `VoiceSession::say()`: the
// message field, and the bus's `session.say`. `say()` itself is silent about
// every one of these — it returns without a sound while the engines are down,
// while the microphone is open, and barges in mid-reply — so whoever calls it
// is the one that has to know, and there is now more than one caller.
const char* send_refusal(const VoiceSession::Snapshot& snap, bool voice_enabled,
                         const char* text) {
  if (blank(text)) return "nothing to send";
  if (!voice_enabled) return "no voice this run (--no-voice)";
  switch (snap.state) {
    case VoiceSession::State::Loading: return "still starting up";
    case VoiceSession::State::Failed: return "the session failed to start";
    // Listening is deliberately **not** a refusal. An open microphone used to
    // stop a typed message being sent at all, which made the keyboard
    // unavailable for as long as the voice channel was up. The session now
    // takes it: what was being heard is discarded, the typed text is the turn,
    // and the latch survives, so listening resumes once the reply is over.
    case VoiceSession::State::Thinking:
    case VoiceSession::State::Speaking: return "Claude is still replying";
    default: return nullptr;
  }
}

bool avatar_engaged(const AvatarEngagement& e) {
  switch (e.state) {
    // A turn in flight, in any of its phases. Thinking is on this list now,
    // and it is the single line that stops the avatar leaving in the middle
    // of the question it was asked (user, 19 Sep 2026).
    case VoiceSession::State::Listening:
    case VoiceSession::State::Thinking:
    case VoiceSession::State::Speaking:
      return true;
    default:
      break;
  }
  // Nothing is happening in the session. That is not the same as nobody being
  // there: the latch, a held Talk button and a half-written message are all
  // the user, mid-interaction, with the session idle behind them.
  return e.mic_on || e.mic_hold || e.composing;
}

bool AvatarPresence::update(AvatarVisibility mode, const AvatarEngagement& e, float dt) {
  // Loading is the one state the avatar is never wanted in, whatever the mode
  // and whatever the user's hands are doing: the loading screen owns the
  // window and the M1.5 handoff is what brings the avatar in.
  if (e.state == VoiceSession::State::Loading) {
    linger_ = 0.0f;
    return false;
  }
  switch (mode) {
    case AvatarVisibility::Always:
      linger_ = 0.0f;
      return true;
    case AvatarVisibility::WhenTalking:
      break;
    default:
      linger_ = 0.0f;
      return false;
  }
  if (avatar_engaged(e)) {
    linger_ = kLingerSeconds;
    return true;
  }
  // Disengaged. The grace runs down from whatever the last engaged frame
  // left, so re-engaging inside it costs nothing at all -- not a fade, not a
  // clip -- because the avatar never stopped being wanted.
  linger_ = std::max(0.0f, linger_ - dt);
  return linger_ > 0.0f;
}

AvatarUiResult draw_avatar_ui(AvatarUiState& state, const VoiceSession::Snapshot& snap,
                              const AvatarOptions& options, bool voice_enabled, bool mic_on,
                              bool mic_hold, std::uint32_t width, std::uint32_t top, bool submit) {
  AvatarUiResult out;
  // Cleared here rather than where it is set, because the control that sets it
  // is not drawn on most frames: a surface that closed on the frame after a
  // drag would otherwise leave the edge stuck true for as long as it stayed
  // shut. Same reason the other one-shot results live on `out`.
  state.custom_colour_changed = false;
  const float w = static_cast<float>(width);
  // Everything below reacts to this one flag. With --no-voice there is no
  // session and the snapshot stays Loading, which is exactly the state the
  // loading overlay draws in too, so the two agree by construction — and
  // main.cpp keeps the snapshot in Loading until the M1.5 handoff is over.
  const bool loading = snap.state == VoiceSession::State::Loading;

  // M1f.3. The session's one-frame edge, latched into something the button can
  // keep drawing; see AvatarUiState::mic_dozed.
  //
  // The clear is an `else if` rather than a second statement, and that is not
  // a tidiness choice: on the frame the timeout fires the session has *already*
  // put itself back in Idle with the microphone shut, so a clear that ran
  // unconditionally would wipe the flag on the same frame it was set and the
  // face would never appear once. The edge wins its own frame; the level takes
  // every frame after it.
  //
  // What clears it is the same rule the slime wakes on (avatar_controller.cpp)
  // stated in the terms this surface has: the microphone being open either way
  // round, or the session doing anything at all. A click, a hold, the latch, a
  // typed turn, a reply — each one is the user back at the desk, and none of
  // them should leave a `z` on the button.
  if (snap.listen_timeout_seq != state.listen_timeout_seq) {
    state.listen_timeout_seq = snap.listen_timeout_seq;
    state.mic_dozed = true;
  } else if (mic_on || mic_hold || snap.state != VoiceSession::State::Idle) {
    state.mic_dozed = false;
  }

  ImGui::SetNextWindowPos(ImVec2(0.0f, static_cast<float>(top)));
  ImGui::SetNextWindowSize(ImVec2(w, 0.0f));  // auto height, fixed width
  // Fully opaque. Only the avatar area above the panel shows the desktop;
  // the panel itself is a solid surface so text always reads cleanly.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_color(0.086f, 0.094f, 0.118f));
  ImGui::Begin("##panel", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_AlwaysAutoResize);

  // M1c.1: the toolbar, above everything else in the panel. The milestone
  // calls for it "above the status line", and the status row (usage, the
  // avatar-mode button, the chat arrow) is the first thing the panel draws, so
  // above it is the top of the panel.
  button_bar(state, loading, w);

  status_bar(state, snap, loading, w, out);

  // What the loop is doing, or why it is not doing anything.
  if (voice_enabled) {
    const bool listening = snap.state == VoiceSession::State::Listening;
    ImGui::PushStyleColor(ImGuiCol_Text, listening ? accent() : fg());
    ImGui::Text("[%s]", VoiceSession::state_name(snap.state));
    ImGui::PopStyleColor();
    if (!snap.status.empty()) {
      ImGui::SameLine(0.0f, 6.0f);
      ImGui::TextColored(dim(), "%s", snap.status.c_str());
    }
  } else {
    ImGui::TextColored(dim(), "(no voice: --no-voice)");
  }

  // There is no transcript before the first turn, so during load the chat is
  // a tall empty box: it makes the window twice as tall as it needs to be and
  // drags the centred loader down onto the status line. Withheld (not closed —
  // `chat_open` is untouched) the panel shrinks to its two text rows and the
  // transport, so the loader centres clear of both and the widget sits compact
  // in the corner until the engines are up.
  //
  // M1c.3: the settings surface takes this same region when it is open, and
  // takes it in preference to the chat. Both are kChatHeight tall, so opening
  // the cog over an open chat changes the window's height by exactly nothing
  // and not one control on screen moves — which is the whole reason the two
  // share a region rather than stacking. Opening it over a *closed* chat grows
  // the window the same way the chat arrow does, through the same deferred
  // path, and the click that did it has already completed on the release that
  // toggled the flag.
  if (loading) {
    // nothing here while the loading screen owns the window
  } else if (state.settings_open) {
    settings_surface(state, options);
  } else if (state.chat_open) {
    chat(snap);
  }

  // M1b.2: the message field, then the transport row at the very bottom of the
  // window (the user asked for that order).
  message_field(state, snap, voice_enabled, loading, submit, out);

  // The transport row: three small icon buttons in fixed slots. The row's own
  // top is captured before it is drawn, because what the panel's height must
  // be cannot come from the last item any more — the last item is Stop's slot,
  // which on most frames is a Dummy, and on the frames it is not it is a
  // button. Taking the height from the row's geometry instead makes the panel
  // exactly as tall whether Stop is there or not, which is the same invariant
  // as the slots themselves: nothing about this row may move because the
  // session started or stopped doing something.
  const float row_top = ImGui::GetCursorScreenPos().y;
  transport(state, snap, voice_enabled, loading, mic_on, mic_hold, out);

  // Measured from the row rather than read off the window. An auto-resizing
  // window is capped at the viewport — which here is the OS window this number
  // sets — so asking the window how tall it is makes the widget unable to grow
  // past its own current height: with the avatar band gone, `top` is 0 and
  // opening the chat could never make the window taller than the panel already
  // was. The content bottom is not clamped, so it always reports what the
  // layout actually wants.
  const float bottom = row_top + kTransportButton + ImGui::GetStyle().WindowPadding.y;
  out.desired_height = static_cast<std::uint32_t>(bottom + 0.5f);
  ImGui::End();
  ImGui::PopStyleColor();
  return out;
}

}  // namespace aii

