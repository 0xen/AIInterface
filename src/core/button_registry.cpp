#include "core/button_registry.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "core/text_util.h"

namespace aii {
namespace {

// Truncation is by character, not by byte: a Japanese label is three bytes a
// character, and a byte cap would cut four of them off at "twelve" while
// letting twelve English words through. Cutting mid-sequence would also leave
// invalid UTF-8 in a string that goes straight into a draw call.
std::string truncate_chars(const std::string& s, std::size_t max_chars) {
  std::size_t chars = 0;
  for (std::size_t i = 0; i < s.size();) {
    if (chars == max_chars) return s.substr(0, i);
    const unsigned char c = static_cast<unsigned char>(s[i]);
    i += c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    ++chars;
  }
  return s;
}

// An id is a key, not prose: it is matched for the replace-in-place rule and
// it is what a future verb would name to remove a button. Restricting it to
// this alphabet keeps it comparable and keeps anything surprising (spaces,
// control characters, a newline that would split one command into two) out of
// the store entirely.
bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > kButtonIdMax) return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '-' || c == '.';
  });
}

// Windows wants UTF-16 and the registry holds UTF-8. Shared by the shell call
// only, so it stays here rather than growing a header.
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

}  // namespace

ButtonRegistry& ButtonRegistry::instance() {
  static ButtonRegistry reg;
  return reg;
}

ButtonRegistry::ButtonRegistry() {
  // The built-ins are entries like any other, first in the row. They are
  // constructed here rather than in the panel so that the panel never has a
  // list of its own to keep in step with this one.
  ToolbarButton cog;
  cog.id = "settings";
  cog.glyph = ButtonGlyph::Cog;
  cog.tooltip = "Settings";
  cog.action.kind = ButtonActionKind::OpenSettings;
  cog.builtin = true;
  buttons_.push_back(std::move(cog));

  // The agent's working directory is this process's: the conversational Claude
  // instance is a child process that inherits it (ClaudeCodeClient only sets a
  // directory for background workers, which are given one explicitly). Read
  // once at startup, because it is what the agent has been looking at all
  // along and nothing in this app chdirs.
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  ToolbarButton dir;
  dir.id = "cwd";
  dir.glyph = ButtonGlyph::Folder;
  dir.action.kind = ButtonActionKind::OpenPath;
  dir.action.path = ec ? std::string() : cwd.string();
  dir.tooltip = "Open the working directory" +
                (dir.action.path.empty() ? std::string() : "\n" + dir.action.path);
  dir.builtin = true;
  buttons_.push_back(std::move(dir));
}

bool ButtonRegistry::add_path_button(const std::string& id, const std::string& label,
                                     const std::string& tooltip, const std::string& path,
                                     std::string* error) {
  // The lock is taken for the whole call, validation included. The checks are
  // a few string tests and two stat calls and nothing else contends for this
  // mutex, and holding it throughout is what lets a refusal record itself in
  // `status_` at the point it happens instead of being reconstructed after.
  std::lock_guard<std::mutex> l(mutex_);
  const auto refuse = [&](std::string why) {
    if (error) *error = why;
    status_.push_back(std::move(why));
    return false;
  };
  const std::string clean_id = trim(id);
  if (!valid_id(clean_id)) return refuse("button id '" + id + "' is not a plain short name");

  const std::string clean_label = trim(label);
  if (clean_label.empty()) return refuse("button '" + clean_id + "' has no label");

  // The path is checked now, not at the click. A button that exists but does
  // nothing when pressed is worse than one that was never added: the user has
  // no way to tell it apart from the app being broken. `is_directory` also
  // settles the file case — Explorer would happily "explore" a file into its
  // parent folder, which is not what the button says it does.
  std::error_code ec;
  const std::filesystem::path p(path);
  if (path.empty()) return refuse("button '" + clean_id + "' has no path");
  if (!std::filesystem::exists(p, ec) || ec)
    return refuse("button '" + clean_id + "': no such path: " + path);
  if (!std::filesystem::is_directory(p, ec) || ec)
    return refuse("button '" + clean_id + "': not a directory: " + path);

  ToolbarButton b;
  b.id = clean_id;
  b.glyph = ButtonGlyph::Label;
  // Truncated rather than refused: the layout is safe either way, but a label
  // the agent made slightly too long should still give the user their button,
  // and the full text is one hover away in the tooltip.
  b.label = truncate_chars(clean_label, kButtonLabelMax);
  const std::string tip = trim(tooltip).empty() ? clean_label : trim(tooltip);
  b.tooltip = truncate_chars(tip, kButtonTooltipMax) + "\n" + p.string();
  b.action.kind = ButtonActionKind::OpenPath;
  b.action.path = p.string();

  for (auto& existing : buttons_) {
    if (existing.id != b.id) continue;
    if (existing.builtin)
      return refuse("button '" + b.id + "' is a built-in and cannot be replaced");
    existing = std::move(b);  // in place: the row keeps its order across turns
    return true;
  }
  std::size_t registered = 0;
  for (const auto& e : buttons_)
    if (!e.builtin) ++registered;
  if (registered >= kButtonsMax)
    return refuse("button '" + b.id + "' refused: the bar already holds " +
                  std::to_string(kButtonsMax));
  buttons_.push_back(std::move(b));
  return true;
}

std::vector<ToolbarButton> ButtonRegistry::snapshot() const {
  std::lock_guard<std::mutex> l(mutex_);
  return buttons_;
}

void ButtonRegistry::clear_registered() {
  std::lock_guard<std::mutex> l(mutex_);
  buttons_.erase(std::remove_if(buttons_.begin(), buttons_.end(),
                                [](const ToolbarButton& b) { return !b.builtin; }),
                 buttons_.end());
}

std::vector<std::string> ButtonRegistry::take_status() {
  std::lock_guard<std::mutex> l(mutex_);
  std::vector<std::string> s;
  s.swap(status_);
  return s;
}

bool open_directory(const std::string& path, std::string* error) {
  if (path.empty()) {
    if (error) *error = "no path";
    return false;
  }
  // "explore" rather than "open": the user asked for the folder in Explorer,
  // and `open` on a directory is at the mercy of whatever has claimed the
  // Folder verb. The result is an HINSTANCE-shaped error code; <= 32 is a
  // failure, per ShellExecute's contract.
  const std::wstring wpath = widen(path);
  const HINSTANCE r = ShellExecuteW(nullptr, L"explore", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(r) > 32) return true;
  if (error) *error = "Explorer refused " + path + " (" + std::to_string(reinterpret_cast<INT_PTR>(r)) + ")";
  return false;
}

}  // namespace aii
