"""M28 -- one of everything `aii.ui` can draw, in one window.

Copy this one level up (into %APPDATA%\\AIInterface\\scripts) to have it run,
or pass it with --script. It is a *policy*: started once at launch, on its
own thread, and it runs until the window is closed or the app quits.

There is no helper class for this one -- it calls `aii.ui` directly, at
10 Hz, through `aii_ui.Panel`, so it doubles as a worked example of writing a
`draw(ui)` function from scratch rather than using `StatusPanel`. It shows a
button with a click counter, a slider, a checkbox, an input box that echoes
back what you type, a combo box, a progress bar that animates on its own, a
line plot, two tabs, and a small table -- each is the smallest useful call
from the table in `AII-UI.md` section 4.
"""

import math
import time

from aii_ui import Panel

_state = {
    "clicks": 0,
    "slider": 0.5,
    "checked": False,
    "text": "",
    "combo_index": 0,
    "combo_items": ["alpha", "beta", "gamma"],
    "start": time.time(),
    "history": [0.0] * 60,
}


def draw(ui):
    ui.text("aii.ui widgets demo")
    ui.separator()

    if ui.button("Click me (%d)" % _state["clicks"]):
        _state["clicks"] += 1

    _state["slider"] = ui.slider_float("a slider", _state["slider"], 0.0, 1.0)
    _state["checked"] = ui.checkbox("a checkbox", _state["checked"])
    _state["text"] = ui.input_text("type here", _state["text"], hint="say something")
    if _state["text"]:
        ui.text("you typed: " + _state["text"])

    _state["combo_index"] = ui.combo("a combo", _state["combo_index"],
                                      _state["combo_items"])
    ui.text("chosen: " + _state["combo_items"][_state["combo_index"]])

    # A progress bar that fills and loops on its own, so the window has
    # something moving even before anyone touches it.
    t = time.time() - _state["start"]
    frac = (t % 4.0) / 4.0
    ui.progress_bar(frac, overlay="%d%%" % int(frac * 100))

    # A small rolling plot -- a sine wave, so it is obviously "live".
    _state["history"] = _state["history"][1:] + [math.sin(t * 2.0)]
    ui.plot_lines("a plot", _state["history"], -1.0, 1.0, 0.0, 60.0)

    ui.separator()
    if ui.begin_tab_bar("##demo_tabs"):
        if ui.begin_tab_item("Table"):
            if ui.begin_table("##demo_table", 2):
                ui.table_setup_column("Field")
                ui.table_setup_column("Value")
                ui.table_headers_row()
                for label, value in (("clicks", _state["clicks"]),
                                      ("slider", "%.2f" % _state["slider"]),
                                      ("checked", _state["checked"])):
                    ui.table_next_row()
                    ui.table_next_column()
                    ui.text(str(label))
                    ui.table_next_column()
                    ui.text(str(value))
                ui.end_table()
            ui.end_tab_item()
        if ui.begin_tab_item("About"):
            ui.text_wrapped(
                "Every widget here is one call from the table in AII-UI.md. "
                "Read that file for the full list, including the ones this "
                "demo does not use (drag boxes, colour pickers, tree nodes).")
            ui.end_tab_item()
        ui.end_tab_bar()


Panel("ui_widgets_demo", "Widgets Demo", w=420, h=460, hz=10).run(draw)
