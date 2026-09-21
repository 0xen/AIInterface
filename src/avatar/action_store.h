#pragma once
// M10.2/M10.5: actions — the scripts the AI can call, and can write.
//
// The design note is `docs/design-scripts.md` and this file implements §3, §4,
// §6 and §9 of it. The short form:
//
//   **An action is a `.py` in `%APPDATA%\AIInterface\scripts\actions\` that
//   defines `run()`. Its name is its filename and its description is the first
//   line of its module docstring.** There is no manifest, no version, no
//   permission block and no lifecycle, because the filesystem is the registry
//   — the same way a script runs because a `.py` sits in one flat directory, an
//   avatar exists because a folder has its name, and a leading `_` or `.`
//   already means "not this one".
//
// `actions\` is a *subdirectory* of `scripts\`, and `ScriptHost::discover()`
// iterates non-recursively (`script_host.cpp:72`), so adding an action can
// never accidentally create a long-running policy. That is the same opt-in
// boundary `examples\` already relies on.
//
// ## Two gates, in series, and neither implies the other
//
// `scripts.authoring` decides whether this app will **load and offer** what it
// finds here. It is not a gate on the *write*: `tools.file_write` grants
// `Write,Edit` with `--permission-prompts none`, so a model with that on can
// already drop a `.py` anywhere on disk and no app-side design can stop it.
// The only gate that was ever available is this one, and it is honest about
// which it is.
//
// `armed` is the second, and it is **per action, once, forever** — the user's
// own decision, 19 Sep 2026. A newly discovered action is not callable until
// the user says so; after they have, every later call is silent. One click per
// *new* action, not per call, which is what keeps "write me a script for that"
// → "now use it" down to a single click.
//
// ## Why neither gate is reachable from the ```aii``` block
//
// Both keys are in `kSettingKeys` as `NotSettable`. They are listed rather than
// hidden — M3.14's rule, because an invisible key is an inert failure — but the
// model may not write them. **A consent gate that the party being consented to
// can flip is not a gate**, and the `setting` verb is the model's hand. The
// panel's own checkboxes are the user's, and they are the only writers.
//
// ## What is never accepted from the model
//
// `run name=x` resolves `x` against the set discovered by looking at a
// directory, and refuses anything else. It is **a name, never a path and never
// a body** — `PromptInjector::request()`'s existing rule (`prompt_store.h:311`)
// applied verbatim. The model cannot name a file, cannot pass Python source,
// and cannot cause a byte to be read that this app did not already find.
//
// ## Undo
//
// Undo is deleting the file, from the Scripts row in the settings surface. No
// versioning, no quarantine, no trash — the artefact is a plain `.py` in a
// known directory and the user already has the tools. **The AI cannot delete
// its own actions**: `remove()` has exactly one caller and it is a button.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace aii {

// What the prompt pays for, and the two caps §9 of the note insists on.
//
// The digest is on **every turn forever**, on top of the settings digest, so
// the bound is explicit here in a way `PromptInjector` never needed — its
// bodies are human-authored and injected at most once per session, and neither
// of those holds for a list of actions the AI wrote itself.
constexpr std::size_t kActionDigestMax = 24;   // rows the digest will carry
constexpr std::size_t kActionDescMax = 80;     // bytes of one description
constexpr std::size_t kActionsMax = 256;       // actions held at all

// One `.py` in `scripts\actions\`.
struct Action {
  std::string name;         // the filename without `.py`, and the only handle
  std::string description;  // first docstring line, truncated to kActionDescMax
  std::string path;         // absolute, for Explorer and for the dispatcher
  bool armed = false;
  // False for the rows past `kActionDigestMax`. **Listed and refused by name
  // with a reason, never silently omitted** — growing past the cap has to fail
  // visibly, which is the `NotSettable` pattern and the M3.14 finding behind
  // it: the whole hazard of a thing that is quietly not there is that nobody
  // can say how it went missing.
  bool in_digest = true;
  // M19.1. The bytes as they are on disk *now* — FNV-1a of the whole file, in
  // hex. Consent is recorded against this value at arm time, so "armed" means
  // "armed, with these bytes", not "armed, this filename". Empty when the file
  // could not be read, and that counts as a difference rather than a match: an
  // action whose body cannot be seen is not one to run.
  //
  // It is a change detector and cannot be anything more: whatever can rewrite
  // the action can rewrite `_armed.json` beside it. What it closes is the
  // honest-mistake shape of finding 11 — a worker at bypass, a seed on
  // upgrade, or the assistant rewriting the body behind a yes the user gave to
  // different code.
  std::string fingerprint;
  // M19.2. True for a policy script (a `.py` directly in `scripts\`), which is
  // announced and gated through the same queue but is **not callable by
  // name**. Never true for a row in `all()`.
  bool policy = false;
};

// Why a call was refused, so the caller can say the right sentence. Every one
// of these is spoken or logged with the action's name in it; none of them is a
// silent no-op, because a silent no-op is the model inventing a reason.
enum class ActionRefusal {
  None,
  NoSuchAction,   // the model made the name up
  NotArmed,       // exists, found, listed — the user has not said yes yet
  Changed,        // armed once, but these are not the bytes that were armed
  AuthoringOff,   // `scripts.authoring` is off, so nothing here loads
  PastCap,        // discovered, but past kActionDigestMax
  HostUnavailable // Python could not be started
};

class ActionStore {
 public:
  // Seeds `assets/scripts/actions` into the user's copy (the shared
  // `recursive | update_existing` rule), then scans. Never throws; a missing
  // or unreadable directory is an empty list.
  void load();

  // Frame loop, cheap, rate-limited internally. A `directory_iterator` over a
  // small flat folder once a second is how a file the model just wrote becomes
  // an action without anybody asking for a reload — which is the whole of §4's
  // "re-reading the file *is* the reload".
  //
  // Returns true when the set changed, so the caller knows to re-publish.
  bool tick(float dt);

  const std::vector<Action>& all() const { return actions_; }
  const Action* find(const std::string& name) const;

  // Whether the app loads and offers what it finds. Mirrored from the panel
  // each frame, exactly as every other setting is.
  void set_authoring(bool on) { authoring_ = on; }
  bool authoring() const { return authoring_; }
  // When on, a newly discovered action is armed on sight and no window opens.
  // Default **off**, which is the shipped case and the interesting one: the
  // user's own framing was "if this is disabled, have a pop-up appear".
  void set_auto_allow(bool on);
  bool auto_allow() const { return auto_allow_; }

  // Can this name be run right now, and if not, why not. Never speaks.
  ActionRefusal check(const std::string& name) const;

  // ---- the approval queue ---------------------------------------------
  //
  // **One window listing all of them, never a window each.** The model can
  // write several files in one turn and a stack of six windows on the user's
  // desktop is a failure, so what is exposed here is a *list* and the window
  // that draws it is one window whose height follows the list.
  const std::vector<std::string>& awaiting() const { return awaiting_; }
  // Confirm: armed now and forever, and the row leaves the queue.
  void arm(const std::string& name);
  // Dismiss: **"not now", and nothing else.** The file stays on disk, the
  // action stays in the list as not armed, and it can be armed or deleted from
  // the Scripts row later. It is not a delete, and the AI is not told it was
  // refused — a mis-click has to cost nothing.
  void dismiss(const std::string& name);
  void arm_all();
  void dismiss_all();

  // The undo. Deletes the file. One caller, and it is a button.
  bool remove(const std::string& name);

  // ---- what the prompt is told ----------------------------------------
  //
  // §9's digest: `name [not armed] -- one line`. The armed mark is on the row
  // rather than in a paragraph, which is M3.13's finding — the lever on this
  // model's behaviour was the syntax line and the row, not the prose beside
  // them — and it is why the user's "the AI should tell them to go and arm it"
  // costs one bracket per row instead of a paragraph on every turn.
  std::string digest() const;

  // Actions created or armed since the last call, for `pending_context()`.
  // Empty on every turn where nothing happened, which is the property the
  // system prompt cannot have: the digest is fixed when the child starts, so
  // this is the only thing that can keep a running conversation accurate.
  std::vector<std::string> take_news();

  // One line for the Scripts section: how many, how many armed.
  std::string summary() const;

  static std::filesystem::path actions_root();

  // ---- policy scripts (M19.2) -----------------------------------------
  //
  // A `.py` directly in `scripts\` is a *policy*: `ScriptHost` hands it to the
  // interpreter at launch and it runs for the life of the app. Until M19.2
  // that happened with no consent of any kind — finding 12 — while an action,
  // which is far smaller in what it can do, needed a click.
  //
  // It is gated here rather than in a second registry of its own because the
  // machinery an announcement needs already exists in this class and nowhere
  // else: one persisted seen/armed record, one queue, and one window that
  // draws the queue. A policy therefore appears in `awaiting()` like anything
  // else, is armed by the same button, and is bound to its bytes by the same
  // fingerprint. What it never gets is a row in `all()` or `digest()`, so
  // nothing the model can say resolves to one.
  //
  // **Answered once, and re-asked when the bytes change.** There is no Scripts
  // row for a policy to be armed from later, so an editor saving the file is
  // the way back into the queue after a dismissal — which is also exactly the
  // event that should re-ask.
  //
  // The key `policy:<stem>` is what the consent record and the queue carry, so
  // an action and a policy of the same filename are two separate decisions.
  static std::string policy_key(const std::string& name);
  // True when this exact file, by name and by bytes, has been armed. Static
  // and read-only: `ScriptHost::discover()` runs before any store exists and
  // needs the answer without one. It reads the same `_armed.json` this class
  // writes, and it is the only other reader.
  static bool policy_allowed(const std::filesystem::path& file);

 private:
  void rescan();
  void write_armed() const;
  void read_armed();

  // Only ever resolves a row in `actions_`. `find()` also answers for a policy
  // so that the approval window can draw one; `check()` must not, or a name in
  // an ```aii``` block could reach a file that was never meant to be callable.
  const Action* find_action(const std::string& name) const;
  void rescan_policies();
  // Is this row armed, given what is recorded and what is on disk right now?
  // Not a query: a record whose fingerprint no longer matches is *revoked*
  // here — written back, put into the queue and said out loud — because the
  // scan is the only place that has both halves of the comparison.
  bool resolve_armed(Action* a);
  void note_changed(const Action& a);

  std::vector<Action> actions_;
  // M19.2. The policy scripts sitting directly in `scripts\`, which run for
  // the whole life of the app from the next launch. They are held apart from
  // `actions_` on purpose: they share the consent queue and nothing else. They
  // are not in `all()`, not in `digest()`, and `check()` cannot resolve one.
  std::vector<Action> policies_;
  std::vector<std::string> awaiting_;
  // The persisted consent set: key -> the fingerprint that was armed. An
  // action's key is its name; a policy's is `policy:<name>`, so the two can
  // never be confused for one another in a file that outlives both.
  //
  // A value may be empty, which is what a record written before M19.1 looks
  // like. The first scan that sees one adopts the file's current bytes and
  // says so in the log: an upgrade cannot invent an arm-time it never had, and
  // re-asking about every action the user already answered tells them nothing
  // they could act on.
  std::map<std::string, std::string> armed_;
  // Every action the user has answered about, armed or not. Persisted beside
  // the armed set and in the same file, because "have I been asked" and "did I
  // say yes" are two different facts and only one of them is consent.
  std::vector<std::string> seen_names_;
  std::vector<std::string> news_;
  bool authoring_ = false;
  bool auto_allow_ = false;
  float since_scan_ = 0.0f;
};

}  // namespace aii
