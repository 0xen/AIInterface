// M14. The memory file, checked as a rule rather than as a hope.
//
// The properties worth having a test stand between the store and the user:
// an id is issued once and never reused, so `forget id=3` a week later still
// means the line it meant; the cap refuses rather than evicts; a hand-edited
// line without an id becomes addressable; and the whole thing round-trips
// through a real file, because the file *is* the feature.
//
// Same shape as the other tests here: a plain main, printf, non-zero exit.
// It works in a temp directory and never touches %APPDATA%.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/memory_store.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
  using namespace aii;
  const fs::path dir = fs::temp_directory_path() / "aii_memory_store_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  const fs::path file = dir / "memories.md";

  std::printf("empty\n");
  {
    MemoryStore s(file);
    check(s.all().empty(), "a missing file is an empty store, not an error");
    check(has(s.digest(), "nothing yet"), "the digest says there is nothing yet");
    check(!s.digest().empty(), "the digest is never empty");
    check(!fs::exists(file), "asking does not create the file");
  }

  std::printf("append\n");
  std::uint64_t id1 = 0, id2 = 0, id3 = 0;
  {
    MemoryStore s(file);
    std::string why;
    check(s.append("The user prefers metric units.", "2026-09-21", &id1, &why), "first append " + why);
    check(id1 == 1, "the first id is 1");
    check(fs::exists(file), "the file exists after the first write");
    check(s.append("  Their cat is\ncalled   Miso.  ", "2026-09-21", &id2, &why), "second append " + why);
    check(id2 == 2, "the second id is 2");
    check(s.all()[1].text == "Their cat is called Miso.", "text is trimmed and flattened to one line");
    check(!s.append("   \n ", "2026-09-21", nullptr, &why), "empty text is refused");
    check(has(why, "empty"), "and the refusal says why: " + why);
    check(!s.append(std::string(kMemoryLineCap + 1, 'x'), "2026-09-21", nullptr, &why),
          "a line over the per-memory cap is refused");
    check(has(why, "too long"), "and the refusal says why: " + why);
    check(s.all().size() == 2, "refused appends leave the list alone");
    const std::string d = s.digest();
    check(has(d, "1. (2026-09-21) The user prefers metric units.") &&
              has(d, "2. (2026-09-21) Their cat is called Miso."),
          "the digest lists both with id and date");
  }

  std::printf("round trip\n");
  {
    MemoryStore s(file);
    check(s.all().size() == 2, "a new store over the same file reads both back");
    check(s.all()[0].id == 1 && s.all()[0].date == "2026-09-21" &&
              s.all()[0].text == "The user prefers metric units.",
          "id, date and text survive the file");
    const std::string raw = slurp(file);
    check(has(raw, "1. 2026-09-21 The user prefers metric units.\n"),
          "the file is the documented `<id>. <date> <text>` form");
  }

  std::printf("stable ids\n");
  {
    MemoryStore s(file);
    std::string why;
    check(s.remove(1, &why), "remove id 1 " + why);
    check(s.all().size() == 1 && s.all()[0].id == 2, "id 2 is still id 2 after 1 is removed");
    check(s.append("A third thing.", "2026-09-22", &id3, &why), "append after a removal " + why);
    check(id3 == 3, "the next id is max + 1, never a reused gap");
    check(!s.remove(1, &why), "removing a gone id is refused");
    check(has(why, "no memory has id 1"), "and says which: " + why);
    check(!s.remove(0, &why), "id 0 is never a memory");
  }

  std::printf("hand edits\n");
  {
    // A person adds a line without an id, a comment, a blank line and a
    // duplicate id, and a stale copy in another store must not clobber it.
    MemoryStore stale(file);
    (void)stale.all();  // loaded now, with 2 and 3
    {
      std::ofstream out(file, std::ios::binary | std::ios::app);
      out << "\n# a comment\nWritten by hand, no id.\n3. 2026-09-22 A duplicate of three.\n";
    }
    MemoryStore s(file);
    check(s.all().size() == 4, "the hand-written and duplicate lines are both read");
    check(s.all()[2].id == 4 && s.all()[2].text == "Written by hand, no id.",
          "the unnumbered line is given the next id");
    check(s.all()[3].id == 5 && s.all()[3].text == "A duplicate of three.",
          "the duplicate id becomes a fresh one rather than being dropped");
    check(!s.all()[2].date.empty(), "an undated hand line is dated today");
    std::string why;
    std::uint64_t id = 0;
    check(stale.append("Added by the stale store.", "2026-09-22", &id, &why),
          "the stale store re-reads before it writes " + why);
    check(id == 6, "and issues the id after the hand-edited ones");
    MemoryStore again(file);
    check(again.all().size() == 5, "nothing the hand edit added was lost");
    check(has(slurp(file), "4. "), "the file is normalised with the issued ids");
  }

  std::printf("cap\n");
  {
    fs::remove(file, ec);
    MemoryStore s(file);
    std::string why;
    // Fill to just under the cap with per-line-legal chunks.
    const std::string chunk(kMemoryLineCap, 'm');
    std::size_t n = 0;
    while (s.text_size() + chunk.size() <= kMemoryTextCap) {
      if (!s.append(chunk, "2026-09-21", nullptr, &why)) break;
      ++n;
    }
    check(n == kMemoryTextCap / kMemoryLineCap, "filled to the cap in whole chunks");
    check(s.full(), "the store reports full");
    check(!s.append("one more", "2026-09-21", nullptr, &why), "an append at the cap is refused");
    check(has(why, "full"), "and the refusal says it is full: " + why);
    check(s.all().size() == n, "the refused one was not written");
    check(s.remove(1, &why), "forgetting one makes room");
    check(!s.full(), "and the store is no longer full");
    why.clear();
    check(s.append("fits now", "2026-09-21", nullptr, &why), "so the append succeeds " + why);
  }

  fs::remove_all(dir, ec);
  std::printf("%s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
  return failures ? 1 : 0;
}
