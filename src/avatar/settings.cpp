#include "settings.h"

#include <fstream>
#include <sstream>
#include <system_error>

#include "core/config.h"
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

}  // namespace aii
