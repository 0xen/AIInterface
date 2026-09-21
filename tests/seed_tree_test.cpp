// Seeding by content, against real directories in the temp folder.
//
// The rule `seed_tree` used until M16 was `copy_options::update_existing`:
// refresh the installed file when the shipped one has the newer mtime. Git sets
// mtime to *checkout* time, so the shipped copy is newer after any pull that
// touched it — including pulls that changed some other file in the same commit
// — and a prompt the user had rewritten was silently replaced, while the header
// promised the edit was kept. The mirror image was just as bad: one hand edit
// froze that file against every later upstream change.
//
// Timestamps cannot answer "did the user edit this?", and bytes can, so the
// cases below are written in bytes. They are the five rows of the table in
// `core/user_paths.h`, plus the two things that made the old rule wrong in the
// first place: a file added upstream must still reach an install that has
// already run (636f24e), and mtime must not be able to decide anything.
#include <process.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/user_paths.h"

namespace fs = std::filesystem;

// `user_paths.cpp` borrows exactly one symbol from `config.cpp`, and compiling
// that file in would drag the language table and the tool policy behind it for
// a test that reads no environment variable at all. Supplying it here keeps the
// target to the two files the rule actually lives in.
namespace aii {
std::string env_or(const char* name, const std::string& def) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : def;
}
}  // namespace aii

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

void put(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << text;
}

std::string get(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return "<missing>";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool there(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

// Make the destination look older than the shipped file, the way a fresh git
// checkout leaves every install on disk. Under the old rule this alone decided
// the outcome; under the new one it must decide nothing.
void age(const fs::path& p) {
  std::error_code ec;
  fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(24 * 30), ec);
}

struct Run {
  bool ok = false;
  std::string error;
  std::vector<std::string> notes;
};

Run seed(const fs::path& src, const fs::path& dst) {
  Run r;
  r.ok = aii::seed_tree(src, dst, &r.error, &r.notes);
  return r;
}

}  // namespace

int main() {
  std::error_code ec;
  const fs::path base =
      fs::temp_directory_path(ec) / ("aii_seed_test_" + std::to_string(::_getpid()));
  fs::remove_all(base, ec);
  const fs::path src = base / "shipped";
  const fs::path dst = base / "installed";

  std::printf("a fresh install\n");
  put(src / "graph.json", "shipped v1\n");
  put(src / "system" / "voice.md", "voice v1\n");
  {
    const Run r = seed(src, dst);
    check(r.ok && r.error.empty(), "the first seed succeeds");
    check(get(dst / "graph.json") == "shipped v1\n", "a missing file is copied");
    check(get(dst / "system" / "voice.md") == "voice v1\n", "so is one in a subdirectory");
    check(r.notes.empty(), "and it has nothing to say about it");
    check(there(dst / ".seeded"), "the manifest is written beside the tree it describes");
  }
  {
    const Run r = seed(src, dst);
    check(r.ok && r.notes.empty(), "seeding again changes nothing and says nothing");
  }

  std::printf("upstream moves, the user has not touched it\n");
  put(src / "graph.json", "shipped v2\n");
  age(dst / "graph.json");
  {
    const Run r = seed(src, dst);
    check(get(dst / "graph.json") == "shipped v2\n",
          "**the refresh still happens**: the installed bytes are the ones we put there");
    check(!there(fs::path(dst / "graph.json").string() + ".new"), "with no `.new` left behind");
    check(r.notes.empty(), "and silently");
  }

  std::printf("a file added upstream after the install (636f24e)\n");
  put(src / "system" / "memory.md", "memory v1\n");
  {
    const Run r = seed(src, dst);
    check(get(dst / "system" / "memory.md") == "memory v1\n",
          "a newly shipped file reaches a machine that has already run the app");
    check(r.notes.empty(), "and needs no note");
  }

  std::printf("the user edits, upstream does not\n");
  put(dst / "system" / "voice.md", "voice, in my own words\n");
  {
    const Run r = seed(src, dst);
    check(get(dst / "system" / "voice.md") == "voice, in my own words\n",
          "the edit survives the seed");
    check(r.notes.empty(), "silently -- this is every start, and it must not chatter");
    check(!there(fs::path(dst / "system" / "voice.md").string() + ".new"),
          "and nothing is written beside it, because there is nothing new to offer");
  }

  std::printf("**the M16 case**: the user edited it and upstream changed it too\n");
  put(src / "system" / "voice.md", "voice v2\n");
  // The shape that broke it: git has just checked out the shipped file, so it
  // is the newer of the two by a month.
  age(dst / "system" / "voice.md");
  {
    const Run r = seed(src, dst);
    check(get(dst / "system" / "voice.md") == "voice, in my own words\n",
          "**the edit is not overwritten, however new the shipped file looks**");
    check(get(fs::path(dst / "system" / "voice.md").string() + ".new") == "voice v2\n",
          "the shipped version is put beside it as `.new`");
    check(r.notes.size() == 1 && r.notes[0].find("voice.md.new") != std::string::npos,
          "and the caller is told, by name");
  }
  {
    const Run r = seed(src, dst);
    check(r.notes.empty(), "said once, not at every start");
    check(get(dst / "system" / "voice.md") == "voice, in my own words\n",
          "and the edit is still theirs on the next start");
  }
  {
    // Having offered v2 once, offer v3 when it arrives: the record moved on,
    // so a further upstream change is a further thing worth saying.
    put(src / "system" / "voice.md", "voice v3\n");
    const Run r = seed(src, dst);
    check(r.notes.size() == 1, "a further upstream change is offered again");
    check(get(fs::path(dst / "system" / "voice.md").string() + ".new") == "voice v3\n",
          "and `.new` carries the latest shipped bytes");
  }

  std::printf("the migration: an install with no manifest at all\n");
  {
    const fs::path old_dst = base / "installed_old";
    put(old_dst / "graph.json", "shipped v2\n");              // untouched by the user
    put(old_dst / "system" / "voice.md", "my own words\n");   // hand-edited, long ago
    put(old_dst / "mine.md", "a file I added myself\n");      // not ours at all
    put(src / "graph.json", "shipped v2\n");
    put(src / "system" / "voice.md", "voice v3\n");
    const Run r = seed(src, old_dst);
    check(get(old_dst / "graph.json") == "shipped v2\n",
          "a file that already matches is recorded, not rewritten");
    check(get(old_dst / "system" / "voice.md") == "my own words\n",
          "**an unrecorded difference is assumed to be a user edit and kept**");
    check(there(fs::path(old_dst / "system" / "voice.md").string() + ".new"),
          "with the shipped version beside it");
    check(get(old_dst / "mine.md") == "a file I added myself\n",
          "a file the user added is never touched");
    check(r.notes.size() == 1, "one note, for the one file it could not decide");
    const Run again = seed(src, old_dst);
    check(again.notes.empty(), "and the migration happens exactly once");
  }

  std::printf("deleting the installed copy is still the reset\n");
  {
    fs::remove_all(dst, ec);
    const Run r = seed(src, dst);
    check(get(dst / "system" / "voice.md") == "voice v3\n",
          "the tree comes back shipped, manifest and all");
    check(r.notes.empty(), "with nothing to report");
  }

  std::printf("a single file as the source\n");
  {
    const fs::path one_src = base / "one.md";
    const fs::path one_dst = base / "one_installed" / "one.md";
    put(one_src, "one v1\n");
    seed(one_src, one_dst);
    check(get(one_dst) == "one v1\n", "it is copied");
    put(one_dst, "mine\n");
    put(one_src, "one v2\n");
    const Run r = seed(one_src, one_dst);
    check(get(one_dst) == "mine\n" && there(one_dst.string() + ".new"),
          "and it obeys the same rule, with the manifest beside it");
    check(r.notes.size() == 1, "reported once");
  }

  std::printf("a source that is not there\n");
  {
    const Run r = seed(base / "nothing_here", base / "nowhere");
    check(r.ok && r.error.empty() && r.notes.empty(),
          "is not an error: the caller decides what a missing asset means");
  }

  fs::remove_all(base, ec);
  std::printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
