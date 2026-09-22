"""`StatusPanel` -- the window from the original ask: "`X Project Name`"
titled window with a "Status: Building" line that goes orange, then red on
failure or green on done. It is the one class most scripts that show a
window will actually use; `Panel` underneath it is there for when you want
something that does not look like this.

    from aii_ui import StatusPanel
    panel = StatusPanel("X Project Name")
    panel.set_status("Building")
    panel.run()   # blocks; call from an action's own thread or run_in_thread()

Add fields to what a `StatusPanel` can show here, in this file, rather than
building a parallel class -- most "I want a window that shows..." requests
are a `StatusPanel` plus one more row.
"""

import re
import threading

from . import state, widgets
from .panel import Panel


def _slug(title):
    s = re.sub(r"[^a-zA-Z0-9]+", "_", title).strip("_").lower()
    return s or "status"


class StatusPanel:
    """A titled window with a status line, zero or more labelled fields, an
    optional progress bar and a scrolling log -- the shape `AII-UI.md`
    documents as the JSON a state file may hold, because `watch()` applies
    exactly those fields from one."""

    def __init__(self, title, key=None):
        self.title = title
        self.key = key or _slug(title)
        self._panel = Panel(self.key, title)
        self._lock = threading.Lock()
        self._status = ""
        self._fields = {}          # field -> (text, color)
        self._field_order = []
        self._progress = None
        self._progress_text = ""
        self._log_lines = []
        self._watch_path = None
        self._watched_log = []     # the raw log list last seen from the file

    def open(self):
        return self._panel.open()

    def close(self):
        self._panel.close()

    def set_status(self, text):
        """Set the "Status: ..." line; its colour comes from
        `colors.for_state(text)`."""
        with self._lock:
            self._status = text

    def set(self, field, text, color=None):
        """Add or update a labelled row, e.g. set("branch", "master")."""
        with self._lock:
            if field not in self._fields:
                self._field_order.append(field)
            self._fields[field] = (text, color)

    def progress(self, fraction, text=""):
        """Show a progress bar. `fraction` is 0..1; pass None to hide it."""
        with self._lock:
            self._progress = fraction
            self._progress_text = text

    def log(self, line):
        """Append one line to the scrolling log, keeping the last 200."""
        with self._lock:
            self._log_lines.append(str(line))
            if len(self._log_lines) > 200:
                self._log_lines = self._log_lines[-200:]

    def watch(self, path):
        """Make `run()` re-read `path` as JSON on every tick it changes
        (via `state.watch_json`) and apply it: `status` (str), `fields`
        (dict of field -> text), `progress` (0..1) and `log` (a list of
        lines -- only the ones past what was already seen are appended, so
        rewriting the whole file each time does not repeat old lines)."""
        self._watch_path = path

    def _apply_state(self, data):
        if not isinstance(data, dict):
            return
        # `title` too: the assistant writes the file *after* the action has
        # opened the window, so the title it chose would otherwise never land.
        if data.get("title"):
            self.title = str(data["title"])
            self._panel.set_title(self.title)
        if "status" in data:
            self.set_status(str(data["status"]))
        fields = data.get("fields")
        if isinstance(fields, dict):
            for k, v in fields.items():
                self.set(str(k), str(v))
        if "progress" in data and data["progress"] is not None:
            self.progress(float(data["progress"]))
        log_lines = data.get("log")
        if isinstance(log_lines, list):
            log_lines = [str(x) for x in log_lines]
            if log_lines[: len(self._watched_log)] == self._watched_log:
                new = log_lines[len(self._watched_log):]
            else:
                # The file was rewritten with a different history (e.g.
                # truncated); show the whole thing rather than guess a diff.
                new = log_lines
            for line in new:
                self.log(line)
            self._watched_log = log_lines

    def _draw(self, ui):
        with self._lock:
            status = self._status
            fields = [(f, self._fields[f]) for f in self._field_order]
            progress = self._progress
            progress_text = self._progress_text
            log_lines = list(self._log_lines)

        widgets.status_line(ui, "Status", status)
        for field, (text, color) in fields:
            widgets.label_value(ui, field, text, color)
        if progress is not None:
            ui.progress_bar(max(0.0, min(1.0, progress)), overlay=progress_text)
        if log_lines:
            ui.separator()
            widgets.log_view(ui, "##" + self.key + "_log", log_lines)

    def _tick(self, ui):
        if self._watch_path:
            data = state.watch_json(self._watch_path)
            if data is not None:
                self._apply_state(data)
        self._draw(ui)

    def run(self):
        """Open the window (if needed) and loop until it is closed or the
        app quits, applying `watch()`'s file on every tick it changes.
        Blocks -- call this on the action's own thread, or use
        `run_in_thread()`."""
        self._panel.run(self._tick)

    def run_in_thread(self):
        """Start `run()` on a daemon thread and return it."""
        return self._panel.run_in_thread(self._tick)
