# The `aii` module — everything a script can do

This is the reference for the module every script in this folder gets for free.
It is seeded here by the app; edits are overwritten when a newer copy ships.

A script is a `.py` file **directly in this folder** — one level up from
`actions\` and `examples\`. It runs for the life of the app, on its own thread.
An *action* lives in `actions\`, defines `run()`, and is called by name instead.
`tmp\` is also actions, called the same way, but armed the moment they are
written there -- no approval, because the folder itself is temporary and may
be cleared at any time, by the user or by a later version of the app, and
remembers nothing between sessions. It is the default place to write anything
meant for one occasion; `actions\` is for a script the user wants to keep, and
each new file there asks once. `lib\` is neither: it holds helper packages such
as `aii_ui` that a script or action can `import`, and it is never scanned for
scripts of its own.

```python
import aii

def run():
    aii.say("Ready.")
```

Nothing here blocks the app: a script that hangs, throws or floods the bus is
contained, and its last error line appears in the settings panel under Scripts.

## Saying and showing

| Call | What it does |
|---|---|
| `aii.say(text, echo="")` | Speak a sentence, through the same path a reply takes |
| `aii.log(text)` | A line in the app's log |
| `aii.status(text, ok=True)` | A line in the Scripts section of the settings panel |
| `aii.chat(on=True, echo="")` | Open or close the chat panel — needed before anything fenced is visible |

## Being quiet, and listening

| Call | What it does |
|---|---|
| `aii.mute(on=True, echo="")` | **Mute Claude's own voice.** Replies still arrive as text; audio already playing is cut |
| `aii.mic(on=True, echo="")` | Open or close the microphone |
| `aii.auto_listen(on=True, echo="")` | Whether the mic latches open once the engines are up |
| `aii.listen_timeout(seconds, echo="")` | How long a latched mic waits before closing; 0 never closes |
| `aii.stop(echo="")` | Cancel the reply in flight, close the mic, pause every worker |
| `aii.reset(echo="")` | Throw the conversation away and start a new one |
| `aii.handoff(echo="")` | Hand this session over to a fresh one, carrying a summary |

## The avatar

| Call | What it does |
|---|---|
| `aii.play(clip, hold=0.0)` | Play a named clip |
| `aii.release()` | Give the avatar back to the app's own policy |
| `aii.sprite(name, on=True)` | Show or hide one of the sub-sprites |
| `aii.cells(cells)` | Write cells directly: a list of `[x, y, rgba]`, up to 64×64 |
| `aii.clear_cells()` | Drop what `cells()` wrote |
| `aii.load_avatar(name)` | Switch to another avatar definition |
| `aii.theme(name)` / `aii.colour(value)` | Its palette |
| `aii.avatar_mode(value, echo="")` | `always`, `when_talking` or `hidden` |

## The window and its buttons

| Call | What it does |
|---|---|
| `aii.button(id, label, tip, path)` | Add a toolbar button that opens a folder |
| `aii.clear_buttons()` | Remove the ones a script added |
| `aii.open_settings(on=True, echo="")` | Open or close the settings surface |

## Later, and elsewhere

| Call | What it does |
|---|---|
| `aii.schedule(in_, say="", task="", cwd="", name="", label="", grade="", echo="")` | Do something later: `say=` speaks those words, `task=` sends a worker |
| `aii.cancel_schedule(id, echo="")` | Call one off |
| `aii.list_schedules(echo="")` | Ask what is pending |

## Workers

| Call | What it does |
|---|---|
| `aii.tell(name, text, echo="")` | Pass a note to a worker by name, without starting a new one -- answered by `worker.told` |

## Settings and state

| Call | What it does |
|---|---|
| `aii.settings_info(echo="")` | Ask what every setting is; answered by a `settings.info` event |
| `aii.session_info(echo="")` | Ask about the session — model, usage, state |
| `aii.model(name, echo="")` | Change the model (restarts the conversation) |
| `aii.model_worker(name, echo="")` | Change the model background workers run on (no restart; reaches the next worker spawned) |
| `aii.tools(group, on=True, echo="")` | Turn a tool group on or off (`web`, `file_read`, `file_write`, `browser`) |
| `aii.language(english, japanese, echo="")` | Which languages are on |

## Listening to the app

The app publishes events; a script reads them. This is the loop the module is
shaped for:

```python
import aii

while not aii.should_quit():
    for e in aii.wait(0.2):
        if e["t"] == "session.state" and e["value"] == "listening":
            aii.play("listen")
```

| Call | What it does |
|---|---|
| `aii.poll()` | Every event since the last call, as dicts. Never blocks |
| `aii.wait(timeout=0.1)` | The same, but sleeps for the first one. Returns early on shutdown |
| `aii.should_quit()` | True once the app is closing |
| `aii.send(t, **fields)` | Post any message, including one with no helper here |
| `aii.unsubscribe()` | Forget this thread's event cursor, before a thread that has polled exits. The bootstrap already does this for the threads it starts |
| `aii.scripts` | The scripts this run found |

Events worth knowing: `session.state`, `session.level` (mic and speaker
loudness), `session.usage`, `worker.state`, `turn.text`, `settings.info`.

## Windows

A script can open a small ImGui window beside the chat -- a project's build
status, a progress bar, a log, anything you would otherwise only be able to
say. That is `aii.ui`, plus a Python toolkit (`aii_ui`, seeded in
`scripts\lib\`) built on top of it with a ready-made status panel. It is
enough to be its own document: read `AII-UI.md`, next to this file, before
you open one.

## Two things that are deliberately missing

There is **no way to press a button by id**, and there never will be: half the
controls are levels rather than presses, so a retried `mute` would unmute.

There is no way to **quit the app**. `should_quit()` is the app's word to the
script, not the other way round.
