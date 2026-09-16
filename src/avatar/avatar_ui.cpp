#include "avatar_ui.h"

#include "imgui.h"
#include "imgui_layer.h"

#include <cstdio>
#include <ctime>
#include <string>

namespace aii {
namespace {

constexpr float kChatHeight = 260.0f;  // the scrollback area when open

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
  // The live transcript while the microphone is open.
  if (!snap.partial.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, accent());
    ImGui::TextWrapped("You: %s", snap.partial.c_str());
    ImGui::PopStyleColor();
  }

  // Follow the newest line, but only while the user has not scrolled up.
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);

  ImGui::EndChild();
  ImGui::PopStyleColor();
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
                              bool voice_enabled, bool mic_on, std::uint32_t width,
                              std::uint32_t top) {
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

  // Transport. Three equal buttons across the content width.
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float button_w = (ImGui::GetContentRegionAvail().x - 2.0f * spacing) / 3.0f;
  const ImVec2 size(button_w, 34.0f);
  // A latch, not a hold: one click opens the mic, the next mutes it. While
  // the latch is on the button reads "Mute" and goes red in every state
  // (hover and press included, or ImGui's blue would take over the moment the
  // pointer touched it) so an open microphone is unmistakable. That includes
  // the stretch where Claude is replying and the mic is briefly shut.
  // Nothing routes anywhere until the engines are up, so during load the three
  // read as unavailable rather than as live controls inviting a click.
  ImGui::BeginDisabled(loading);
  if (mic_on) {
    ImGui::PushStyleColor(ImGuiCol_Button, red());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, red_hot());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, red_deep());
    ImGui::PushStyleColor(ImGuiCol_Text, ui_color(1.00f, 0.96f, 0.95f));
  }
  out.talk_clicked = ImGui::Button(mic_on ? "Mute" : "Talk", size);
  if (mic_on) ImGui::PopStyleColor(4);
  ImGui::SameLine();
  out.silence = ImGui::Button("Silence", size);
  ImGui::SameLine();
  out.pause = ImGui::Button("Pause", size);
  ImGui::EndDisabled();

  // Measured from the last widget rather than read off the window. An
  // auto-resizing window is capped at the viewport — which here is the OS
  // window this number sets — so asking the window how tall it is makes the
  // widget unable to grow past its own current height: with the avatar band
  // gone, `top` is 0 and opening the chat could never make the window taller
  // than the panel already was. The content bottom is not clamped, so it
  // always reports what the layout actually wants.
  const float bottom = ImGui::GetItemRectMax().y + ImGui::GetStyle().WindowPadding.y;
  out.desired_height = static_cast<std::uint32_t>(bottom + 0.5f);
  ImGui::End();
  ImGui::PopStyleColor();
  return out;
}

}  // namespace aii
