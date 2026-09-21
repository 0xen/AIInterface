// prompt_format_test: a prompt file written for a different binary must never
// be read as though it were written for this one.
//
// The incident, in full: a refreshed `workers.md` full of `{{#file_write}}`
// markers was seeded over an install whose binary had no template expander.
// Nothing was corrupt and nothing failed -- the markers simply went through as
// prose, the conditional that was supposed to hide the file-writing paragraph
// hid nothing, and the assistant told the user it could write files it had
// never been granted. M16.1 closed the half of that about *whose* bytes win.
// This is the other half: which format the bytes are in, said by the file and
// checked by the binary.
//
// What is checked here, in the order the cases are written:
//   1. `declared_prompt_format` itself -- a marker counts inside an HTML
//      comment and nowhere else, because outside one the model would read it;
//   2. an installed body declaring a *newer* format: the shipped copy is in
//      force, the user's file is untouched on disk, and `problems()` says so
//      in words naming both;
//   3. an *older* declared format: the same fallback, a different sentence --
//      see `PromptStore::load()` in the header for why those two are not
//      treated differently;
//   4. a declared format with no shipped copy to fall back to: the user's file
//      is used anyway and the sentence says that plainly. The app is never
//      left with no prompt;
//   5. a body with no header at all -- every install that predates this -- is
//      read as the current format and noted, not refused;
//   6. the header is stripped before composition, together with everything
//      else in a comment, so a `{{` inside one reaches neither
//      `expand_tool_sections` nor Claude;
//   7. `graph.json` declaring a format this build does not know: the shipped
//      graph is in force for that load and the user's file is untouched; and a
//      `graph.json` with no `format` is read, with a note.
//
// It builds its stores under the temp directory and points `AII_PROMPTS_DIR`
// at each in turn, so it never touches the user's prompts. Same shape as the
// other tests here: a plain main, printf, non-zero exit.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/prompt_store.h"

namespace fs = std::filesystem;
using namespace aii;

static int failures = 0;

static void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

static void write_file(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

static std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Every problem the store reported, as one blob -- the checks below ask
// whether a sentence was said, not which position it was said in.
static std::string all_problems(const PromptStore& s) {
  std::string out;
  for (const std::string& p : s.problems()) out += p + "\n";
  return out;
}

int main() {
  std::error_code ec;

  std::printf("-- the marker itself --\n");
  check(declared_prompt_format("<!-- aii-prompt-format: 1 -->\nbody") == 1,
        "a marker in a comment is the format it names");
  check(declared_prompt_format("<!--\n  notes\n  aii-prompt-format: 42\n-->\nbody") == 42,
        "and it may sit anywhere inside that comment");
  check(declared_prompt_format("plain prose, no header") == -1, "a body with no header declares nothing");
  check(declared_prompt_format("aii-prompt-format: 3\nbody") == -1,
        "the same words outside a comment are prose, not a declaration");
  check(declared_prompt_format("<!-- aii-prompt-format: -->\nbody") == -1,
        "a marker with no number declares nothing, which is read as no header");
  check(declared_prompt_format("<!-- nothing here -->\n<!-- aii-prompt-format: 2 -->") == 2,
        "a later comment is looked in too");

  // ---- a store whose bodies declare things ------------------------------
  const fs::path dir = fs::temp_directory_path() / "aii_prompt_format_test";
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  _putenv_s("AII_PROMPTS_DIR", dir.string().c_str());

  // Two of these name a file the app ships (`voice.md`, `memory.md`), which is
  // what makes the fallback reachable; the rest are the test's own.
  write_file(dir / "graph.json", R"({
  "version": 1,
  "format": 1,
  "graphs": {
    "system": {
      "start": {"id": "start", "pos": [0, 0]},
      "nodes": [
        {"id": "newer", "title": "Newer", "file": "system/voice.md", "kind": "global",
         "enabled": true, "pos": [0, 0]},
        {"id": "older", "title": "Older", "file": "system/memory.md", "kind": "global",
         "enabled": true, "pos": [0, 0]},
        {"id": "orphan", "title": "Orphan", "file": "system/orphan.md", "kind": "global",
         "enabled": true, "pos": [0, 0]},
        {"id": "bare", "title": "Bare", "file": "system/bare.md", "kind": "global",
         "enabled": true, "pos": [0, 0]},
        {"id": "hidden", "title": "Hidden", "file": "system/hidden.md", "kind": "global",
         "enabled": true, "pos": [0, 0]}
      ],
      "links": [
        {"from": "start", "to": "newer", "order": 0, "enabled": true},
        {"from": "start", "to": "older", "order": 1, "enabled": true},
        {"from": "start", "to": "orphan", "order": 2, "enabled": true},
        {"from": "start", "to": "bare", "order": 3, "enabled": true},
        {"from": "start", "to": "hidden", "order": 4, "enabled": true}
      ]
    }
  }
})");

  // One load first, so that seeding writes its manifest and the shipped
  // prompts land in the store the way they would on a real install. The
  // fixtures below are then *edits* to seeded files, which is the exact shape
  // the incident had.
  {
    PromptStore warm;
    std::string e;
    warm.load(&e);
  }

  const char* kNewerBody =
      "<!-- aii-prompt-format: 99 -->\nINSTALLED-NEWER-BODY, written for a build that does not exist yet.\n";
  const char* kOlderBody =
      "<!-- aii-prompt-format: 0 -->\nINSTALLED-OLDER-BODY, written for a build that is gone.\n";
  write_file(dir / "system" / "voice.md", kNewerBody);
  write_file(dir / "system" / "memory.md", kOlderBody);
  write_file(dir / "system" / "orphan.md",
             "<!-- aii-prompt-format: 99 -->\nORPHAN-BODY, and nothing ships under this name.\n");
  write_file(dir / "system" / "bare.md", "BARE-BODY, with no header of any kind.\n");
  write_file(dir / "system" / "hidden.md",
             "<!-- aii-prompt-format: 1\n  {{#file_write}} and a brace, inside the comment\n-->\n"
             "HIDDEN-BODY, the only part of this file anyone should ever see.\n");

  PromptStore store;
  std::string err;
  const bool ok = store.load(&err);
  const std::string composed = store.compose("system");
  const std::string problems = all_problems(store);
  const PromptGraph* g = store.graph("system");

  std::printf("\n-- problems() --\n");
  for (const std::string& p : store.problems()) std::printf("     %s\n", p.c_str());
  std::printf("\n");

  check(ok, "a store full of formats this build cannot read still loads");
  check(g != nullptr, "the system graph is there");
  if (!g) { std::printf("FAILURES\n"); return 1; }

  std::printf("-- a newer declared format --\n");
  check(!has(composed, "INSTALLED-NEWER-BODY"), "the file this build cannot read is not in the prompt");
  check(has(composed, "You are a voice assistant"), "the copy that shipped with this build is");
  check(read_file(dir / "system" / "voice.md") == kNewerBody,
        "and the user's file is untouched, byte for byte, where they left it");
  check(has(problems, "system/voice.md declares format 99") && has(problems, "newer version of this app"),
        "the log says which file, which format, and why it is not being read");
  check(has(problems, "The copy that shipped with this build is in force") &&
            has(problems, (dir / "system" / "voice.md").lexically_normal().string()),
        "and names the copy in force and the path of the one that is not");

  std::printf("\n-- an older declared format --\n");
  check(!has(composed, "INSTALLED-OLDER-BODY") && has(composed, "You can remember things between conversations"),
        "falls back exactly the same way");
  check(has(problems, "system/memory.md declares format 0") && has(problems, "older format this build no longer reads"),
        "with the sentence that tells the user to update the file rather than the app");

  std::printf("\n-- nothing to fall back to --\n");
  check(has(composed, "ORPHAN-BODY"),
        "a format this build cannot read, with no shipped copy, still reaches Claude: an app with no "
        "prompt is worse");
  check(has(problems, "could not be used either") && has(problems, "may describe things this build cannot do"),
        "and the log says plainly that the file in force is one this build does not understand");

  std::printf("\n-- no header at all --\n");
  check(has(composed, "BARE-BODY"), "a body with no header is read");
  check(has(problems, "no format header") && has(problems, "system/bare.md"),
        "and noted by name, once, rather than refused");
  check(!has(problems, "system/bare.md declares"), "it is a note and not a mismatch");

  std::printf("\n-- the header never reaches Claude --\n");
  check(has(composed, "HIDDEN-BODY"), "the prose in the file is composed");
  check(!has(composed, "aii-prompt-format"), "the header line is not");
  // Not "no `{{` anywhere": two of the nodes above fell back to shipped
  // prompts, and one of those really does carry a slot. The claim is about
  // this file's comment, so it is asserted about this file's comment.
  check(!has(composed, "{{#file_write}}") && !has(composed, "inside the comment"),
        "and neither is a `{{` sitting in the same comment, which is the whole reason it is a comment");
  const PromptNode* hidden = g->find("hidden");
  check(hidden && has(hidden->body, "aii-prompt-format"),
        "the node still holds the file's own bytes, header and all: `save()` writes these back");

  fs::remove_all(dir, ec);

  // ---- a graph.json this build cannot read -------------------------------
  std::printf("\n-- graph.json declaring a format this build does not know --\n");
  const fs::path dir2 = fs::temp_directory_path() / "aii_prompt_format_graph_test";
  fs::remove_all(dir2, ec);
  fs::create_directories(dir2, ec);
  _putenv_s("AII_PROMPTS_DIR", dir2.string().c_str());
  const std::string bogus_graph = R"({
  "version": 1,
  "format": 99,
  "graphs": {
    "system": {
      "start": {"id": "start", "pos": [0, 0]},
      "nodes": [{"id": "mine", "title": "Mine", "file": "system/bare.md", "kind": "global",
                 "enabled": true, "pos": [0, 0]}],
      "links": [{"from": "start", "to": "mine", "order": 0, "enabled": true}]
    }
  }
})";
  write_file(dir2 / "graph.json", bogus_graph);
  write_file(dir2 / "system" / "bare.md", "MINE-BODY\n");
  PromptStore store2;
  std::string err2;
  const bool ok2 = store2.load(&err2);
  const std::string problems2 = all_problems(store2);
  for (const std::string& p : store2.problems()) std::printf("     %s\n", p.c_str());
  const PromptGraph* g2 = store2.graph("system");
  check(ok2 && g2 != nullptr, "it loads, on the graph that shipped with this build");
  check(g2 && g2->find("mine") == nullptr && g2->find("voice") != nullptr,
        "the shipped graph is the one in force, not the one this build cannot read");
  check(has(problems2, "graph.json declares format 99") && has(problems2, "is in force"),
        "and the log says so");
  check(read_file(dir2 / "graph.json") == bogus_graph, "the user's graph.json is untouched");
  fs::remove_all(dir2, ec);

  std::printf("\n-- graph.json with no format at all --\n");
  const fs::path dir3 = fs::temp_directory_path() / "aii_prompt_format_old_graph_test";
  fs::remove_all(dir3, ec);
  fs::create_directories(dir3, ec);
  _putenv_s("AII_PROMPTS_DIR", dir3.string().c_str());
  write_file(dir3 / "graph.json", R"({
  "version": 1,
  "graphs": {
    "system": {
      "start": {"id": "start", "pos": [0, 0]},
      "nodes": [{"id": "mine", "title": "Mine", "file": "system/bare.md", "kind": "global",
                 "enabled": true, "pos": [0, 0]}],
      "links": [{"from": "start", "to": "mine", "order": 0, "enabled": true}]
    }
  }
})");
  write_file(dir3 / "system" / "bare.md", "MINE-BODY\n");
  PromptStore store3;
  std::string err3;
  const bool ok3 = store3.load(&err3);
  const std::string problems3 = all_problems(store3);
  for (const std::string& p : store3.problems()) std::printf("     %s\n", p.c_str());
  check(ok3 && has(store3.compose("system"), "MINE-BODY"),
        "an install written before any of this is read exactly as it always was");
  check(has(problems3, "graph.json declares no format"), "and told so, once");
  fs::remove_all(dir3, ec);

  std::printf("\n%s\n", failures == 0 ? "all ok" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
