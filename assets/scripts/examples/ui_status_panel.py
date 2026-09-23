"""A worked example: a project status window that goes orange while
building, green when done, then keeps itself up to date from a state file.

Copy this one level up (into %APPDATA%\\AIInterface\\scripts) to have it run,
or pass it with --script. It is a *policy* -- started once at launch, on its
own thread, and it runs until the window is closed or the app quits -- not an
action, so it belongs in `scripts\\`, not `scripts\\actions\\`.

What it shows, in order:

  1. A `StatusPanel` titled "Example Project" opens, coloured orange
     ("Building"), then after a couple of seconds turns green ("Done") with
     a couple of extra fields and a log line: the title, a status line that
     goes red on failure and green on success, plus room for extra fields.
  2. It then calls `watch("...\\scripts\\state\\example.json")` and runs:
     from that point on, editing that file (with a Write tool, or by hand)
     changes what the window shows on the next tick, without restarting the
     script. Try it: while this is running, write

         {"status": "running", "fields": {"note": "hello from the file"},
          "progress": 0.4, "log": ["a line written from outside"]}

     to `scripts\\state\\example.json` and watch the window pick it up.

See `AII-UI.md` for the full API this is built on, and `aii_ui/status.py`
for what `StatusPanel` does with each of those fields.
"""

import time

import aii
from aii_ui import StatusPanel, state

panel = StatusPanel("Example Project")
panel.set_status("Building")
panel.set("step", "1 of 3")
panel.log("build started")

thread = panel.run_in_thread()

# Simulate a build finishing, entirely in this script -- nothing here reads
# the bus, so this is plain time, not an event wait.
time.sleep(3.0)
if not aii.should_quit():
    panel.set("step", "3 of 3")
    panel.progress(1.0, "complete")
    panel.set_status("Done")
    panel.log("build finished")

# From here the window is driven by the state file until it is closed or the
# app quits. `state.path_for` is `...\\scripts\\state\\example.json`; that
# directory (`aii_ui.state.STATE_DIR`) is created on first use.
panel.watch(state.path_for("example"))

thread.join()
