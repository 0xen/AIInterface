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
#include <vector>

#include "avatar_def.h"
#include "voice_session.h"

namespace aii {

// The three avatar visibility modes, in the order the button cycles them
// (user, 16 Sep 2026). `WhenTalking` is listening or speaking only — it goes
// dark through the thinking pause, which the user chose knowing it does.
enum class AvatarVisibility { Always, WhenTalking, Hidden };
constexpr int kAvatarVisibilityCount = 3;
// How the three modes are spelled in the settings file (M1b.5), in enum
// order. A name rather than the integer, because that file is meant to be
// read and edited by hand like an avatar definition and `2` says nothing.
// These are an on-disk format: changing one silently resets the setting for
// everyone who already has a file, so they stay as they are — the tooltip
// prose in avatar_ui.cpp is the string that is free to change.
inline constexpr const char* kAvatarVisibilityNames[kAvatarVisibilityCount] = {
    "always", "when_talking", "hidden"};

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

// M1c.3: what the settings surface has to offer this frame, from the parts of
// the app the panel has no business reaching into. The panel owns no art, no
// files and no engines; it renders lists and reports a choice back through
// `AvatarUiState`, which is what keeps the surface addable-to without the
// panel growing dependencies on everything it can set.
struct AvatarOptions {
  // Every avatar definition that could be opened, and every theme the one
  // currently open declares. Both come from the art, so both can change while
  // the app is running (a hot edit that adds a theme, a directory dropped into
  // %APPDATA%) and both are re-read every frame rather than cached here.
  std::vector<std::string> avatars;
  std::vector<std::string> themes;
  // AvatarSource's one-line status, shown under the pickers. A theme that did
  // not exist, or art that failed to load, is a thing the user did with the
  // control they are looking at, so the answer belongs beside it and not only
  // in a log nobody is reading.
  std::string art_status;
  bool art_status_ok = true;

  // M1c.5. What the picked body colour currently derives to, so the surface
  // can show the derived inks and say honestly how far it had to go. The
  // panel does not derive anything itself: the rule is one function in
  // avatar_def, and the picker's swatches and the avatar on screen are the
  // same numbers rather than two implementations that agree today.
  AvatarDerivedPalette derived;
  // True when the derived theme is the one in force, i.e. when the picker is
  // worth showing at all.
  bool custom_theme = false;

  // M8.3. What the session is actually doing with the language selection, as
  // opposed to what the checkboxes say. The two differ for about a second when
  // Japanese is switched on mid-run and its voice is still loading, and
  // permanently if that voice fails — the surface says so in both cases rather
  // than showing a ticked box for something that cannot happen.
  VoiceSession::VoiceLoad japanese_voice = VoiceSession::VoiceLoad::Absent;
  std::string japanese_voice_error;
  // The recogniser's `language` option in force: "auto", "en" or "ja".
  const char* stt_language = "auto";

  // M2.6. One line under Scripts: how many are running, why the host did not
  // start, or the last failure a script reported through `aii.status()`. Empty
  // is the normal case — nobody is scripting — and the section then says where
  // to put a script rather than nothing at all, because a feature with no
  // visible door is a feature nobody finds.
  std::string script_status;
  bool script_status_ok = true;
};

struct AvatarUiState {
  // Closed until the user clicks the chat arrow (user, 16 Sep 2026). The
  // widget lives in the corner of a desktop that is being worked in, so it
  // starts as small as it can be; nothing opens the chat implicitly, not a
  // new transcript line and not the end of loading. Once opened it stays
  // open until the same arrow closes it.
  //
  // M1b.5: and across runs. The default here is still the default, but it is
  // overwritten from the settings file before the first frame is drawn, so
  // the widget comes back the way it was left rather than flipping out of the
  // default once a load lands. The panel itself knows nothing about that —
  // main.cpp reads the file into this struct and mirrors changes back out.
  bool chat_open = false;
  // Claude's voice is muted (voice-only: the reply still arrives as text).
  // Same contract as `chat_open` — user state, written only by its own button
  // and the S key, persisted to settings.json and read back before the first
  // frame. The panel is the owner of record and main.cpp pushes it into the
  // session every frame, which is the same "mirror the level, not the edge"
  // pattern the persisted fields already use.
  bool muted = false;
  // Same contract as `chat_open`, persistence included: user state, written
  // only by its button. Defaults to the behaviour that predates the button.
  AvatarVisibility avatar_mode = AvatarVisibility::Always;

  // M1c.3. The settings surface is open, i.e. the cog has been clicked.
  //
  // Deliberately *not* persisted, unlike the two fields above it. Those are
  // preferences about how the widget sits on the desktop; this is a drawer the
  // user opened to change one, and an app that comes back up showing its own
  // settings has forgotten what it is for.
  //
  // It takes the same region of the panel the chat does, and takes it in
  // preference — the chat is withheld while it is open, exactly as the loading
  // screen withholds it, and `chat_open` is not touched, so closing the cog
  // gives back whatever was there. That is what keeps the widget's tallest
  // layout the height it already was: the settings surface adds no height to
  // the window that the chat had not already asked for, and the ceiling this
  // window can never exceed (kWindowH in main.cpp, a DirectComposition limit)
  // is 87 px above the tallest layout today. A surface that stacked on top of
  // the chat instead of replacing it would have gone straight through it.
  bool settings_open = false;
  // Which avatar definition and which of its themes the user has chosen
  // (M1c.3/M1c.4). Persisted by name in settings.json. The panel writes these
  // and main.cpp is what makes them true — and then writes back what actually
  // loaded, so a name that no longer resolves corrects itself in the picker
  // instead of sitting there claiming to be in force.
  std::string avatar_name;
  std::string theme;
  // M1c.5. The picked body colour, as the three floats ImGui's colour widgets
  // work in, 0..1 sRGB. Owned here for the same reason `theme` is: the panel
  // writes it, main.cpp makes it true and writes back what actually took, and
  // it is persisted under the same `avatar` section.
  //
  // `custom_colour_changed` is the edge, set on any frame the picker moved.
  // main.cpp uses it to push the colour down and to decide when to persist;
  // the level alone would not distinguish "the user is dragging" from "the
  // value main.cpp just wrote back", which on a control dragged sixty times a
  // second is the difference between one write and sixty.
  float custom_colour[3] = {0.0f, 0.0f, 0.0f};
  bool custom_colour_changed = false;
  // M8.3. Which languages are on. Same contract as `muted`: the panel is the
  // owner of record, main.cpp persists the level and pushes it into the
  // session every frame, and the default is what the app did before the
  // setting existed.
  //
  // **At least one is always on, and that is enforced by the control rather
  // than by a check after the fact.** The checkbox for the only enabled
  // language is drawn disabled, so the invariant is something the user can see
  // before they click. A box that could be clicked and then sprang back would
  // be worse than one that cannot be clicked: it would read as a bug in the
  // app rather than as a rule about the setting.
  bool lang_english = true;
  bool lang_japanese = true;
  // What is in the message field (M1b.2). A fixed buffer rather than a
  // std::string because imgui_stdlib is not in this build, and a corner
  // window's typed message has no business being longer than this anyway.
  char message[2048] = {};
  // The wrapped view of `message`, and the only buffer InputTextMultiline is
  // ever given. ImGui does not word-wrap a multiline field at all — it counts
  // '\n' and nothing else — so a long sentence ran off the right-hand edge
  // while the field's height, computed from a *wrapped* measurement, grew as
  // though it had flowed. The panel therefore does the wrapping itself: the
  // view is `message` with a soft '\n' inserted at every position ImGui's own
  // ImFont::CalcWordWrapPositionA would have broken a line at, and `message`
  // stays exactly what the user typed or dictated.
  //
  // The split is what keeps the soft breaks out of the message: everything
  // that matters — what is sent to Claude, what a refusal is tested against,
  // what dictation appends to and compares itself with — reads `message` and
  // has no idea the view exists. Nothing downstream changed.
  //
  // Bigger than `message` because the breaks are extra bytes; `message` is
  // still the cap on what can be typed, enforced when the view is unwrapped.
  char message_view[4096] = {};
  // The view as this panel last left it, and where the breaks it inserted
  // are. The pair is what makes unwrapping exact rather than a guess: a '\n'
  // the user typed with Shift+Enter and a '\n' the wrapper inserted are the
  // same byte, and only a record of which is which can tell them apart. The
  // offsets are carried across the user's edit rather than re-derived, so a
  // deliberate line break is never silently swallowed.
  std::string message_view_last;
  std::vector<int> message_soft;
  // The width `message_view` was wrapped at. A change re-wraps, which is what
  // makes the field correct after a font or a layout change rather than only
  // after the next keystroke.
  float message_wrap_w = -1.0f;
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
  // A typed Enter sent this frame, so the message field takes the keyboard
  // again before it is submitted. Set and cleared inside one draw(); it is a
  // member only because the send is decided before the widget is built.
  bool refocus_field = false;
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
  // Stop pressed: cancel the reply, drop the latch, pause the workers. The
  // Silence button that used to sit beside it is gone — mute replaced it, and
  // mute is a level in `AvatarUiState`, not an event, so it needs nothing
  // here.
  bool stop = false;
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
// `mic_on` is the latch; `mic_hold` is a Talk press being held right now. Both
// are frame-loop facts the session owns rather than snapshot fields, and the
// microphone button needs both to tell its five faces apart — a hold and a
// latch look identical from the snapshot, and a SPACE hold never touches the
// button at all.
AvatarUiResult draw_avatar_ui(AvatarUiState& state, const VoiceSession::Snapshot& snap,
                              const AvatarOptions& options, bool voice_enabled, bool mic_on,
                              bool mic_hold, std::uint32_t width, std::uint32_t top, bool submit);

}  // namespace aii
