"""The hand-off from the assistant to a window that is already open.

A running panel cannot see the conversation, and the assistant cannot call
into a thread it does not own -- so the way the assistant updates a panel
that is showing is the plainest one available: it writes a small JSON file
with its own Write tool, and the panel's `draw` reads it back once per tick.
`watch_json` is that read, done cheaply (a stat, not a re-read, on every
frame that has not changed) and safely (a bad file is reported once, not
raised into the draw loop).

Add helpers here for other on-disk hand-offs a panel might want -- reading a
log file's tail, watching a directory for a new file -- rather than writing a
one-off `open()` inside a `draw` function.
"""

import json
import os

import aii

# `aii.lib_dir` is `...\scripts\lib`; the state directory is its sibling.
STATE_DIR = os.path.normpath(os.path.join(aii.lib_dir, "..", "state"))

_mtimes = {}
_reported = set()


def path_for(name):
    """The state file for `name` (e.g. "status" -> ...\\scripts\\state\\status.json).
    Accepts a bare key or a filename that already ends in .json."""
    if not name.endswith(".json"):
        name = name + ".json"
    return os.path.join(STATE_DIR, name)


def watch_json(path):
    """Return the parsed JSON at `path` if its mtime changed since the last
    call for this exact path, else None. None also means "unchanged" and
    "does not exist yet" and "malformed" -- a panel's draw loop treats all
    three the same way, which is "keep showing what you last had". A
    malformed file is reported once via aii.status, not on every tick."""
    try:
        mtime = os.path.getmtime(path)
    except OSError:
        return None

    if _mtimes.get(path) == mtime:
        return None

    try:
        # utf-8-sig: Notepad and PowerShell's Set-Content write a byte-order mark,
        # and the first hand-off test failed on exactly that.
        with open(path, "r", encoding="utf-8-sig") as f:
            data = json.load(f)
    except (OSError, ValueError) as exc:
        if path not in _reported:
            _reported.add(path)
            aii.status("aii_ui: could not read %s: %s" % (path, exc), False)
        return None

    _mtimes[path] = mtime
    _reported.discard(path)
    return data


def ensure_dir():
    """Create the state directory if it does not exist yet. Called lazily by
    anything that watches a path under it; you do not need to call this
    yourself before `watch_json`."""
    os.makedirs(STATE_DIR, exist_ok=True)


ensure_dir()
