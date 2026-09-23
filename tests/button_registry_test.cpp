// ButtonRegistry's untrusted doors, checked as a format rather than as a
// habit.
//
// M33 added a second door beside add_path_button(): add_run_button(), which
// registers a button that runs a named action instead of opening a
// directory. Nothing links this registry into aii_block_test (that file
// counts button lines rather than registering them, because ButtonRegistry
// opens folders through <shellapi.h> and dragging that in would cost that
// file the property it was split out to gain), so this is the one place
// add_run_button()'s own shape validation -- non-empty, at most
// kActionNameMax characters, [A-Za-z0-9_.-] -- is checked directly. Whether
// a name *resolves* is ActionStore::check()'s job at click time and is
// deliberately out of scope here: this file never touches VoiceSession.
//
//   button_registry_test   prints every case and exits non-zero on failure
#include <cstdio>
#include <string>

#include "core/button_registry.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

}  // namespace

int main() {
  using aii::ButtonActionKind;
  using aii::ButtonGlyph;
  using aii::ButtonRegistry;

  std::printf("add_run_button: shape only\n");
  {
    std::string error;
    const bool ok = ButtonRegistry::instance().add_run_button(
        "go1", "Go", "runs a thing", "tidy_downloads", &error);
    check(ok, "a plain action name is accepted");
    check(error.empty(), "and no error is recorded for it");

    const auto snap = ButtonRegistry::instance().snapshot();
    const aii::ToolbarButton* found = nullptr;
    for (const auto& b : snap)
      if (b.id == "go1") found = &b;
    check(found != nullptr, "the button is in the registry");
    if (found) {
      check(found->action.kind == ButtonActionKind::RunAction,
            "its action kind is RunAction");
      check(found->action.action == "tidy_downloads", "and it carries the action's name");
      check(found->glyph == ButtonGlyph::Script, "and it draws the Script glyph");
      check(found->action.path.empty(), "path is untouched -- this is not an OpenPath button");
    }
  }
  {
    std::string error;
    const bool ok =
        ButtonRegistry::instance().add_run_button("go2", "Go", "", "", &error);
    check(!ok, "an empty action name is refused");
    check(!error.empty(), "with a reason recorded");
  }
  {
    // kActionNameMax is 64; one character over it is refused, exactly at it
    // is not -- the same off-by-one shape valid_id() already gets right for
    // button ids.
    std::string error;
    const std::string exactly64(64, 'a');
    const bool ok =
        ButtonRegistry::instance().add_run_button("go3", "Go", "", exactly64, &error);
    check(ok, "an action name at exactly the cap is accepted");
  }
  {
    std::string error;
    const std::string over64(65, 'a');
    const bool ok =
        ButtonRegistry::instance().add_run_button("go4", "Go", "", over64, &error);
    check(!ok, "one character over the cap is refused");
  }
  {
    // The alphabet is the same closed set the id itself is held to: letters,
    // digits, underscore, hyphen, dot. A space is not in it -- an action name
    // is a filename stem, never prose.
    std::string error;
    const bool ok =
        ButtonRegistry::instance().add_run_button("go5", "Go", "", "not a name", &error);
    check(!ok, "an action name with a space is refused");
  }
  {
    std::string error;
    const bool ok =
        ButtonRegistry::instance().add_run_button("go6", "Go", "", "weird$name", &error);
    check(!ok, "an action name with a character outside the alphabet is refused");
  }
  {
    // A duplicate id replaces in place, the same rule add_path_button follows,
    // so an assistant that mentions the same run button twice does not grow a
    // second one.
    std::string error;
    ButtonRegistry::instance().add_run_button("go1", "Go", "", "another_action", &error);
    int count = 0;
    std::string action;
    for (const auto& b : ButtonRegistry::instance().snapshot()) {
      if (b.id != "go1") continue;
      ++count;
      action = b.action.action;
    }
    check(count == 1, "re-registering the same id replaces it rather than duplicating it");
    check(action == "another_action", "and the replacement carries the new action name");
  }

  std::printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
