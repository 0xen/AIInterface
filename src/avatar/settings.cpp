#include "settings.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "avatar_ui.h"
#include "core/app_bus.h"
#include "core/config.h"
#include "core/model_choice.h"
#include "core/user_paths.h"

namespace aii {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// How long a value has to sit unchanged before it is written. Long enough
// that cycling the avatar button through its three modes to reach the third
// is one write rather than three, short enough that the file is on disk well
// before a user who just clicked something reaches for the close button —
// and flush() on the way out covers them if they do not.
constexpr float kDebounceSeconds = 1.0f;

// nlohmann's const operator[] asserts on a missing key, and a missing key is
// the ordinary case here (it is what "this field has never been set" looks
// like), so every lookup goes through this and every check is a type test.
const json& member(const json& j, const char* key) {
  static const json kNull;
  if (!j.is_object()) return kNull;
  const auto it = j.find(key);
  return it == j.end() ? kNull : *it;
}

}  // namespace

fs::path settings_file_path() {
  if (const std::string override = env_or("AII_SETTINGS_FILE", ""); !override.empty())
    return fs::path(override);
  // Per-user roaming data, beside avatars/ and prompts/ — one root, named in
  // `core/user_paths.h`, so nothing here can drift from where the rest writes.
  return user_data_root() / "settings.json";
}

void Settings::load(fs::path path) {
  path_ = std::move(path);
  root_ = json::object();
  dirty_ = false;
  since_change_ = 0.0f;

  std::ifstream in(path_, std::ios::binary);
  // Not an error and not worth a line: every first run is this, and so is
  // every run after the user deletes the file to start again.
  if (!in) return;
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  // A UTF-8 BOM is invisible in an editor but is not JSON; Notepad adds one.
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);
  }

  // allow_exceptions=false: a file truncated by a crash or a power cut is
  // exactly what this has to survive, so a parse failure is a return value
  // rather than something thrown out of startup.
  json parsed = json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    // Left as the empty object, so every get_* below answers with its default
    // and the next save rewrites the file with something valid in it. The
    // unusable content is not preserved: there is nothing in it to preserve.
    note("settings unusable, starting from defaults - " + path_.string() + ": not valid JSON",
         false);
    return;
  }
  root_ = std::move(parsed);
}

bool Settings::get_bool(const char* section, const char* key, bool def) const {
  const json& v = member(member(root_, section), key);
  return v.is_boolean() ? v.get<bool>() : def;
}

float Settings::get_float(const char* section, const char* key, float def) const {
  const json& v = member(member(root_, section), key);
  // is_number(), not is_number_float(): `60` and `60.0` are the same setting,
  // and a file a person has edited by hand will have whichever one they typed.
  // JSON has no NaN or infinity to defend against — nlohmann refuses to parse
  // either — so a number here is always a usable one.
  return v.is_number() ? v.get<float>() : def;
}

int Settings::get_enum(const char* section, const char* key, const char* const* names, int count,
                       int def) const {
  const json& v = member(member(root_, section), key);
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    for (int i = 0; i < count; ++i) {
      if (s == names[i]) return i;
    }
    return def;
  }
  if (v.is_number_integer()) {
    const auto i = v.get<long long>();
    // Range-checked rather than cast: a stale or fat-fingered `7` would
    // otherwise become an enum value with no case, which is a broken window
    // rather than a wrong setting.
    if (i >= 0 && i < count) return static_cast<int>(i);
  }
  return def;
}

std::string Settings::get_string(const char* section, const char* key,
                                 const std::string& def) const {
  const json& v = member(member(root_, section), key);
  // Non-empty, because every string this file stores is a *name* — an avatar
  // directory, a theme, later a voice — and an empty name is never a thing
  // that exists. A hand edit that blanks one therefore means "the default"
  // rather than "look for the avatar called nothing".
  if (!v.is_string()) return def;
  std::string s = v.get<std::string>();
  return s.empty() ? def : s;
}

void Settings::set_string(const char* section, const char* key, const std::string& value) {
  // Unlike set_bool there is a value that is not ours to write: a name the
  // caller could not resolve. Writing it would persist a typo or a theme that
  // has since been deleted, and the next run would show the fallback while the
  // file went on claiming otherwise.
  if (value.empty()) return;
  const json& cur = member(member(root_, section), key);
  if (cur.is_string() && cur.get<std::string>() == value) return;
  if (!root_[section].is_object()) root_[section] = json::object();
  root_[section][key] = value;
  dirty_ = true;
  since_change_ = 0.0f;
}

void Settings::set_bool(const char* section, const char* key, bool value) {
  const json& cur = member(member(root_, section), key);
  if (cur.is_boolean() && cur.get<bool>() == value) return;
  if (!root_[section].is_object()) root_[section] = json::object();
  root_[section][key] = value;
  dirty_ = true;
  since_change_ = 0.0f;
}

void Settings::set_float(const char* section, const char* key, float value) {
  const json& cur = member(member(root_, section), key);
  // Compared through float, not double: the stored value is round-tripped
  // through this type, so comparing the double nlohmann hands back against a
  // float would report a change on every frame for any value that is not
  // exactly representable — which is a settings write per frame, the one thing
  // the no-op contract exists to prevent.
  if (cur.is_number() && cur.get<float>() == value) return;
  if (!root_[section].is_object()) root_[section] = json::object();
  root_[section][key] = value;
  dirty_ = true;
  since_change_ = 0.0f;
}

void Settings::set_enum(const char* section, const char* key, const char* const* names, int count,
                        int value) {
  if (value < 0 || value >= count) return;  // not ours to write; leave the file alone
  const json& cur = member(member(root_, section), key);
  if (cur.is_string() && cur.get<std::string>() == names[value]) return;
  if (!root_[section].is_object()) root_[section] = json::object();
  root_[section][key] = names[value];
  dirty_ = true;
  since_change_ = 0.0f;
}

void Settings::tick(float dt) {
  if (!dirty_) return;
  since_change_ += dt;
  if (since_change_ >= kDebounceSeconds) save();
}

void Settings::flush() {
  if (dirty_) save();
}

bool Settings::take_status_change() {
  const bool was = status_new_;
  status_new_ = false;
  return was;
}

void Settings::note(std::string line, bool ok) {
  status_ = std::move(line);
  status_ok_ = ok;
  status_new_ = true;
}

void Settings::save() {
  // Clear first either way: a failed save that keeps trying every second
  // would turn one unwritable directory into a log full of the same line.
  dirty_ = false;
  since_change_ = 0.0f;
  if (path_.empty()) return;

  if (!member(root_, "version").is_number_integer()) root_["version"] = 1;

  std::error_code ec;
  if (path_.has_parent_path()) fs::create_directories(path_.parent_path(), ec);

  // Written beside the target, not in %TEMP%: rename is only atomic within a
  // volume, and the whole point of this is that a half-written file can never
  // be what startup reads. The reader either sees the old file or the new one.
  fs::path tmp = path_;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      note("settings not saved - cannot write " + tmp.string(), false);
      return;
    }
    // Indented: this file is meant to be opened and edited by hand, the same
    // way the avatar definitions are.
    out << root_.dump(2) << '\n';
    out.flush();
    if (!out) {
      out.close();
      fs::remove(tmp, ec);
      note("settings not saved - write failed for " + tmp.string(), false);
      return;
    }
  }

  fs::rename(tmp, path_, ec);
  if (ec) {
    // fs::rename replaces an existing file on Windows, so a failure here is
    // something else holding it open (an editor, a virus scanner). One retry
    // through remove costs nothing and loses only the atomicity we cannot
    // have in that case anyway.
    std::error_code ec2;
    fs::remove(path_, ec2);
    fs::rename(tmp, path_, ec);
  }
  if (ec) {
    const std::string why = ec.message();
    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    note("settings not saved - " + path_.string() + ": " + why, false);
  }
}

// ----------------------------------------------------------------- M3.14
//
// The key table. Read the header for why this is a translation table and not
// a file writer; read `kSettingKeys` itself for the answer to "does this key
// cost anything", which is the one question the spoken warning turns on.
//
// **Every key `settings.json` defines is here, including the ones nothing can
// change.** The user was offered a curated subset and chose the whole file,
// and a row with `NotSettable` on it honours that better than an omission
// does: the AI can name the key, read its value and say exactly why it will
// not write it, which is more than a shorter table could do. What is *not*
// here is a key this format does not define -- see `setting_key()`.

namespace {

const SettingKey kSettingKeys[] = {
    // -- levels the frame loop pushes down. Nothing to warn about, so nothing
    //    is said: `Msg::Count` is the table's spelling of silence.
    {"panel.chat_open", SettingValue::Bool, SettingCost::Live, Msg::Count, "settings.chat",
     "on or off", "off"},
    {"panel.muted", SettingValue::Bool, SettingCost::Live, Msg::Count, "session.mute", "on or off",
     "off"},
    {"panel.avatar_mode", SettingValue::AvatarMode, SettingCost::Live, Msg::Count,
     "settings.avatar_mode", "always, speaking or hidden", "always"},
    {"language.enabled", SettingValue::Languages, SettingCost::Live, Msg::Count,
     "settings.language", "en, ja or en,ja", "en,ja"},
    {"timing.listen_timeout", SettingValue::Seconds, SettingCost::Live, Msg::Count,
     "settings.listen_timeout", "15 to 600 seconds, or 0 for never", "60"},
    {"avatar.name", SettingValue::Free, SettingCost::Live, Msg::Count, "avatar.load",
     "the name of an avatar folder", "(whatever is installed)"},
    {"avatar.theme", SettingValue::Free, SettingCost::Live, Msg::Count, "theme.set",
     "a theme the current avatar declares", "(the avatar's own)"},
    {"avatar.colour", SettingValue::Colour, SettingCost::Live, Msg::Count, "theme.colour",
     "#rrggbb", "(none)"},

    // -- stored now, read at startup. Real, saved, and not in force this run.
    {"startup.auto_listen", SettingValue::Bool, SettingCost::NextLaunch, Msg::SettingNextLaunch,
     "settings.auto_listen", "on or off", "on"},

    // -- read when the `claude` child is created. Since M3.12 these replace
    //    that child at once, so they are the only rows that ask first.
    {"model.name", SettingValue::ModelKey, SettingCost::Restart, Msg::SettingRestartModel,
     "settings.model", "default, opus, sonnet or haiku", "default"},
    {"tools.web", SettingValue::Bool, SettingCost::Restart, Msg::SettingRestartTools,
     "settings.tools", "on or off", "on"},
    {"tools.file_read", SettingValue::Bool, SettingCost::Restart, Msg::SettingRestartTools,
     "settings.tools", "on or off", "on"},
    {"tools.file_write", SettingValue::Bool, SettingCost::Restart, Msg::SettingRestartTools,
     "settings.tools", "on or off", "off"},

    // -- in the file, and not this app's to write. Each says its own why.
    {"inspector.placed", SettingValue::Opaque, SettingCost::NotSettable, Msg::SettingWindowOwns, "",
     "written by the inspector window", "false"},
    {"inspector.x", SettingValue::Opaque, SettingCost::NotSettable, Msg::SettingWindowOwns, "",
     "written by the inspector window", "(unplaced)"},
    {"inspector.y", SettingValue::Opaque, SettingCost::NotSettable, Msg::SettingWindowOwns, "",
     "written by the inspector window", "(unplaced)"},
    {"inspector.w", SettingValue::Opaque, SettingCost::NotSettable, Msg::SettingWindowOwns, "",
     "written by the inspector window", "(unplaced)"},
    {"inspector.h", SettingValue::Opaque, SettingCost::NotSettable, Msg::SettingWindowOwns, "",
     "written by the inspector window", "(unplaced)"},
    // Read once, before there is a window to ask, and never written back. The
    // file is genuinely the master for this one key -- which is why the
    // sentence for it points the user at the file rather than apologising.
    {"window.dodge_watermark", SettingValue::Opaque, SettingCost::NotSettable,
     Msg::SettingStartupOnly, "", "off, activated or always", "activated"},
    {"version", SettingValue::Opaque, SettingCost::NotSettable, Msg::SettingFormatField, "",
     "the file format's own number", "1"},
};

constexpr int kSettingKeyCount = static_cast<int>(sizeof(kSettingKeys) / sizeof(kSettingKeys[0]));

// "model.name" -> ("model", "name"). An empty section when there is no dot,
// which is `version` and is why the split is tolerant rather than a parse.
std::pair<std::string, std::string> split_key(const std::string& dotted) {
  const size_t dot = dotted.find('.');
  if (dot == std::string::npos) return {std::string(), dotted};
  return {dotted.substr(0, dot), dotted.substr(dot + 1)};
}

bool read_bool(const std::string& v, bool* out) {
  if (v == "on" || v == "true" || v == "yes" || v == "1") return (*out = true), true;
  if (v == "off" || v == "false" || v == "no" || v == "0") return (*out = false), true;
  return false;
}

}  // namespace

int setting_key_count() { return kSettingKeyCount; }

const SettingKey& setting_key_at(int i) {
  if (i < 0 || i >= kSettingKeyCount) return kSettingKeys[0];
  return kSettingKeys[i];
}

const SettingKey* setting_key(const std::string& dotted) {
  // **Exact, and a miss is a miss.** The user's "every key" was a decision
  // about scope -- the whole file rather than a chosen few -- and this table
  // *is* the whole file. It is not a safety whitelist narrowing that scope; it
  // is the format, and a key outside it is one the model made up. Refusing it
  // is cheaper than writing it: an inert key in the file is dead weight the
  // user meets a week later, and the only alternative on offer was to write it
  // *and* say so, which costs the same sentence and leaves the weight behind.
  for (const SettingKey& k : kSettingKeys)
    if (dotted == k.key) return &k;
  return nullptr;
}

std::string setting_bus_line(const SettingKey& k, const std::string& value, std::string* error) {
  const auto fail = [&](const char* why) {
    if (error) *error = why;
    return std::string();
  };
  if (k.cost == SettingCost::NotSettable || !k.bus || !*k.bus)
    return fail("nothing running owns that key");

  switch (k.value) {
    case SettingValue::Bool: {
      bool on = false;
      if (!read_bool(value, &on)) return fail("expected on or off");
      BusLine line(k.bus);
      // `tools` is the one row whose door takes two fields, and the second is
      // derived from the key rather than stored: `tools.file_read` is group
      // `file_read`. Derived, so a fourth tool group is a row here and
      // nothing else, exactly as `tool_policy.h` promises for the panel.
      if (std::string(k.bus) == "settings.tools") line.str("group", split_key(k.key).second);
      return line.flag("on", on).done();
    }
    case SettingValue::Seconds: {
      // Parsed here rather than trusted, and range-checked against the same
      // numbers the DragInt clamps to, so a refusal is a sentence the user
      // hears instead of a log line they do not.
      char* end = nullptr;
      const double s = std::strtod(value.c_str(), &end);
      if (value.empty() || (end && *end != '\0')) return fail("expected a number of seconds");
      if (s < 0.0) return fail("seconds cannot be negative");
      if (s > 0.0 && (s < 15.0 || s > 600.0)) return fail("15 to 600 seconds, or 0 for never");
      return BusLine(k.bus).num("seconds", s, 0).done();
    }
    case SettingValue::ModelKey: {
      // By the settings.json key, and an unknown one is refused before it is
      // written. This is what makes the picker's whole argument hold for the
      // voice path too: a `--model` the CLI rejects is a child that starts
      // and then fails every turn, so the user would hear the conversation
      // thrown away and then hear nothing work.
      if (model_choice_for_key(value) < 0) return fail("no such model");
      return BusLine(k.bus).str("name", value).done();
    }
    case SettingValue::AvatarMode: {
      for (int i = 0; i < kAvatarVisibilityCount; ++i)
        if (value == kAvatarVisibilityNames[i]) return BusLine(k.bus).str("value", value).done();
      return fail("no such avatar mode");
    }
    case SettingValue::Languages: {
      // The file's own spelling, one string rather than two flags, for the
      // reason the header gives: a pair of booleans has a spelling for
      // "neither" and this does not.
      const bool en = value == "en" || value == "en,ja" || value == "ja,en";
      const bool ja = value == "ja" || value == "en,ja" || value == "ja,en";
      if (!en && !ja) return fail("expected en, ja or en,ja");
      return BusLine(k.bus).flag("english", en).flag("japanese", ja).done();
    }
    case SettingValue::Colour: {
      if (value.size() != 7 || value[0] != '#') return fail("expected #rrggbb");
      return BusLine(k.bus).str("value", value).done();
    }
    case SettingValue::Free:
      // An avatar folder or a theme name. **Only the art knows**, so this is
      // the one shape passed through unvalidated and refused at the bus door
      // instead. Written down because it is the one case where a refusal
      // reaches the log and not the user.
      if (value.empty()) return fail("expected a name");
      return BusLine(k.bus).str("name", value).done();
    case SettingValue::Opaque:
      return fail("nothing running owns that key");
  }
  return fail("unknown value shape");
}

std::string Settings::value_text(const char* section, const char* key) const {
  const json& j = section && *section ? member(member(root_, section), key) : member(root_, key);
  if (j.is_null()) return std::string();
  if (j.is_string()) return j.get<std::string>();
  // dump() gives JSON's own spelling of a number or a bool, which is the
  // spelling the file has and therefore the one the user would read back.
  return j.dump();
}

std::string settings_digest(const Settings& s) {
  std::string out;
  for (const SettingKey& k : kSettingKeys) {
    const std::pair<std::string, std::string> parts = split_key(k.key);
    const std::string have = s.value_text(parts.first.c_str(), parts.second.c_str());
    out += k.key;
    out += " = ";
    // A value the file does not name is shown as the default *and said to
    // be*, because "60" and "60 by default, nobody has ever set it" are
    // different facts, and the second is the one that explains why a hand
    // edit did not stick.
    out += have.empty() ? std::string(k.def) + " (default)" : have;
    out += "  -- ";
    out += k.shape;
    switch (k.cost) {
      case SettingCost::Live: out += "; takes effect at once"; break;
      case SettingCost::NextLaunch: out += "; takes effect at the next launch"; break;
      case SettingCost::Restart: out += "; RESTARTS the conversation"; break;
      case SettingCost::NotSettable: out += "; cannot be changed while running"; break;
    }
    out += "\n";
  }
  return out;
}

}  // namespace aii
