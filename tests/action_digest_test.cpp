// action_digest_test: an unarmed action has to be *visible as unarmed* in the
// bytes that actually become `--system-prompt`.
//
// The user's instruction (19 Sep 2026) was that the AI should know it cannot
// use a script that has not been armed, and should tell them to go and arm it.
// That is a prompt-content requirement, and this project has learned twice
// that a prompt-content requirement is not met by the code that was supposed
// to meet it existing: M3.14's digest had to be added to the prompt cache key
// before a restart stopped handing the new child the old file's values, and a
// prompt body that failed to read was quietly dropped from `--system-prompt`
// with nothing anywhere saying so.
//
// So this checks the end of the pipe rather than the middle of it:
//
//   1. `digest()` marks an unarmed action and does not mark an armed one;
//   2. a description is picked up from the first docstring line, **including
//      from a file with a UTF-8 BOM** -- measured, not imagined: the first
//      capture of this feature showed a script with a perfectly good docstring
//      listed as "(no description)" because the tool that wrote it added one;
//   3. the composed system prompt actually contains those rows, with no
//      `{{scripts}}` left in it -- i.e. the substitution ran;
//   4. actions past the digest cap are **named and refused**, not silently
//      omitted, which is the `NotSettable` pattern this repo already follows.
//
// It points APPDATA and AII_PROMPTS_DIR at the temp directory, so it never
// touches the user's scripts or prompts. Plain main, printf, non-zero exit,
// like the other tests here.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "action_store.h"
#include "core/prompt_store.h"
#include "core/tool_policy.h"

namespace fs = std::filesystem;
using namespace aii;

static int failures = 0;

static void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

static void write_file(const fs::path& p, const std::string& text, bool bom) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (bom) f << "\xEF\xBB\xBF";
  f << text;
}

int main() {
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / "aii_action_digest_test";
  fs::remove_all(root, ec);
  const fs::path actions = root / "AIInterface" / "scripts" / "actions";
  fs::create_directories(actions, ec);

  // `user_data_root()` reads APPDATA, which is what makes this isolated.
  _putenv_s("APPDATA", root.string().c_str());

  write_file(actions / "armed_one.py", "\"\"\"Do the armed thing.\"\"\"\n\ndef run():\n    pass\n",
             false);
  // The BOM case, written exactly as a Windows tool writes one.
  write_file(actions / "unarmed_one.py",
             "\"\"\"Do the unarmed thing.\"\"\"\n\ndef run():\n    pass\n", true);

  ActionStore store;
  store.set_authoring(true);
  store.load();

  // Three, not two: `load()` also seeds the shipped example, which is the
  // behaviour that puts an action in front of a user who has never written
  // one. Asserted as "at least the two written here" so that adding another
  // shipped example does not fail an unrelated test.
  check(store.all().size() >= 2, "the actions written here are found");
  const Action* armed = store.find("armed_one");
  const Action* unarmed = store.find("unarmed_one");
  check(armed != nullptr && unarmed != nullptr, "both are findable by name");
  if (!armed || !unarmed) {
    std::printf("%d failure(s)\n", failures + 1);
    return 1;
  }
  check(unarmed->description == "Do the unarmed thing.",
        "a description survives a UTF-8 BOM");
  check(armed->description == "Do the armed thing.", "a description is the first docstring line");

  // Nothing is armed until somebody says so, and `load()` queues both rather
  // than treating "was there at startup" as "already answered" -- a file the
  // model wrote while the app was closed still has to ask.
  check(!armed->armed && !unarmed->armed, "nothing is armed on discovery");
  const std::size_t queued = store.awaiting().size();
  check(queued == store.all().size(), "everything unanswered is waiting, seeded or not");
  check(store.check("armed_one") == ActionRefusal::NotArmed, "an unarmed action is refused");
  check(store.check("nope") == ActionRefusal::NoSuchAction, "an invented name is refused");

  store.arm("armed_one");
  check(store.check("armed_one") == ActionRefusal::None, "an armed action is allowed");
  check(store.awaiting().size() == queued - 1, "arming takes it out of the queue");
  store.dismiss("unarmed_one");
  check(store.awaiting().size() == queued - 2, "dismissing takes it out of the queue");
  check(store.find("unarmed_one") != nullptr && !store.find("unarmed_one")->armed,
        "dismiss keeps the action, unarmed");
  check(fs::exists(actions / "unarmed_one.py", ec), "dismiss does not delete the file");

  const std::string digest = store.digest();
  check(digest.find("armed_one") != std::string::npos, "the digest names the armed action");
  check(digest.find("unarmed_one  [NOT ARMED") != std::string::npos,
        "the digest marks the unarmed action NOT ARMED, on its own row");
  // The armed one must NOT carry the mark. Checked by looking at its own row
  // rather than the whole string, since the other row has the words in it.
  {
    const std::size_t at = digest.find("armed_one -- ");
    check(at != std::string::npos, "the armed action's row has no mark on it");
  }

  // ---- the end of the pipe ---------------------------------------------
  //
  // The store's own prompts, so this composes the shipped `scripts.md` without
  // reading or writing the user's.
  const fs::path prompts = root / "prompts";
  fs::create_directories(prompts, ec);
  _putenv_s("AII_PROMPTS_DIR", prompts.string().c_str());
  set_actions_digest(digest);
  ToolPolicy policy;
  const std::string& composed = system_prompt(policy);
  check(composed.find("{{scripts}}") == std::string::npos,
        "no {{scripts}} is left in the composed prompt");
  check(composed.find("unarmed_one  [NOT ARMED") != std::string::npos,
        "the composed system prompt carries the NOT ARMED mark");
  check(composed.find("run name=") != std::string::npos,
        "the composed system prompt carries the run syntax line");

  // ---- M29: the temporary folder -----------------------------------------
  //
  // Armed by location, never by a record: a file dropped in `tmp\` is
  // callable on the next scan with no click and nothing written about it to
  // `_armed.json`.
  const fs::path tmp = aii::ActionStore::tmp_root();
  check(tmp == root / "AIInterface" / "scripts" / "tmp", "tmp_root() sits beside actions_root()");
  fs::create_directories(tmp, ec);
  write_file(tmp / "temp_one.py", "\"\"\"A throwaway thing.\"\"\"\n\ndef run():\n    pass\n", false);

  store.tick(999.0f);  // past kScanEvery, forces a rescan

  const Action* temp_one = store.find("temp_one");
  check(temp_one != nullptr, "a file dropped in tmp\\ is found");
  if (temp_one) {
    check(temp_one->temporary, "it is marked temporary");
    check(temp_one->armed, "it is armed on sight, by location, with no click");
    check(store.check("temp_one") == ActionRefusal::None,
          "check() allows it with authoring on and no consent record");
  }

  // Nothing about it reaches the consent file.
  {
    std::ifstream f(actions / "_armed.json", std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    check(body.find("temp_one") == std::string::npos,
          "a temp row never appears in _armed.json, armed or seen");
  }

  const std::string digest2 = store.digest();
  check(digest2.find("temp_one  [temp]") != std::string::npos,
        "the digest marks a temp row with [temp], after the name");

  // A same-named action wins; the temp file is dropped from the set.
  write_file(actions / "collide.py", "\"\"\"The action.\"\"\"\n\ndef run():\n    pass\n", false);
  write_file(tmp / "collide.py", "\"\"\"The temp one, never seen.\"\"\"\n\ndef run():\n    pass\n",
             false);
  store.tick(999.0f);
  {
    const Action* collide = store.find("collide");
    check(collide != nullptr && !collide->temporary,
          "a same-named action wins over a same-named temp file");
  }

  // `tmp_overview()`: a pure function, no store needed.
  {
    std::vector<Action> rows;
    Action a;
    a.name = "example";
    a.description = "Does a thing.";
    a.temporary = true;
    rows.push_back(a);
    const std::string overview = aii::ActionStore::tmp_overview(rows);
    check(overview.find("example.py -- Does a thing.") != std::string::npos,
          "tmp_overview lists a file with its docstring line");
    const std::string empty_overview = aii::ActionStore::tmp_overview({});
    check(empty_overview.find("(empty)") != std::string::npos,
          "tmp_overview says (empty) for none");
  }

  // `CLAUDE.md` lands in tmp\ after load()/rescan(), and describes what is
  // there right now.
  {
    const fs::path claude_md = tmp / "CLAUDE.md";
    check(fs::exists(claude_md, ec), "CLAUDE.md is written in tmp\\");
    std::ifstream f(claude_md, std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    check(body.find("temp_one.py") != std::string::npos,
          "CLAUDE.md in tmp\\ lists the file that is there");
  }

  fs::remove_all(root, ec);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
