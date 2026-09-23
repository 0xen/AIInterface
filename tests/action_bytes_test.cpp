// action_bytes_test: consent is to the bytes, not to the name (M19.1/M19.2).
//
// Finding 11 of the 21 Sep 2026 code review: an armed action was trusted by
// filename and its body was re-read from disk on every call, so anything that
// could write the file — a worker at bypass, the assistant with file writing
// on, the seeder on an upgrade — silently changed what a yes the user had
// already given now meant. Finding 12: a `.py` directly in `scripts\` ran at
// the next launch with no consent of any kind, though a policy can do strictly
// more than an action.
//
// Both are decided in `ActionStore` against real files, so this checks them
// against real files: a temp directory, `APPDATA` pointed at it, and the
// store's own persisted record read back the way the next launch would read
// it. What is asserted is the behaviour, not the hash:
//
//   1. arming records the bytes, and the same bytes stay runnable;
//   2. a rewritten action is refused with `Changed`, not run and not silently
//      allowed, and the refusal survives a reload — consent is withdrawn on
//      disk, not just in this process's memory;
//   3. it comes back into the approval queue, which is what makes it
//      re-confirmable rather than merely broken;
//   4. re-arming it binds the *new* bytes, and restoring the old ones is then
//      a change again — the record is not a one-way ratchet;
//   5. a consent record written before M19.1 (an array of names, no bytes) is
//      honoured rather than thrown away;
//   6. a policy script is held until it is armed, is armed under its own
//      prefixed key, and is never callable by name through `check()`;
//   7. `auto_allow` does not reach into the queue — neither for a policy nor
//      for an action the user is already being asked about.
//
// Plain main, printf, non-zero exit, like the other tests here.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "action_store.h"

namespace fs = std::filesystem;
using namespace aii;

static int failures = 0;

static void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

static void write_file(const fs::path& p, const std::string& text) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << text;
}

static std::string read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main() {
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / "aii_action_bytes_test";
  fs::remove_all(root, ec);
  const fs::path scripts = root / "AIInterface" / "scripts";
  const fs::path actions = scripts / "actions";
  fs::create_directories(actions, ec);
  _putenv_s("APPDATA", root.string().c_str());

  const fs::path file = actions / "lights.py";
  write_file(file, "\"\"\"Turn the lights on.\"\"\"\n\ndef run():\n    pass\n");

  ActionStore store;
  store.set_authoring(true);
  store.load();

  check(store.check("lights") == ActionRefusal::NotArmed, "an action starts unarmed");
  store.arm("lights");
  check(store.check("lights") == ActionRefusal::None, "arming it allows these bytes");

  // The body changes under the name. This is exactly the shape of finding 11:
  // the name is still the name, the file is still the file, and the code the
  // user said yes to is gone.
  write_file(file, "\"\"\"Turn the lights on.\"\"\"\n\ndef run():\n    print('something else')\n");
  check(store.check("lights") == ActionRefusal::Changed,
        "a rewritten body is refused at the moment of the call");

  // The scan is what withdraws consent and re-asks; `check()` only refuses.
  store.tick(2.0f);
  const Action* a = store.find("lights");
  check(a != nullptr && !a->armed, "the scan marks the changed action unarmed");
  bool queued = false;
  for (const std::string& n : store.awaiting())
    if (n == "lights") queued = true;
  check(queued, "it is back in the approval queue, so it can be confirmed again");
  check(store.digest().find("lights  [NOT ARMED") != std::string::npos,
        "the prompt digest says so too, on its own row");

  // Written through, not just remembered: the app can be killed between the
  // rewrite and the next start, and the record has to be the one that decides.
  {
    ActionStore reloaded;
    reloaded.set_authoring(true);
    reloaded.load();
    check(reloaded.check("lights") == ActionRefusal::NotArmed,
          "a fresh store agrees: the yes did not survive the rewrite");
  }

  // Re-arming binds what is there now, and the old bytes are then the change.
  const std::string current = read_file(file);
  store.arm("lights");
  check(store.check("lights") == ActionRefusal::None, "re-arming binds the new bytes");
  write_file(file, "\"\"\"Turn the lights on.\"\"\"\n\ndef run():\n    pass\n");
  check(store.check("lights") == ActionRefusal::Changed,
        "going back to the old body is a change like any other");
  write_file(file, current);
  check(store.check("lights") == ActionRefusal::None, "and restoring the armed bytes allows it");

  // ---- a record from before M19.1 --------------------------------------
  //
  // An array of names with no bytes is what every install in the wild has. It
  // must not re-ask about everything, and it must not stay byte-blind either:
  // the first scan adopts what is on disk and the next rewrite is caught.
  {
    write_file(actions / "_armed.json", "{\n  \"armed\": [\"lights\"],\n  \"seen\": [\"lights\"]\n}\n");
    ActionStore legacy;
    legacy.set_authoring(true);
    legacy.load();
    check(legacy.check("lights") == ActionRefusal::None,
          "a pre-M19.1 record still allows what it armed");
    write_file(file, "\"\"\"Turn the lights on.\"\"\"\n\ndef run():\n    import os\n");
    check(legacy.check("lights") == ActionRefusal::Changed,
          "and the bytes it adopted are then enforced");
    write_file(file, current);
  }

  // ---- policy scripts (M19.2, finding 12) ------------------------------
  const fs::path policy = scripts / "watcher.py";
  write_file(policy, "\"\"\"Watch the session.\"\"\"\nimport aii\n");
  {
    ActionStore s;
    s.set_authoring(true);
    s.load();
    check(!ActionStore::policy_allowed(policy),
          "a new policy script is not allowed to run");
    const std::string key = ActionStore::policy_key("watcher");
    bool waiting = false;
    for (const std::string& n : s.awaiting())
      if (n == key) waiting = true;
    check(waiting, "it is announced through the same queue an action uses");
    check(s.find(key) != nullptr, "the queue row resolves, so the window can draw it");
    check(s.find("watcher") == nullptr, "but never under a bare name");
    check(s.check("watcher") == ActionRefusal::NoSuchAction,
          "and `run name=watcher` cannot reach it");
    for (const Action& row : s.all())
      check(row.name != "watcher", "it is not in the action list either");
    check(s.digest().find("watcher") == std::string::npos, "nor in the prompt digest");

    s.arm(key);
    check(ActionStore::policy_allowed(policy), "arming it lets the next launch run it");
    write_file(policy, "\"\"\"Watch the session.\"\"\"\nimport aii, os\n");
    check(!ActionStore::policy_allowed(policy),
          "and a policy edited afterwards is held again, like an action");
  }

  // ---- auto_allow does not reach into the queue ------------------------
  {
    write_file(scripts / "second.py", "\"\"\"Another policy.\"\"\"\n");
    write_file(actions / "fresh.py", "\"\"\"A fresh action.\"\"\"\n\ndef run():\n    pass\n");
    ActionStore s;
    s.set_authoring(true);
    s.load();  // both are queued, because auto_allow is off
    s.set_auto_allow(true);
    s.tick(2.0f);
    check(s.check("fresh") == ActionRefusal::NotArmed,
          "turning auto-allow on does not arm what was already being asked about");
    check(!ActionStore::policy_allowed(scripts / "second.py"),
          "and it never arms a policy script, which is the case that wanted a pop-up");
  }

  // ---- a new action, with auto_allow on from the start ------------------
  {
    ActionStore s;
    s.set_authoring(true);
    s.set_auto_allow(true);
    s.load();
    write_file(actions / "later.py", "\"\"\"Written while running.\"\"\"\n\ndef run():\n    pass\n");
    s.tick(2.0f);
    check(s.check("later") == ActionRefusal::None,
          "auto-allow still arms an action that appears while running");
    write_file(actions / "later.py", "\"\"\"Written while running.\"\"\"\n\ndef run():\n    x = 1\n");
    check(s.check("later") == ActionRefusal::Changed,
          "and even then the yes is to those bytes");
  }

  fs::remove_all(root, ec);
  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
