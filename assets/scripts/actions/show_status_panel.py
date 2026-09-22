"""Show a status panel driven by scripts\\state\\status.json.

The point of this action is that it does almost nothing itself: it opens a
`StatusPanel` and then watches a state file, and from then on the panel is
whatever that file says. Writing to `scripts\\state\\status.json` (with the
Write tool -- the same file this action reads) is how you update a panel
that is already open, without calling this action again.

The file, if present, is:

    {
      "title": "My Build",
      "status": "building",
      "fields": {"branch": "master", "step": "2 of 5"},
      "progress": 0.4,
      "log": ["configuring", "compiling"]
    }

Every key is optional. `title` is only read once, when the window is first
opened (opening a window twice under the same key is a no-op, so changing
the title later in the file has no effect on an already-open panel). See
`AII-UI.md` for the full shape and `aii_ui/status.py` for what each field
does.

If the file does not exist yet, the panel opens anyway with "Status: waiting
for state file" and keeps watching -- it will pick the file up as soon as
something writes it.

`run()` returns immediately after starting the panel's own thread; the
panel keeps running (and watching the file) until its window is closed or
the app quits, exactly like any other `aii_ui` panel.
"""

import json
import os

from aii_ui import StatusPanel, state

STATUS_PATH = state.path_for("status")


def _read_title(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return "Status"
    if isinstance(data, dict) and data.get("title"):
        return str(data["title"])
    return "Status"


def run():
    panel = StatusPanel(_read_title(STATUS_PATH), key="status")
    if not os.path.exists(STATUS_PATH):
        panel.set_status("waiting for state file")
    panel.watch(STATUS_PATH)
    panel.run_in_thread()
