#include "core/user_paths.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
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

namespace {

// The record of what was last seeded, kept beside the tree it describes rather
// than once per install. Three reasons it is a sidecar:
//
//   * the destinations are not all under one root. `AII_PROMPTS_DIR` points the
//     prompt store anywhere the caller likes, and a redirected `APPDATA` moves
//     the rest; one global manifest would end up describing a tree it is no
//     longer next to.
//   * the trees are per-thing. `avatars/<name>` is seeded per avatar, and an
//     avatar deleted by hand should take its record with it.
//   * "delete it and let it re-seed" is this project's documented reset for a
//     store, and a sidecar keeps that gesture meaning what it says.
//
// It is plain text rather than JSON because `user_paths` is the bottom of
// `aii_core` and has no business growing a parser dependency for a few lines of
// bookkeeping -- and because hand-escaping Windows paths into JSON is exactly
// the sort of thing that would be the next bug. One entry per line,
// `<16 hex digits> <relative path>`, the path last so spaces need no quoting.
constexpr const char* kManifestName = ".seeded";
constexpr const char* kManifestHeader = "# AIInterface seed manifest v1";

// FNV-1a, 64-bit. Not a security hash and does not need to be: it answers "are
// these the same bytes?" for files this app shipped, and keeps `aii_core`'s
// lowest layer standard-library-only.
std::uint64_t fnv1a(const std::string& bytes) {
  std::uint64_t h = 1469598103934665603ull;
  for (const char c : bytes) {
    h ^= static_cast<std::uint8_t>(c);
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex64(std::uint64_t v) {
  char buf[17] = {};
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

bool read_bytes(const fs::path& p, std::string* out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

bool write_bytes(const fs::path& p, const std::string& bytes) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  f.close();
  return !f.fail();
}

using Manifest = std::map<std::string, std::string>;

Manifest read_manifest(const fs::path& p) {
  Manifest m;
  std::ifstream f(p, std::ios::binary);
  if (!f) return m;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line.size() < 18 || line[16] != ' ') continue;  // not a line this version wrote
    m[line.substr(17)] = line.substr(0, 16);
  }
  return m;
}

bool write_manifest(const fs::path& p, const Manifest& m) {
  std::string out = std::string(kManifestHeader) + "\n";
  for (const auto& [rel, hash] : m) out += hash + " " + rel + "\n";
  return write_bytes(p, out);
}

}  // namespace

bool seed_tree(const fs::path& source, const fs::path& dest, std::string* error,
               std::vector<std::string>* notes) {
  std::error_code ec;
  if (!fs::exists(source, ec)) return true;  // the caller decides what that means

  const bool source_is_file = fs::is_regular_file(source, ec);
  // Where the manifest goes: inside the tree, or beside the file when a single
  // file is what is being seeded.
  const fs::path book_dir = source_is_file ? dest.parent_path() : dest;
  fs::create_directories(book_dir, ec);
  if (ec) {
    if (error) *error = "seeding " + dest.string() + ": " + ec.message();
    return false;
  }
  const fs::path book_path = book_dir / kManifestName;
  const Manifest before = read_manifest(book_path);
  Manifest now = before;

  bool ok = true;
  const auto fail = [&](const std::string& what) {
    ok = false;
    if (error && error->empty()) *error = what;
  };

  // One shipped file against one destination. Everything this function is for
  // happens in here; the loop below only decides which pairs to hand it.
  const auto reconcile = [&](const fs::path& shipped_path, const fs::path& dest_path,
                             const std::string& key) {
    std::string shipped;
    if (!read_bytes(shipped_path, &shipped)) {
      fail("seeding " + dest_path.string() + ": cannot read " + shipped_path.string());
      return;
    }
    const std::string shipped_hash = hex64(fnv1a(shipped));

    std::error_code e2;
    if (!fs::exists(dest_path, e2)) {
      if (!write_bytes(dest_path, shipped)) {
        fail("seeding " + dest_path.string() + ": cannot write it");
        return;
      }
      now[key] = shipped_hash;
      return;
    }

    std::string current;
    if (!read_bytes(dest_path, &current)) {
      fail("seeding " + dest_path.string() + ": cannot read the installed copy");
      return;
    }
    const std::string current_hash = hex64(fnv1a(current));
    if (current_hash == shipped_hash) {
      now[key] = shipped_hash;  // already in step; record it so a later edit is visible
      return;
    }

    const auto rec = now.find(key);
    if (rec != now.end() && rec->second == current_hash) {
      // The bytes last put there are still the bytes that are there: nobody has
      // touched this file, so the new shipped version simply wins.
      if (!write_bytes(dest_path, shipped)) {
        fail("seeding " + dest_path.string() + ": cannot write it");
        return;
      }
      now[key] = shipped_hash;
      return;
    }
    if (rec != now.end() && rec->second == shipped_hash) {
      // Edited by the user, and the shipped file has not moved since that was
      // recorded. Leave it, silently -- this is the common case on every start.
      return;
    }

    // Edited (or, with no record at all, possibly edited) *and* the shipped
    // bytes are not the ones this destination was last reconciled against.
    // Refuse to choose: keep theirs, put ours beside it, say so.
    fs::path beside = dest_path;
    beside += ".new";
    if (!write_bytes(beside, shipped)) {
      fail("seeding " + dest_path.string() + ": cannot write " + beside.string());
      return;
    }
    if (notes) {
      notes->push_back("kept your edited " + dest_path.string() +
                       "; the shipped version has changed and is beside it as " +
                       beside.filename().string());
    }
    // Recorded although it was not written: an entry means "the shipped bytes
    // this destination has been reconciled against", so the note is said once
    // rather than at every start until the user gets round to it.
    now[key] = shipped_hash;
  };

  if (source_is_file) {
    reconcile(source, dest, dest.filename().generic_string());
  } else {
    for (fs::recursive_directory_iterator it(source, ec), end; it != end; it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec) || ec) continue;
      const fs::path rel = fs::relative(it->path(), source, ec);
      if (ec || rel.empty()) continue;
      const std::string key = rel.generic_string();
      if (key == kManifestName) continue;  // never seed a manifest over a manifest
      reconcile(it->path(), dest / rel, key);
    }
  }

  if (now != before && !write_manifest(book_path, now)) {
    fail("seeding " + dest.string() + ": cannot write " + book_path.string());
  }
  return ok;
}

}  // namespace aii
