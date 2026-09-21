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
#include <utility>
#include <vector>

#include "core/tool_policy.h"

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
  // Non-empty when `file` was declared and could not be read. The node stays
  // in the graph -- it is still declared, it is still what the user wrote --
  // but `body` is empty and everything downstream has to be able to tell that
  // apart from a body that is genuinely empty. Silently composing without it
  // is the avatar-seed bug again: wired up correctly, never arrives, nothing
  // says so.
  std::string body_error;
};

// ------------------------------------------- M16.2: the format of a prompt
//
// The incident this closes: a refreshed `workers.md` full of `{{#file_write}}`
// markers reached a binary with no template expander, the markers went through
// as prose, and the assistant told the user it could write files it had never
// been granted. M16.1 made seeding content-based -- it now knows whether the
// user edited a file -- but it stayed format-blind on purpose: nothing on
// either side said which *language* the file was written in, so a new-format
// prompt landing on an old binary still looked exactly like an ordinary
// refresh.
//
// So both sides say it now. `graph.json` carries a top-level `"format": 1`,
// and every body under `prompts/system/` carries one line inside an HTML
// comment:
//
//     <!-- aii-prompt-format: 1 -->
//
// A comment, because that is how the headers in these files are already
// written, and because `compose()` strips every `<!-- ... -->` span out of a
// body before it is sent: the line therefore costs no tokens, is never read by
// the model as an instruction, and a `{{` inside a comment can reach neither
// `expand_tool_sections` nor Claude. The marker only counts inside a comment,
// so the same words in ordinary prose -- which the model *would* see -- are
// not a declaration.
//
// **Bump this in the same commit as the change that needs it.** It is not a
// version of the app and not a version of the prose. It is the version of what
// a binary has to understand in order to read one of these files *correctly*:
// a new `{{...}}` mechanism, a change to the tag syntax, a slot the app
// substitutes. Rewording a paragraph does not touch it. Bumping it here
// without re-heading the shipped files makes every shipped prompt fall back to
// itself and log about it at every launch; re-heading the files without
// bumping it here is the incident again, exactly.
constexpr int kPromptFormat = 1;

// What a body declares, or -1 when it declares nothing. `<!-- aii-prompt-format: 2 -->`
// is 2; a marker with no number after it, or one outside any comment, is -1,
// which is the same answer as "no header" and is handled as such.
int declared_prompt_format(const std::string& text);

// The marker itself, in one place, so the parser and anything that writes a
// header agree on the spelling.
extern const char kPromptFormatMarker[];

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
  //
  // **A body it could not read does not make this false, and that is on
  // purpose.** The return value answers "is this store usable", and one
  // unreadable body does not stop the other eight prompts reaching Claude --
  // making it fatal would turn a partial failure into a total one. What was
  // actually wrong before was that such a body was invisible: `*error` was set
  // and the return value was `true`, and the one caller only looked at
  // `*error` when the call had returned `false`. So the failure got its own
  // channel instead, `problems()`, which cannot be missed by a caller that
  // checks the return value, and `PromptNode::body_error`, which carries it as
  // far as the inspector.
  //
  // **M16.2, the format check.** An installed file that declares a format this
  // binary does not know is not read. The copy that shipped with this build is
  // used for that node instead, the user's file is left exactly where it is
  // and untouched, and `problems()` carries a sentence naming which of the two
  // is in force and where the other one is. It is never fatal: an app with no
  // prompt at all is a worse outcome than an app running on the prompt it was
  // built with, and there is always a shipped copy to fall back to.
  //
  // A *newer* declared format does the same thing as an older one, with a
  // different sentence, and that is the honest answer rather than a cautious
  // one. A binary cannot read a format it has never seen -- it can only guess
  // that the parts it recognises still mean what they used to, and guessing is
  // the whole of what went wrong the first time. What differs between the two
  // cases is what the user should do about it (update the app, versus update
  // the file), so the difference lives in the wording and nowhere else.
  //
  // A file with **no header at all** -- every install that predates this, and
  // any prompt the user writes from scratch -- is read as the current format
  // and noted, not refused. One note per load, naming the files.
  bool load(std::string* error = nullptr);
  bool reload(std::string* error = nullptr) { return load(error); }

  // Everything that went wrong during the last `load()` without stopping it:
  // one sentence per unreadable body, plus a seeding failure if there was one.
  // Empty on a clean load. Sentences, not codes -- these are logged and shown.
  const std::vector<std::string>& problems() const { return problems_; }

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

  // M5.2. The same walk, stopping one step short: the ids of the nodes whose
  // bodies `compose()` concatenates, in the order it concatenates them.
  //
  // It exists so the inspector can *name* what went into the system prompt
  // without re-deriving the traversal. A second walk that drifted from this
  // one would show the user a list that is not what Claude was actually sent,
  // which is the precise failure this window exists to prevent — so
  // `compose()` is written in terms of this function rather than beside it.
  std::vector<std::string> compose_order(const std::string& name = "system") const;

 private:
  std::vector<PromptGraph> graphs_;
  std::vector<std::string> problems_;
};

// ------------------------------------------------- the pre-prompt beside the exe
//
// `pre-prompt.md`, in the directory holding the running executable. The user
// asked for "a file that is at the base directory of this AI, for example next
// to the exe file, describing the purpose of this AI", and this is it: one
// plain Markdown file, no graph, no JSON, nothing to learn before editing it.
//
// **It is composed, not substituted, and that is the whole design.** The store
// still supplies everything it supplied before and this text is appended after
// it. A file that *replaced* the composed prompt would be the obvious reading
// of "the pre-prompt lives here", and it is the dangerous one: `system/voice.md`
// carries the language rule and the fence rule that the speech path depends on,
// and `system/workers.md` is the only place the fenced `aii` protocol is
// documented to the model. Drop that and the model simply stops emitting the
// block — no error, no warning, workers quietly never start again. So the exe
// file cannot delete anything; it can only add, and being last it has the final
// word wherever it disagrees with the store, which is what "the user's own file
// wins" should mean here.
//
// **Precedence, since both files exist.** They are not rivals: `%APPDATA%` is
// per-user and survives a rebuild, and holds the rules the *app* needs; the exe
// folder is per-install, travels with a copied app, and holds the rules the
// *user* wants. Emptying `pre-prompt.md` opts out cleanly; deleting it re-seeds
// it on the next launch.
//
// **Seeded when missing, and only when missing** — simpler than `seed_tree`,
// which compares bytes against its manifest and offers a changed shipped file
// as `<name>.new`. That is right for assets the app reads and the user rarely
// touches; this is a file the user is being invited to rewrite, so it is not
// even offered a newer version, and a rebuild must never overwrite what they
// wrote. The cost of the strict rule
// (a shipped edit not reaching an existing install) is the 636f24e trap, but it
// does not bite here: the exe directory is created fresh by every install, and
// the shipped text is a starting point rather than something the app depends on.
//
// **No hot-reload**, for the same reason the store has none: this becomes
// `--system-prompt`, which the CLI receives once at child creation. Editing the
// file while the app runs changes nothing until the next launch, and the file's
// own header comment says so. That comment — any `<!-- … -->` span — is stripped
// before the text is sent, so the file can explain itself without explaining
// itself to Claude.
std::filesystem::path local_prompt_path();

// The contents of that file: seeded if absent, comments stripped, trailing
// whitespace trimmed. Empty when the file is empty or unreadable.
std::string local_prompt();

// The rule that makes the line above possible, for the other files that are
// prose the user edits and the model reads: every `<!-- … -->` span goes, and
// an unterminated `<!--` swallows the rest of the file rather than letting a
// half-written explanation through as an instruction. M3.15's
// `prompts/system/handoff.md` is the second caller.
std::string strip_html_comments(const std::string& s);

// ------------------------------------------------- M3.9: what the model is told it has
//
// The prompt has to describe the grant the user actually made. M3.9 put the
// conversational instance's tools behind tick boxes (`core/tool_policy.h`),
// and `system/workers.md` still opened with "You can search the web yourself,
// with two tools" and "These are your only tools" — true of exactly one of the
// eight settings, and a lie in the others in both directions: it promised a
// search the model no longer had, and said nothing about the file tools it now
// did have.
//
// So the prose stays in the Markdown and the *choosing* happens here. A body
// may carry conditional sections, in the one syntax a person recognises
// without being taught it:
//
//     {{#web}}kept when Web search is on{{/web}}
//     {{^web}}kept when it is off{{/web}}
//
// The keys are the tool groups' `key` fields — `web`, `file_read`,
// `file_write` — plus `tools`, which is true when *any* group is in force and
// is how "You have no tools of your own" gets said without three nested
// negations. Sections nest. The test a key applies is `tool_group_active()`,
// the same call `tool_list()` makes to build `--allowedTools`, so the sentence
// the model reads and the tools it is handed cannot drift apart: adding a
// group to the table gives the prose a key for free.
//
// **Why not generate the paragraph in C++.** It was the obvious shape and it
// is the wrong one here. These bodies are prose the user is invited to rewrite
// (that is the whole of M3.1's one-file-per-prompt design), and a paragraph
// assembled from string literals in a .cpp would be the one paragraph they
// could not touch — while reading, in the file, as a hole. This way the
// variants sit side by side in the Markdown where they can be edited and
// diffed, and the code knows only the flag names.
//
// An unknown key keeps its section and is reported through `problems()`: a
// typo should make the prompt slightly wrong out loud, not silently delete a
// paragraph. Unbalanced tags are reported the same way. Blank runs left behind
// by a dropped paragraph are collapsed, so a prompt with a section missing is
// byte-identical to the same prompt written without it.
std::string expand_tool_sections(const std::string& text, const ToolPolicy& policy,
                                 std::vector<std::string>* problems = nullptr);

// ------------------------------------------- M3.14: what the settings say now
//
// The block `system/settings.md` substitutes for `{{settings}}` — every key of
// `settings.json`, its value on this machine at this launch, and what changing
// it costs. Built by `avatar/settings.cpp`, which owns the table, and handed
// down here because `core` must not learn what a settings file is.
//
// **Values, not prose, and that is why this one is generated.** The rule
// `expand_tool_sections` is written to protect — the paragraph the user may
// rewrite must stay in the Markdown, not in a string literal — does not reach
// here, because a list of current values could not be written in the Markdown
// at all: it is different on every machine and after every change. The prose
// around it is still in the file, still editable, still diffable.
//
// Set once, before the session is built. It is part of the cache key below, so
// a restart that changes a value composes fresh bytes rather than handing the
// new child the old file's values — which would be this app telling Claude
// something it has just made untrue.
void set_settings_digest(std::string text);

// M10.5. The block `system/scripts.md` substitutes for `{{scripts}}`: every
// action this machine has found, its one-line description, and whether the user
// has armed it. Built by `avatar/action_store.cpp` and handed down here for the
// same reason as the settings digest -- `core` must not learn what a scripts
// directory is.
//
// **The armed mark is on the row and not in a paragraph.** The user asked that
// the model know it cannot use an unarmed script and tell them to go and arm
// it; M3.13 found that what moves this model is the row and the syntax line,
// where added prose bought hallucinated readings. So an unarmed action is
// spelled unarmed where its name is, and the file's prose says once what to do
// about it.
//
// Part of the cache key below, like the settings digest and for the same
// reason: a child restarted after an action was armed must not be handed the
// previous list, which would be this app telling Claude something it had just
// made untrue.
void set_actions_digest(std::string text);

// M13.3. The block `system/voices.md` substitutes for `{{voices}}`: the syntax
// line for inline voice markers and a count of how many voices each language
// has. Built by the app, for the same reason as the two above -- `core` must
// not learn what a voice list is.
//
// **It expands to nothing when there are no secondary voices**, which is what
// makes the feature free for anyone who never configures one: no block, no
// tokens, and the blank-run collapse removes the gap the dropped text leaves.
// Configured, it costs about 83 tokens a turn, inside the cached prefix.
//
// A count and not a list of names, deliberately. The model cannot hear these
// voices, and a name is something it would then describe to the user; a count
// bounds it to slots that exist and says nothing it cannot know.
//
// Part of the cache key, like the other two: a child restarted after the voices
// changed must not be told the old count.
void set_voices_digest(std::string text);

// M14. The `{{memories}}` slot in `system/memory.md`: what the user has asked
// the assistant to remember, as `MemoryStore::digest()` renders it. Set at
// launch from the file and again after every `remember`/`forget`, and part
// of the cache key like the other three, for the same reason: a child
// restarted after a memory was saved must be handed the list that has it.
void set_memory_digest(std::string text);

// The absolute path of the user's scripts folder, substituted for
// `{{scripts_dir}}` in `system/scripts.md`.
//
// A path rather than prose, and it exists because the reference sheet the model
// reads before writing a script is a *file*, and a file it cannot find is a
// file it will not read. `%APPDATA%` is not a thing the model's Read tool
// expands, so the prompt has to carry the resolved path or the instruction is
// decorative. Left unset, the substitution falls back to the `%APPDATA%` form,
// which is still the right answer for a human reading the prose.
void set_scripts_dir(std::string path);

// The composed system prompt for this process, computed once on first use:
// the `system` graph, its conditional sections resolved against `policy`, then
// `pre-prompt.md`.
//
// A function and not a `const char* const` any more: it reads files, so it
// cannot be a static initialiser, and it is cached because `build_llm` asks
// for it and so does anything that wants to show it. Every caller in one
// process therefore sees the same bytes, which is the property prompt caching
// is built on — and still does, because the policy cannot change inside a
// running process: `--allowedTools` is fixed when the child starts, so a tick
// box reaches Claude at the next launch and not before. The cache is keyed on
// the policy anyway rather than trusting that, since a test may compose
// several in one process, and it is rebuilt on the rare miss.
const std::string& system_prompt(const ToolPolicy& policy);

// M3.3 / M3.4: the lazy half of the store.
//
// `global` prompts are composed into the system prompt at launch. `project` and
// `skill` prompts are not — they are injected into **one user turn**, the first
// turn that mentions them, and never sent again for the life of the session.
//
// **Why the user turn and not the system prompt.** The system prompt is a
// launch argument: the CLI receives it once, when the child process is created,
// and prompt caching is keyed on it being byte-stable. Appending a project
// prompt to it mid-session would therefore mean killing the child, restarting
// it and throwing the cache away — for a paragraph of text. Prepending a
// `<context>` block to the next user message costs one turn's worth of tokens,
// once, and the model treats it exactly the same.
//
// **Why substrings and not word boundaries.** Prompt bodies here are English,
// but the transcript is not. The user may name an English project inside a
// Japanese sentence, and Japanese is written without spaces, so there is no
// word boundary to anchor to — `「プロスパーのビルドを…」` has no break either
// side of the name. Matching is therefore a case-insensitive substring search
// over the raw UTF-8, and a node may carry katakana aliases beside its English
// name. The cost of that choice is false positives from very short triggers, so
// a trigger has to be at least three characters when it is pure ASCII (two
// otherwise, since a two-character Japanese word is a real word and a
// two-letter English one usually is not).
class PromptInjector {
 public:
  // Collect every `project`/`skill` node in the store. Nodes are taken by
  // `kind`, not by which graph they sit in, so the grouping M6 chooses later
  // cannot silently change what is injectable.
  void reset(const PromptStore& store);

  // What the model is actually sent for this turn: `user_text` with a
  // `<context …>` block prepended for each prompt this turn newly pulls in.
  // Mutates the loaded set, so call it exactly once per turn, on the thread
  // that runs the turn.
  std::string decorate(const std::string& user_text);

  // M3.4's `load name=`. Queues a prompt for the next `decorate()`.
  //
  // **This is the model's own channel into the app, so it resolves by name in
  // the store and does nothing else.** `name` is matched against node ids,
  // titles and triggers; anything that does not resolve is refused. It is
  // never a path and never a body — a `load` line cannot name a file, read one,
  // or introduce a byte of text the store does not already contain.
  bool request(const std::string& name);

  bool is_loaded(const std::string& id) const;
  // Ids in the order they were injected; M5's inspector wants this.
  std::vector<std::string> loaded() const { return loaded_; }
  // Forget everything: M3.6 restarts the child, and a fresh session has a
  // fresh context window, so the loaded set has to go with it.
  void clear_session() { loaded_.clear(); pending_.clear(); }

 private:
  struct Lazy {
    std::string id, title, kind, body;
    std::vector<std::string> match;  // lowercased id + title + triggers
  };
  const Lazy* resolve(const std::string& name) const;

  std::vector<Lazy> lazy_;
  std::vector<std::string> loaded_;
  std::vector<std::string> pending_;  // ids queued by `load name=`
};

// ------------------------------------------------------- M5.2: the inventory
//
// What the prompt inspector draws: one flat description of everything in
// Claude's head, built where the truth lives and handed to the window.
//
// **It is a value, and that is the point.** `PromptStore` is read on the load
// thread and `PromptInjector` is written on the turn thread; the inspector is
// drawn on the frame loop. So the frame loop never touches either — it asks
// the session for a copy of this, taken under the session's own lock, and
// draws that. No window in this app reaches into the store.
//
// It is rebuilt (strictly: re-copied) every frame rather than once, because
// the list has to be *live*: a project prompt injected by the turn that is
// running right now must appear without the window being reopened.

// Which pool a row belongs to, and therefore which section draws it. `Cli` is
// the fourth one and is not ours: see kCliSource.
enum class PromptSection { Cli, Global, Project, Skill };

struct PromptRow {
  PromptSection section = PromptSection::Global;
  std::string title;   // what the user calls it
  std::string source;  // the file it came from, or where it came from
  // Has this text actually reached the model? Global rows are true from the
  // moment the session starts (they *are* `--system-prompt`). Project and
  // skill rows are false until the turn that mentions them, which is what
  // makes M5.4's greyed-out unloaded rows a flag rather than a second list.
  bool injected = false;
  // True when it arrived with the session rather than during it. Session start
  // and "eleven minutes in" are different facts about a prompt and the row
  // says which; `at` is seconds since the session began, so the window can age
  // it without owning a clock.
  bool at_session_start = true;
  double at = 0.0;
  // M5.4's trigger words. Collected now because the injector already has them
  // and a second pass to fetch them would be a second source of truth.
  std::vector<std::string> triggers;
  // M5.3. A ratio, not a count -- `estimate_tokens` explains the two ratios --
  // and **-1 means we cannot see the text at all**, which is the honest state
  // for every CLI row: that context exists, reaches the model and costs
  // tokens, and this app never sees a byte of it. A 0 there would have read as
  // "free", which is the one thing it is not.
  int est_tokens = -1;
  // The prompt is declared but its body could not be read, so none of it
  // reached Claude. It is a row of its own and not an absence, because an
  // absence is what the old behaviour already looked like from here -- an
  // empty body drops out of `compose_order()` and the prompt simply vanished
  // from this window. It must also never be drawn as an ordinary row: a
  // prompt that failed to load, listed beside ones that did, is a worse lie
  // than not listing it. `failed` rows carry `injected == false` and
  // `est_tokens == -1`, both of which are literally true of them.
  bool failed = false;
};

struct PromptInventory {
  // False until the store has been read — which happens on the load thread,
  // several seconds in. It is a distinct state from "read, and empty": one is
  // "not yet", the other is "nothing here", and the inspector must not draw
  // them the same way. An empty Project section is the *normal* state of this
  // app (nothing authors project prompts since M6 was removed), so it must not
  // read as a failure, and "still loading" must not read as "you have none".
  bool ready = false;
  // Seconds since the session started, stamped when the copy is taken. Every
  // row's age is `uptime - at`, so the window needs no clock of its own and
  // cannot disagree with the session about what time it is.
  double uptime = 0.0;
  std::vector<PromptRow> rows;
  // M5.3: the real context fullness, as the CLI reports it -- the fraction of
  // the window in use, and the window it is a fraction of. Negative / zero
  // until a turn has been answered, because nothing has reported yet.
  //
  // It rides on the inventory rather than being fetched beside it so that the
  // footer's two numbers always come from the same instant. An estimate taken
  // this frame against a fullness taken last frame would wander on its own,
  // and this window exists to be trusted about exactly this comparison.
  double ctx = -1.0;
  long long ctx_window = 0;
};

// The rows for context the Claude Code CLI brings in by itself, which no flag
// of ours removes (M3.5, measured against a live model rather than read out of
// `--help`).
//
// **These are not a caveat, they are part of the answer.** A window that
// listed only the prompts this app injects would tell the user that Claude's
// head contains exactly what we put there, and that is false: a harness
// preamble, the working directory, git status, the platform, the model id, the
// token budget, the date and the user's own email address all arrive before a
// byte of ours does. Showing our list alone would be a comfortable falsehood,
// and preventing exactly that is why this milestone was written.
//
// They carry no timestamp of their own beyond "session start" because they are
// the child process's own preamble: they exist from the first token.
std::vector<PromptRow> cli_context_rows();

// What a `Cli` row puts in its source column, and the phrase this window uses
// for anything it cannot change. One definition, because it appears in the
// section header as well as the rows.
extern const char kCliSource[];
extern const char kNotManagedHere[];

// Every row the store and injector can describe right now, in the order the
// inspector draws them: the CLI's own context first, then the composed global
// prompts in composition order, then project and skill prompts.
//
// Call it where both objects are safe to read — for the live session that is
// under `VoiceSession`'s lock, on the thread that owns them. `loaded_at` maps
// a node id to the second it was injected; ids missing from it are drawn as
// not yet injected.
PromptInventory build_inventory(const PromptStore& store, const PromptInjector& injector,
                                const std::vector<std::pair<std::string, double>>& loaded_at);

}  // namespace aii
