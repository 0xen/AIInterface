#include "core/cwd_policy.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace aii {
namespace {

std::mutex& dir_mutex() {
  static std::mutex m;
  return m;
}

std::string& dir_storage() {
  static std::string dir;
  return dir;
}

// Splits on both separators, because a path may arrive typed by the user with
// backslashes or written by the model with forward ones.
std::vector<std::string> components(const std::string& path) {
  std::vector<std::string> out;
  std::string current;
  for (const char ch : path) {
    if (ch == '\\' || ch == '/') {
      if (!current.empty()) out.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

bool is_drive_or_dot(const std::string& component) {
  if (component == "." || component == "..") return true;
  return component.find(':') != std::string::npos;
}

}  // namespace

void set_app_dir(std::string dir) {
  std::lock_guard<std::mutex> l(dir_mutex());
  dir_storage() = std::move(dir);
}

const std::string& app_dir() {
  std::lock_guard<std::mutex> l(dir_mutex());
  std::string& dir = dir_storage();
  if (dir.empty()) {
    // Nothing called set_app_dir(): a test, or a tool that is not the app. The
    // process working directory is the same answer the app would have captured
    // at startup, so this is a fallback, not a different policy.
    std::error_code ec;
    const std::filesystem::path here = std::filesystem::current_path(ec);
    if (!ec) dir = here.string();
  }
  return dir;
}

std::string normalize_for_match(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const unsigned char ch : text) {
    // Only ASCII letters and digits survive. Non-ASCII bytes (a Japanese turn,
    // say) are dropped rather than lowercased: a Windows path component that
    // matters here is ASCII, and half-normalising UTF-8 by byte would match
    // things that are not the same word.
    if (std::isalpha(ch) || std::isdigit(ch))
      out.push_back(static_cast<char>(std::tolower(ch)));
  }
  return out;
}

bool cwd_was_named(const std::string& cwd, const std::string& evidence) {
  const std::string haystack = normalize_for_match(evidence);
  if (haystack.empty()) return false;
  int matched = 0;
  for (const std::string& raw : components(cwd)) {
    if (is_drive_or_dot(raw)) continue;
    const std::string part = normalize_for_match(raw);
    // Two characters or fewer is noise -- "src", "bin" and "lib" are already at
    // the edge of meaning anything, and a two-letter fragment matches almost
    // any sentence. Skipped rather than failed: a real folder should not be
    // rejected because one level of it is called `AI`.
    if (part.size() < 3) continue;
    if (haystack.find(part) == std::string::npos) return false;
    ++matched;
  }
  return matched > 0;
}

CwdDecision resolve_worker_cwd(const std::string& asked, const std::string& evidence) {
  CwdDecision d;
  if (asked.empty()) {
    d.dir = app_dir();
    d.why = "no cwd given, so the app's own folder";
    return d;
  }
  // The absolute check comes first, and it has to (M15.3, review finding 10).
  // The shortcut below matches on `normalize_for_match`, which strips every
  // separator and every case distinction -- that is exactly what makes it able
  // to recognise a dictated path, and it also makes `C:\github\AI\Interface`
  // and a drive-relative `C:github\AIInterface` match the app's own folder. It
  // used to hand those back verbatim, so a string that is not a directory at
  // all, or one that names a different directory, became a child's working
  // directory because it happened to normalise the same.
  if (!std::filesystem::path(asked).is_absolute()) {
    d.dir = app_dir();
    d.why = "cwd=\"" + asked + "\" is not an absolute path, so the app's own folder";
    return d;
  }
  // The app's own folder is always allowed, whoever wrote it down: it is the
  // answer this policy would have given anyway. And it is `app_dir()` that is
  // returned, not the spelling that was asked for -- the match is a loose one,
  // so the two are not the same string, and this function's job is to name a
  // directory rather than to repeat what the model wrote.
  if (normalize_for_match(asked) == normalize_for_match(app_dir())) {
    d.dir = app_dir();
    d.honoured = true;
    d.why = "cwd is the app's own folder";
    return d;
  }
  if (!cwd_was_named(asked, evidence)) {
    d.dir = app_dir();
    d.why = "cwd=\"" + asked + "\" was never named in this conversation, so the app's own folder";
    return d;
  }
  d.dir = asked;
  d.honoured = true;
  d.why = "cwd=\"" + asked + "\" was named in this conversation";
  return d;
}

}  // namespace aii
