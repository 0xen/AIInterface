#pragma once
// M3.1 / M3.2: the prompts this app runs Claude with, owned here rather than
// hard-coded in C++ or discovered by the CLI.
//
// **On disk** (`%APPDATA%\AIInterface\prompts\`, seeded from `assets/prompts\`):
//
//     prompts/
//       graph.json          nodes, links, positions, enabled flags
//       system/voice.md     one Markdown file per prompt body
//       system/workers.md
//
// One file per body, and Markdown, because the bodies are prose the user is
// expected to edit — in this window later (M6), but also in any editor, and in
// a diff. A single JSON document holding every body would have made the one
// thing that changes most the one thing that is hardest to read.
//
// `graph.json` is **M6's data**: it is the node/link/position model the
// node-graph editor will read and write. It is defined here only as far as
// composition actually needs, plus the fields M6 must not have to migrate
// (positions, titles, per-node enable). Nothing here draws it and nothing here
// edits it.
//
// **Composition** (M3.2) walks the `system` graph from its Start node and
// concatenates the enabled bodies it reaches. The resulting string is the
// `--system-prompt` the Claude Code CLI is launched with.
//
// **This is a launch argument, so it does not hot-reload.** `avatar.json` does,
// and that is how the user tunes the avatar, so the question was asked and
// answered the other way on purpose: a system prompt reaches Claude exactly
// once, when the child process is created, and a store that reloaded itself
// mid-session would leave the window showing text that the model in the window
// has never seen. That is worse than not reloading. `reload()` exists and is
// explicit; M3.6's "Apply now" is what makes an edit take effect, by restarting
// the child.
#include <filesystem>
#include <string>
#include <vector>

namespace aii {

// Which pool a prompt belongs to. Only `Global` participates in composition;
// the other two are M3.3's lazy injection, parsed and carried here so that the
// format does not have to change when that lands.
enum class PromptKind { Global, Project, Skill };

struct PromptNode {
  std::string id;     // stable and filename-safe; links refer to this, not to titles
  std::string title;  // what M5 and M6 show
  std::string file;   // body path, relative to the store root, forward slashes
  PromptKind kind = PromptKind::Global;
  bool enabled = true;
  float pos[2] = {0.0f, 0.0f};        // M6's canvas position
  std::vector<std::string> triggers;  // M3.3, en + ja; unused until then
  std::string cwd;                    // M3.3 project scope; unused until then
  std::string body;                   // loaded from `file`
};

struct PromptLink {
  std::string from;
  std::string to;
  // Composition order among the links leaving `from`. Explicit rather than
  // implied by position in the JSON array: an editor that rewrites the file
  // must not be able to change what Claude is told by reordering two lines.
  int order = 0;
  bool enabled = true;
};

struct PromptGraph {
  std::string name;              // "system", "project", "skill"
  std::string start_id = "start";
  float start_pos[2] = {0.0f, 0.0f};
  std::vector<PromptNode> nodes;
  std::vector<PromptLink> links;

  const PromptNode* find(const std::string& id) const;
};

class PromptStore {
 public:
  // `%APPDATA%\AIInterface\prompts`, or `$AII_PROMPTS_DIR` when set — which is
  // how a test composes against a fixture without touching the user's copy.
  static std::filesystem::path root();

  // Seed from `assets/prompts/`, then read `graph.json` and every body.
  // Returns false only on a malformed store; a *missing* one is seeded and so
  // is never missing by the time it is read.
  bool load(std::string* error = nullptr);
  bool reload(std::string* error = nullptr) { return load(error); }

  // Write `graph.json` and every body back. M6 is the caller that will need
  // this; it is here now so the format has exactly one writer from the start.
  bool save(std::string* error = nullptr) const;

  const PromptGraph* graph(const std::string& name) const;
  std::vector<PromptGraph>& graphs() { return graphs_; }
  const std::vector<PromptGraph>& graphs() const { return graphs_; }

  // M3.2. Walk `name` from its Start node along enabled links and concatenate
  // the enabled node bodies. See prompt_store.cpp for the ordering rule and
  // why byte-stability matters here.
  std::string compose(const std::string& name = "system") const;

 private:
  std::vector<PromptGraph> graphs_;
};

// The composed system prompt for this process, computed once on first use.
//
// A function and not a `const char* const` any more: it reads files, so it
// cannot be a static initialiser, and it is cached because `build_llm` asks
// for it and so does anything that wants to show it. Every caller in one
// process therefore sees the same bytes, which is the property prompt caching
// is built on.
const std::string& system_prompt();

}  // namespace aii
