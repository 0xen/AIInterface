// prompt_tools_test: the system prompt has to describe the grant that was
// actually made.
//
// M3.9 put the conversational instance's tools behind tick boxes, and the
// prompt kept opening with "You can search the web yourself, with two tools"
// and "These are your only tools" -- text from when the grant was a fixed
// pair. With Web search off it promised a tool the model did not have; with
// File reading on it denied one it did.
//
// The fix is conditional sections in the Markdown, resolved against the same
// `tool_group_active()` that builds `--allowedTools`. What this test stands on
// is that the two cannot drift: for **every** policy, over the real shipped
// `assets/prompts`, a tool name is in the prompt exactly when it is on the
// command line, and never otherwise. That is the property a future edit to the
// prose -- or a fourth group in the table -- could quietly break, and neither
// a compiler nor a dump of one configuration would notice.
//
// Also checked, because they are the ways this could fail silently rather than
// loudly: no unexpanded `{{` survives into Claude's context; every
// configuration still says what to do instead (the worker block); an unknown
// key keeps its prose and is reported rather than deleting a paragraph; and a
// dropped paragraph leaves no blank gap behind it.
//
// It seeds its own store under the temp directory and points AII_PROMPTS_DIR
// at it, so it never touches the user's prompts.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "core/prompt_store.h"
#include "core/tool_policy.h"

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

// The tool names a group grants, split out of the table's comma-separated
// field -- so this test asks about `Grep` because the table says `Grep`, and
// not because it was written down here a second time.
static std::vector<std::string> names_of(int group) {
  std::vector<std::string> out;
  std::string cur;
  for (const char* c = tool_group(group).tools; *c; ++c) {
    if (*c == ',') { out.push_back(cur); cur.clear(); } else cur += *c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

int main() {
  const fs::path dir = fs::temp_directory_path() / "aii_prompt_tools_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  _putenv_s("AII_PROMPTS_DIR", dir.string().c_str());

  PromptStore store;  // seeds itself from assets/prompts on load
  std::string err;
  if (!store.load(&err)) {
    std::printf("FAIL  the shipped store did not load: %s\n", err.c_str());
    return 1;
  }
  const std::string raw = store.compose("system");
  check(has(raw, "{{"), "the shipped prose really does carry conditional sections");
  // The node that introduces the tools. Asked for by id rather than sliced out
  // of the composed string, so that the name checks below are about this
  // prompt and cannot be thrown off by an ordinary English "Write" somewhere
  // else in the tree.
  const PromptGraph* g = store.graph("system");
  const PromptNode* workers = g ? g->find("workers") : nullptr;
  check(workers != nullptr, "the workers prompt is in the shipped graph");
  if (!workers) return 1;

  // Every policy. All eight are reachable from the surface now that
  // `kFileWritingOffered` is true, and a hand-edited settings.json could ask
  // for any of them even if one were greyed again.
  const int combos = 1 << kToolGroupCount;
  for (int mask = 0; mask < combos; ++mask) {
    ToolPolicy p;
    for (int i = 0; i < kToolGroupCount; ++i) p.on[i] = (mask & (1 << i)) != 0;
    std::vector<std::string> problems;
    const std::string text = expand_tool_sections(raw, p, &problems);
    const std::string label = "[" + tool_summary(p) + "]";

    check(problems.empty(), label + " expands with nothing to report");
    check(!has(text, "{{") && !has(text, "}}"),
          label + " leaves no unexpanded section in Claude's context");
    check(!text.empty() && has(text, "put other Claude instances to work"),
          label + " still says what to do instead of the tools it lacks");
    check(!has(text, "\n\n\n"), label + " leaves no gap where a paragraph was dropped");

    // Only that prompt's opening -- everything up to the worker block -- is
    // where the tools are introduced. Past that the prose says things like
    // "Write the task so a fresh instance can carry it out", which is an
    // ordinary English verb and not a claim about a tool.
    const std::string body = expand_tool_sections(workers->body, p, nullptr);
    const std::string head = body.substr(0, body.find("To control them,"));
    for (int i = 0; i < kToolGroupCount; ++i) {
      const bool active = tool_group_active(p, i);
      for (const std::string& name : names_of(i)) {
        check(has(head, name) == active,
              label + " " + (active ? "names " : "never names ") + name);
      }
    }
    // The one sentence that has to change shape rather than just disappear.
    check(has(text, "You have no tools of your own.") == (tool_list(p).empty()),
          label + " says it has nothing only when it has nothing");

    // The user's own bug, as a check. The report was "it says it can create a
    // file and then cannot", so the denial and the grant are asserted against
    // each other for every policy: the prompt must deny writing exactly when
    // writing is not granted, and never both at once.
    const bool writes = tool_group_active(p, kToolGroupFileWrite);
    check(has(text, "or change anything on this PC") == !writes,
          label + (writes ? " stops denying that it can change files"
                          : " says plainly that it cannot change anything here"));
    check(has(text, "Where a new file goes matters") == writes,
          label + (writes ? " tells it not to guess where a new file goes"
                          : " has no writing advice to give"));
    // With writing on, changing a file is no longer something it has to hand
    // to a worker, and the read paragraph must not still say it is.
    check(has(text, "a project gone through, a file changed") == (!writes && tool_group_active(p, kToolGroupFileRead)),
          label + " calls a file change a worker's job only when it cannot make one");
  }

  std::printf("\n-- the syntax itself --\n");
  ToolPolicy all_off;
  for (int i = 0; i < kToolGroupCount; ++i) all_off.on[i] = false;
  ToolPolicy web_on = all_off;
  web_on.on[kToolGroupWeb] = true;

  check(expand_tool_sections("a{{#web}}b{{/web}}c", web_on, nullptr) == "abc", "an on section is kept");
  check(expand_tool_sections("a{{#web}}b{{/web}}c", all_off, nullptr) == "ac", "an off section is dropped");
  check(expand_tool_sections("a{{^web}}b{{/web}}c", all_off, nullptr) == "abc", "and `^` is the other way round");
  check(expand_tool_sections("{{#web}}x{{^file_read}}y{{/file_read}}{{/web}}", web_on, nullptr) == "xy",
        "sections nest");
  check(expand_tool_sections("{{#tools}}some{{/tools}}{{^tools}}none{{/tools}}", web_on, nullptr) == "some" &&
            expand_tool_sections("{{#tools}}some{{/tools}}{{^tools}}none{{/tools}}", all_off, nullptr) == "none",
        "`tools` is true when any group is in force");

  std::vector<std::string> typo_problems;
  const std::string typo = expand_tool_sections("keep {{#websearch}}this{{/websearch}}", web_on, &typo_problems);
  check(has(typo, "this") && !typo_problems.empty(),
        "a key that is not a tool group keeps its prose and is reported: " +
            (typo_problems.empty() ? std::string("(nothing reported)") : typo_problems.front()));

  std::vector<std::string> unclosed;
  expand_tool_sections("{{#web}}forever", web_on, &unclosed);
  check(!unclosed.empty(), "an unclosed section is reported");
  std::vector<std::string> stray;
  expand_tool_sections("{{/web}}", web_on, &stray);
  check(!stray.empty(), "so is a close with nothing open");
  check(expand_tool_sections("2 {{ 2", web_on, nullptr) == "2 {{ 2", "and a stray brace is prose, not a tag");

  fs::remove_all(dir, ec);
  std::printf("\n%s\n", failures == 0 ? "all ok" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
