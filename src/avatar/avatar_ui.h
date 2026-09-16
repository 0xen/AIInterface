#pragma once
// The avatar window's panel, in ImGui: the status bar, the collapsible chat
// and the transport buttons. Everything above the panel is the avatar itself
// (the cube), which this never draws over.
//
// The panel auto-sizes to its content, so draw() reports back how tall the
// window needs to be; main.cpp resizes the OS window to match, which is what
// makes closing the chat shrink the widget instead of leaving a transparent
// hole that still swallows desktop clicks.
#include <cstdint>

#include "voice_session.h"

namespace aii {

// The three avatar visibility modes, in the order the button cycles them
// (user, 16 Sep 2026). `WhenTalking` is listening or speaking only — it goes
// dark through the thinking pause, which the user chose knowing it does.
enum class AvatarVisibility { Always, WhenTalking, Hidden };

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
};

struct AvatarUiResult {
  bool talk_clicked = false;  // Talk clicked: toggle the mic latch
  bool silence = false;  // Silence pressed
  bool pause = false;    // Pause pressed
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
AvatarUiResult draw_avatar_ui(AvatarUiState& state, const VoiceSession::Snapshot& snap,
                              bool voice_enabled, bool mic_on, std::uint32_t width,
                              std::uint32_t top);

}  // namespace aii
