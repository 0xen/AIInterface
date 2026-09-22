# Windows -- the `aii.ui` module and the `aii_ui` toolkit

This is the reference for making a small window beside the chat: a project's
build status, a progress bar, anything you would otherwise only be able to
say. Read `AII-MODULE.md` first if you have not -- everything below is on
top of the `aii` module, not instead of it.

There are two layers. `aii.ui` (section 1) is the raw, ImGui-shaped API: you
record a frame of calls and the app replays them as a real window. `aii_ui`
(section 2) is a Python package built on top of it, seeded beside your other
scripts, and it is what you should reach for first -- most of what you want
to show is a `StatusPanel`, not a hand-written frame.

## 1. `aii.ui`: the raw API

A window is identified by a **key**, a short string you choose (`"build"`,
`"status"`). Opening it and drawing into it are separate steps, and drawing
happens inside a *recording*: everything between `begin_frame` and
`end_frame` is copied into the window's next frame, all at once, and a
widget's return value is the result of the **previous** recording, not this
exact instant -- see "Rate and latching" below.

```python
import aii.ui as ui

ui.open("build", "My Build")            # False (and aii.log) if refused
ui.begin_frame("build")
ui.text("Status: building")
if ui.button("Cancel"):
    ...
ui.end_frame()
```

### Window management

| Call | What it does |
|---|---|
| `ui.open(key, title, w=360, h=240)` | Open a window. Returns `False` when refused (see "The window cap") |
| `ui.close(key)` | Close it |
| `ui.is_open(key)` | `False` once the user closed it themselves |
| `ui.windows()` | The keys currently open |
| `ui.begin_frame(key)` | Start recording this window's next frame; also collects last frame's results |
| `ui.end_frame()` | Submit the recording; returns how many commands it held |

### Text

| Call | What it does |
|---|---|
| `ui.text(s)` / `ui.text_wrapped(s)` / `ui.text_disabled(s)` | A line of text |
| `ui.text_colored(rgba, s)` | Coloured text; `rgba` is 3 or 4 floats, 0..1 sRGB |
| `ui.bullet_text(s)` | A bulleted line |
| `ui.label_text(label, value)` | `"label: value"`, ImGui's own layout for it |

### Layout

| Call | What it does |
|---|---|
| `ui.separator()` / `ui.separator_text(s)` | A rule, optionally captioned |
| `ui.same_line(offset=0.0, spacing=-1.0)` | Keep the next item on this line |
| `ui.new_line()` / `ui.spacing()` / `ui.dummy(w, h)` | Blank space |
| `ui.indent(w=0.0)` / `ui.unindent(w=0.0)` | Shift what follows |
| `ui.begin_group()` / `ui.end_group()` | Treat several items as one for layout |

### Buttons and inputs

| Call | What it does |
|---|---|
| `ui.button(label, w=0.0, h=0.0)` -> `bool` | Clicked this frame |
| `ui.small_button(label)` -> `bool` | Same, compact |
| `ui.checkbox(label, value)` -> `bool` | The (possibly user-changed) value |
| `ui.radio_button(label, active)` -> `bool` | `True` when clicked |
| `ui.selectable(label, selected=False)` -> `bool` | `True` when clicked |
| `ui.slider_float(label, v, lo, hi, fmt="%.3f")` -> `float` | |
| `ui.slider_int(label, v, lo, hi)` -> `int` | |
| `ui.drag_float(label, v, speed=1.0, lo=0.0, hi=0.0, fmt="%.3f")` -> `float` | |
| `ui.drag_int(label, v, speed=1.0, lo=0, hi=0)` -> `int` | |
| `ui.input_text(label, text, hint="", flags=0)` -> `str` | |
| `ui.input_text_multiline(label, text, w=0.0, h=0.0)` -> `str` | |
| `ui.input_int(label, v, step=1)` -> `int` | |
| `ui.input_float(label, v, step=0.0, fmt="%.3f")` -> `float` | |
| `ui.combo(label, index, items)` -> `int` | |
| `ui.list_box(label, index, items, height_in_items=-1)` -> `int` | |
| `ui.color_edit(label, rgba, flags=0)` -> `tuple` of 4 floats | |

### Display

| Call | What it does |
|---|---|
| `ui.progress_bar(fraction, w=-1.0, h=0.0, overlay="")` | |
| `ui.plot_lines(label, values, lo=FLT_MAX, hi=FLT_MAX, w=0.0, h=0.0, overlay="")` | |
| `ui.plot_histogram(...)` | Same signature |
| `ui.set_tooltip(s)` | Hovering the previous item shows this |

### Grouping: headers, trees, children, tabs, tables, columns

| Call | What it does |
|---|---|
| `ui.collapsing_header(label, default_open=False, flags=0)` -> `bool` | |
| `ui.tree_node(label)` -> `bool` / `ui.tree_pop()` | Only pop when the node returned `True` |
| `ui.begin_child(id, w=0.0, h=0.0, border=False, flags=0)` -> `True` / `ui.end_child()` | A scrollable sub-region |
| `ui.begin_disabled(disabled=True)` / `ui.end_disabled()` | Grey out and block input for what follows |
| `ui.begin_tab_bar(id)` -> `True` / `ui.end_tab_bar()` / `ui.begin_tab_item(label)` -> `bool` / `ui.end_tab_item()` | `begin_tab_item` is `True` for the selected tab (the first one, until a click); only put content inside a tab that returned `True` |
| `ui.begin_table(id, columns, flags=0, w=0.0, h=0.0)` -> `bool` | `ui.table_next_row()`, `ui.table_next_column()`, `ui.table_setup_column(label, flags=0, width=0.0)`, `ui.table_headers_row()`, `ui.end_table()` |
| `ui.columns(count=1, border=True)` / `ui.next_column()` | The older, simpler column layout |

### Ids and style

| Call | What it does |
|---|---|
| `ui.push_id(s)` / `ui.pop_id()` | Disambiguate two widgets with the same label |
| `ui.push_style_color(idx, rgba)` / `ui.pop_style_color(count=1)` | `idx` is one of `ui.COL_*` |
| `ui.push_item_width(w)` / `ui.pop_item_width()` / `ui.set_next_item_width(w)` | |
| `ui.set_scroll_here_y(center=0.5)` | Scroll the enclosing child/window to here |
| `ui.COL_TEXT`, `ui.COL_BUTTON`, ... | The `ImGuiCol_*` values, as ints, for `push_style_color` |

A widget you do not record this frame simply is not drawn this frame -- there
is no need to clear anything.

### Rate and latching

**A button answers for the frames since you last recorded, not this exact
frame.** Recording is per thread and happens whenever your code calls
`begin_frame`/`end_frame`; the window itself redraws at its own rate (around
60 fps) from whatever was last submitted. If you record at 2 Hz, a click is
still caught -- results latch until you read them -- but you will not see it
until your next recording. This is also why a dragged slider does not snap
back between recordings: the window keeps your last-submitted value as an
override until you record a newer one.

**Never call `aii.poll()` or `aii.wait()` from the thread that draws a
panel.** A panel's inputs are the window's own widget results and whatever
a state file says (section 3) -- not the event bus. See `AII-MODULE.md` and
the bootstrap's own comment on this for why: the app has exactly one event
cursor, owned by the one dispatcher thread, and a second one left behind by
a thread that exits is a bug that resurfaces later under a recycled thread
id.

### The window cap

The app allows **at most six** `aii.ui` windows open at once (across every
script and action together). `ui.open()` past the cap returns `False` and
`aii.log`s the refusal instead of raising -- check the return value, or call
it through `aii_ui.Panel`/`StatusPanel`, which already do and log for you.
Closing an unused panel before opening a new one is the way around it; there
is no queue.

## 2. `aii_ui`: the helper package

`scripts\lib\aii_ui\` -- plain Python, seeded next to your other scripts and
put on `sys.path` for you. This is what you should write against; drop to
raw `aii.ui` only for something none of this covers.

| Module | What it has |
|---|---|
| `colors.py` | `ORANGE`, `RED`, `GREEN`, `GREY`, `WHITE`, `BLUE`, `YELLOW`; `for_state(state)` |
| `panel.py` | `class Panel` -- the run loop every window in this package uses |
| `status.py` | `class StatusPanel` -- a titled window with a status line, fields, a progress bar and a log |
| `widgets.py` | free functions: `label_value`, `badge`, `status_line`, `log_view`, `key_value_table` |
| `state.py` | `watch_json(path)`, `path_for(name)`, `STATE_DIR` -- the state-file hand-off (section 3) |

### `Panel`

The base every window in this package runs on.

```python
from aii_ui import Panel

def draw(ui):
    ui.text("hello from a panel")

Panel("demo", "Demo Window").run(draw)   # blocks; loop until closed or quitting
```

`Panel(key, title, w=360, h=240, hz=10)`. `run(draw)` calls `draw(ui)` once
per tick at `hz` frames a second, wrapped in `begin_frame`/`end_frame`, until
the window is closed or `aii.should_quit()`. An exception inside `draw` is
caught, reported once with `aii.status(f"{key}: {last line}", False)`, and
the loop keeps going -- a typo in one frame does not take the window down.
`run_in_thread(draw)` starts `run` on a daemon thread and returns it, which
is the usual way to call this from an action.

### `StatusPanel`

The user's own example, exactly: `"X Project Name"` titled, `"Status:
Building"` in orange, red on failure, green on done.

```python
from aii_ui import StatusPanel

panel = StatusPanel("X Project Name")
panel.set_status("Building")
panel.set("branch", "master")
panel.progress(0.4, "compiling")
panel.log("configuring")
panel.run_in_thread()
```

`StatusPanel(title, key=None)` -- `key` defaults to a slug of `title`.
`set_status(text)` colours from `colors.for_state`. `set(field, text,
color=None)` adds or updates a labelled row. `progress(fraction, text="")`.
`log(line)` keeps the last 200 lines in a scrolling region. `watch(path)`
makes `run()`/`run_in_thread()` re-read that JSON file on every tick it
changes and apply it -- see section 3. `open()`/`close()`/`run()`/
`run_in_thread()` as on `Panel`.

### `widgets`

Free functions, `ui` first, for the pieces used inside `StatusPanel` and
reusable anywhere else: `label_value(ui, label, value, color=None)`,
`badge(ui, text, color)`, `status_line(ui, label, state)`, `log_view(ui, id,
lines, h=150.0)`, `key_value_table(ui, id, pairs)`.

## 3. The state-file hand-off

A panel runs on its own thread, watching its own window; it cannot see the
conversation, and nothing in this app lets the conversation reach into a
running thread. So the way you update a panel that is already open is the
plainest one available: **write a JSON file with your Write tool, and the
panel reads it back on its next tick.**

The file lives at `scripts\state\<key>.json` (`aii_ui.state.path_for(key)`
gives you the path; `aii_ui.state.STATE_DIR` is the directory, created on
first use). `StatusPanel.watch(path)` reads:

```json
{
  "title": "AIInterface build",
  "status": "building",
  "fields": {"branch": "master", "step": "2 of 5"},
  "progress": 0.4,
  "log": ["configuring", "compiling"]
}
```

Every key is optional. `title` re-titles the window (write it in the same
file as the first status, since the action opens the window before it reads
your file); `status` and each entry of `fields` replace what was there; `progress` is 0..1; `log` is appended -- if the new list starts with
everything already shown, only the new tail is added, so rewriting the whole
file each time does not repeat old lines. `state.watch_json(path)` is the
primitive underneath this: it returns the parsed dict only when the file's
mtime changed since the last call for that path, `None` otherwise (unchanged,
missing, or malformed -- a malformed file is reported once via `aii.status`,
not raised into your draw loop).

The shipped action `show_status_panel` is this whole pattern in about twenty
lines: it opens a `StatusPanel` and watches `scripts\state\status.json`.
Run it once, then keep writing that file to update the panel -- you do not
call the action again.

## 4. Rate and latching, restated

Two sentences, because they matter more than anything else here: **a button
answers for the frames since you last recorded**, not this exact instant, so
a script that records at 2 Hz still catches every click but sees it one
recording late. **The window keeps whatever you last submitted** between
recordings, so a slider a user is dragging does not snap back just because
your script has not recorded again yet.

## Extending the toolkit

`scripts\lib\aii_ui\` is yours to add to. When a panel wants something this
package does not have, add it here -- a new file, or a new function in an
existing one -- with a docstring saying what it is for, the same way every
file in this package already does. Keep additions small: a few functions or
one class per file, built out of the `aii.ui` calls in section 1, not a
second framework beside it.

One thing worth knowing before you do: **a helper module is cached by
CPython like any other Python import, and stays stale until the app
restarts.** An *action* is re-read from source on every call, so editing
`scripts\actions\foo.py` and calling it again is the whole of "reloading"
it -- but the moment that action (or a policy) does `import aii_ui`, that
import is cached for the life of the process. Edit a file in
`scripts\lib\aii_ui\`, and the next call to something that imports it still
runs the old code; the edit takes effect on the app's next launch. This is
the bootstrap's own rule for `import`s in general, not something particular
to this package.
