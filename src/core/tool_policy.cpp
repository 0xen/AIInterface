#include "core/tool_policy.h"

namespace aii {
namespace {

// The table. See tool_policy.h for why these three and not twenty-eight.
//
// `Glob` and `Grep` are in the reading group and not a group of their own:
// they are how `Read` finds a file worth reading, a user who says "yes, you
// may read my files" has already said yes to "find them", and neither can
// write. Splitting them would be a control nobody could form an opinion about.
//
// `NotebookEdit` is deliberately *not* in the writing group. It is the only
// withheld tool that a reasonable person might expect to find in "file
// writing", so it is worth saying why: it edits .ipynb cells, this app's
// conversational instance has no business in a notebook, and every tool in a
// granted group is one more thing that runs without asking.
const ToolGroup kGroups[kToolGroupCount] = {
    {"web", "Web search", "WebSearch,WebFetch", true, false,
     "Off: Claude answers from what it already knows, and cannot\n"
     "look up today's weather, a score, a price or anything that\n"
     "happened after its training.",
     "On: Claude can search the web and read a page it has the\n"
     "address of. A search adds a few silent seconds to a turn;\n"
     "the status line says when one is happening.",
     true},
    {"file_read", "File reading", "Read,Glob,Grep", true, false,
     "Off: Claude cannot see any file on this PC. Ask it to spawn\n"
     "a worker instead - a worker reads and writes, in a directory\n"
     "you name out loud.",
     "On: Claude can read, list and search files, and does so\n"
     "without asking first. It starts in the directory the app was\n"
     "launched from, but a path it is given can be anywhere.",
     true},
    // Present, off, and greyed. See kFileWritingOffered.
    {"file_write", "File writing", "Write,Edit", false, true,
     "Off: nothing Claude does while you are talking to it can\n"
     "change a file. Writing is a worker's job.",
     "On: Claude can create and overwrite files without asking\n"
     "first, anywhere it can reach. There is no confirmation step\n"
     "and no undo.",
     kFileWritingOffered},
};

}  // namespace

const char kToolsWithheld[] =
    "Not offered: running commands (Bash, PowerShell), spawning agents, "
    "scheduling and notebooks. An enabled tool here runs without asking, and a "
    "shell that runs without asking is a worker's job - a worker at least "
    "starts in a directory you named.";

const ToolGroup& tool_group(int id) { return kGroups[id]; }

ToolPolicy::ToolPolicy() {
  for (int i = 0; i < kToolGroupCount; ++i) on[i] = kGroups[i].on_by_default;
}

bool ToolPolicy::operator==(const ToolPolicy& o) const {
  for (int i = 0; i < kToolGroupCount; ++i) {
    if (on[i] != o.on[i]) return false;
  }
  return true;
}

// `offered` gates the *value*, not just the control. A row that is greyed out
// must not be able to grant its tools through a hand-edited settings.json
// either, or the greying is decoration.
bool tool_group_active(const ToolPolicy& p, int id) {
  if (id < 0 || id >= kToolGroupCount) return false;
  return p.on[id] && kGroups[id].offered;
}

std::string tool_list(const ToolPolicy& p) {
  std::string out;
  for (int i = 0; i < kToolGroupCount; ++i) {
    if (!tool_group_active(p, i)) continue;
    if (!out.empty()) out += ',';
    out += kGroups[i].tools;
  }
  return out;
}

std::string tool_summary(const ToolPolicy& p) {
  std::string out;
  for (int i = 0; i < kToolGroupCount; ++i) {
    if (!tool_group_active(p, i)) continue;
    if (!out.empty()) out += ", ";
    out += kGroups[i].label;
  }
  return out.empty() ? "none" : out;
}

}  // namespace aii
