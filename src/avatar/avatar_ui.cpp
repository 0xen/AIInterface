#include "avatar_ui.h"

#include "core/button_registry.h"
#include "imgui.h"
#include "imgui_layer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

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
// Drawn as filled cells at a whole-number scale, and only a whole-number scale.
// A 13-cell grid at 2x is 26 px and every cell is exactly 2x2; at 2.3x the
// cells would alternate 2 and 3 px wide and the icon would read as mush, which
// is the one thing an 8-bit icon may not do. The button is sized from the icon
// rather than the icon fitted to the button, so the scale is never in question.
constexpr int kIconCells = 13;
constexpr float kIconScale = 2.0f;
constexpr float kIconPx = kIconCells * kIconScale;
constexpr float kTransportButton = 30.0f;  // kIconPx plus 2 px of air all round

using IconRows = const char* [kIconCells];

// The microphone, five ways. Idle is the bare capsule-and-cradle; everything
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

// One cell per filled character, snapped to whole pixels. `p` is the icon's
// top-left in screen space and is floored for the same reason the scale is an
// integer: half a pixel of origin undoes every bit of the alignment above.
void draw_icon(ImDrawList* dl, const char* const* rows, ImVec2 p, ImU32 ink, ImU32 mark) {
  const float x0 = std::floor(p.x), y0 = std::floor(p.y);
  for (int r = 0; r < kIconCells; ++r) {
    for (int c = 0; rows[r][c]; ++c) {
      const char ch = rows[r][c];
      if (ch == '.') continue;
      const float x = x0 + c * kIconScale, y = y0 + r * kIconScale;
      dl->AddRectFilled(ImVec2(x, y), ImVec2(x + kIconScale, y + kIconScale),
                        ch == 'o' ? mark : ink);
    }
  }
}

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

// One "12% CTX" segment. A negative fraction means the CLI has not said yet.
// `reset_epoch` > 0 turns the label into "Session (2h14m)" — the real time
// left in that window rather than its nominal length.
void segment(double fraction, const char* label, double amber, double red,
             long long reset_epoch = 0) {
  std::string text = label;
  if (const std::string left = until(reset_epoch); !left.empty()) text += " (" + left + ")";
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

void draw_cog(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  dl->AddCircle(c, r * 0.58f, col, 16, 1.5f);
  dl->AddCircleFilled(c, r * 0.17f, col, 10);
  for (int i = 0; i < 6; ++i) {
    const float a = 3.14159265f * 2.0f * static_cast<float>(i) / 6.0f;
    dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.52f, c.y + std::sin(a) * r * 0.52f),
                ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), col, 1.7f);
  }
}

void draw_folder(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  const float w = r * 0.98f, h = r * 0.74f;
  // The tab first, then the body over it, so the two read as one shape.
  dl->AddRectFilled(ImVec2(c.x - w, c.y - h), ImVec2(c.x - w * 0.18f, c.y - h * 0.45f), col, 1.5f);
  dl->AddRectFilled(ImVec2(c.x - w, c.y - h * 0.62f), ImVec2(c.x + w, c.y + h), col, 2.0f);
}

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
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    const ImU32 col = ImGui::GetColorU32(fg());
    if (b.glyph == ButtonGlyph::Cog)
      draw_cog(dl, c, size * 0.30f, col);
    else
      draw_folder(dl, c, size * 0.30f, col);
  }
  if (ImGui::IsItemHovered() && !b.tooltip.empty()) ImGui::SetTooltip("%s", b.tooltip.c_str());
  return clicked;
}

// Draws the whole bar and acts on whatever was clicked. Live while loading,
// like the avatar-mode button beside it: none of these route into an engine,
// and the row has to occupy its height from the first frame or the window
// would change size the moment loading ended.
void button_bar(AvatarUiState& state, float width) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float size = ImGui::GetFrameHeight();
  float avail = width - 2.0f * style.WindowPadding.x;
  bool first = true;
  bool open_settings = false;

  for (const ToolbarButton& b : ButtonRegistry::instance().snapshot()) {
    if (!bar_button(b, size, avail, first)) continue;
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
    }
  }
  // A placeholder until M1c.3 builds the real surface. It is a popup rather
  // than a panel row on purpose: the settings surface is specced to grow into
  // voices, paths and themes, and none of that belongs in a 360 px column.
  if (open_settings) ImGui::OpenPopup("##settings");
  if (ImGui::BeginPopup("##settings")) {
    ImGui::TextColored(dim(), "Settings");
    ImGui::Separator();
    ImGui::TextColored(dim(), "Avatar and theme pickers land in M1c.3.");
    ImGui::EndPopup();
  }
}

void separator() {
  ImGui::SameLine(0.0f, 6.0f);
  ImGui::TextColored(dim(), "|");
  ImGui::SameLine(0.0f, 6.0f);
}

// "12% CTX | 40% Session (2h14m) | 53% Week (3d 5h)" plus the chat toggle,
// pinned to the right edge of the same row. The bracketed times count down to
// when each window actually resets, as reported by the CLI.
// While loading none of the three windows has a number yet, so the row would
// read "--% CTX | --% Session | --% Week" — three placeholders that say
// nothing while the overlay's own caption is already saying what is happening.
// The row still draws (empty) so the panel keeps its height and the chat
// toggle keeps its place; the toggle is disabled with the rest of the
// controls, since there is no chat to open until the engines are up.
void status_bar(AvatarUiState& state, const UsageStats& usage, bool loading, float width) {
  if (loading) {
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
  } else {
    segment(usage.ctx, "CTX", 20.0, 30.0);
    separator();
    segment(usage.session, "Session", 70.0, 90.0, usage.session_reset);
    separator();
    segment(usage.week, "Week", 70.0, 90.0, usage.week_reset);
  }

  const float button = ImGui::GetFrameHeight();
  const float gap = ImGui::GetStyle().ItemSpacing.x;
  ImGui::SameLine();
  ImGui::SetCursorPosX(width - ImGui::GetStyle().WindowPadding.x - 2.0f * button - gap);
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
  for (const auto& w : snap.workers) {
    std::string line = w.name + " [" + worker_state_name(w.state) + "] " + w.activity;
    if (w.tool_calls) line += "  x" + std::to_string(w.tool_calls);
    ImGui::TextColored(worker_color(w.state), "%s", line.c_str());
  }
  if (!snap.workers.empty()) ImGui::Separator();

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

// Why this text cannot be sent right now, or null if it can. Sending routes
// into VoiceSession::say(), which refuses outright while the engines are down
// or the microphone is open, and treats a send during a turn as a barge-in —
// so the field does not offer that: a turn in flight is a wait, not a queue.
const char* refusal_reason(const VoiceSession::Snapshot& snap, bool voice_enabled,
                           const char* text) {
  if (blank(text)) return "nothing to send";
  if (!voice_enabled) return "no voice this run (--no-voice)";
  switch (snap.state) {
    case VoiceSession::State::Loading: return "still starting up";
    case VoiceSession::State::Failed: return "the session failed to start";
    case VoiceSession::State::Listening: return "the microphone is open";
    case VoiceSession::State::Thinking:
    case VoiceSession::State::Speaking: return "Claude is still replying";
    default: return nullptr;
  }
}

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
struct FieldEdit {
  const char* text = nullptr;  // non-null: replace the field's contents with this
};

int replace_contents(ImGuiInputTextCallbackData* data) {
  auto* edit = static_cast<FieldEdit*>(data->UserData);
  if (edit->text) {
    data->DeleteChars(0, data->BufTextLen);
    if (*edit->text) data->InsertChars(0, edit->text);
    edit->text = nullptr;
  }
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
  if (dictate_into_field(state, snap)) {
    pending = state.message;
    edit.text = pending.c_str();
  }
  if (submit && !loading) {
    if (const char* why = refusal_reason(snap, voice_enabled, state.message)) {
      // Refused, never queued and never dropped: the text is left in the field
      // exactly as typed and the reason appears below it.
      state.refusal = why;
      state.refusal_left = kRefusalSeconds;
    } else {
      out.send_text = state.message;
      state.message[0] = '\0';
      pending.clear();
      edit.text = pending.c_str();
      // A typed send ends any dictation that was feeding the field, so the
      // prefix does not come back on the next microphone close.
      state.dictation = AvatarUiState::Dictation::Idle;
      state.dictation_prefix.clear();
      state.dictation_last.clear();
      state.refusal_left = 0.0f;
    }
  }

  // Sized from the *wrapped* height, not from the newlines in the text. A
  // dictation arrives as one long unpunctuated line and the field wraps it
  // (NoHorizontalScroll), so counting '\n' would leave a one-line field with
  // the speech scrolled out of sight — and nobody has to press a key to get
  // there. Capped at four lines so a long utterance cannot push the transport
  // row down the window.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float line_h = ImGui::GetTextLineHeight();
  const float wrap_w = ImGui::GetContentRegionAvail().x - 2.0f * style.FramePadding.x;
  const float text_h =
      ImGui::CalcTextSize(state.message, nullptr, false, wrap_w).y;
  const int lines = std::clamp(static_cast<int>(text_h / line_h + 0.5f), 1, kMessageLinesMax);
  const float h = lines * line_h + 2.0f * style.FramePadding.y;

  ImGui::PushStyleColor(ImGuiCol_FrameBg, ui_color(0.071f, 0.078f, 0.098f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui_color(0.086f, 0.094f, 0.118f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui_color(0.098f, 0.106f, 0.133f));
  ImGui::PushStyleColor(ImGuiCol_Border, ui_color(0.16f, 0.17f, 0.21f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  ImGui::BeginDisabled(loading);
  ImGui::InputTextMultiline("##message", state.message, sizeof(state.message),
                            ImVec2(-FLT_MIN, h),
                            ImGuiInputTextFlags_NoHorizontalScroll |
                                ImGuiInputTextFlags_CallbackAlways,
                            replace_contents, &edit);
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
// Three fixed slots. Stop hides itself when there is nothing to stop (user,
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

// The five faces of the microphone button. Every one of them is something the
// session can actually report — there is no "about to listen" or "hearing
// noise" here, because nothing in VoiceSession knows either.
enum class MicFace {
  Unavailable,  // engines still loading, or --no-voice: nothing to press
  Idle,         // shut, and the next press decides what it means
  Dictating,    // a Talk press is down; the words are going into the box
  Listening,    // latched on and hearing the room
  Latched,      // latched on but deliberately shut while Claude replies
};

MicFace mic_face(const VoiceSession::Snapshot& snap, bool voice_enabled, bool loading,
                 bool mic_on, bool mic_hold) {
  // The same escape hatch `--clip` and `--sprite` give the avatar's art, and
  // for the same reason: two of these five faces are only reachable by talking
  // into a microphone, so without this there is no way to *look* at them — and
  // "verified by looking at a capture" is the standard this panel is held to.
  // `AII_MIC_FACE=0..4` pins one; `cycle` walks all five, two seconds each.
  if (const char* pin = std::getenv("AII_MIC_FACE")) {
    const int n = std::strcmp(pin, "cycle") == 0
                      ? static_cast<int>(ImGui::GetTime() * 0.5) % 5
                      : std::atoi(pin);
    return static_cast<MicFace>(std::clamp(n, 0, 4));
  }
  if (loading || !voice_enabled) return MicFace::Unavailable;
  // A hold is not the latch and never sets it (VoiceSession::talk_pressed),
  // so this order is not a preference between two true things.
  if (mic_hold) return MicFace::Dictating;
  if (!mic_on) return MicFace::Idle;
  return snap.state == VoiceSession::State::Listening ? MicFace::Listening : MicFace::Latched;
}

const char* const* mic_icon(MicFace face) {
  switch (face) {
    case MicFace::Dictating: return kIconMicHold;
    case MicFace::Listening: return kIconMicLive;
    case MicFace::Latched: return kIconMicShut;
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
    case MicFace::Unavailable:
      return neutral_skin(dim());
    default:
      return neutral_skin(fg());
  }
}

const char* mic_tooltip(MicFace face) {
  switch (face) {
    case MicFace::Unavailable: return "Microphone - not ready yet";
    // The two gestures, still spelled out: they are the whole of what this
    // button does and the label that used to hint at it is gone.
    case MicFace::Idle: return "Click to talk  -  hold to dictate into the box";
    case MicFace::Dictating: return "Recording - release to put it in the box";
    case MicFace::Listening: return "Listening - click to stop  (hold to dictate)";
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

void transport(AvatarUiState& state, const VoiceSession::Snapshot& snap, bool voice_enabled,
               bool loading, bool mic_on, bool mic_hold, AvatarUiResult& out) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float gap = style.ItemSpacing.x;
  // One origin, three slot indices. Read off the layout cursor once, before
  // anything is submitted, so nothing that happens inside the row can move a
  // later slot: slot 2 being absent cannot shift slot 0 because slot 0's
  // position was never a function of slot 2.
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  auto slot_pos = [&](int i) {
    return ImVec2(origin.x + i * (kTransportButton + gap), origin.y);
  };

  ImGui::BeginDisabled(loading);

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
  const MicFace face = mic_face(snap, voice_enabled, loading, mic_on, mic_hold);
  transport_slot("##mic", slot_pos(0), mic_icon(face), mic_skin(face));
  if (ImGui::IsItemActivated()) {
    state.talk_pressed_at = ImGui::GetTime();
    out.talk_pressed = true;
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
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mic_tooltip(face));

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
  if (transport_slot("##mute", slot_pos(1), state.muted ? kIconSpeakerMuted : kIconSpeaker,
                     mute_skin))
    state.muted = !state.muted;
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
}

}  // namespace

bool avatar_visible(AvatarVisibility mode, VoiceSession::State state) {
  switch (mode) {
    case AvatarVisibility::Always:
      // Loading is the one state it is never wanted in: the loading screen
      // owns the window and the M1.5 handoff is what brings the avatar in.
      return state != VoiceSession::State::Loading;
    case AvatarVisibility::WhenTalking:
      // Thinking is deliberately not on this list (user, 16 Sep 2026).
      return state == VoiceSession::State::Listening ||
             state == VoiceSession::State::Speaking;
    default:
      return false;
  }
}

AvatarUiResult draw_avatar_ui(AvatarUiState& state, const VoiceSession::Snapshot& snap,
                              bool voice_enabled, bool mic_on, bool mic_hold,
                              std::uint32_t width, std::uint32_t top, bool submit) {
  AvatarUiResult out;
  const float w = static_cast<float>(width);
  // Everything below reacts to this one flag. With --no-voice there is no
  // session and the snapshot stays Loading, which is exactly the state the
  // loading overlay draws in too, so the two agree by construction — and
  // main.cpp keeps the snapshot in Loading until the M1.5 handoff is over.
  const bool loading = snap.state == VoiceSession::State::Loading;

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
  button_bar(state, w);

  status_bar(state, snap.usage_stats, loading, w);

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
  if (state.chat_open && !loading) chat(snap);

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

