# M10.1 — What a script is

A design note, written 19 Sep 2026, before any of M10.2–M10.5 is built. Every later task in
M10 depends on the answers here, which is why it is a document and not code.

Its shape is set by two things the user has already settled and one thing this repository
turned out to already contain. The settled things are restated in §1 and are not reopened.
The thing already built is §2, and it moves more than it looks like it should: **most of
M10's "wider surface" exists, the failure boundary M10.4 was convened to build is mostly
already there, and the reload problem is much smaller than the milestone states — but it is
smaller for a reason that also makes one of the milestone's own sentences wrong.**

Every factual claim about existing code in this note carries a file and a line. Where I am
inferring rather than reading, the sentence says so. §8 is the ledger.

---

## 1. Settled by the user, and not reopened here

1. **There is no "skill". There is a script**, and a script is a reusable action the AI can
   call and can write (user, 19 Sep 2026). The prompt's only job is to say which ones exist.
2. **Authoring has its own toggle, default off** — not the file-writing toggle. Writing a
   file you asked for and adding code that runs inside the app on every later call are
   different risk classes, and the second never asks again.
3. **A newly written script is callable on the next turn**, through the per-turn context the
   app already composes (`VoiceSession::pending_context()`, `src/avatar/voice_session.cpp:2294`),
   not by recomposing the system prompt — which is a launch argument, and since M3.12
   restarting the child discards the conversation.

---

## 2. What is already built, and what that changes

### 2.1 The surface is already 35 verbs wide

`src/pyhost/aii_pyhost.cpp` exposes 35 functions on the `aii` module: `post`, `poll_lines`,
`wait_lines`, `should_quit`, `log`, `status`, `play`, `release`, `sprite`, `cells`,
`clear_cells`, `load_avatar`, `theme`, `colour`, `button`, `clear_buttons`, `schedule`,
`cancel_schedule`, `list_schedules`, `mic`, `mute`, `stop`, `reset`, `handoff`, `say`,
`session_info`, `model`, `tools`, `language`, `auto_listen`, `listen_timeout`, `chat`,
`open_settings`, `avatar_mode`, `settings_info` — plus `poll`, `wait` and `send` defined in
the Python shim at `aii_pyhost.cpp:542-567`. `send()` posts *any* bus message, including one
the module has no helper for, so the module is not even the limit; the bus families are.

That is the M2.9 control inventory (`a02e16e`) and it is what M10.3 was going to widen.
**It is already the whole panel.** `src/avatar/bus_bindings.h:34-59` states the rule it was
built to — every verb lands on the call the control itself makes, so nothing on the bus is a
second implementation of anything.

### 2.2 The "what is never exposed" question already has a written answer

`src/avatar/bus_bindings.h:110-119` is headed **"What is deliberately not here"** and gives
reasons, not a list: hold-to-dictate is excluded because its meaning is *how long it lasted
and whether the pointer was still on the button*, which a script does not have, and exposing
its halves would mostly buy a script the ability to open the microphone and never close it;
the sidebar folder button, the inspector and the worker windows are excluded because they
open OS windows, which is not app state and cannot be observed on the bus; quitting is
excluded because `should_quit()` is the app's word to the script and not the reverse.

`bus_bindings.h:39-52` adds the rule that killed the obvious verb: **there is no
`button.press` and there never will be**, because half the controls are levels rather than
presses (`mute` retried would unmute) and a press-by-id verb would reach
`ButtonActionKind::Invoke`, which is deliberately unreachable from the bus.

M10.1 does not need to invent an exclusion policy. It needs to *name the one already being
followed*, which §5 does.

### 2.3 The milestone's frame-loop sentence is wrong

`docs/MILESTONES.md:2881` says "`drain_events()` is on the frame loop today", and offers
that as the reason a script needs a failure boundary. Checked against the code, that is true
of one caller and false of the one that matters:

- `AppBus::drain_events()` is defined at `src/core/app_bus.cpp:152`.
- It is called **on the frame loop** only from `BusFileHatch::poll()`
  (`src/avatar/bus_bindings.cpp:937`), which is guarded by `if (out_ok_)` — i.e. only when
  `--bus-out` is passed, a debug flag.
- It is called **off the frame loop** from `take_for_this_thread()`
  (`src/pyhost/aii_pyhost.cpp:57`), on the Python script threads.
- `ScriptHost::tick()` (`src/avatar/script_host.cpp:292-295`) does run on the frame loop
  (`src/avatar/main.cpp:2036`), but it drains the **engine's** `rend::renderer::MessageQueue`,
  not the app bus, and `script_host.cpp:157-159` says why: nothing consumes it and `tick()`
  is a bin that empties it.

So **a script cannot stall the frame loop**, and `src/avatar/script_host.h:36-41` already
claims this with a measurement behind it: "60 fps held through a 5 s sleep and a 3 s
GIL-holding busy spin". The app's own side is a short mutex around bounded deques, and
`AppBus::apply_pending()` (`src/core/app_bus.cpp:275`) swaps the inbox out under the lock and
runs handlers outside it, bounded by `kBusInboxMax = 64` (`src/core/app_bus.h:112`).

This does not delete M10.4. It relocates it: see §7.

### 2.4 The prompt store's "skill" half has never had a node in it

`assets/prompts/graph.json` ships three graphs; `"skill"` is `{"nodes": [], "links": []}`.
`PromptKind::Skill` (`src/core/prompt_store.h:47`) and its `triggers` field
(`prompt_store.h:56`) have existed since M3.3 and nothing has ever authored one. The
machinery is fully wired — `injector_.reset()` at `src/avatar/voice_session.cpp:435`,
`injector_.decorate()` on every turn at `voice_session.cpp:1764` — so trigger injection runs
every turn and matches nothing. And it cannot match anything, because **`PromptStore::save()`
(declared `prompt_store.h:118`, implemented `prompt_store.cpp:224`) has zero callers in
`src/`**: no node has ever been authored at runtime by the model, the bus or the UI. The
graph editor that would have authored them is M6, which the user removed.

The M10 entry hopes "a script that can write the store is the authoring route M6 was going to
be".

**With the user's answer in hand, that hope should be dropped, not built.** The user's
sentence is about *doing*, not about prompt text. A prompt node is text injected into a turn;
a script is a callable. Keeping the empty `skill` graph alive as M10's authoring target
would reintroduce, under a new name, exactly the thing the user just deleted. The `skill`
graph should be left empty and, when someone is in that file anyway, removed.

### 2.5 The one existing bug shape that reload will land on

The milestone asks for "a documented case of exactly that shape (`drain_events()` and two
consumers)". It is M2.6, commit `56ddd6e`, and the write-up is at `docs/MILESTONES.md:1264-1271`
and `src/pyhost/aii_pyhost.cpp:33-45`: with two scripts running, **the second received
exactly zero events, silently** — no error, no log line, no dropped counter. One queue,
destructive drain, whoever calls first takes everything. Fixed by fanning out to a per-thread
cursor (`aii_pyhost.cpp:55-71`), capped at `kSubMax = 256` (`aii_pyhost.cpp:48`).

That fix has a property that matters to M10.2 and has not been noticed before:
**`g_host.subs` is never erased.** The map is written at `aii_pyhost.cpp:60` via
`operator[]` on `std::this_thread::get_id()` and there is no `erase` anywhere in the file
(grepped: the only occurrences of `subs` are lines 39, 41, 45, 60, 61). Two consequences,
neither of which exists today because nothing is ever reloaded:

- A reloaded script's dead thread leaves a cursor behind, and every later drain copies every
  event into it forever. It is bounded in memory (256 lines) but the per-event copy cost
  grows linearly in the number of dead cursors.
- Windows recycles thread ids. A new script thread that happens to receive a dead one's id
  **inherits up to 256 stale events on its first poll**. That is a silent wrong-data bug of
  precisely the M2.6 shape, and it is latent in the reload design before a line of reload is
  written.

**This is the single most important thing this note found**, and it is a hard requirement on
M10.2 rather than an observation: see §4.

---

## 3. The decision: two kinds of script, and only one of them is new

The word "script" covers two things in this app and conflating them is what makes reload
look hard.

**A policy** is what exists today: a `.py` file in `%APPDATA%\AIInterface\scripts` that the
bootstrap runs on its own daemon thread (`aii_pyhost.cpp:600-607`), which ends in a loop over
`aii.wait()` and runs for the life of the app. `assets/scripts/examples/handoff_policy.py` is
one. It is long-lived, it holds state, it owns a thread, it has a bus cursor. This is the
thing that is genuinely hard to reload.

**An action** is what the user described: *"actions that they're able to reuse and create
themselves"*. It is called, it does something, it returns. It holds no state between calls,
owns no thread and has no cursor.

**The decision: M10 builds actions. It does not make policies reloadable.**

The reasons, in order of weight:

1. **The user's sentence is about actions.** "Call upon to do actions that they're able to
   reuse" is a verb with a caller. Nothing in it asks for a hot-swappable long-running
   policy.
2. **An action has no reload problem at all.** If a call is `exec` of the file's source in a
   fresh namespace, then re-reading the file on each call *is* the reload, and there is
   nothing left over to go stale — no thread, no cursor, no registration. The milestone's
   hardest question stops being a question rather than being answered.
3. **A policy's reload problem is unbounded and buys nothing yet.** There is exactly one
   policy in the repo and it is an example file. Building sub-interpreters or a re-entrancy
   convention for a population of one, before anyone has asked for a second, is the framework
   the brief warns against.
4. **The developer's iteration loop — the milestone's other audience — is served by actions
   too**, because an action re-read per call is the shortest edit-to-effect loop this app
   could have: save the file, say "do it again". A policy under development can be written as
   an action and called from a `while` loop in the file the developer is already editing.

A policy that wants to change therefore still costs a restart, and the note says so out loud
rather than pretending otherwise. If the user later wants live policy swapping, it is a
separate, honestly-priced task and §6 says what it would have to answer.

### What an action is, concretely

A `.py` file in `%APPDATA%\AIInterface\scripts\actions\`, defining `run()`. Its **name is its
filename** and its **description is the first line of its module docstring**.

That is the whole definition. There is no registry, no manifest, no version, no permission
block and no API to implement. The reason is that this app already decides four separate
things this way and it has worked every time: a script runs because a `.py` sits in one flat
directory (`script_host.cpp:145`, `:190-197`), an avatar exists because a folder has its
name, a prompt body is a Markdown file named in `graph.json`, and a leading `_` or `.`
already means "not this one" (`script_host.cpp:195`). **The filesystem is the registry.** A
manifest would be a second place for the same fact, and this repo's own rule
(`bus_bindings.h:57-59`) is that nothing may be a second implementation of anything, because
a second one can drift.

`actions\` is a subdirectory of `scripts\`, and `script_host.cpp:190` iterates
non-recursively, so **adding actions cannot accidentally turn an action into a policy** —
the existing scan will not see them. That is the same opt-in boundary `examples\` already
relies on (`script_host.h:20-25`).

---

## 4. Reload semantics

**Decision: an action is `exec`'d from source in a fresh namespace on every call. There is
no cache, no `importlib.reload`, no sub-interpreter, and no unload.**

- Python cannot truly unload a module — so do not create one. `runpy.run_path` /
  `exec(compile(src))` into a fresh `dict` leaves nothing in `sys.modules` to go stale.
- Re-reading per call means the file on disk is always the authority. "Reload" is not an
  operation the user or the AI ever has to ask for, and there is no half-reloaded state to
  be in, because there is no state.
- The cost is a compile per call. For an action that is tens of lines and called
  conversationally, this is irrelevant; I have not measured it and am not claiming a number.
- If an action wants a helper library, it `import`s it normally and that module *is* cached
  and *is* stale until restart. **This is a real limitation and it must be documented rather
  than fixed**, because fixing it is the unload problem again.

**The hard requirement this creates on M10.2**, from §2.5:

> An action must never call `aii.poll()`, `aii.wait()` or `aii.poll_lines()`. Doing so
> registers a per-thread cursor in `g_host.subs` (`aii_pyhost.cpp:60`) that is never erased,
> and whose thread id Windows may later recycle onto a live script.

Two ways to honour it, and M10.2 should do both:

1. **Run every action on one long-lived dispatcher thread** that owns the only cursor. The
   dispatcher polls; actions never do. This is the structural fix and it costs nothing.
2. **Erase the cursor** when a thread that had one is done with it, and give the `aii` module
   an explicit way to say so. This is a two-line change at `aii_pyhost.cpp:60` in the same
   `lock_guard` and closes the recycled-id hole for every future caller, not just actions.

A one-page throwaway would settle whether Windows recycles ids quickly enough in practice to
make (2) urgent. I did not run it: (1) makes the question moot for the design being proposed,
and the brief's budget is better spent elsewhere. **This is an inference, not a measurement,
and it is flagged as one.**

### Where an action runs, and why not on the turn thread

The call arrives from `VoiceSession::run_commands()`, which runs on the turn thread
(`src/avatar/voice_session.cpp:2351` states this explicitly). An action **must not** run
there: the turn thread is what produces the reply, and a slow action would stall speech.

It is dispatched the way every other verb in that function is — by posting a bus line — and
executed on the Python dispatcher thread. This is not a new mechanism: `run_commands()`
already queues rather than acts for exactly this reason, and `voice_session.cpp:2351-2378`
gives the full argument for why a cancel is queued to the frame loop rather than done where
it is asked.

**A call is therefore fire-and-forget: the model does not get the action's return value in
the turn that called it.** That is a genuine limitation and it is the right one, because the
alternative is blocking the reply on arbitrary Python. What the action *can* do is speak
(`aii.say`), log (`aii.log`), put a line in the settings surface (`aii.status`), or post a
bus event the app publishes — all of which already exist, and the last of which reaches the
model on the following turn through the same path as everything else.

---

## 5. What is never exposed

The exclusion policy is not new and should not be invented. It is the one `bus_bindings.h`
has been following since M2.5, stated as three rules:

1. **A verb must land on a door the app already has.** `bus_bindings.h:8-9`: "every inbound
   verb lands on a door the app already has". A verb that needs new behaviour written for it
   is not a binding, it is a feature, and it goes through the milestone list like any other.
2. **A gesture whose meaning is its physical performance is not exposed.** Hold-to-dictate
   (`bus_bindings.h:110-115`) is the worked case: its meaning is duration and pointer
   position, a script has neither, and its two useful outcomes are already reachable.
3. **A verb may not reach a capability the untrusted entry point cannot already produce.**
   `ButtonActionKind::Invoke` — the one that runs a command string — is unreachable from the
   bus and `bus_bindings.h:20-22` says there is no verb that could produce one. This is the
   rule that forbids the obvious `button.press`.

**To which M10 adds a fourth, and it is the one that matters for code the AI wrote:**

4. **A name, never a path and never a body.** This is `PromptInjector::request()`'s existing
   rule, `src/core/prompt_store.h:311-315`, verbatim: *"`name` is matched against node ids,
   titles and triggers; anything that does not resolve is refused. It is never a path and
   never a body — a `load` line cannot name a file, read one, or introduce a byte of text the
   store does not already contain."*

   The `run` verb inherits it exactly. `run name=x` resolves `x` against the set of actions
   the app has already discovered and refuses anything else. It cannot name a path, cannot
   pass Python source, and cannot cause a file to be read that the app did not already find.
   The consequence is the one that makes the whole feature safe to reason about: **the
   channel the model speaks through can only ever select from a set the app built by looking
   at a directory.** Getting new code into that set is a separate act with a separate toggle,
   which is §6.

**What that leaves deliberately unexposed, named:** hold-to-dictate; pressing a registered
button by name; running a command string; opening or closing OS windows (sidebar, inspector,
worker windows); quitting the app; the five `inspector.*` settings keys and
`window.dodge_watermark` and `version`, which `src/avatar/settings.cpp:302-320` marks
`NotSettable` and refuses *by name with a reason* rather than by omission; and the CLI's own
`CLAUDE.md`, auto-memory and slash commands, which the conversational instance never sees
because it runs `--safe-mode --disable-slash-commands` (M3.5).

The `NotSettable` rows are worth copying as a pattern and not just citing. M3.14's finding
was that a real key nothing running owns should be **listed and refused with its own
sentence**, not hidden, because "the whole hazard of an invented key is that it is inert, and
inert failures are the ones that turn up in a file a week later with nobody able to say how"
(`voice_session.cpp:2915-2919`). An action the app found but will not run — because
authoring is off, or because it failed to compile — should be visible and refused the same
way, for the same reason.

---

## 6. Authoring, review and undo

### How the AI writes one

**Decision: the model writes the file itself, with its own `Write` tool, into
`scripts\actions\`. The authoring toggle governs whether the app will load and offer what it
finds there.**

The alternative considered and rejected was an app-mediated write — the M3.14 pattern, where
the AI names a key and the app writes the file. That pattern is right for settings and wrong
here, for two reasons, the second of which is decisive.

**It does not fit the grammar.** The ```aii``` block is a line-oriented parser of
`verb field=value` (`parse_commands()`, `src/core/worker_pool.cpp:262`), whose value rules
are token-or-rest-of-line (`worker_pool.cpp:302-324`). A Python body is neither one line nor
`key=value`. Carrying source through it needs a new multi-line parser in the one message
where a parse failure is most expensive and least recoverable.

**More importantly, an app-mediated write would not actually gate anything.** `file_write`
grants exactly `Write,Edit` (`src/core/tool_policy.cpp:37`) and they reach the child as
`--tools`/`--allowedTools` together with `--permission-prompts none`
(`src/llm/claude_code_client.cpp:108-110`) — the prompt already tells the model "Nothing asks
the user to confirm first and nothing you do can be undone"
(`assets/prompts/system/workers.md:5`). **A model with `file_write` on can already put a
`.py` anywhere on disk, including in `scripts\actions\`, and no app-side design can prevent
it.** So the authoring toggle gating the *write* is not a choice that was available; the only
gate that exists is on whether the app loads and offers what it finds. This is forced by the
existing tool grant, not selected from options.

Writing the file with `Write` instead means the artefact is **a plain `.py` on disk**, which
is also the answer to half of "who reviews it": the user can open it, diff it, edit it and
delete it with the tools they already have, and nothing about it is opaque or app-owned.

The consequence to state plainly: **authoring requires `tools.file_write` on as well as the
authoring toggle.** That is two gates in series for the highest-risk act in the milestone,
and it matches the user's own reasoning that the risk classes differ — `file_write` says
"may put bytes on disk", the authoring toggle says "may that code run inside me". They are
not the same toggle and neither implies the other.

### The toggle

A new `scripts.authoring` key in `settings.json`, default off, added as a row in
`kSettingKeys` (`src/avatar/settings.cpp:267`) like every other. **Its cost class is `Live`**,
and the contrast with `tools.file_write` is the point: that key is `SettingCost::Restart`
(`settings.cpp:299`) because it becomes a command-line argument and `--allowedTools` is fixed
when the child starts (`src/core/prompt_store.h:266`, `src/avatar/avatar_ui.cpp:1061`), so
turning it on **discards the conversation**. `scripts.authoring` changes only what the app
itself will load, reaches no command line, and therefore costs nothing to turn on mid-
conversation — which matters, because it will most often be turned on in the middle of the
exchange that wanted it.

**It gates loading, not writing.** An action file that exists while the toggle is off is
found, listed in the settings surface, and refused with a reason when called — the
`NotSettable` pattern from §5. It does not silently not-exist, because that is the inert
failure M3.14 warned about.

### What the user sees — and the one thing I cannot decide

The toggle decides *whether*. It does not decide what the user sees afterwards, and the user
has not been asked. **This is the one question in the brief I am putting back rather than
answering**, because every reasonable answer is a different amount of work and they are the
only one who knows how much friction they want:

> **When the AI writes a script, should it (a) run on the next call with only a line in the
> log and a row in the settings surface, (b) require one click to arm it the first time, or
> (c) require the user to read it first?**

My recommendation, if they want one, is **(b)**: the first call of a newly authored action is
refused with a spoken sentence and an armed/not-armed row in the settings surface, and every
call after arming is silent. It costs one click per *new* action rather than per call, which
is the shape of the file-write toggle the user already accepted, and it keeps the loop the
milestone exists for — "write me a script for that", "now use it" — down to one click rather
than a reading session. But (a) is defensible if they trust the sandbox, and (c) is
defensible if they do not, and it is not my call.

### Undo

**Decision: undo is deleting the file, and the app must make that reachable without leaving
the conversation.** No versioning, no quarantine, no trash. The artefact is a `.py` in a
known directory; a "Scripts" row in the settings surface with the action's name, its
one-line description and a delete control is the whole of it. The AI can be asked to delete
one through the same `run`-style channel only if the user decides it may; I would not ship
that in M10 — an AI that can both write and unwrite its own actions is harder to reason about
than one that can only write, and nothing in the user's sentence asks for it.

---

## 7. The failure boundary, and what M10.4 actually is

Because §2.3 establishes that a script cannot stall the frame loop, M10.4 is **not** about
protecting the renderer. The boundary it must draw is around three things, all narrower:

1. **A throw.** Already handled and already correct: the bootstrap wraps each script in a
   `try` and sends the last line of the traceback to the Scripts section via `aii.status()`
   (`aii_pyhost.cpp:582-597`), because "a script that fails must say so where the user will
   see it" (`aii_pyhost.cpp:573-577`). An action call gets the same wrapper. **This is
   done; M10.4 inherits it.**
2. **A hang.** A Python thread cannot be killed. The answer is therefore not a timeout that
   kills but a boundary that contains: **each call runs on its own daemon thread, so a hung
   action costs one leaked thread and never the dispatcher, and the app still exits** because
   daemon threads do not hold the process open (`aii_pyhost.cpp:605`, `:609-611`). A call
   that has not returned after a stated wall-clock time gets a line in the Scripts section
   saying so and the action is marked as still running; concurrent calls of the same action
   are refused while one is outstanding. That is containment and honesty, which is all that
   is available, and it should be written down as such rather than dressed as a timeout.
   — **Note the tension with §4's dispatcher-owns-the-cursor rule:** a per-call thread must
   still never poll. Both hold together only if the cursor rule is structural, which is why
   §4 asks for `g_host.subs` erasure as well.
3. **A flood.** Already bounded: `kBusInboxMax = 64` drops the *newest* inbound and logs a
   counted refusal (`app_bus.h:71-74`, `app_bus.cpp:249`), `kBusEventsMax = 256` caps
   outbound with keyed coalescing (`app_bus.h:63-69`), a refused `post` is not an exception
   (`aii_pyhost.cpp:72-78`), and every avatar grab is a lease with a ceiling that the
   microphone outranks (`bus_bindings.h:170-173`). **This is done; M10.4 inherits it.**

So M10.4 is one genuinely new thing — the per-call thread, the outstanding-call bookkeeping
and the Scripts row that shows it — and the rest is writing down what M2.5 and M2.6 already
built.

---

## 8. What a script may be called by

**Decision: the AI may call an action. A schedule may. A policy script may. A toolbar button
may not, and the bus's untrusted end may not introduce one.**

- **The AI**, via a new ```aii``` verb `run name=…`, resolving by name against the discovered
  set and nothing else (§5 rule 4). **It must be dispatched from `run_commands()`
  (`src/avatar/voice_session.cpp:2809`), not from the parser.** Note the trap: `button` is the
  one verb applied *inside* `parse_commands()` — it calls `ButtonRegistry::add_path_button`
  directly at `src/core/worker_pool.cpp:336` and is never returned to the caller — so a verb
  added by copying `button` would execute during parsing, on whatever thread happened to be
  parsing, with no session context and no way for the session to refuse it. Copy `setting`
  (`voice_session.cpp:2849`) instead.
- **A schedule**, because `ScheduleAction` already carries a `kind` (`say`, `worker`) and
  `deliver_schedule()` already dispatches on it; a third kind is a data change of the sort
  `bus_bindings.h:120-122` describes as "adding a family is a data change". This is what makes
  "remind me every morning by doing X" possible at all, and it is the first thing the user
  will want after the loop works. *(Inferred from the shape of `src/core/schedule.h:129` and
  `ScheduleAction`'s fields as used in `pending_context()`, `voice_session.cpp:2315-2321`; I
  did not read `deliver_schedule()` in full.)*
- **Another script**, trivially, because Python can call Python and forbidding it would be
  theatre.
- **A toolbar button: no.** This is rule 3 in §5 and it is not a close call.
  `ButtonActionKind::Invoke` is deliberately unreachable from the bus, and a button that runs
  an action is `Invoke` by another name. A worker and the assistant can both register
  buttons (`bus_bindings.h:47-51`), so a run-on-click button would let anything that can
  register a button cause arbitrary discovered code to run on a user click that looks
  innocuous. If the user wants a button that runs an action, it should be a button *the user
  made*, which is a different feature with a different trust story.

**Is it the same permission?** **Yes, with one exception.** Calling a discovered action is
the same act whoever initiates it, so the authoring toggle — which gates whether an action
loads at all — covers every caller uniformly. The exception is the arming click, if the user
chooses (b) in §6: an action that has never been armed should not first run from a schedule
at 3 a.m. with nobody watching. **A schedule may only call an action that has already run
once from a turn.**

---

## 9. What the script list costs the prompt

The model must be told which actions exist, and the pattern to copy is M3.14's settings
digest: data generated at launch, substituted into a Markdown file the user can edit
(`assets/prompts/system/settings.md` holds `{{settings}}`; `settings_digest()` at
`src/avatar/settings.cpp:440` generates it).

**The cost, measured.** The shipped system prompt is 14,683 bytes across four Markdown files
(`assets/prompts/system/`), of which `workers.md` is 10,529. The settings digest adds 19 rows
of the form `key = value  -- shape; cost`; summing the table's own strings
(`settings.cpp:267-320`, 1,039 characters of quoted literals) plus per-row fixed text gives
roughly 1.7 KB, on the order of 450 tokens. **That is an estimate computed from the table,
not a token count from a tokenizer**, and it is stated as one.

A script list is cheaper per row than the settings digest, because a row is a name and one
sentence with no shape and no cost class: `name — first line of the docstring`. At ~60
characters a row, **ten actions cost ~600 bytes and fifty cost ~3 KB**, the latter being a
fifth again on top of the entire current system prompt, paid on every turn forever.

**So it must be bounded, and the bound is the decision:**

- **A cap on the number of actions listed, and a cap on the description.** The description is
  the first line of the docstring, truncated — one line, not a paragraph. A docstring may be
  as long as the author likes; only its first line is ever paid for.
- **Actions past the cap are not listed and are refused by name with a reason**, the §5
  `NotSettable` pattern, so growing past it fails visibly rather than by quiet omission.
- **No prose is added around the list.** The brief notes that every previous attempt to add
  prose bought hallucinated readings. `settings.md` is 1,201 bytes of prose around its
  digest and that is the budget to match, not exceed. The list's framing sentence should be
  one sentence.

The cap is worth insisting on because the nearest comparable thing has none. `PromptInjector`
applies **no byte cap at all** to an injected body — `decorate()` (`prompt_store.cpp:439`)
prepends whole files with no length check anywhere. It gets away with it because its bodies
are human-authored files the user put in the store and each is injected at most once per
session (`prompt_store.cpp:445`, `:456`). **Neither of those holds for a list of actions the
AI wrote itself and that is paid on every turn**, so the bound has to be explicit here in a
way it never had to be there.

**The second half of the prompt cost is free, and is the mechanism for consequence 3 in §1.**
A *newly written* action does not need the system prompt at all: it goes into the turn
through `pending_context()` (`voice_session.cpp:2294`), which already composes a
`<context name="Pending" kind="state">` block from live state and already returns an empty
string when there is nothing to say (`voice_session.cpp:2302`). A block naming actions
created since the last turn costs one line on the turns after an action is written and
**nothing on every other turn**, which is exactly the property the system prompt cannot have.

Note the existing rule it must respect: `pending_context()` is **user turns only**
(`voice_session.cpp:1786-1793`) — an injected turn is the app reporting one finished thing
and must not be handed the queue. A newly-written action follows a user turn by construction,
so this costs nothing.

---

## 10. Are M10.2–M10.5 still the right four tasks?

**No. On this design they are three, and two of them shrink.**

| Task | As written | After this note |
|---|---|---|
| M10.2 Reload | load / replace / unload, with an answer for callbacks, subscriptions and schedules | **Mostly deleted.** An action is `exec`'d fresh per call, so there is no load, no unload and nothing left behind. What survives is one real bug fix — erase `g_host.subs` entries and keep polling on one dispatcher thread (§4) — plus discovery of `scripts\actions\`. |
| M10.3 Widen the surface | widen beyond the control inventory | **Delete it.** The surface is already 35 verbs plus `send()` for anything unbound (§2.1), and `bus_bindings.h`'s own rule is that a verb must land on an existing door. There is nothing identified as missing. Re-open it if M10.6 finds something the user actually reached for and could not get. |
| M10.4 Failure boundary | throws, hangs, floods must not take the app with it | **Shrinks to one item.** Throws and floods are already handled and measured (§7); the frame-loop premise is wrong (§2.3). What remains is the per-call thread, outstanding-call bookkeeping and the Scripts row. |
| M10.5 Authoring | how it writes one, where it lands, what the user sees, how it is undone | **Stays, and is now the biggest.** It is the toggle, the `run` verb, the digest, the `pending_context()` block, the Scripts surface with arm/delete, and the user's answer to §6. |

The honest remainder is therefore:

- **M10.2′ — Actions exist and can be called.** `scripts\actions\` discovery, the dispatcher
  thread, the `g_host.subs` fix, `script.run` on the bus, the ```aii``` `run name=` verb.
- **M10.4′ — The call boundary.** Per-call daemon thread, concurrent-call refusal,
  outstanding-call row, and the traceback line that already works.
- **M10.5 — Authoring.** Unchanged in scope, now with §6's question answered by the user.

M10.3 should be struck, and M10.6 — confirm with the user whether the loop is actually
shorter in real use — becomes more important rather than less, because it is now the only
thing that would justify reinstating M10.3.

---

## 11. Verification ledger

Read and quoted: `src/avatar/script_host.h` (whole), `src/avatar/script_host.cpp` (whole),
`src/pyhost/aii_pyhost.cpp:1-200, 540-643`, `src/core/app_bus.h:55-113, 160-210`,
`src/core/app_bus.cpp:120-320`, `src/avatar/bus_bindings.h:1-200`,
`src/core/prompt_store.h:40-60, 285-345`, `src/avatar/settings.h:95-310`,
`src/avatar/settings.cpp:255-330, 435-465`, `src/avatar/voice_session.cpp:1775-1815,
2294-2430, 2809-2960`, `assets/prompts/graph.json`, `assets/prompts/system/settings.md`,
`assets/scripts/examples/handoff_policy.py`, `docs/MILESTONES.md:2795-2905`.

Also read and verified by direct grep during this task: `src/core/worker_pool.cpp:262-360`
(`parse_commands`, the ```aii``` grammar), `src/core/tool_policy.cpp:13-80`,
`src/llm/claude_code_client.cpp:82-129`, `src/core/engines.cpp:69-75`,
`src/core/worker_pool.cpp:103-114`, `src/core/prompt_store.cpp:224-239, 417-475`,
`assets/prompts/system/workers.md:1-17`.

Verified by measurement or direct grep, not inference:

- 35 `m.def` bindings in `aii_pyhost.cpp`; verb names extracted from the source.
- `g_host.subs` has no `erase` — grep for `subs` in `aii_pyhost.cpp` returns lines 39, 41,
  45, 60, 61 only.
- `assets/prompts/graph.json` `"skill"` graph has zero nodes.
- System prompt corpus is 14,683 bytes across four files (`wc -c`).
- `kSettingKeys` is 19 rows, 1,039 characters of quoted literals across the table
  (`sed` + `grep -o` + `awk`).
- `PromptStore::save()` has zero callers in `src/`.
- `--safe-mode --disable-slash-commands` is emitted at exactly one site
  (`claude_code_client.cpp:117`), gated by `suppress_cli_context`, which is set only in
  `build_llm()` (`engines.cpp:75`) and deliberately not in `worker_pool.cpp:103-114`.
- `tools.file_write` grants exactly `Write,Edit` and nothing else (`tool_policy.cpp:37`).
- Nothing in `src/` writes a `.py` from model input, and nothing reloads or `exec`s Python
  after startup. The only generated `.py` is `_aii_boot.py`, written once at startup from a
  string baked into the DLL (`script_host.cpp:132-141`, `aii_pyhost.cpp:640`).

**Stated as inference, not reading:**

- The schedule-calls-an-action claim in §8 is inferred from `ScheduleAction`'s fields as used
  in `pending_context()` and from `src/core/schedule.h:129`; `deliver_schedule()` was not
  read in full.
- The per-call `exec` cost in §4 is not measured.
- Whether Windows recycles thread ids fast enough for the stale-cursor hole to bite in
  practice is not measured; §4 argues it should be closed structurally regardless.
- The ~450-token figure for the settings digest is computed from character counts, not from a
  tokenizer.
