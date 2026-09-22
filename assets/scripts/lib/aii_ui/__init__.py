"""`aii_ui` -- the assistant's own toolkit for windows made with `aii.ui`.

`aii.ui` (documented in `AII-UI.md`) is the raw, ImGui-shaped API: open a
window, record a frame of calls like `ui.text(...)` and `ui.button(...)`,
submit it. This package is what is built on top of it -- a `Panel` run loop,
a ready-made `StatusPanel`, small widget helpers and a state-file watcher --
so that showing a window does not mean re-deriving the run loop and the
error handling every time.

This is explicitly the assistant's own toolkit, seeded next to every other
script and meant to grow: when a panel wants something this package does not
have, the answer is to add it here, in a new file or an existing one, with a
docstring saying what it is for, rather than to write it once inline and
lose it. See `AII-UI.md`'s closing section, "Extending the toolkit", for the
one rule that matters when you do: a helper module is cached by CPython the
way any Python import is, so an edit here is picked up on the next launch of
the app, not by the running one.

    from aii_ui import StatusPanel
    StatusPanel("Build").run()

Everything below is a re-export; the real docstrings are on the classes and
functions themselves, in their own files.
"""

from . import colors, state, widgets
from .colors import for_state
from .graph import NodeGraph
from .panel import Panel
from .status import StatusPanel

__all__ = [
    "NodeGraph",
    "Panel",
    "StatusPanel",
    "colors",
    "for_state",
    "state",
    "widgets",
]
