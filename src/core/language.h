#pragma once
// M8.3: which languages this app is running with, and the three things that
// follow from that choice.
//
// The user turns English and Japanese on and off in the settings surface, with
// **at least one always on**. A language that is off stops both halves of the
// loop: the recogniser is pinned away from it, and Claude is told not to reply
// in it. Turning Japanese off also means VOICEVOX is never loaded (about a
// second of startup); that part lives in `engines.cpp` and `VoiceSession`.
//
// ---------------------------------------------------------------------------
// Why the instruction to Claude is a per-turn block and not the system prompt
// ---------------------------------------------------------------------------
//
// The system prompt is a launch argument: `ClaudeCodeClient` hands it to the
// child with `--system-prompt` at creation and there is no way to change it
// without restarting the child and replaying the history (M3.6, not built).
// A setting the user can flip mid-session therefore cannot live there.
//
// `PromptInjector` (prompt_store.h) already solves "text that has to reach the
// model without a restart" by prepending a `<context>` block to one user turn.
// This reuses the *shape* and deliberately not the *machinery*: the injector
// keeps a loaded set so a prompt is sent once and never again, which is the
// exact opposite of what a changeable setting needs. A language the user
// switched off in turn nine has to be off in turn ten, and there is no un-send
// for something already in the context window. So this is stateless — it reads
// the current selection and emits the block every turn while the selection is
// restricted — and holding no state is precisely what makes it impossible for
// it to disagree with the checkbox.
//
// With both languages on it emits nothing at all, so the default configuration
// is byte-identical to what the app sent before this existed.
#include <string>

namespace aii {

// At least one is always true. Nothing here enforces that — the UI does, by
// refusing to let the last one be cleared — but every consumer may assume it,
// and `from_spec()` below repairs a file or an environment variable that broke
// it rather than propagating a selection with no languages in it.
struct LanguageSelection {
  bool english = true;
  bool japanese = true;

  bool both() const { return english && japanese; }
  bool operator==(const LanguageSelection& o) const {
    return english == o.english && japanese == o.japanese;
  }
  bool operator!=(const LanguageSelection& o) const { return !(*this == o); }
};

// A comma-separated list: "en", "ja", "en,ja". Anything that names no known
// language yields the default (both on) rather than nothing on.
LanguageSelection language_selection_from_spec(const std::string& spec);
std::string language_spec(LanguageSelection sel);

// What the recogniser's `language` option should be set to.
//
// `auto` is not the safe default it looks like. Measured 16 Sep 2026
// (docs/research-multilingual-input.md): Nemotron's `auto` is inertial and
// asymmetric — it switches English to Japanese and never back, and a short
// Japanese insert inside an English sentence is *silently deleted*, byte-for-
// byte identical to the same audio with the Japanese cut out. Pinning removes
// that failure mode outright, so a single enabled language is pinned and
// `auto` is used only when the user has actually asked for both.
const char* stt_language_for(LanguageSelection sel);

// The `<context>` block prepended to a user turn, or "" when both languages
// are on. See the note above for why this is per-turn and stateless.
//
// The wording is the user's to approve (M8.3 is an open question in the plan);
// this is the proposal, and it is one string in one place for that reason.
std::string language_directive(LanguageSelection sel);

// `user_text` with the directive prepended, or `user_text` unchanged when
// there is no directive. Mirrors PromptInjector::decorate()'s contract: what
// comes back is what Claude is sent, and the caller keeps the original for the
// transcript, the avatar and the mute path.
std::string decorate_language(const std::string& user_text, LanguageSelection sel);

}  // namespace aii
