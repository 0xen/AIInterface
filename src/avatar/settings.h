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
//     "tools": { "web": true, "file_read": true, "file_write": false },
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
#include <filesystem>
#include <string>

#include "json.hpp"

namespace aii {

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

 private:
  void note(std::string line, bool ok);
  void save();

  std::filesystem::path path_;
  // The whole document, not just our keys: this is what preserves fields a
  // future version wrote and this one knows nothing about.
  nlohmann::json root_ = nlohmann::json::object();
  bool dirty_ = false;
  float since_change_ = 0.0f;
  std::string status_;
  bool status_ok_ = false;
  bool status_new_ = false;
};

}  // namespace aii
