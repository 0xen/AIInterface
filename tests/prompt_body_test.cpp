// prompt_body_test: a declared prompt whose body cannot be read has to be
// visible, everywhere it matters.
//
// The bug this stands on: `PromptStore::load()` set `*error` for a body it
// could not read and **still returned true**, and the one caller only looked
// at `*error` when the call had returned false. So a prompt the user declared
// was quietly left out of `--system-prompt`, and nothing in the log, the
// window or the app said so. Same shape as the avatar-seed bug: wired up
// correctly, never arrives, nobody is told.
//
// Four things are checked, and the fourth is the one worth having:
//   1. the store still loads -- one missing body does not cost the other
//      prompts, so `load()` is still true;
//   2. the node carries `body_error`, so the failure is attached to the thing
//      that failed rather than to a single out-parameter that the next node
//      overwrites;
//   3. `problems()` lists it, which is the channel a caller cannot miss by
//      checking the return value;
//   4. the inspector's inventory carries a row for it, marked `failed` -- not
//      an ordinary row (that would be a worse lie than silence) and not
//      nothing (which is what the old behaviour looked like from here, since
//      an empty body drops out of `compose_order()` entirely).
//
// It builds its own store under the temp directory and points
// `AII_PROMPTS_DIR` at it, so it never touches the user's prompts.
//
// Same shape as the other tests here: a plain main, printf, non-zero exit.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/prompt_store.h"

namespace fs = std::filesystem;
using namespace aii;

static int failures = 0;

static void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

static void write_file(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

int main() {
  const fs::path dir = fs::temp_directory_path() / "aii_prompt_body_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  // Two global prompts on one chain: `here` has a body on disk, `gone`
  // declares one that does not exist. The good one is there so the test can
  // say that the bad one did not cost it anything.
  write_file(dir / "system" / "here.md", "This one is real.\n");
  write_file(dir / "graph.json", R"({
  "version": 1,
  "graphs": {
    "system": {
      "start": {"id": "start", "pos": [0, 0]},
      "nodes": [
        {"id": "here", "title": "Here", "file": "system/here.md", "kind": "global",
         "enabled": true, "pos": [0, 0]},
        {"id": "gone", "title": "Gone", "file": "system/gone.md", "kind": "global",
         "enabled": true, "pos": [0, 0]}
      ],
      "links": [
        {"from": "start", "to": "here", "order": 0, "enabled": true},
        {"from": "here", "to": "gone", "order": 0, "enabled": true}
      ]
    }
  }
})");

  _putenv_s("AII_PROMPTS_DIR", dir.string().c_str());
  std::printf("store: %s\n\n", PromptStore::root().string().c_str());

  PromptStore store;
  std::string err;
  const bool ok = store.load(&err);

  std::printf("-- load --\n");
  std::printf("     returned %s, error \"%s\"\n", ok ? "true" : "false", err.c_str());
  check(ok, "a missing body is not fatal: the rest of the store still loads");

  const PromptGraph* g = store.graph("system");
  check(g != nullptr, "the system graph is there");
  if (!g) { std::printf("FAILURES\n"); return 1; }

  const PromptNode* here = g->find("here");
  const PromptNode* gone = g->find("gone");
  check(here && here->body_error.empty() && !here->body.empty(),
        "the readable prompt is unharmed, body and all");
  check(gone && !gone->body_error.empty(),
        "the unreadable one carries its own reason: " +
            (gone ? gone->body_error : std::string("(no node)")));
  check(gone && gone->body.empty(), "and nothing composes from it");

  std::printf("\n-- problems() --\n");
  for (const std::string& p : store.problems()) std::printf("     %s\n", p.c_str());
  bool named = false;
  for (const std::string& p : store.problems())
    if (p.find("gone") != std::string::npos) named = true;
  check(!store.problems().empty() && named,
        "the failure is on a channel the caller cannot miss, and it names the prompt");

  // The composed prompt: the good one is in it, and the missing one has left
  // no trace -- which is exactly why it has to be reported somewhere else.
  const std::string composed = store.compose("system");
  check(composed.find("This one is real.") != std::string::npos,
        "the prompt that did load is still in --system-prompt");

  std::printf("\n-- the inspector's inventory --\n");
  PromptInjector injector;
  injector.reset(store);
  const PromptInventory inv = build_inventory(store, injector, {});
  int failed_rows = 0, plain_rows = 0;
  for (const PromptRow& r : inv.rows) {
    if (r.section != PromptSection::Global) continue;
    std::printf("     %-12s %-16s %s\n", r.title.c_str(), r.source.c_str(),
                r.failed ? "FAILED, not in Claude" : (r.injected ? "injected" : "not injected"));
    if (r.title == "Gone") {
      if (r.failed) ++failed_rows;
      else ++plain_rows;
    }
  }
  check(failed_rows == 1, "the prompt that could not be read has a row of its own");
  check(plain_rows == 0, "and is never drawn as an ordinary one");

  fs::remove_all(dir, ec);
  std::printf("\n%s\n", failures == 0 ? "all ok" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
