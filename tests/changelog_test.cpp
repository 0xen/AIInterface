// changelog_test: the changelog reaches the assistant, whole entries only,
// on the turns that ask for it and on no others.
//
// M27 wired one file beside the executable into one lazily injected prompt.
// Four things can go wrong with that, and each of them is silent:
//
//   1. the `{{changelog}}` slot is not substituted, and the model is handed
//      the literal characters `{{changelog}}` under a sentence telling it that
//      is the list of recent changes. Lazy bodies were injected raw until this
//      milestone, so this is the default failure and not a hypothetical one;
//   2. the cap cuts mid-entry, and the model summarises half a sentence as
//      though it were a change;
//   3. the file is missing -- an exe run out of a folder the build did not
//      write to -- and the slot goes empty, which under that same sentence is
//      an invitation to invent the list;
//   4. the trigger words do not resolve, so the prompt is never pulled in and
//      the model answers what is new from memory. In both languages, because
//      the app is used in both.
//
// The store is built under the temp directory with `AII_PROMPTS_DIR`, and the
// changelog is a fixture pointed at with `AII_CHANGELOG`, so nothing here
// reads or writes the user's own files. Plain main, printf, non-zero exit.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

// A section of `n` entries, each one long enough that a cap set between two of
// them has to choose. The entries are numbered so a cut can be located.
static std::string section(const std::string& heading, int first, int n) {
  std::string s = "## " + heading + "\n\n### Added\n\n";
  for (int i = 0; i < n; ++i) {
    s += "- **Entry " + std::to_string(first + i) + ".** ";
    s += "One change, written the way the file writes them: a sentence or two of plain prose "
         "about what is different, with no file paths in it and nothing a person would have to "
         "be technical to follow.\n";
  }
  return s + "\n";
}

int main() {
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path() / "aii_changelog_test";
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  const fs::path log = dir / "CHANGELOG.md";
  const std::string newest = section("2026-09-21", 1, 4);
  const std::string older = section("2026-09-14", 100, 4);
  const std::string preamble =
      "# Changelog\n\nWhat changed in this app, newest first. This paragraph is about the file "
      "and not about any release.\n\n";
  write_file(log, preamble + newest + older);
  _putenv_s("AII_CHANGELOG", log.string().c_str());

  std::printf("-- the digest --\n");
  check(changelog_path() == log, "AII_CHANGELOG points the app at a file of its own");
  {
    const std::string d = changelog_digest(kChangelogCap);
    check(has(d, "## 2026-09-21") && has(d, "Entry 1."), "the newest section is in it");
    check(!has(d, "# Changelog\n"), "the file's own preamble is not: it is about the file, not the app");
    check(d.size() <= kChangelogCap + 200, "and the whole thing is about the size of the cap");
  }

  std::printf("\n-- the cap holds, and cuts where a person would --\n");
  {
    // Both sections together are over this; the newest alone is under it.
    const std::size_t cap = newest.size() + 200;
    const std::string d = changelog_digest(cap);
    check(has(d, "Entry 1.") && has(d, "Entry 4."), "the newest section comes through whole");
    check(!has(d, "2026-09-14") && !has(d, "Entry 100."), "the one before it does not, because it does not fit");
    check(has(d, "older ones are in the app's changelog"), "and the model is told that something was left out");
  }
  {
    // Not even the newest section fits, so the cut happens inside it -- at an
    // entry, never inside one.
    const std::string d = changelog_digest(700);
    check(has(d, "## 2026-09-21") && has(d, "### Added"), "the heading survives a cut inside a section");
    check(has(d, "Entry 1."), "and so does the first entry");
    check(!has(d, "Entry 4."), "the last one is dropped rather than truncated");
    // The cut is at an entry boundary: every `- ` bullet that is present is
    // present as a whole line ending in a newline.
    bool whole = true;
    for (std::size_t at = d.find("- **Entry"); at != std::string::npos; at = d.find("- **Entry", at + 1)) {
      const std::size_t nl = d.find('\n', at);
      whole = whole && nl != std::string::npos && has(d.substr(at, nl - at), "be technical to follow.");
    }
    check(whole, "every entry in the digest is a whole entry, not the front half of one");
    check(has(d, "older ones are in the app's changelog"), "and the omission is stated");
  }

  std::printf("\n-- no changelog beside the exe --\n");
  {
    _putenv_s("AII_CHANGELOG", (dir / "nothing-here.md").string().c_str());
    const std::string d = changelog_digest(kChangelogCap);
    check(!d.empty(), "a missing file is a sentence and not an empty slot");
    check(has(d, "no changelog") && has(d, "say so"),
          "and the sentence tells the model to say so rather than describe changes it cannot see");
    _putenv_s("AII_CHANGELOG", log.string().c_str());
  }

  std::printf("\n-- the slot, and the shipped node that holds it --\n");
  {
    check(has(substitute_slots("before {{changelog}} after"), "Entry 1."),
          "`{{changelog}}` substitutes wherever it appears");
    check(!has(substitute_slots("before {{changelog}} after"), "{{changelog}}"), "and nothing of it is left");
    // The one property that matters for the section parser: substitution runs
    // first and produces nothing it would read as a tag.
    check(!has(substitute_slots("{{changelog}}"), "{{#") && !has(substitute_slots("{{changelog}}"), "{{^"),
          "and produces no `{{#` or `{{^` for `expand_tool_sections` to find");
  }

  // The shipped store, read from a copy of its own: this is the file the app
  // actually ships, not a fixture, so a node renamed or a trigger deleted
  // fails here.
  const fs::path store_dir = dir / "prompts";
  _putenv_s("AII_PROMPTS_DIR", store_dir.string().c_str());
  PromptStore store;
  std::string err;
  const bool ok = store.load(&err);
  check(ok, "the shipped store loads");
  for (const std::string& p : store.problems()) std::printf("     [prompts] %s\n", p.c_str());

  const PromptGraph* g = store.graph("project");
  const PromptNode* node = g ? g->find("changelog") : nullptr;
  check(node != nullptr, "there is a `changelog` node in the project graph");
  check(node && node->kind == PromptKind::Project, "it is a project prompt: lazy, not composed");
  check(node && has(node->body, "{{changelog}}"), "its body carries the slot");
  check(!has(store.compose("system"), "{{changelog}}"),
        "and it is not in the system prompt, which is the whole point of it being lazy");

  std::printf("\n-- the words that pull it in --\n");
  {
    PromptInjector inj;
    inj.reset(store);
    const std::string out = inj.decorate("What's new in this app?");
    check(has(out, "<context name=") && has(out, "kind=\"project\""), "an English trigger injects it");
    check(has(out, "Entry 1."), "with the slot substituted, not the slot itself");
    check(!has(out, "{{changelog}}"), "the model never sees the slot");
    check(!has(out, "aii-prompt-format"), "nor the file's own header comment");
    check(has(out, "What's new in this app?"), "and the user's words are still there, last");
    check(inj.is_loaded("changelog"), "the injector records it as loaded");

    const std::string again = inj.decorate("How long will the build take?");
    check(again == "How long will the build take?",
          "an unrelated turn afterwards carries nothing: sent once per session, never again");
  }
  {
    // Each trigger on its own, in a fresh session, because a list of triggers
    // that is right in aggregate and wrong in one entry is the failure that
    // never gets noticed.
    const char* words[] = {"what's new", "what changed", "changelog",   "new features",
                           "any update", "最近の変更",   "変更点は？", "新機能",
                           "アップデート"};
    for (const char* w : words) {
      PromptInjector inj;
      inj.reset(store);
      const std::string out = inj.decorate(w);
      check(has(out, "<context name=") && has(out, "Entry 1."), std::string("\"") + w + "\" pulls it in");
    }
  }
  {
    PromptInjector inj;
    inj.reset(store);
    check(inj.decorate("Tell me a joke about a badger.") == "Tell me a joke about a badger.",
          "and a turn that mentions none of them pulls in nothing");
  }

  std::printf("\n-- load name=changelog --\n");
  {
    PromptInjector inj;
    inj.reset(store);
    check(inj.request("changelog"), "the model can ask for it by name");
    check(!inj.request("the-changes"), "and cannot ask for anything the store does not hold");
    const std::string out = inj.decorate("Anything I should know?");
    check(has(out, "Entry 1."), "the next turn carries it, words or no words");
    check(has(out, "Anything I should know?"), "with the user's turn after it");
  }

  fs::remove_all(dir, ec);
  std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
