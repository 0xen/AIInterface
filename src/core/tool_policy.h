#pragma once
// M3.8. Which built-in Claude Code tools the *conversational* instance gets,
// as a small table that both the settings surface and `build_llm` read.
//
// One table rather than a list of booleans, because the grouping is the whole
// design decision here and it should be visible in one place. The user asked
// for "a Tools subsection where I can toggle tools on and off, file access for
// example, and web searches". The CLI's built-in set, **measured** against
// 2.1.275 by asking a `--safe-mode` instance to name its own tools rather than
// read off `--help` (which describes none of this) or off any list written
// down elsewhere, is:
//
//   Agent  Bash  Edit  Glob  Grep  ListAgents  PowerShell  Read  ReportFindings
//   ScheduleWakeup  ToolSearch  Workflow  Write  CronCreate  CronDelete
//   CronList  DesignSync  EnterWorktree  ExitWorktree  Monitor  NotebookEdit
//   PushNotification  RemoteTrigger  SendMessage  TaskOutput  TaskStop
//   WebFetch  WebSearch
//
// Twenty-eight names. A tick box each would be a wall of jargon in a 360 px
// panel — nobody has an opinion about `ExitWorktree` — and one switch for all
// of them would put `Bash` behind the same gesture as `WebSearch`. So they are
// grouped the way a person thinks about what they are handing over: *can it
// look things up on the internet*, *can it read my files*, *can it change my
// files*. The rest are withheld entirely and said to be withheld
// (`kToolsWithheld`), because "what else could it do" is a question the user
// asked out loud and an empty answer is worse than a refused one.
//
// **Permission and this table are the same decision.** Every name here is
// passed to `--tools` *and* `--allowedTools` with `--permission-prompts none`
// behind it (see `ClaudeCodeClient::start`), which means an enabled tool is
// *granted*, not offered: nothing in this app can answer a permission prompt,
// so there is no third state between "runs" and "denied". That is why
// `Write`/`Edit` are a separate group and why `Bash`/`PowerShell` are not a
// group at all. A worker at least runs in a directory the user named aloud;
// the conversational instance runs wherever the app was launched from.
//
// Workers are untouched by any of this. `WorkerPool::spawn` passes "default",
// which is every built-in tool and no allowlist, and it never reads this file.
#include <string>

namespace aii {

// The toggles, in the order the surface draws them. Adding one is a row here
// and nothing else: the settings surface loops over the table, `settings.json`
// keys off `key`, and `tool_list()` concatenates whatever is on.
enum ToolGroupId {
  kToolGroupWeb = 0,
  kToolGroupFileRead,
  kToolGroupFileWrite,
  // M31. Claude in Chrome (the user's decision, 22 Sep 2026: "the workers as
  // well as the main AI have the ability to drive Claude in Chrome"). Unlike
  // the other three this is not a `--tools` name at all -- it is the CLI's
  // `--chrome` flag, whose tools then arrive as `mcp__claude-in-chrome__*` --
  // so `tools` is deliberately empty for this row (see `ToolGroup::tools`) and
  // `tool_list()` skips it rather than putting an empty entry on the command
  // line. `build_llm` reads this group with `tool_group_active()` exactly like
  // the other three and sets `Options::chrome` from it.
  kToolGroupBrowser,
  kToolGroupCount,
};

struct ToolGroup {
  const char* key;    // the settings.json key, under "tools". On-disk format.
  const char* label;  // the row label in the settings surface
  // The CLI tool names this group grants, comma separated, exactly as
  // `--allowedTools` spells them. **Empty for the `browser` row**: what it
  // grants is not a named tool but the `--chrome` flag, so there is nothing
  // to put on `--tools`/`--allowedTools` and `tool_list()` skips it.
  const char* tools;
  bool on_by_default;
  // Drawn amber with a warning under it. True for anything that can change
  // the user's disk without asking.
  bool risky;
  // Shown when the group is off, and when it is on: what turning it on
  // actually hands over, in the tense it happens in.
  const char* tip_off;
  const char* tip_on;
  // True when the control is live. See `kFileWritingOffered`.
  bool offered;
};

const ToolGroup& tool_group(int id);

// **The user has now answered.** They tried to have the conversational
// instance write a file, found it could not, and asked for it — so the
// `File writing` row is a live toggle, still **off by default**. Offering it
// and defaulting it on are different decisions and only the first was made:
// a tool enabled here is *granted*, so the box has to be ticked deliberately,
// by someone who has read the amber line under it.
//
// The flag stays rather than being deleted. It is the table's `offered`
// column, a fourth group may arrive greyed the way this one did, and
// `tool_group_active()` gates the stored value on it so that a hand-edited
// `settings.json` cannot grant what the surface refuses to offer.
inline constexpr bool kFileWritingOffered = true;

// The built-in tools this app does not offer the conversational instance at
// all, and the reason, for the read-only row under the toggles. Shells and
// sub-agents are a worker's job: granted-without-asking is survivable for a
// file read and is not survivable for `Bash`.
extern const char kToolsWithheld[];

// What is on. Lives in `Config`, seeded from `settings.json` in main.cpp and
// turned into a flag by `build_llm`.
struct ToolPolicy {
  bool on[kToolGroupCount];
  ToolPolicy();  // from the table's `on_by_default`
  bool operator==(const ToolPolicy& o) const;
  bool operator!=(const ToolPolicy& o) const { return !(*this == o); }
};

// Is this group's grant actually in force? Ticked *and* offered — the two
// conditions `tool_list()` applies before it writes a name onto the command
// line, in one place so that nothing can answer the question differently.
//
// It exists because the system prompt has to answer it too (see
// `core/prompt_store.h`): the sentence the model reads about what it can do
// and the list of tools it is handed have to come from the same test, or the
// prompt goes back to describing a grant the app is not making.
bool tool_group_active(const ToolPolicy& p, int id);

// The `--tools` / `--allowedTools` list for a policy: the enabled groups'
// names joined with commas, in table order. **The empty string is a legal and
// intended result** — every toggle off is `--tools ""`, a conversational
// instance with no tools at all, which is exactly what this app shipped
// before it had any.
std::string tool_list(const ToolPolicy& p);

// The same thing for a human: "web search, file reading" or "none". Used in
// the startup log line and in the settings surface's "in force" note.
std::string tool_summary(const ToolPolicy& p);

}  // namespace aii
