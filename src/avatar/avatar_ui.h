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
#include "core/model_choice.h"
#include "core/tool_policy.h"
#include "voice_session.h"

namespace aii {

// The three avatar visibility modes, in the order the button cycles them
// (user, 16 Sep 2026). `WhenTalking` is *while the user is engaged* — see
// AvatarEngagement below for what that means and why it is no longer a
// property of the session's state alone (user, 19 Sep 2026).
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

// M1f.2. The bounds of the auto-listen timeout *control*, in whole seconds.
//
// The floor is the user-facing one and is not the mechanism's: VoiceSession
// enforces a 1 s sanity floor so a harness can measure the clock without
// sitting through a minute, and so a hand-edited 0.001 cannot make the latch
// close the frame it opens. This one is the lowest value a person is allowed
// to *choose*, and it is 15 s because below that the timeout stops reading as
// a timeout and starts reading as a bug: M1f.1 measured 5 s as already abrupt
// on the harness, endpointing alone spends 1 s of every pause, and a person
// composing a sentence aloud leaves gaps of several seconds without having
// left the room. 15 s is comfortably past the longest of those and still far
// short of the walked-away case the feature exists for.
//
// **The control cannot express anything between "never" and the floor.** The
// tick box is the whole of "never" and the number below it starts at the
// floor, so there is no gesture that produces 7 s and is then silently
// rewritten to 15 — a setting that lies about itself is worse than one that
// refuses.
//
// The ceiling is ten minutes: past that the latch is effectively open, which
// is what the tick box is for. Neither bound is applied to a value that
// arrives from AII_LISTEN_TIMEOUT or from a hand-edited file — those are shown
// as they are and honoured as they are, because a control that corrected the
// number it was given would hide what is actually in force. They bind the
// moment the user touches the control.
constexpr int kListenTimeoutUserFloorSec = 15;
constexpr int kListenTimeoutMaxSec = 600;

// M1f.5. Whether the app latches the microphone on for itself once the engines
// are up (user, 19 Sep 2026: "I would like the toggle in the settings to have
// the AI auto start by listening. And I would like this by default to be on.")
//
// **The default is on, and it is on here rather than in main.cpp**, for the
// reason `listen_timeout_sec` states in the other direction: a default written
// twice is a default that drifts. `AvatarUiState::auto_listen` starts at this
// value and `settings.json` is read *over* it, so a missing key and a fresh
// install are the same code path — which is exactly the case the user asked
// about.
//
// It is worth saying plainly what defaulting it on means, because nothing else
// in this panel does anything of the kind: **the app begins listening the
// moment it finishes starting, with nobody having touched it.** That is what
// was asked for, on the user's own machine, and it is built — but it puts the
// whole weight of the feature on the app being legible about it. Three
// surfaces say so from the first frame after the loader leaves (the microphone
// icon's `Listening` face, the status line, and the avatar being present
// because the latch counts as engagement), and none of them is new: this
// reaches exactly the state a click on the microphone reaches, so there is no
// second spelling of "the microphone is live" for one of them to get wrong.
constexpr bool kAutoListenDefault = true;

// ------------------------------------------------- when the avatar is here
//
// (user, 19 Sep 2026: "When I have it with mic toggled on, whenever I ask
// something, the AI keeps jumping in and out of the window. I do not want
// this. I only want the AI's avatar to physically leave the window when I
// have stopped interacting with it, for example, and I've turned the
// microphone off.")
//
// This reverses the 16 Sep rule, and the reversal is the user's own. The old
// one asked which state the *session* was in — Listening or Speaking, and
// deliberately not Thinking — so one exchange with the latch open was
// Listening (in), Thinking (**out**), Speaking (**in**), Idle (**out**): two
// entrances and two exits for one question. Measured, in this mode, every
// time.
//
// The new rule asks about the **user**, not about the middle of a turn. The
// avatar is here while they are engaged and leaves when they have stopped
// interacting, which is what "shown when talking" was always meant to say —
// a conversation is the unit, not a turn, and certainly not a phase of one.
struct AvatarEngagement {
  // Where the session is. Loading is the one answer that outranks everything
  // else here: that overlay owns the window, and no mode wants the avatar
  // underneath it.
  VoiceSession::State state = VoiceSession::State::Loading;
  // The microphone latch (VoiceSession::mic_open). **The user's own example
  // of being engaged**, and the reason this struct exists: a latch that is
  // open is a user who has not finished, whatever the turn is doing this
  // instant.
  bool mic_on = false;
  // A Talk press is down (VoiceSession::mic_hold). Someone holding a button
  // is interacting with it by definition.
  bool mic_hold = false;
  // There is text in the message field. Typing is an interaction too, and it
  // is the one kind that leaves the session sitting in Idle with the
  // microphone shut for as long as it takes to write a sentence — exactly the
  // shape the old rule read as "gone away". A draft is also where a
  // hold-to-dictate release puts its words, so the avatar stays to be seen
  // handing them over.
  bool composing = false;
};

// Engaged: a turn in flight, or the user's hand on one of the three things
// they can hold open. Pure, and the whole of the policy — AvatarPresence adds
// only the grace period, and nothing else in the program restates either.
bool avatar_engaged(const AvatarEngagement& e);

// Whether the avatar belongs on screen, for this mode, this frame. The rule
// lives next to the button that sets the mode rather than being restated in
// the frame loop, which only turns the answer into a fade.
//
// The one piece of state is the grace period, and it is one number doing two
// jobs that must not be allowed to disagree:
//
//   * **Nothing flaps at the boundary.** A mic toggled off and straight back
//     on — a mis-click, a second thought — must not cost an exit clip and an
//     entrance. Two seconds is longer than any hand takes to change its mind
//     and shorter than any pause that means "I have finished".
//   * **Dozing and leaving are one idea, in that order.** M1f's auto-listen
//     timeout closes the latch on a silent room and the slime droops
//     (`sleepy`, avatar_controller.cpp). That is the stopped-interacting case
//     par excellence, and it arrives here as `mic_on` going false with the
//     session already Idle — so the same grace becomes the beat between the
//     droop and the departure. It reads as one gesture: it gives up on the
//     room, then it leaves it. Departing on the same frame it dozed would
//     show the sleepy pose for no frames at all, which is two reactions
//     racing rather than one sentence; waiting on a second timer of its own
//     would be a third number for a silence M1f has already measured.
//
// Deliberately *not* applied to `always` or `hidden`: those change only when
// the user cycles the mode button, which is a deliberate act, and a band that
// is already 0 px wide with an avatar still fading inside it would be the
// resize bug back in a new shape.
class AvatarPresence {
 public:
  static constexpr float kLingerSeconds = 2.0f;

  // `dt` in seconds. Returns `wanted` for AvatarAppearance::update.
  bool update(AvatarVisibility mode, const AvatarEngagement& e, float dt);

 private:
  float linger_ = 0.0f;
};

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

  // M3.8. What the conversational instance was *actually launched with*, as
  // opposed to what the tick boxes say. The two differ the moment a toggle is
  // changed and stay different until the app is next started, because
  // `--allowedTools` is fixed when the child process starts and there is no
  // way to change it on a running one without throwing the conversation away
  // (M3.6). The surface's whole job in that state is to say so: a setting
  // that appears to do nothing is the worst failure a setting has, and this
  // one cannot do anything yet, so it must at least be honest about when it
  // will.
  //
  // Seeded once, before the first frame, from the same value `build_llm` was
  // given. Nothing writes it afterwards.
  ToolPolicy tools_in_force;
  // False on the `api` backend, which is not the Claude Code CLI and has no
  // tools of any kind. The toggles are then inert and say so rather than
  // claiming to have removed something that was never there.
  bool tools_supported = true;

  // M1f.5. What *this* run actually started with, beside what the tick box
  // says — the same shape as `tools_in_force` above and for the same reason,
  // though a much milder case of it. The two differ from the moment the box is
  // changed until the app is next started, because the thing the setting
  // decides happened once, seconds after launch, and cannot be made to happen
  // again without taking the microphone off the user. So the surface shows the
  // gap and names the launch, rather than letting a toggle look inert.
  //
  // Seeded once, before the first frame, from the same bool the latch itself
  // was armed from. Nothing writes it afterwards.
  bool auto_listen_in_force = kAutoListenDefault;
  // False with --no-voice, where there is no session and nothing was ever
  // going to listen. The row then says that rather than claiming a microphone
  // this run does not have.
  bool voice_enabled = true;
  // M3.11. What the conversational instance was actually launched with, as a
  // `--model` argument (empty = no flag, the CLI's own default). Exactly
  // `tools_in_force`'s contract and for exactly the same reason: `--model` is
  // fixed when the `claude` child starts, so the picker and this disagree from
  // the moment it is moved until the app is next started, and the section's
  // job in that state is to say which one Claude is holding.
  //
  // A string rather than an index, because it also has to be able to hold a
  // value the picker cannot produce — `AII_MODEL` naming a dated id, or a
  // stale key read out of `settings.json` — and say what it is.
  //
  // Seeded once, before the first frame, from the same value `build_llm` was
  // given. Nothing writes it afterwards.
  std::string model_in_force;
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
  // M1f.5. Latch the microphone on for the user once the engines are up. Same
  // persistence contract as `muted` above — the panel owns it, main.cpp seeds
  // it from settings.json before the first frame and mirrors it back every
  // frame — with one difference that the control on screen has to say out
  // loud: **nothing pushes this down into the session.** It is read once, at
  // the moment the loading screen leaves, and after that it is a note for the
  // next launch. Toggling it mid-run therefore cannot open or close the
  // microphone that is running, and must not: the microphone button is what
  // does that, and a setting that reached across and moved it would be a
  // second control for one piece of state.
  //
  // The default lives in `kAutoListenDefault`, not here, so the value and the
  // paragraph explaining it stay in one place.
  bool auto_listen = kAutoListenDefault;
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
  // M1f.2. The auto-listen timeout, as the control holds it: on/off and a
  // whole number of seconds. Same contract as `muted` — the panel is the owner
  // of record, main.cpp pushes the level into the session every frame and
  // mirrors it into settings.json — with one difference worth stating: there
  // is no default here.
  //
  // Both fields are seeded by main.cpp before the first frame from
  // `Config::listen_timeout` (the default, AII_LISTEN_TIMEOUT, then
  // settings.json over it), and `config.h` says that nothing below it should
  // ever spell "60" again. A default in this struct would be exactly that
  // second spelling, and the two would drift apart the first time one of them
  // changed. So they start at values that are obviously not a setting.
  //
  // `listen_timeout_sec` keeps its number while the box is unticked, so
  // switching the timeout off and back on within a run returns the value the
  // user chose rather than a default. Across runs it does not: the file stores
  // 0 and nothing else, because a remembered-but-inactive number is a second
  // piece of state that can disagree with the one in force.
  bool listen_timeout_on = false;
  int listen_timeout_sec = 0;
  // M3.8. The Tools toggles, as the control holds them. Same contract as
  // `muted` in one half — the panel is the owner of record and main.cpp
  // mirrors it into settings.json — and the opposite in the other: there is
  // no level to push down. `--allowedTools` is decided when the `claude` child
  // process starts, so what is ticked here is what the *next* launch gets, and
  // `AvatarOptions::tools_in_force` is what this one got. The section draws
  // both whenever they disagree.
  //
  // Seeded from `Config::tools` before the first frame, so the defaults in
  // ToolPolicy's constructor are never the second spelling of anything.
  ToolPolicy tools;
  // M3.11. Which model the picker holds, as an index into the table in
  // `core/model_choice.h`. Same contract as `tools` — the panel is the owner
  // of record, main.cpp mirrors it into settings.json, and there is no level
  // to push down, because `--model` is decided when the `claude` child starts.
  // `AvatarOptions::model_in_force` is what this run got.
  //
  // Seeded by main.cpp before the first frame from `Config::model_override`,
  // so the default is not spelled twice.
  int model = kModelChoiceDefault;
  // M1f.3. The latched microphone shut itself on silence, and nothing has
  // happened since. What the microphone button draws while this is true is a
  // face of its own (MicFace::Dozed) rather than the bare Idle capsule, which
  // is the difference between the button saying "it gave up on you" and the
  // button saying nothing at all.
  //
  // **A level here because the session only offers an edge.**
  // `Snapshot::listen_timeout_seq` is bumped on exactly one frame, and the
  // panel has to keep drawing the consequence for as long as the user is away
  // — which is the entire point of a reaction aimed at somebody who is not at
  // the desk. So the edge is converted here, once, and `listen_timeout_seq`
  // below is the counter it is converted against.
  //
  // **Not persisted**, unlike the two fields above it. It describes a moment
  // in this run; an app that came back up claiming it had just dozed off would
  // be reporting something that never happened.
  bool mic_dozed = false;
  unsigned listen_timeout_seq = 0;
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
  // M1f.2's pose flag, in the spirit of --clip and --message: keep the
  // settings surface scrolled to the Timing section. The surface is a fixed
  // height that scrolls, and Timing is below the fold — so "a screenshot of
  // the auto-listen control" was a state nothing could ask for, which in this
  // project is the documented way a widget goes unlooked-at (see --message).
  // Harness only (`--settings timing`); nothing in the UI sets it.
  bool settings_scroll_timing = false;
  // M3.8's, for the same reason: Tools sits below the fold of a surface that
  // scrolls, so without this there is no state in which it can be
  // screenshotted. Harness only (`--settings-tools`).
  bool settings_scroll_tools = false;
  // M1f.5's, and the same again: Startup sits below the fold too.
  bool settings_scroll_startup = false;
  // M3.11's, for the same reason again: Model sits just above Tools, below the
  // fold. Harness only (`--settings-model`).
  bool settings_scroll_model = false;
};

// M1f.2. What the two fields above mean as one number, in the one spelling the
// rest of the feature uses: seconds, and 0 for never — which is what
// `Config::listen_timeout` holds, what `VoiceSession::set_listen_timeout()`
// takes and what settings.json stores. Written once, here, so the level pushed
// into the session and the value written to the file cannot come to differ.
inline float listen_timeout_seconds(const AvatarUiState& state) {
  return state.listen_timeout_on ? static_cast<float>(state.listen_timeout_sec) : 0.0f;
}

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
