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

### Replacing a window you already opened

Re-running a script whose window is still up is the normal case -- you fixed
something and want to see it. **Open the same key again** and the window is
taken over: the earlier run's loop sees `ui.is_open(key)` go `False` and ends
on its own, its last frames are dropped, and the window shows only the new
run. Without this, two loops record into one window and it flickers between
old and new content every other frame. `Panel` does this for you (its `run()`
always opens); a hand-written loop must use the same key and check
`is_open()` every pass. Node positions and edited values start fresh after a
takeover, as they would in a new window. Do not give the rewritten script a
new key to "avoid" the old one -- that leaves the old window up as well.

`ui.epoch(key)` is the counter behind this, for a loop that wants to know
explicitly.

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

### Node graphs

Boxes with pins, wired together, that the user can drag around and connect --
a diagram of the microphone-to-recogniser-to-Claude pipeline, a wiring view
of an action's own steps, anything shaped like nodes and links rather than a
form. This is `aii.ui`'s node editor, one imnodes context per script window.

Japanese renders in these windows exactly as in the chat -- the same merged
font -- so write titles and hints in kana or kanji when that is what the user
wants. `text_disabled` is a dim hint colour: fine for a footnote, wrong for
the one line that carries the content of a node. Use `text` for content.

How the user moves around it, so you can say so when you open one: **drag a
node** by its title bar; **pan** the canvas with a right-button drag (or
Alt + left drag, or the middle button); **wire** two pins by dragging from one
to the other; **select** a link by clicking it and press **Delete** to remove
it, or Ctrl-click a pin to detach its link; **box-select** nodes with a left
drag on empty canvas. There is no zoom -- imnodes does not have one -- so the
minimap is how a large graph is found.

| Call | What it does |
|---|---|
| `ui.begin_node_editor()` / `ui.end_node_editor()` | Wraps the whole graph for this frame |
| `ui.begin_node(id)` / `ui.end_node()` | One box |
| `ui.begin_node_title_bar()` / `ui.end_node_title_bar()` | The box's title, drawn first inside it |
| `ui.begin_input_attribute(id, shape=1)` / `ui.end_input_attribute()` | An input pin |
| `ui.begin_output_attribute(id, shape=1)` / `ui.end_output_attribute()` | An output pin |
| `ui.begin_static_attribute(id)` / `ui.end_static_attribute()` | A row with no pin -- widgets only |
| `ui.link(id, start_attr, end_attr)` | Draw a link between two pins |
| `ui.set_node_pos(id, x, y, force=False)` | Place a node in grid space |
| `ui.mini_map(fraction=0.2, location=1)` | An overview corner, called just before `end_node_editor()` |
| `ui.push_node_color(idx, rgba)` / `ui.pop_node_color()` | `idx` is one of `ui.NODE_COL_*` |
| `ui.links_created()` -> `[(start_attr, end_attr), ...]` | New links the user dragged, since your last recording |
| `ui.links_destroyed()` -> `[link_id, ...]` | Links the user deleted |
| `ui.node_pos(id)` -> `(x, y)` or `None` | Where a node ended up, as of the last render |
| `ui.selected_nodes()` -> `[id, ...]` | The current node selection |
| `ui.PIN_CIRCLE` .. `ui.PIN_QUAD_FILLED` | Pin shapes, 0..5, for `shape` |
| `ui.MINIMAP_BOTTOM_LEFT` .. `ui.MINIMAP_TOP_RIGHT` | Minimap corners, 0..3, for `location` |
| `ui.NODE_COL_NODE_BACKGROUND`, `ui.NODE_COL_TITLE_BAR`, `ui.NODE_COL_LINK`, `ui.NODE_COL_PIN` | Colour targets for `push_node_color` |

**Ids.** `id` is your own int for a node, an attribute or a link. imnodes
requires every id inside one editor to be unique across all three kinds --
a node id and a link id must not collide either -- so hand-rolling them is
easy to get wrong on a graph that grows. `aii_ui.NodeGraph` (below)
allocates all of its ids from one counter for exactly this reason; write
against that rather than calling these ops directly unless you need
something it does not do.

**Positions are applied once.** `set_node_pos` only moves a node the first
time you call it for that id in that editor; call it again with `force=True`
if you really mean to override wherever the node is now. Otherwise a script
that records every tick would drag every node back to its starting position
out from under the user mid-drag.

**Everything between `begin_node`/`end_node` is ordinary widgets** -- `text`,
`slider_float`, `input_text`, and so on -- called exactly as anywhere else,
just drawn inside the box instead of the window.

**`links_created()` and `links_destroyed()` latch**, the same as a button's
`clicked`: they report what happened since you last recorded, and reading
them clears them.

#### `aii_ui.NodeGraph`

The model + renderer built on the calls above -- add nodes and links once,
call `draw(ui)` every tick, and it folds the user's edits back into itself.

```python
from aii_ui import NodeGraph, Panel

g = NodeGraph()
mic = g.add_node("Microphone", outputs=["audio"])
rec = g.add_node("Recogniser", inputs=["audio"], outputs=["text"])
g.add_link(g.pin(mic, "audio"), g.pin(rec, "audio"))
g.layout_grid()

def draw(ui):
    g.draw(ui)

Panel("pipeline", "Pipeline").run(draw)
```

`add_node(title, inputs=(), outputs=(), pos=None, color=None, body=None)`
returns the node id; `inputs`/`outputs` are pin labels or `(label, shape)`
pairs; `body(ui, node)` draws widgets between the pins -- give each one an
explicit width with `ui.set_next_item_width(...)`, because ImGui's default width
is a share of the window and would stretch the node across the whole editor,
and keep any value it returns on the `node` dict so it survives the next
recording. `pin(node_id, label)`
looks up a pin's attribute id. `add_link`/`remove_link`/`remove_node`,
`links()`, `nodes()`, `selected_nodes()`. `layout_grid(columns=3, dx=220,
dy=140)` gives a grid position to any node that does not have one, which is
what turns a graph read from a state file straight into something readable.
Callbacks: `on_link(start, end) -> bool` (return `False` to refuse a link the
user just dragged), `on_unlink(link_id)`, `on_select(node_ids)`.

**The state-file hand-off.** `to_dict()`/`from_dict()` round-trip a graph
through the same JSON shape `aii_ui.state.watch_json` reads:

```json
{
  "nodes": [
    {"title": "Microphone", "inputs": [], "outputs": ["audio"], "pos": [20, 40], "color": null},
    {"title": "Recogniser", "inputs": ["audio"], "outputs": ["text"], "pos": null, "color": null}
  ],
  "links": [
    {"from": [0, "audio"], "to": [1, "audio"]}
  ]
}
```

A node with no `pos` is left for `layout_grid()`. A link names its endpoints
by `[node index, pin label]`, not by the internal attribute id, so a graph
the assistant writes never needs to know what ids the running panel already
handed out.

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
| `graph.py` | `class NodeGraph` -- a node-graph model and renderer over `aii.ui`'s node editor calls (section 1, "Node graphs") |

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

A panel written for one occasion -- to watch this build, to show this one
check -- belongs in `tmp\`, the same as any other script written for the
moment: it is armed the instant it is written, and the folder may be cleared
at any time. A panel worth keeping is promoted into `actions\` the normal
way, by writing the same file there; the approval window then appears once
for it.

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
second framework beside it. This is reusable code, not a script, so it
belongs here regardless of whether the panel using it lives in `tmp\` or
`actions\`.

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
