#pragma once
// The app's settings file (M1b.5): the bits of user state that have to come
// back the way they were left.
//
// It is a JSON file in %APPDATA%\AIInterface\, beside the `avatars/`
// directory the definitions already live in — the same root, for the same
// reason: it is the user's data, it has to survive a rebuild, and it is meant
// to be readable and hand-editable exactly as `avatar.json` is. It is
// deliberately *not* ImGui's `imgui.ini`: `io.IniFilename` is null on purpose
// (imgui_layer.cpp, "a corner widget has no layout worth persisting") and
// these fields live in `AvatarUiState`, not in any ImGui window.
//
//   {
//     "version": 1,
//     "panel": { "chat_open": false, "avatar_mode": "always" },
//     "language": { "enabled": "en,ja" },
//     "startup": { "auto_listen": true },
//     "timing": { "listen_timeout": 60.0 },
//     "wake": { "phrase": "" },
//     "tools": { "web": true, "file_read": true, "file_write": false },
//     "model": { "name": "default" },
//     "inspector": { "placed": true, "x": 1180, "y": 420, "w": 1100, "h": 700 }
//   }
//
// `inspector` (M5.1) is the prompt inspector window's geometry: `x`/`y` are its
// **window** rect's top-left and `w`/`h` its **client** size, which is the pair
// the window is actually created at — mixing the two is how a remembered window
// walks down the screen by one title bar per restart. `placed` is a flag rather
// than a sentinel coordinate because 0,0 is a real position and a second
// monitor to the left has real negative ones; false means "never placed" and
// the window centres itself. Nothing here is trusted as still legal: a rect
// that no longer lands on any monitor is re-fitted when the window is made.
//
// `timing.listen_timeout` (M1f.2) is seconds, and **0 means never** — the same
// spelling the mechanism already uses (`Config::listen_timeout`,
// `VoiceSession::set_listen_timeout`), so there is one way to say "never" in
// the whole feature rather than a number and a flag that can contradict
// each other.
//
// `wake.phrase` (M12.2) is the word or short phrase that opens full listening
// when it is heard. **An empty string means off, and off is the default** —
// deliberately the same spelling `timing.listen_timeout` uses for "never", so
// the file has one way of saying a feature is switched off rather than a value
// and a separate flag that can disagree with each other. A phrase shorter than
// `kWakeMinChars` characters is also off, and the panel says why rather than
// arming something that would fire inside ordinary words.
//
// It is its own section rather than a key under `timing` or `startup` because
// what it switches on is not a timing and not a startup choice: it is a
// microphone that stays open, decoding locally, for as long as the app is idle
// and unlatched. The user chose that over matching only while the mic was
// already open (20 Sep 2026), on the understanding that nothing reaches Claude
// until the phrase matches — and the window has a seventh microphone face that
// says the microphone is open whenever it is. See `core/wake_word.h` for the
// matching rule and which way it errs.
//
// `startup.auto_listen` (M1f.5) is whether the app latches its own microphone
// on once the engines are up. **A missing key means true**, which is the whole
// of the user's "by default it should be on": a fresh install with no file at
// all and a file written before this key existed both come up listening. Like
// `tools` it is read only at startup — the thing it decides happens once, a
// second after launch — and the settings surface says which state the running
// app is actually in rather than letting the box look inert.
//
// `tools` (M3.8) is one boolean per *group* of Claude Code tools, keyed by the
// table in `core/tool_policy.h` — not one per tool, of which there are
// twenty-eight. A missing key is that group's own default, so a file written
// before this section existed reads as the shipped policy. Unlike everything
// else in this file these are only read at startup: they become
// `--allowedTools` on the `claude` child's command line, which is fixed when
// that process starts, so a change here reaches Claude at the next launch and
// the settings surface says so rather than appearing to do nothing.
//
// `model.name` (M3.11) is a *key* from the table in `core/model_choice.h` —
// "default", "opus", "sonnet", "haiku" — and not a model string. A key survives
// the alias behind it moving on, and a key this build does not recognise (a
// hand edit, or an entry a newer version added and this one has not) falls
// back to "default" with a line in the log, because a model string the CLI
// rejects is a `claude` child that starts and then fails every turn. "default"
// means no `--model` flag at all. Read only at startup, like `tools` and for
// the same reason.
//
// `language.enabled` (M8.3) is one string — "en", "ja" or "en,ja" — and not
// two booleans, because at least one language must always be on and a pair of
// booleans has a spelling for "neither". A value naming nothing known is read
// as the default rather than obeyed.
//
// Two fields today, but this is the app's settings file rather than a cache
// for those two: voices, endpoint timing, the chosen avatar and its theme and
// the window position are all named in the milestones as coming here. So the
// shape is a store, not a struct. Three consequences, all deliberate:
//
//  - Adding a field later is one more get/set call with its own default and
//    no migration, because a key that is absent simply means "the default" —
//    which is the same code path as the whole file being absent on first run.
//  - The parsed document is *kept*, and a save rewrites only the keys it owns.
//    A key written by a future version therefore survives being read and saved
//    by an older one, instead of being deleted by it. That costs one member.
//  - Values are grouped under a named section, so a later "voice" or "window"
//    group cannot collide with this one.
//
// `version` is written for a human reading the file and for a future format
// break that actually needs one; nothing reads it today, because per-key
// defaults make the ordinary case of new keys a non-event.
//
// ----------------------------------------------------------------- M3.14
//
// **This file is a mirror, not a master, and everything M3.14 does follows
// from that.** Read `main.cpp`'s frame loop: `panel`, `language`, `timing`,
// `startup`, `tools`, `model` and `avatar` are all written out of
// `AvatarUiState` *every frame*. A value written straight into
// `settings.json` by anything else is therefore overwritten within about
// sixteen milliseconds, silently, and the write looks like it worked.
//
// So "the AI can modify its own settings file" cannot be built as a file
// writer. It is built as a translator: from the key vocabulary of this file,
// which is what the user reads and what they asked the AI to be aware of, to
// the control that owns each key -- and the control writes the file, exactly
// as it did before any of this existed. There is one writer of
// `settings.json` and M3.14 does not add a second.
//
// `kSettingKeys` below is that translation, and it is also the machine-readable
// form of the doc comment above: one row per key this format defines, its
// value's shape, what changing it costs, and the sentence the app says about
// that cost. Adding a key to the format means a row here, or the AI will say
// there is no such setting -- which is the intended failure, not an oversight.
#include <filesystem>
#include <string>

#include "core/app_strings.h"
#include "json.hpp"

namespace aii {

// What a key costs to change, which is the whole of what the warning has to
// get right. **Per key, never per change**: three of these four classes cost
// the user nothing, and a "this needs a restart" spoken over one of them is
// the failure this enum exists to make unreachable.
enum class SettingCost {
  // A level the frame loop pushes down. It is true the moment it is set and
  // the app says nothing, because there is nothing to warn about.
  Live,
  // Stored now, read at startup. Nothing breaks and nothing is lost; the
  // thing it decides simply already happened this run.
  NextLaunch,
  // Read only when the `claude` child is created, so since M3.12 setting it
  // replaces that child **now** and the conversation goes with it. This is
  // the only class that asks before it acts.
  Restart,
  // In the file and not this app's to write. Not the same as "unknown": the
  // key is real, the user can see it, and the app can say exactly why it will
  // not touch it.
  NotSettable,
};

// The shape of a key's value, which is what turns "haiku" into a bus message
// and what refuses "banana" before anything is written.
enum class SettingValue {
  Bool,        // on / off / true / false / 1 / 0
  Seconds,     // a number; 0 is never, as everywhere else in this file
  ModelKey,    // a key from core/model_choice.h
  AvatarMode,  // a name from kAvatarVisibilityNames
  Languages,   // "en", "ja" or "en,ja"
  Colour,      // #rrggbb
  // M12.2. A short phrase the user chose, in either language, and the one
  // value in this file whose *content* is arbitrary text rather than a name
  // from a set. Separate from `Free` because `Free` means "only the art can
  // say whether this is legal, so pass it through"; this one can be checked
  // here and is -- `wake_phrase_problem()` refuses anything too short to be
  // safe to listen for, and an empty string is legal and means off.
  Phrase,
  Free,        // a name only the art can validate (an avatar, a theme)
  Opaque,      // NotSettable rows, which never parse a value at all
};

// One key of `settings.json`, as the AI is allowed to see it.
struct SettingKey {
  // `section.key`, spelled exactly as the file spells it. The dotted form is
  // deliberate: it is how a person reads the file out loud, so the AI's
  // vocabulary and the user's are the same words.
  const char* key;
  SettingValue value;
  SettingCost cost;
  // What the app says. `Msg::Count` means "nothing to say", which is the
  // right and only answer for a `Live` key. For `Restart` it is the question
  // that must be answered before the change happens; for `NextLaunch` it is
  // said after; for `NotSettable` it is said instead.
  Msg say;
  // The bus topic that owns this key -- the same door a Python script uses
  // (`avatar/bus_bindings.h`), which is the same door the panel's own control
  // writes through. Empty for `NotSettable`. Nothing here is a second
  // implementation of anything, so nothing here can drift from the button.
  const char* bus;
  // What a legal value looks like, and the shipped default, both for the
  // digest the system prompt carries. Prose about *values* rather than prose
  // about behaviour, which is why it can live in a table: see settings.md for
  // the half a person is meant to edit.
  const char* shape;
  const char* def;
};

int setting_key_count();
const SettingKey& setting_key_at(int i);
// The row for a dotted key, or null -- which is the AI having invented one.
const SettingKey* setting_key(const std::string& dotted);

// The bus line that makes this change, or an empty string with `*error` set.
//
// Validation happens here, before anything is posted, because the bus
// handler's own refusal goes to the log and not to the user: a value refused
// two hops away is a change the user believes they made. What cannot be
// checked here is checked there -- a theme or avatar name is only knowable to
// the art, so `Free` is passed through and the bus is the one that says no.
std::string setting_bus_line(const SettingKey& k, const std::string& value, std::string* error);

class Settings;  // the digest is declared under it, where it can be read

// %APPDATA%\AIInterface\settings.json, or AII_SETTINGS_FILE if that is set —
// the same escape hatch AII_AVATAR_DIR gives the avatar loader, and for the
// same reason: the failure paths have to be testable without writing over the
// settings the user is actually running with.
std::filesystem::path settings_file_path();

// Reads the file once, hands out typed values, and writes changes back.
//
// Nothing here throws and nothing fails hard. The contract is the avatar
// loader's: a missing file is the ordinary first-run case and silently means
// defaults, while a file that is there but unusable falls back to defaults
// with one line saying why, through the same status()/take_status_change()
// pair AvatarSource uses so main.cpp logs it the same way.
class Settings {
 public:
  // Never fails. After this the store is usable whatever was on disk.
  void load(std::filesystem::path path);

  // A key that is missing, of the wrong type, or out of range yields `def` —
  // per key, never per file. A partly-sane file keeps the fields that are
  // sane, because the alternative is one bad hand edit silently resetting
  // everything else the user had set.
  bool get_bool(const char* section, const char* key, bool def) const;
  // M1f.2. A number, for the settings that are a quantity rather than a state
  // or a name — the auto-listen timeout is the first, and the milestones name
  // endpoint timing as the next. Stored as JSON's own number, so the file
  // stays hand-editable: `"listen_timeout": 60.0` is what a person would
  // write, and a hand-typed `60` is accepted as the same value.
  //
  // Deliberately *not* range-checked here, unlike get_enum. An enum has a set
  // of legal values this class can see; a quantity's legal range belongs to
  // the control that owns it — M1f.2's floor is a fact about microphones, not
  // about the file format — and a clamp in two places is a clamp that can
  // disagree with itself.
  float get_float(const char* section, const char* key, float def) const;
  // An enum stored by name. `names` is the spelling of each value in
  // declaration order; the names are the on-disk format and must stay stable,
  // since they are what a user editing the file by hand reads and writes.
  // An integer is accepted too, so a file written by hand as `2` still works,
  // but only in range: `7` is not a mode and falls back to `def`.
  int get_enum(const char* section, const char* key, const char* const* names, int count,
               int def) const;
  // A free-form name, for the settings whose vocabulary is not a fixed enum
  // the program can list at compile time: which avatar definition is shown and
  // which of *its* themes (M1c.3/M1c.4). Those live in the art, so the set of
  // legal values changes when the user adds a directory, and validation
  // belongs where the art is read rather than here. This stores the name and
  // nothing else; the loader is what decides whether it still means anything.
  std::string get_string(const char* section, const char* key, const std::string& def) const;
  // M12.2. The same thing for a string **whose empty value is meaningful**,
  // which `get_string`/`set_string` above deliberately cannot store.
  //
  // That pair is for *names* — an avatar directory, a theme — and both ends of
  // it treat "" as "not set": the getter answers with the default and the
  // setter refuses to write, because there is no avatar called nothing and
  // persisting one would mean a file that claims a setting the app is not
  // honouring. Every word of that reasoning is right for a name and wrong for
  // `wake.phrase`, where "" is the shipped default, is what the feature being
  // off looks like, and is a value the user must be able to get back to by
  // clearing the box. Routed through `get_string` the box could be typed into
  // and never emptied again.
  //
  // So this is a second pair rather than a relaxation of the first: an empty
  // name still cannot be written, and an empty phrase still can.
  std::string get_text(const char* section, const char* key, const std::string& def) const;
  void set_text(const char* section, const char* key, const std::string& value);

  // Both are no-ops when the value already matches what is stored, so the
  // frame loop can call them unconditionally every frame and "on change" is
  // decided here rather than duplicated into every button that writes one.
  void set_bool(const char* section, const char* key, bool value);
  // M1f.2. Same contract as set_bool, no-op-when-unchanged included: the frame
  // loop mirrors the value out of the panel every frame, and this is what
  // decides that a control merely being looked at is not a write.
  void set_float(const char* section, const char* key, float value);
  void set_enum(const char* section, const char* key, const char* const* names, int count,
                int value);
  // An empty value is ignored rather than written: see the note in the .cpp.
  void set_string(const char* section, const char* key, const std::string& value);

  // Advances the debounce. A settings file is not worth a write per frame,
  // and the avatar mode button in particular is cycled through three states
  // in as many clicks on the way to the one the user wants.
  void tick(float dt);
  // Writes now if there is anything to write. Called on the way out, so a
  // change made inside the debounce window is not lost by quitting quickly.
  void flush();

  // One line, empty when there is nothing to say (no file, or a clean load).
  const std::string& status() const { return status_; }
  bool status_ok() const { return status_ok_; }
  // True once per new status, so the frame loop can log a change without
  // repeating it — same shape as AvatarSource::take_status_change().
  bool take_status_change();

  // M20.1. True while the last write failed and the change is still owed to
  // the file. The settings surface draws its status amber on this rather than
  // on `!status_ok()` alone, because a *recovered* save is also a status the
  // user should see and it is not a warning.
  bool save_failed() const { return !status_ok_ && dirty_; }

  // M20.3. True once per save that actually reached the disk. The frame loop
  // recomposes the settings digest on it, the same way it recomposes the
  // actions digest when `ActionStore::tick` reports a change — so a restart
  // hands the new child the values the file has now and not the ones it had
  // at launch.
  bool take_saved();

  // M3.14. What is actually on disk under `section`/`key`, rendered the way
  // the file spells it, or an empty string when the file does not name it.
  //
  // Untyped on purpose, and it is the one accessor here that is. Every other
  // getter answers "what should this setting be", which is a question with a
  // default; this one answers "what does the file say", which is a question
  // with an absence -- and the digest has to be able to tell a value that was
  // written from one that was never there. It is also how a key this build
  // has never heard of can still be *seen*, which is what stops an invented
  // key being invisible.
  std::string value_text(const char* section, const char* key) const;

 private:
  void note(std::string line, bool ok);
  // M20.1. note() plus "and it is still owed": keeps `dirty_` and advances
  // the retry backoff.
  void fail(std::string line);
  void save();

  std::filesystem::path path_;
  // The whole document, not just our keys: this is what preserves fields a
  // future version wrote and this one knows nothing about.
  nlohmann::json root_ = nlohmann::json::object();
  bool dirty_ = false;
  float since_change_ = 0.0f;
  // M20.1. Zero while nothing has failed, which is also how `tick()` tells
  // the debounce from the backoff.
  float retry_wait_ = 0.0f;
  // M20.3. Set by a save that reached the disk, cleared by take_saved().
  bool saved_ = false;
  std::string status_;
  bool status_ok_ = false;
  bool status_new_ = false;
};

// M3.14. Every key, its value as this file has it, and what changing it costs
// -- substituted into `system/settings.md` at launch, which is the "aware of
// the settings file" half of the milestone and the half the user put first.
//
// It is *data*, which is why it is generated. The paragraph around it stays in
// the Markdown where the user can edit it, for the reason `prompt_store.h`
// gives about never lifting prose into string literals. A table of current
// values is not prose and could not be written in the Markdown at all: it is
// different on every machine and on every launch.
std::string settings_digest(const Settings& s);

}  // namespace aii
