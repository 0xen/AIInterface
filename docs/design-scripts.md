# What a script is

This is the design reference for the script system: what a script is in this app, the two
kinds that exist, what either one can and cannot do, and why the boundaries are drawn where
they are. It is not a tutorial — for that, read `assets/scripts/AII-MODULE.md` and the
examples in `assets/scripts/examples/`. This document is for anyone changing the C++ side of
scripts, or trying to understand why a rule exists before changing it.

Every factual claim about the code below carries a file and, where useful, a line, so a
claim can be checked rather than taken on trust.

---

## 1. The starting principles

1. **There is no "skill". There is a script**, and a script is a reusable action the AI can
   call and can write. The system prompt's job is to say which ones exist, not to describe
   what they do beyond a one-line description.
2. **Authoring has its own toggle, separate from file-writing.** Writing a file the user
   asked for and adding code that runs inside the app on every later call are different risk
   classes: the first is a one-time act, the second never asks again once granted.
3. **A newly written script is callable on the next turn**, through the per-turn context the
   app already composes (`VoiceSession::pending_context()`, `src/avatar/voice_session.cpp`),
   not by recomposing the system prompt — which is fixed for the life of the child process,
   and restarting the child discards the conversation.

---

## 2. The surface a script has to work with

### 2.1 The control surface is 35 verbs wide

`src/pyhost/aii_pyhost.cpp` exposes 35 functions on the `aii` module: `post`, `poll_lines`,
`wait_lines`, `should_quit`, `log`, `status`, `play`, `release`, `sprite`, `cells`,
`clear_cells`, `load_avatar`, `theme`, `colour`, `button`, `clear_buttons`, `schedule`,
`cancel_schedule`, `list_schedules`, `mic`, `mute`, `stop`, `reset`, `handoff`, `say`,
`session_info`, `model`, `tools`, `language`, `auto_listen`, `listen_timeout`, `chat`,
`open_settings`, `avatar_mode`, `settings_info` — plus `poll`, `wait` and `send` defined in
the Python shim in the same file. `send()` posts *any* bus message, including one the module
has no dedicated helper for, so the module itself is not the limit on what a script can
reach; the bus families are.

`src/avatar/bus_bindings.h` states the rule this surface is built to: every verb lands on the
call the control itself makes, so nothing on the bus is a second implementation of anything.
A verb is added when the app already has the door it opens, not invented to widen the panel
for its own sake.

### 2.2 What is deliberately not exposed

`src/avatar/bus_bindings.h`, headed "What is deliberately not here", gives reasons rather
than a bare list: hold-to-dictate is excluded because its meaning is *how long it lasted and
whether the pointer was still on the button*, which a script does not have, and exposing its
halves would mostly buy a script the ability to open the microphone and never close it; the
sidebar folder button, the inspector and the worker windows are excluded because they open OS
windows, which is not app state and cannot be observed on the bus; quitting is excluded
because `should_quit()` is the app's word to the script and not the reverse.

`bus_bindings.h` also states the rule that rules out the obvious verb: **there is no
`button.press` and there never will be**, because half the controls are levels rather than
presses (`mute` retried would unmute) and a press-by-id verb would reach
`ButtonActionKind::Invoke`, which is deliberately unreachable from the bus.

§5 names the exclusion policy in full; it is one policy, not a special case per verb.

### 2.3 A script cannot stall the frame loop

`AppBus::drain_events()` (`src/core/app_bus.cpp`) is called on the frame loop only from
`BusFileHatch::poll()` (`src/avatar/bus_bindings.cpp`), and only when a debug flag
(`--bus-out`) is set. It is called off the frame loop from `take_for_this_thread()`
(`src/pyhost/aii_pyhost.cpp`), on the Python script threads. `ScriptHost::tick()`
(`src/avatar/script_host.cpp`) does run on the frame loop (`src/avatar/main.cpp`), but it
drains the engine's own render message queue, not the app bus — nothing consumes that queue,
and `tick()` exists to empty it.

So a script cannot stall the frame loop: the two loops do not share a thread. The app's own
side of the bus is a short mutex around bounded deques (`AppBus::apply_pending()`,
`src/core/app_bus.cpp`, swaps the inbox out under the lock and runs handlers outside it,
bounded by `kBusInboxMax = 64`, `src/core/app_bus.h`), and `src/avatar/script_host.h` records
that 60 fps held through a five-second sleep and a three-second GIL-holding busy spin in one
script thread. The failure boundary a script needs (§7) is therefore not about protecting the
renderer; it is about the three things that can actually go wrong in a script's own thread.

### 2.4 Prompt authoring and script authoring are different things

`assets/prompts/graph.json` carries a `"skill"` prompt graph alongside `"changelog"` and the
project prompts. `PromptStore::save()` (`src/core/prompt_store.h`, `src/core/prompt_store.cpp`)
is the only way a graph node could be written at runtime, and it has no caller anywhere in
`src/`: nothing the model does, no bus verb and no UI action ever authors a prompt node.
Every node in `graph.json`, including the `skill` graph's own node, is put there by hand-
editing the file, not by anything running inside the app.

This is a deliberate boundary, not a gap to be closed by a script. A prompt node is text
injected into a turn; a script is a callable the model invokes and that returns. Giving a
script the ability to write prompt nodes would let it rewrite what the model is told about
itself, which is a different and much larger capability than running an action, and nothing
requires it: the two systems solve different problems and are kept apart on purpose.

### 2.5 The bug shape reload has to avoid

With two scripts polling the bus at once, the second one to call `poll_lines()` used to
receive exactly zero events, silently: one queue, a destructive drain, whoever calls first
takes everything. The fix fans events out to a per-thread cursor
(`src/pyhost/aii_pyhost.cpp`), capped at `kSubMax = 256` per cursor.

That fix has a property that matters for anything that runs once and exits: **a thread's
cursor is never erased when the thread ends.** Two consequences follow, and both are
containment concerns rather than active bugs today, because nothing that registers a cursor
is expected to end and be replaced quickly:

- A script whose thread ends leaves a cursor behind, and every later drain copies every event
  into it forever. It is bounded per cursor (256 lines) but the per-event copy cost grows
  linearly in the number of dead cursors.
- Windows recycles thread ids. A new thread that happens to receive a dead one's id inherits
  up to 256 stale events on its first poll — a silent wrong-data bug of exactly this shape,
  latent for anything short-lived that polls.

This is the reason an action (§3) must never poll the bus itself — see §4.

---

## 3. Two kinds of script, and only one of them is reloaded per call

The word "script" covers two things in this app, and conflating them is what makes "reload"
look harder than it is.

**A policy** is a `.py` file directly in `%APPDATA%\AIInterface\scripts\`, run by the
bootstrap on its own daemon thread (`src/pyhost/aii_pyhost.cpp`) in a loop over `aii.wait()`,
for the life of the app. `assets/scripts/examples/handoff_policy.py` is one. It is
long-lived, it holds state, it owns a thread, it has a bus cursor. A policy that changes
costs a restart; there is no live reload of a running policy, and that is a deliberate
scope decision rather than an oversight — see the reasoning below.

**An action** is called, does something, and returns. It holds no state between calls, owns
no thread, and has no cursor. This is what the AI writes and calls conversationally.

**The decision: this app makes actions reloadable by construction and does not make policies
reloadable.** The reasons:

1. Actions are what the model is asked to produce — something with a caller that runs and
   returns — not a hot-swappable long-running policy.
2. An action has no reload problem at all. A call is `exec` of the file's source in a fresh
   namespace (§4), so re-reading the file on each call *is* the reload; there is nothing left
   over to go stale — no thread, no cursor, no registration.
3. A policy's reload problem is open-ended (sub-interpreters, or a re-entrancy convention for
   in-flight state) against a population of long-lived scripts that is small by design. Solving
   it before there is a concrete need would be unused machinery.
4. The same property serves iterating on a policy under development: write it as an action and
   call it from a loop in the file being edited, and each edit takes effect on the next call
   with no restart.

A `.py` file dropped directly in `scripts\actions\`, or in `scripts\` itself, is discovered by
`ScriptHost` on a non-recursive scan (`src/avatar/script_host.cpp`), so a policy and an action
can never be confused by directory alone — a leading `_` or `.` on either still means "not
this one".

### What an action is, concretely

A `.py` file in `%APPDATA%\AIInterface\scripts\actions\`, defining `run()`. Its **name is its
filename** and its **description is the first line of its module docstring**
(`src/avatar/action_store.h`, `src/avatar/action_store.cpp`).

That is the whole definition. There is no registry, no manifest, no version, no permission
block and no API to implement, because this app already decides several things exactly this
way: a script runs because a `.py` sits in one flat directory, an avatar exists because a
folder has its name, a prompt body is a Markdown file named in `graph.json`, and a leading `_`
or `.` already means "not this one". **The filesystem is the registry.** A manifest would be a
second place for the same fact, and this repo's own rule (`bus_bindings.h`) is that nothing
may be a second implementation of anything, because a second one can drift.

`actions\` is a subdirectory of `scripts\`, scanned non-recursively, so **adding actions
cannot accidentally turn an action into a policy** — the policy scan will not see them. That
is the same opt-in boundary `examples\` relies on (`src/avatar/script_host.h`).

`scripts\tmp\`, beside `actions\`, holds throwaway actions: everything in it is callable the
moment it is written, with no one-time approval, because the folder itself — not any record
the app keeps — is the trust boundary. It can be cleared at any point, by the user or by a
later version of the app, and nothing written there is remembered across a restart. It exists
because an action meant for one occasion should not cost the same click as one meant to be
kept; a script the user wants to keep belongs in `actions\`, where it goes through the normal
one-time approval in §6.

---

## 4. Reload semantics

**An action is `exec`'d from source in a fresh namespace on every call.** There is no cache,
no `importlib.reload`, no sub-interpreter, and no unload.

- Python cannot truly unload a module, so an action call does not create one:
  `runpy.run_path` / `exec(compile(src))` into a fresh `dict` leaves nothing in `sys.modules`
  to go stale.
- Re-reading per call means the file on disk is always the authority. "Reload" is not an
  operation the user or the AI ever has to ask for, and there is no half-reloaded state to be
  in, because there is no state.
- The cost is a compile per call. For an action that is tens of lines and called
  conversationally, this is not a measured bottleneck.
- If an action imports a helper library, that module *is* cached by Python and *is* stale
  until restart. This is a real limitation, documented rather than worked around, because
  working around it is the module-unload problem again.

**The requirement this creates, from §2.5:** an action must never call `aii.poll()`,
`aii.wait()` or `aii.poll_lines()` directly. Doing so registers a per-thread cursor that is
never erased, and whose thread id Windows may later recycle onto a live script. Two things
close this:

1. **Every action runs on one long-lived dispatcher thread**, which owns the only cursor.
   The dispatcher polls; actions never do. This is the structural fix.
2. **The cursor is erased** when the thread that had one is done with it, closing the
   recycled-id hole for every future caller as well, not just actions.

### Where an action runs, and why not on the turn thread

The call arrives from `VoiceSession::run_commands()`, which runs on the turn thread
(`src/avatar/voice_session.cpp`). An action does not run there: the turn thread produces the
reply, and a slow action would stall speech. It is dispatched the way every other verb in that
function is — by posting a bus line — and executed on the Python dispatcher thread. This is
not a special case: `run_commands()` already queues rather than acts for other verbs for the
same reason.

**A call is therefore fire-and-forget: the model does not get the action's return value in
the turn that called it.** What the action can do instead is speak (`aii.say`), log
(`aii.log`), put a line in the settings surface (`aii.status`), or post a bus event the app
publishes — all of which reach the model on the following turn through the same path as
everything else.

---

## 5. What is never exposed

The exclusion policy in §2.2 is stated here as three rules, plus a fourth this section adds
for scripts specifically:

1. **A verb must land on a door the app already has.** A verb that needs new behaviour
   written for it is not a binding, it is a feature, and it is built as one.
2. **A gesture whose meaning is its physical performance is not exposed.** Hold-to-dictate is
   the worked case: its meaning is duration and pointer position, a script has neither, and
   its two useful outcomes are already reachable another way.
3. **A verb may not reach a capability the untrusted entry point cannot already produce.**
   `ButtonActionKind::Invoke` — the one that runs a command string — is unreachable from the
   bus, and there is no verb that could produce one. This is the rule that forbids
   `button.press`.
4. **A name, never a path and never a body.** This is `PromptInjector::request()`'s existing
   rule (`src/core/prompt_store.h`), verbatim: *"`name` is matched against node ids, titles
   and triggers; anything that does not resolve is refused. It is never a path and never a
   body — a `load` line cannot name a file, read one, or introduce a byte of text the store
   does not already contain."*

   The `run` verb follows the same rule. `run name=x` resolves `x` against the set of actions
   the app has already discovered and refuses anything else. It cannot name a path, cannot
   pass Python source, and cannot cause a file to be read that the app did not already find.
   The consequence: **the channel the model speaks through can only ever select from a set the
   app built by looking at a directory.** Getting new code into that set is a separate act
   with a separate gate, which is §6.

**What that leaves deliberately unexposed, named:** hold-to-dictate; pressing a registered
button by name; running a command string; opening or closing OS windows (sidebar, inspector,
worker windows); quitting the app; the settings keys `src/avatar/settings.cpp` marks
`NotSettable` and refuses *by name with a reason* rather than by omission (the `inspector.*`
window-geometry keys, `window.dodge_watermark`, `version`, and the script gates
`scripts.authoring` and `scripts.auto_allow` themselves); and the CLI's own `CLAUDE.md`,
auto-memory and slash commands, which the conversational instance never sees because it runs
`--safe-mode --disable-slash-commands`.

The `NotSettable` rows are a pattern worth reusing, not just citing: a real key that nothing
running owns should be **listed and refused with its own sentence**, not hidden, because the
hazard of an invented key is that it is inert, and inert failures are the ones that turn up
unexplained later. An action the app found but will not run — because authoring is off, or
because the user has not armed it yet — is visible and refused the same way, for the same
reason (`ActionRefusal` in `src/avatar/action_store.h`).

---

## 6. Authoring, review and undo

### How the AI writes one

**The model writes the file itself, with its own `Write` tool, into `scripts\actions\`. The
`scripts.authoring` toggle governs whether the app will load and offer what it finds there.**

An app-mediated write — where the AI names something and the app writes the file, as it does
for settings — was considered and rejected here, for two reasons, the second decisive.

**It does not fit the grammar.** The ```aii``` block is a line-oriented parser of
`verb field=value` (`parse_commands()`, `src/core/worker_pool.cpp`), whose value rules are
token-or-rest-of-line. A Python body is neither one line nor `key=value`; carrying source
through it needs a new multi-line parser in the message where a parse failure is most
expensive and least recoverable.

**More importantly, an app-mediated write would not actually gate anything.** `file_write`
grants exactly `Write,Edit` (`src/core/tool_policy.cpp`), and they reach the child as
`--tools`/`--allowedTools` together with `--permission-prompts none`
(`src/llm/claude_code_client.cpp`) — nothing asks the user to confirm first and nothing the
model does through those tools can be undone. A model with `file_write` on can already put a
`.py` anywhere on disk, including in `scripts\actions\`, and no app-side design can prevent
that. So gating the *write* was never an option that existed; the only gate available is on
whether the app loads and offers what it finds, and that is the one it uses.

Writing the file with `Write` means the artefact is a plain `.py` on disk, which is also half
the answer to "who reviews it": the user can open it, diff it, edit it and delete it with the
tools they already have, and nothing about it is opaque or app-owned.

**Authoring therefore requires `tools.file_write` on as well as `scripts.authoring`.** These
are two gates in series for the highest-risk act in the system, and they are not the same
gate: `file_write` says "may put bytes on disk", `scripts.authoring` says "may that code run
inside me". Neither implies the other.

### The toggle

`scripts.authoring` is a `settings.json` key, default off, listed in `kSettingKeys`
(`src/avatar/settings.cpp`) like every other setting, marked `NotSettable` so the model cannot
flip its own gate (§5). Its cost class is `Live`: unlike `tools.file_write`, which is
`SettingCost::Restart` because it becomes a command-line argument fixed when the child starts,
`scripts.authoring` changes only what the app itself will load. It reaches no command line and
costs nothing to turn on mid-conversation, which matters, because it is usually turned on in
the middle of the exchange that wanted it.

**It gates loading, not writing.** An action file that exists while the toggle is off is
found, listed in the settings surface, and refused with a reason when called — the
`NotSettable` pattern from §5. It does not silently not-exist, because that is the inert
failure a hidden key would produce.

### What the user sees, and the arming queue

A newly discovered action is not callable on sight. It is added to an approval queue
(`ActionStore::awaiting()`, `src/avatar/action_store.h`) and shown in one window listing every
pending action together — never a window per action, because the model can write several
files in one turn. Arming an action from that window is **per action, once, forever**: after
the user arms it, every later call is silent. Dismissing it is "not now" and nothing else —
the file stays on disk, the action stays listed as not armed, and it can be armed or deleted
later from the Scripts row in the settings panel; a mis-click costs nothing.

`scripts.auto_allow` (also `NotSettable`) is the escape from this queue: when on, a newly
discovered action is armed on sight and no window opens. It is default off, because the
useful case is the one where the user is asked.

The digest sent to the model marks an unarmed action's row so the model can tell the user to
go and arm it, rather than the app writing a paragraph of explanation on every turn (§9).

`scripts\tmp\` is the other escape valve, described in §3: an action written there runs the
moment it is named in a `run` line, with no approval step, because the folder itself — cleared
at any time, remembering nothing across a restart — is the trust boundary. A script meant to
last is rewritten into `actions\`, where it goes through the queue above.

### Undo

**Undo is deleting the file, from the Scripts row in the settings surface.** No versioning, no
quarantine, no trash — the artefact is a `.py` in a known directory and the user already has
the tools to inspect and remove it. **The model cannot delete its own actions**: the delete
path has exactly one caller, and it is a button the user presses. An AI that could both write
and unwrite its own actions would be harder to reason about than one that can only write, and
nothing requires it.

---

## 7. The failure boundary

Because §2.3 establishes that a script cannot stall the frame loop, the failure boundary is
not about protecting the renderer. It is drawn around three narrower things:

1. **A throw.** The bootstrap wraps each script in a `try` and sends the last line of the
   traceback to the Scripts section via `aii.status()` (`src/pyhost/aii_pyhost.cpp`), because a
   script that fails must say so where the user will see it. An action call gets the same
   wrapper.
2. **A hang.** A Python thread cannot be killed. The answer is not a timeout that kills but a
   boundary that contains: each call runs on its own daemon thread, so a hung action costs one
   leaked thread and never the dispatcher, and the app still exits, because daemon threads do
   not hold the process open (`src/pyhost/aii_pyhost.cpp`). A call that has not returned after
   a stated wall-clock time gets a line in the Scripts section saying so, and the action is
   marked as still running; concurrent calls of the same action are refused while one is
   outstanding. That is containment and honesty, which is all that is available here, and it
   should be described as such rather than as a timeout.
   — Note the interaction with §4's dispatcher-owns-the-cursor rule: a per-call thread must
   still never poll the bus itself. Both hold together only because the cursor rule is
   structural, which is why §4 also erases dead cursors rather than relying on discipline
   alone.
3. **A flood.** `kBusInboxMax = 64` drops the *newest* inbound message and logs a counted
   refusal (`src/core/app_bus.h`, `src/core/app_bus.cpp`); `kBusEventsMax = 256` caps outbound
   with keyed coalescing (`src/core/app_bus.h`); a refused `post` is not an exception
   (`src/pyhost/aii_pyhost.cpp`); and every avatar grab is a lease with a ceiling that the
   microphone outranks (`src/avatar/bus_bindings.h`).

---

## 8. What may call a script

**The AI may call an action. A schedule may call an action. A policy script may call an
action. A toolbar button may not, and the bus's untrusted end may not introduce one.**

- **The AI**, via the ```aii``` verb `run name=…`, resolving by name against the discovered
  set and nothing else (§5, rule 4). It is dispatched from `run_commands()`
  (`src/avatar/voice_session.cpp`), not from the parser. Note the trap this avoids: `button`
  is applied *inside* `parse_commands()` itself, calling `ButtonRegistry::add_path_button`
  directly and never returning to the caller — so a verb added by copying `button` would
  execute during parsing, on whatever thread happened to be parsing, with no session context
  and no way for the session to refuse it. `run` is dispatched the way `setting` is instead:
  queued and applied on the turn thread with full context.
- **Another script**, trivially, because Python can call Python and forbidding it would be
  theatre.
- **A toolbar button: no.** This is rule 3 in §5 and it is not a close call.
  `ButtonActionKind::Invoke` is deliberately unreachable from the bus, and a button that runs
  an action is `Invoke` by another name. A worker and the assistant can both register buttons
  (`src/avatar/bus_bindings.h`), so a run-on-click button would let anything that can register
  a button cause arbitrary discovered code to run on a user click that looks innocuous. A
  button that runs an action would need to be a button *the user made*, which is a different
  feature with a different trust story and does not exist.
- **A schedule calling an action does not exist yet.** `ScheduleAction` currently carries two
  kinds — `worker`, which starts a background worker, and a bare timer that speaks a stored
  sentence — and `deliver_schedule()` (`src/avatar/voice_session.cpp`) dispatches only on
  those two. A third kind that calls a named action the way a schedule already carries a
  worker's name would be a small, additive change (`src/core/schedule.h` already shapes
  `ScheduleAction` this way), but it has not been built. "Remind me every morning by doing X"
  is not yet possible; "remind me every morning" and "run this worker in ten minutes" both are.

**Is it the same permission whoever calls it?** Calling a discovered action is the same act
regardless of caller, so `scripts.authoring` and per-action arming (§6) cover every caller
uniformly. There is one asymmetry worth stating for whenever a scheduled or policy-driven
caller is added: an action that has never been run once from a live turn should not be the
first thing that fires unattended, at a time when nobody is watching to notice it went wrong.

---

## 9. What the script list costs the prompt

The model must be told which actions exist, and the pattern follows the settings digest: data
generated at launch, substituted into a Markdown file the user can edit
(`assets/prompts/system/settings.md` holds `{{settings}}`; `settings_digest()` in
`src/avatar/settings.cpp` generates it).

**The cost.** The shipped system prompt is on the order of 26 KB across the Markdown files in
`assets/prompts/system/`, of which `workers.md` is the largest single file. The settings
digest adds one row per setting, of the form `key = value  -- shape; cost`, on the order of
450 tokens by a character-count estimate (not a tokenizer count).

A script list is cheaper per row than the settings digest, because a row is just a name and
one sentence with no shape and no cost class: `name — first line of the docstring`. At around
60 characters a row, ten actions cost roughly 600 bytes and fifty cost roughly 3 KB — a
meaningful fraction of the whole system prompt, paid on every turn forever if left uncapped.

**So it is bounded, and the bound is a design decision:**

- **A cap on the number of actions listed (`kActionDigestMax`), and a cap on the description
  (`kActionDescMax`)** — both in `src/avatar/action_store.h`. The description is the first
  line of the docstring, truncated; a docstring may be as long as its author likes, but only
  its first line is ever paid for on every turn.
- **Actions past the cap are not listed and are refused by name with a reason** (the §5
  `NotSettable` pattern), so growing past the cap fails visibly rather than by quiet omission.
- **No prose is added around the list.** Every attempt to add explanatory prose around a
  digest has bought hallucinated readings rather than better ones; `settings.md`'s own
  framing is a short paragraph around its digest, and that is the budget to match, not exceed.

The cap is worth insisting on because the nearest comparable mechanism has none:
`PromptInjector` applies no byte cap at all to an injected prompt body — `decorate()`
(`src/core/prompt_store.cpp`) prepends whole files with no length check. It gets away with
that because its bodies are human-authored files the user put in the store, each injected at
most once per session. Neither of those holds for a list of actions the AI wrote itself and
that is paid on every turn, so the bound has to be explicit here in a way it never had to be
there.

**The second half of the prompt cost is free**, and is the mechanism behind principle 3 in
§1. A *newly written* action does not need the system prompt at all: it goes into the turn
through `pending_context()` (`src/avatar/voice_session.cpp`), which already composes a
`<context name="Pending" kind="state">` block from live state and already returns an empty
string when there is nothing to say. A block naming actions created or armed since the last
turn costs one line on the turn after an action changes state and nothing on every other
turn — a property the system prompt itself cannot have, because it is fixed when the child
starts.

`pending_context()` only runs on user turns, never on an injected one (an injected turn is the
app reporting one finished thing, and must not also be handed a backlog), and a newly written
action is always reported after a user turn by construction, so this costs nothing extra.

---

## 10. How the work is scoped

The system described above splits into three pieces that can be reasoned about and tested
separately, and the split matches how the boundaries above were argued for:

- **Discovery and dispatch.** `scripts\actions\` discovery, the dispatcher thread that owns
  the one bus cursor, the `run name=` verb and its refusal reasons — §3, §4, §5, §8.
- **The call boundary.** The per-call daemon thread, concurrent-call refusal, the
  outstanding-call row, and the traceback line on failure — §7.
- **Authoring.** The toggle, the arming queue, the Scripts surface with arm/dismiss/delete,
  and the `tmp\` escape valve — §6.

The one piece of the original design space that turned out to need nothing new was widening
the verb surface (§2.1): the existing 35 verbs plus `send()` for anything unbound already
covered what a script needed, and `bus_bindings.h`'s own rule — a verb must land on a door the
app already has — is the reason to keep it that way rather than adding verbs speculatively.
The one piece that remains open, not because it is hard but because nobody has needed it yet,
is a schedule that calls an action by name (§8).

---

## 11. Where this is implemented

The primary files, if you are changing this system rather than just using it:

- `src/avatar/action_store.h` / `.cpp` — discovery, arming, the digest, refusal reasons.
- `src/pyhost/aii_pyhost.cpp` — the `aii` module, the bus cursor fan-out, the dispatcher.
- `src/avatar/script_host.h` / `.cpp` — the non-recursive scan that finds policies and
  actions, and the frame-loop tick that drains the engine's own queue.
- `src/avatar/bus_bindings.h` / `.cpp` — the verb-to-door mapping and the exclusion policy.
- `src/core/app_bus.h` / `.cpp` — the bounded inbox and outbox behind every verb.
- `src/core/prompt_store.h` / `.cpp` — prompt nodes, `load`, and why they are a separate
  mechanism from actions.
- `src/avatar/voice_session.cpp` — `run_commands()`, `pending_context()`, `deliver_schedule()`.
- `src/avatar/settings.cpp` — `scripts.authoring`, `scripts.auto_allow`, and the `NotSettable`
  pattern.
- `assets/scripts/AII-MODULE.md` and `assets/scripts/examples/*.py` — the user-facing
  reference and worked examples for anyone writing a script rather than the app that runs it.
