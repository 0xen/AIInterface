// Where a worker lands, checked as a rule rather than as a hope.
//
// M3.8 left this open: asked to research something with no folder named, the
// conversational instance invented one in three of ten measured spawns, almost
// always `C:\Users\johng\Documents`, and three separate prompt wordings failed
// to move it. The user's answer was "use same folder as primary agent", so the
// decision moved out of the prompt and into `core/cwd_policy.h` -- and a
// decision in code is a decision that can be tested, which is the whole reason
// it is worth moving.
//
// The cases below are the real ones, in the words they actually arrive in: a
// dictated path with its separators mangled by speech, an invented `Documents`
// off the back of a task description, and the trap that makes "any component
// matches" the wrong rule -- `...\Documents\Research` against a user who said
// the word "research" in the very request that triggered the spawn.
#include <cstdio>
#include <string>
#include <vector>

#include "core/cwd_policy.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

}  // namespace

int main() {
  using namespace aii;
  const std::string kApp = "C:\\github\\AIInterface";
  set_app_dir(kApp);

  std::printf("normalise\n");
  check(normalize_for_match("C:\\github\\AIInterface") == "cgithubaiinterface",
        "a typed path loses its separators and its case");
  check(normalize_for_match("see colon github, A.I. Interface") == "seecolongithubaiinterface",
        "dictated, the same path normalises to text containing the same words");
  check(normalize_for_match("").empty(), "empty text normalises to nothing");

  std::printf("was it ever named?\n");
  check(cwd_was_named("C:\\github\\Renderer", "have a look in the Renderer checkout on github"),
        "a folder the user described in words is corroborated");
  check(cwd_was_named("C:/github/Renderer", "the Renderer checkout on github"),
        "forward slashes are the same path");
  check(!cwd_was_named("C:\\Users\\johng\\Documents",
                       "look into whether the RX 7700 XT drivers fixed the Vulkan noise"),
        "**the measured failure**: an invented Documents folder corroborates nothing");
  check(!cwd_was_named("C:\\Users\\johng\\Documents\\Research",
                       "research whether the RX 7700 XT drivers fixed the Vulkan noise"),
        "**the trap**: `Research` matching the task description is not consent to "
        "`Users\\johng\\Documents`");
  check(!cwd_was_named("C:\\github\\Renderer", ""),
        "with nothing said at all, nothing is corroborated");
  check(!cwd_was_named("C:\\", "anything at all"),
        "a bare drive root names no folder and is not waved through");

  std::printf("the decision\n");
  check(resolve_worker_cwd("", "whatever was said").dir == kApp,
        "no cwd given: the folder the app was launched from");
  check(!resolve_worker_cwd("", "whatever was said").honoured,
        "and it is reported as not-what-was-asked-for");
  check(resolve_worker_cwd("C:\\Users\\johng\\Documents", "research the driver situation").dir == kApp,
        "an invented cwd is replaced by the app's folder rather than obeyed");
  {
    const CwdDecision d = resolve_worker_cwd("C:\\github\\Renderer",
                                             "the Renderer checkout on github, please");
    check(d.dir == "C:\\github\\Renderer" && d.honoured,
          "**a folder the user actually named is still obeyed**");
  }
  check(resolve_worker_cwd("src\\core", "work in src core").dir == kApp,
        "a relative cwd is not a folder a child can be started in");
  check(resolve_worker_cwd(kApp, "").honoured,
        "the app's own folder is always allowed, whoever wrote it down");

  // M15.3, review finding 10. The shortcut above matches through
  // `normalize_for_match`, which throws away every separator and every case
  // distinction -- that is what lets it recognise a dictated path, and it is
  // also what makes strings that are *not* the app's folder normalise to the
  // same thing. It used to return the asked-for spelling, so those strings
  // became a bypassPermissions child's working directory. It returns the real
  // folder now, and the absolute check runs first.
  check(resolve_worker_cwd("C:\\github\\AI\\Interface", "").dir == kApp,
        "**a path with a separator in the wrong place normalises the same, and is not "
        "handed back as written**");
  check(resolve_worker_cwd("c:/GITHUB/aiinterface", "").dir == kApp,
        "nor is a differently-cased, differently-slashed spelling of the same folder");
  check(resolve_worker_cwd("C:\\github\\AI\\Interface", "").honoured,
        "it is still honoured -- it is the folder this policy would have chosen anyway");
  check(resolve_worker_cwd("C:github\\AIInterface", "").dir == kApp,
        "a drive-relative spelling normalises the same too, and is refused as not absolute");
  check(!resolve_worker_cwd("C:github\\AIInterface", "").honoured,
        "and unlike the two above it is reported as not-what-was-asked-for, because it names "
        "whatever the current directory on C: happens to be");
  check(!resolve_worker_cwd("C:\\Users\\johng\\Documents", "").why.empty(),
        "every decision carries a reason for the log");

  std::printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
