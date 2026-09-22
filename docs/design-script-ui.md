# Design: script windows (M28)

**Ask (22 Sep 2026):** the assistant should be able to make ImGui windows from Python to
show things to the user — a panel beside the chat saying `X Project Name` / `Status:
Building` in orange, going red on failure and green on success — and, more generally,
"a dynamic way for the AI to make any ImGui window and display information on it", with
"a shared set of imgui window helper python files that it can use and expand upon itself".

## 1. Shape

Three layers, from the bottom:

1. **`aii::UiBridge`** (`src/core/ui_bridge.h/.cpp`). A mutex-guarded store of *windows*,
   each holding the latest recorded *frame* (a flat list of `UiCommand`) and a map of
   *results* keyed by widget id. Compiled into both `aii_core` and `aii_pyhost.dll`; the
   frame loop owns the one instance and hands its pointer to the DLL over a new C ABI
   function, `aiiPyHostSetUiBridge(aii::UiBridge*)`, for the same reason the bus pointer
   is handed over (two copies of a singleton talk past each other).
2. **`aii.ui`** — a submodule of the `aii` Python module (`src/pyhost/aii_pyhost.cpp`).
   ImGui-shaped functions that append to a per-thread *recording* and return the latched
   result for their id. `begin_frame(key)` starts a recording, `end_frame()` submits it.
3. **`ScriptWindow`** (`src/avatar/script_window.h/.cpp`). One OS window per bridge
   window, on `ToolWindowCore` like the worker window: decorated, resizable, opaque, not
   always-on-top, placed to the left of the widget in the free slot. Each render frame it
   copies the latest recording and replays it through real ImGui inside one full-client
   ImGui window (no title bar; the OS window has one), writing every interactive widget's
   outcome back with `set_results()`.

Above those, in Python and shipped as assets: **`aii_ui`**, the helper package
(`assets/scripts/lib/aii_ui/`), seeded to `%APPDATA%\AIInterface\scripts\lib\aii_ui\` by the
existing recursive `seed_tree` and put on `sys.path` by the bootstrap. It is plain Python,
readable and editable, and the assistant is told to extend it there.

## 2. Why record-and-replay, not a direct binding

`script_host.h`: scripts run on their own thread and *the frame loop never touches the
interpreter*. A direct ImGui binding would need one of: the render thread taking the GIL
every frame (a `time.sleep(5)` in a script freezes the app), or Python calling ImGui from
its own thread into a context the render thread is using (torn frames, crashes). Recording
keeps both invariants: Python only ever copies a list into a mutex; the app only ever copies
it out. A script can record at 2 Hz and the window still renders at 60.

The cost is that `button()` answers for *the frames since the script last recorded*, not
this exact frame, which is why results latch (`ui_bridge.h`, "Results latch") and why the
window keeps overrides so a dragged slider does not snap back between recordings.

## 3. The command table

`UiOp` → fields used. Everything not listed is ignored. `label` is the ImGui label (with
any `##`/`###` suffix passed through); `id` for results is `ui_compose_id(stack, label)`.

| op | label | text | f | i | b | items / values |
|---|---|---|---|---|---|---|
| Text, TextWrapped, TextDisabled, BulletText | the text | | | | | |
| TextColored | the text | | rgba | | | |
| LabelText | label | value | | | | |
| Separator, NewLine, Spacing | | | | | | |
| SeparatorText | the text | | | | | |
| SameLine | | | f[0]=offset, f[1]=spacing (0,-1 default) | | | |
| Dummy | | | f[0]=w, f[1]=h | | | |
| Indent, Unindent | | | f[0]=width (0 default) | | | |
| Button | label | | f[0]=w, f[1]=h (0 = auto) | | | |
| SmallButton | label | | | | | |
| Checkbox | label | | | | value | |
| RadioButton | label | | | | active | |
| Selectable | label | | | | selected | |
| SliderFloat, DragFloat | label | format ("%.3f" default) | f[0]=value, f[1]=min, f[2]=max, f[3]=speed (Drag) | | | |
| SliderInt, DragInt | label | format | f[3]=speed (Drag) | i[0]=value, i[1]=min; max in f[2] | | |
| InputText | label | hint | | i[1]=ImGuiInputTextFlags | | value `items[0]` |
| InputTextMultiline | label | | f[0]=w, f[1]=h | | | value `items[0]` |
| InputInt | label | | | i[0]=value, i[1]=step | | |
| InputFloat | label | format | f[0]=value, f[1]=step | | | |
| Combo | label | | | i[0]=index | | items |
| ListBox | label | | | i[0]=index, i[1]=height_in_items | | items |
| ColorEdit | label | | rgba | i[1]=flags | | |
| ProgressBar | | overlay | f[0]=fraction, f[1]=w, f[2]=h | | | |
| PlotLines, PlotHistogram | label | overlay | f[0]=min, f[1]=max, f[2]=w, f[3]=h | | | values |
| CollapsingHeader | label | | | i[1]=ImGuiTreeNodeFlags | default open | |
| TreeNode | label | | | | | |
| TreePop | | | | | | |
| BeginChild | id string | | f[0]=w, f[1]=h | i[1]=ImGuiChildFlags | border | |
| EndChild, BeginGroup, EndGroup, EndDisabled, EndTabBar, EndTabItem, EndTable | | | | | | |
| BeginDisabled | | | | | disabled | |
| BeginTabBar | id string | | | | | |
| BeginTabItem | label | | | | | |
| TableNextRow, TableNextColumn, TableHeadersRow | | | | | | |
| BeginTable | id string | | f[0]=w, f[1]=h | i[0]=columns, i[1]=flags | | |
| TableSetupColumn | label | | f[0]=init width | i[1]=flags | | |
| Columns | | | | i[0]=count | border | |
| NextColumn | | | | | | |
| PushId | the id | | | | | |
| PopId | | | | | | |
| PushStyleColor | | | rgba | i[0]=ImGuiCol | | |
| PopStyleColor | | | | i[0]=count (1 default) | | |
| PushItemWidth, SetNextItemWidth | | | f[0]=width | | | |
| PopItemWidth | | | | | | |
| SetTooltip | the text | | | | | |
| SetScrollHereY | | | f[0]=center ratio | | | |

To keep the InputText row unambiguous: **the current text of InputText and
InputTextMultiline is carried in `items[0]`** (`text` holds the hint), so it gets the
`kUiTextMax` truncation a string value gets, and results return it in `UiResult::s`.

Results written per op: Button/SmallButton/Selectable/BeginTabItem/TreeNode → `clicked`
(Selectable, TreeNode, BeginTabItem, CollapsingHeader also `b` = selected/open);
Checkbox/RadioButton → `b`, `changed`; sliders/drags/InputFloat/ColorEdit → `f`, `changed`;
SliderInt/DragInt/InputInt/Combo/ListBox → `i`, `changed`; InputText* → `s`, `changed`.

**Balancing.** The replayer tracks its own depth for every Begin/End pair, PushId/PopId,
PushStyleColor/PopStyleColor, PushItemWidth/PopItemWidth, TreeNode/TreePop, Columns. At
the end of a frame it closes whatever is still open, and it ignores an End with nothing
open. ImGui asserts are compiled out in Release, so an unbalanced stack would otherwise be
a silent crash. CollapsingHeader and TreeNode work exactly as in ImGui: the Python call
returns the latched open state and the script decides what to record after it. The app
replays what was recorded — with one exception, found on the first run: a TreeNode or
BeginTabItem that ImGui reports *closed* skips the recorded commands up to its matching
TreePop/EndTabItem. The script's answer is one recording old, so on the frame the user
switches tabs the old tab's children would otherwise be drawn below the bar as loose
widgets. Python's `tree_node()` and `begin_tab_item()` return `b` (open/selected), not
`clicked`, for the same reason: the first tab is selected without anyone clicking it.

**Colours.** Every rgba handed to ImGui goes through `ui_color()` (`imgui_layer.h`) because
the swapchain is sRGB. Scripts give 0..1 sRGB components, as in any colour picker.

## 4. The Python API (`aii.ui`)

All functions are module-level on `aii.ui`. A *recording* is per thread; calling a widget
function outside `begin_frame()`..`end_frame()` raises `RuntimeError`.

```python
ui.open(key, title, w=360, h=240) -> bool     # False (and aii.log) when refused
ui.close(key)
ui.is_open(key) -> bool                        # False once the user closed it
ui.windows() -> list[str]
ui.begin_frame(key)                            # takes this window's results, starts recording
ui.end_frame()                                 # submits; returns the command count
ui.push_id(s) / ui.pop_id()
ui.text(s); ui.text_colored(rgba, s); ui.text_wrapped(s); ui.text_disabled(s)
ui.bullet_text(s); ui.label_text(label, value)
ui.separator(); ui.separator_text(s); ui.same_line(offset=0.0, spacing=-1.0)
ui.new_line(); ui.spacing(); ui.dummy(w, h); ui.indent(w=0.0); ui.unindent(w=0.0)
ui.button(label, w=0.0, h=0.0) -> bool; ui.small_button(label) -> bool
ui.checkbox(label, value) -> bool              # returns the (possibly user-changed) value
ui.radio_button(label, active) -> bool         # True when clicked
ui.selectable(label, selected=False) -> bool   # True when clicked
ui.slider_float(label, v, lo, hi, fmt="%.3f") -> float
ui.slider_int(label, v, lo, hi) -> int
ui.drag_float(label, v, speed=1.0, lo=0.0, hi=0.0, fmt="%.3f") -> float
ui.drag_int(label, v, speed=1.0, lo=0, hi=0) -> int
ui.input_text(label, text, hint="", flags=0) -> str
ui.input_text_multiline(label, text, w=0.0, h=0.0) -> str
ui.input_int(label, v, step=1) -> int; ui.input_float(label, v, step=0.0, fmt="%.3f") -> float
ui.combo(label, index, items) -> int; ui.list_box(label, index, items, height_in_items=-1) -> int
ui.color_edit(label, rgba, flags=0) -> tuple[4 floats]
ui.progress_bar(fraction, w=-1.0, h=0.0, overlay="")
ui.plot_lines(label, values, lo=FLT_MAX, hi=FLT_MAX, w=0.0, h=0.0, overlay="")
ui.plot_histogram(...same...)
ui.collapsing_header(label, default_open=False, flags=0) -> bool
ui.tree_node(label) -> bool (open); ui.tree_pop()
ui.begin_child(id, w=0.0, h=0.0, border=False, flags=0) -> True; ui.end_child()
ui.begin_group(); ui.end_group()
ui.begin_disabled(disabled=True); ui.end_disabled()
ui.begin_tab_bar(id) -> True; ui.end_tab_bar(); ui.begin_tab_item(label) -> bool (selected); ui.end_tab_item()
ui.begin_table(id, columns, flags=0, w=0.0, h=0.0) -> bool (always True in recording)
ui.end_table(); ui.table_next_row(); ui.table_next_column(); ui.table_setup_column(label, flags=0, width=0.0); ui.table_headers_row()
ui.columns(count=1, border=True); ui.next_column()
ui.push_style_color(idx, rgba); ui.pop_style_color(count=1)
ui.push_item_width(w); ui.pop_item_width(); ui.set_next_item_width(w)
ui.set_tooltip(s); ui.set_scroll_here_y(center=0.5)
ui.COL_TEXT, ui.COL_BUTTON, ... (the ImGuiCol_ values a script is likely to want, as ints)
```

Widget functions with a return value look up `ui_compose_id(stack, label)` in the results
`begin_frame()` took; missing → the value passed in (and `False` for clicks). `rgba` is any
sequence of 3 or 4 floats.

## 5. The helper package `aii_ui`

`scripts\lib\aii_ui\` — the assistant's toolkit, and the thing it is told it may extend.
Small, plain, documented at the top of each file.

| file | what |
|---|---|
| `__init__.py` | re-exports the below; docstring is the one-paragraph orientation |
| `colors.py` | named rgba tuples: `ORANGE`, `RED`, `GREEN`, `GREY`, `WHITE`, `BLUE`, `YELLOW`; `for_state(state)` mapping `pending/building/running → ORANGE`, `failed/error → RED`, `done/ok/complete → GREEN`, else GREY |
| `panel.py` | `class Panel(key, title, w=360, h=240, hz=10)`: `open()`, `close()`, `run(draw)` loops `begin_frame`/`draw(ui)`/`end_frame` at `hz` until the window is closed or `aii.should_quit()`; `run_in_thread(draw)`; `frame(draw)` for one recording |
| `status.py` | `class StatusPanel(title, key=None)`: `set(field, text, color=None)`, `set_status(text)` (colour from `for_state`), `progress(fraction, text="")`, `log(line)` (bounded scrollback), `run()`. **This is the user's example**: `StatusPanel("X Project Name").set_status("Building")` |
| `widgets.py` | free functions over a `ui` module: `label_value(label, value, color=None)`, `badge(text, color)`, `status_line(label, state)`, `log_view(id, lines, h)`, `key_value_table(id, pairs)` |
| `state.py` | `watch_json(path)`: returns the parsed dict when the file's mtime changed, else `None`. The **hand-off from the assistant to a running panel**: the assistant writes `%APPDATA%\AIInterface\scripts\state\<key>.json` with its Write tool, the panel's draw reads it |

An action that shows a panel is long-running (its `run()` loops); it runs on its own daemon
thread and *never polls the bus* (the bootstrap's rule), so its inputs are the state file
and the window's own widget results.

Shipped examples (`assets/scripts/examples/`, not scanned): `ui_status_panel.py` (the user's
example, driven by `state\build.json`) and `ui_widgets_demo.py` (one of everything, so a
person can see what the toolkit draws). Shipped action (`assets/scripts/actions/`):
`show_status_panel.py` — opens a status panel whose name and state come from
`scripts\state\status.json`, the shape the assistant is told to write.

## 6. Prompt and docs

- `assets/scripts/AII-MODULE.md` gains a section "Windows", pointing at `AII-UI.md`.
- `assets/scripts/AII-UI.md` (new, seeded beside it): the API of §4, the helper package of
  §5, the state-file hand-off, and the rule that the assistant may add files to
  `scripts\lib\aii_ui\` and should prefer extending it over inlining.
- `assets/prompts/system/scripts.md`: one paragraph — scripts can open windows, read
  `AII-UI.md` first. Prose only; no format bump.
- `CHANGELOG.md` entry; `docs/MILESTONES.md` M28 entry with what was measured.

## 7. Tests

`tests/ui_bridge_test.cpp` (no ImGui, no Python): key validation; window cap refused at
seven with a reason; submit truncates commands, strings, items and reports; generation
increments; `clicked` latches across two `set_results` and clears on `take_results` while
`f` stays; `mark_closed` flips `is_open` without removing; `close` puts the key in
`closing` and `remove` forgets it; reopen after user close clears the flag;
`ui_compose_id` composition.

## 8. What is not in this milestone

- Images and fonts. No `Image` op; the ImGui contexts share one font.
- Docking a script window *inside* the widget. Every extra surface in this app is its own
  OS window (spike-two-windows.md), and this follows that.
- Menus and popups (`BeginMenuBar`, `BeginPopup`, modal). Popups need per-frame open
  state the replay model handles badly; add when a script needs one.
- A window per *script*: a window is per key, and any thread may record for any key.
