#pragma once
// The avatar window's panel, in ImGui: the status bar, the collapsible chat
// and the transport buttons. Everything above the panel is the avatar itself
// (the pixel grid), which this never draws over.
//
// The panel auto-sizes to its content, so draw() reports back how tall the
// window needs to be; main.cpp resizes the OS window to match, which is what
// makes closing the chat shrink the widget instead of leaving a transparent
// hole that still swallows desktop clicks.
#include <cstdint>
#include <string>

#include "voice_session.h"

namespace aii {

// The three avatar visibility modes, in the order the button cycles them
// (user, 16 Sep 2026). `WhenTalking` is listening or speaking only — it goes
// dark through the thinking pause, which the user chose knowing it does.
enum class AvatarVisibility { Always, WhenTalking, Hidden };

// How long the Talk button (or SPACE) has to be down before the release means
// "dictate into the field" rather than "latch the microphone on" (M1b.3).
//
// Biased hard toward reading a press as a click, because the two failure
// directions are not symmetric. A hold misread as a click sends an utterance
// the user meant to edit: they see it go, and the words are in the transcript.
// A click misread as a hold leaves the text sitting in the field unsent, with
// the microphone shut and nothing having happened — indistinguishable from the
// app having ignored them. So the threshold sits well above any click: a
// deliberate one measures 60-120 ms and a sluggish one around 250 ms, while a
// real hold-to-dictate is a second or more before the user has said anything
// at all. 400 ms leaves a wide margin on the side that matters and costs the
// hold gesture nothing, since the microphone opens on the press either way and
// no speech is lost while the meaning is still undecided.
constexpr float kTalkHoldSeconds = 0.40f;

// Whether the avatar belongs on screen for this mode in this state. The rule
// lives next to the button that sets the mode rather than being restated in
// the frame loop, which only turns the answer into a fade.
bool avatar_visible(AvatarVisibility mode, VoiceSession::State state);

struct AvatarUiState {
  // Closed until the user clicks the chat arrow (user, 16 Sep 2026). The
  // widget lives in the corner of a desktop that is being worked in, so it
  // starts as small as it can be; nothing opens the chat implicitly, not a
  // new transcript line and not the end of loading. Once opened it stays
  // open until the same arrow closes it.
  bool chat_open = false;
  // Same contract as `chat_open`: user state, written only by its button and
  // kept for the session. Defaults to the behaviour that predates the button.
  AvatarVisibility avatar_mode = AvatarVisibility::Always;
  // What is in the message field (M1b.2). A fixed buffer rather than a
  // std::string because imgui_stdlib is not in this build, and a corner
  // window's typed message has no business being longer than this anyway.
  char message[2048] = {};
  // Seconds left on the "why that Enter did nothing" line under the field.
  // Counted down here so the refusal is visible for a moment without the row
  // itself appearing and disappearing, which would resize the window.
  float refusal_left = 0.0f;
  std::string refusal;

  // M1b.4: the live partial transcript is written into the message field
  // itself, not into a label of its own (user, 16 Sep 2026) — so what is being
  // heard is visible whether or not the chat is open, and a dictation can be
  // edited where it lands. `Writing` is speech filling the field; `Yielded` is
  // the user having touched it mid-utterance, after which speech keeps its
  // hands off until the microphone next opens.
  enum class Dictation { Idle, Writing, Yielded };
  Dictation dictation = Dictation::Idle;
  // Whatever the user had already typed when the microphone opened. Speech
  // appends after it rather than over it, and an auto-sent utterance reverts
  // the field to it — which is a plain clear in the normal case where nothing
  // was typed, and keeps the typed half otherwise.
  std::string dictation_prefix;
  // The exact text speech last wrote. Anything else in the field means the
  // user edited it, which is how `Yielded` is detected.
  std::string dictation_last;
  VoiceSession::State prev_state = VoiceSession::State::Loading;
  // The last finalised-but-unsent utterance this panel has written into the
  // field, against `Snapshot::dictated_seq`. Equal means there is nothing new.
  unsigned dictated_seq = 0;
  // When the Talk button went down, on ImGui's clock. Only meaningful between
  // the press and the release that reads it.
  double talk_pressed_at = 0.0;
};

struct AvatarUiResult {
  // The Talk gesture, as press and release rather than as a click (M1b.3).
  // The press opens the microphone; the release says what the press meant.
  bool talk_pressed = false;
  bool talk_released = false;
  bool talk_held = false;        // the release came after kTalkHoldSeconds
  bool talk_over_button = false; // ...and the pointer was still on the button
  bool silence = false;  // Silence pressed
  bool pause = false;    // Pause pressed
  // The message field's contents, on the frame Enter sent them; the field has
  // already been cleared. Empty on every other frame, including the ones where
  // a send was refused — the text stays in the field then, never swallowed.
  std::string send_text;
  // Total window height the layout wants, avatar area included.
  std::uint32_t desired_height = 0;
};

// Builds the panel for this frame. `top` is where the panel starts, i.e. the
// height reserved for the avatar above it — zero once the visibility mode has
// hidden the avatar, which is how hiding it collapses the widget toward the
// corner exactly as closing the chat does.
//
// The loading layout is keyed off `snap.state`, and main.cpp holds that state
// at Loading for the whole M1.5 crossfade rather than only until the session
// reports itself idle — so this stays a pure function of the snapshot it is
// given, and everything the loading screen covers changes on one frame.
// `submit` is one plain-Enter press, from WinTextInput — which withholds that
// key from ImGui so the multiline message field never turns it into a newline.
// Shift+Enter never arrives here; ImGui sees it and inserts the newline itself.
AvatarUiResult draw_avatar_ui(AvatarUiState& state, const VoiceSession::Snapshot& snap,
                              bool voice_enabled, bool mic_on, std::uint32_t width,
                              std::uint32_t top, bool submit);

}  // namespace aii
