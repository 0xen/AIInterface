#include "core/user_paths.h"

#include <windows.h>

#include <iterator>
#include <system_error>

#include "core/config.h"

namespace fs = std::filesystem;

namespace aii {

fs::path exe_dir() {
  wchar_t buf[MAX_PATH * 4] = {};
  const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  if (n == 0 || n >= std::size(buf)) return fs::current_path();
  return fs::path(buf, buf + n).parent_path();
}

fs::path user_data_root() {
  const std::string appdata = env_or("APPDATA", "");
  if (appdata.empty()) return fs::path("AIInterface");
  return fs::path(appdata) / "AIInterface";
}

fs::path memories_path() { return user_data_root() / "memories.md"; }

bool seed_tree(const fs::path& source, const fs::path& dest, std::string* error) {
  std::error_code ec;
  if (!fs::exists(source, ec)) return true;  // the caller decides what that means
  fs::create_directories(dest.parent_path(), ec);
  fs::copy(source, dest, fs::copy_options::recursive | fs::copy_options::update_existing, ec);
  if (ec) {
    if (error) *error = "seeding " + dest.string() + " from " + source.string() + ": " + ec.message();
    return false;
  }
  return true;
}

}  // namespace aii
