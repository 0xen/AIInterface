#pragma once
// Where a worker actually runs, decided by the app rather than by the model.
//
// **The problem this exists for (M3.8's open edge).** A worker is a Claude Code
// child started with `--permission-mode bypassPermissions`: it writes freely in
// whatever directory it is handed. Asked to research something with no folder
// named, the conversational instance invented one in **three of ten** measured
// spawns, almost always `C:\Users\johng\Documents`. It is not wild
// hallucination -- the CLI's irreducible preamble tells the model its working
// directory (M3.5: no flag removes it), so a plausible home folder is always in
// front of it. Three prompt wordings were written and measured against this and
// **none of them moved the behaviour**.
//
// **So this is not another wording.** The app spawns the worker, so the app can
// decide where it lands. The user's instruction was "use same folder as primary
// agent": when no folder was named, a worker runs where the app itself was
// launched from -- the same directory the conversational instance was told it
// is in -- and an invented path is simply not usable.
//
// **The crux: a supplied `cwd` cannot be trusted on its face.** "The user named
// this folder" and "I made this folder up" look identical in a command line, so
// honouring every supplied path would leave the hole exactly where it was. What
// the app *can* check is whether the folder was ever actually said: **the
// user's own turns are the evidence, and nothing else is** (narrowed in M15.4,
// review finding 9). It used to say "and the app-authored text handed to the
// model (worker reports, context blocks)" as well, and that was a hole with a
// respectable name on it: a worker report is app-authored only on the outside,
// and the sentence inside it was written by another Claude instance. A worker
// that invented a folder could corroborate it for the instance that spawned it,
// which is the self-corroboration this policy exists to prevent, one process
// further round. `VoiceSession::run_turn` now records evidence for
// non-injected turns only. A supplied `cwd` is honoured only when its path
// components appear there. `C:\Users\johng\Documents` off the back
// of "look into the RX 7700 XT drivers" corroborates nothing and falls back to
// the app's folder; `C:\github\Renderer` after the user said "the Renderer
// checkout on github" corroborates and is honoured. The model's *own* replies
// are deliberately never evidence -- otherwise an invented folder would
// corroborate itself the moment it was spoken once.
//
// The cost of being strict is a false negative: a folder the user meant but
// never named in words runs in the app's directory instead. That is a wrong but
// **safe** folder, in the open, one sentence from being corrected -- which is
// the trade M3.8 asked for.
#include <string>

namespace aii {

// The folder the app itself was launched from. Captured once at startup rather
// than read per call, because `std::filesystem::current_path()` is process
// state that anything may change, and "where the app started" is the thing the
// conversational instance was told it is in.
void set_app_dir(std::string dir);
// Falls back to the process working directory if nothing set it.
const std::string& app_dir();

// Lowercased, with everything that is not a letter or a digit removed. This is
// what makes speech comparable to a path: dictation turns `C:\github\Renderer`
// into "see colon github renderer" and a typed path keeps its separators, and
// both normalise to text containing "github" and "renderer".
std::string normalize_for_match(const std::string& text);

// True when every path component of `cwd` worth matching (three characters or
// more once normalised, drive letters and `.`/`..` excluded) appears in
// `evidence`, which is raw text and is normalised here. Requires at least one
// such component, so a bare drive root corroborates nothing.
//
// Every component rather than any one of them, on purpose: the measured failure
// is a research spawn, and "research" is a word the user says in the very
// request that triggers it -- so `C:\Users\johng\Documents\Research` would pass
// an any-component test on the strength of the task description alone.
bool cwd_was_named(const std::string& cwd, const std::string& evidence);

struct CwdDecision {
  std::string dir;        // where the worker will actually run; never empty
  bool honoured = false;  // true when `dir` is the path that was asked for
  std::string why;        // one line for the log, always filled
};

// The one place the rule is applied, shared by the ```aii``` `spawn` verb and
// by the deferred worker a `schedule` line creates. A deferred worker matters
// more, not less: it starts minutes later, possibly with nobody at the desk.
CwdDecision resolve_worker_cwd(const std::string& asked, const std::string& evidence);

}  // namespace aii
