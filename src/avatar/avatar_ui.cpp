#include "avatar_ui.h"

#include "imgui.h"
#include "imgui_layer.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
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
  // The live partial used to be echoed here as well. It is not any more: it
  // now streams into the message field (M1b.4), which is visible whether or
  // not this region is open, and two places showing the same words while they
  // are still being recognised is one too many (user, 16 Sep 2026).

  // Follow the newest line, but only while the user has not scrolled up.
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);

  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// How long the reason an Enter did nothing stays under the field.
constexpr float kRefusalSeconds = 2.5f;
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
                              std::uint32_t top, bool submit) {
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

  // M1b.2: the message field, then the transport row at the very bottom of the
  // window (the user asked for that order).
  message_field(state, snap, voice_enabled, loading, submit, out);

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

