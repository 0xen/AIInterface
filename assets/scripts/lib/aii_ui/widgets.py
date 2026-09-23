"""Small, reusable pieces of a frame, built out of the raw `aii.ui` calls in
`AII-UI.md`. Every function here takes the `ui` module as its first
argument -- the same `aii.ui` a draw function already has -- rather than
importing it itself, so these compose with whatever else a panel draws in
the same frame and there is never a question of which recording they append
to.

Add a helper here when you catch yourself writing the same few `ui.*` calls
in more than one panel. Keep each one to a handful of lines; if it needs
its own state (a counter, a history), it probably belongs in `panel.py` or
a new file instead.
"""

import contextlib

from . import colors


def label_value(ui, label, value, color=None):
    """A "Label: value" line, optionally coloured (e.g. a status word)."""
    if color is not None:
        ui.text(label + ": ")
        ui.same_line()
        ui.text_colored(color, str(value))
    else:
        ui.label_text(label, str(value))


def badge(ui, text, color):
    """A short coloured word, e.g. a state name, on its own."""
    ui.text_colored(color, text)


def status_line(ui, label, state):
    """A "Label: state" line coloured by `colors.for_state(state)` -- the
    building block `StatusPanel.set_status` is written on top of."""
    label_value(ui, label, state, colors.for_state(state))


def log_view(ui, id_, lines, h=150.0):
    """A scrolling, bordered child region holding `lines` (newest last),
    auto-scrolled to the bottom. `id_` must be unique within the window."""
    ui.begin_child(id_, 0.0, h, True)
    for line in lines:
        ui.text_wrapped(line)
    ui.set_scroll_here_y(1.0)
    ui.end_child()


def key_value_table(ui, id_, pairs):
    """A two-column table of (key, value) pairs. `id_` must be unique within
    the window; `pairs` is any iterable of (str, str) tuples."""
    if not ui.begin_table(id_, 2):
        return
    for key, value in pairs:
        ui.table_next_row()
        ui.table_next_column()
        ui.text(str(key))
        ui.table_next_column()
        ui.text(str(value))
    ui.end_table()


@contextlib.contextmanager
def font_scale(ui, scale):
    """Draw everything inside the `with` block `scale` times the normal text
    size (1.0 = normal), e.g. `with font_scale(ui, 3.0): ui.text("漢")`.
    Nested blocks compound. On a build without `ui.push_font_scale` the block
    is simply drawn at the normal size."""
    has = hasattr(ui, "push_font_scale")
    if has:
        ui.push_font_scale(scale)
    try:
        yield
    finally:
        if has:
            ui.pop_font_scale()


def big_text(ui, text, scale=2.0, color=None):
    """One line of text at `scale` times the normal size, optionally
    coloured -- a large kanji, a headline number. Falls back to normal size
    on a build without `ui.push_font_scale`."""
    with font_scale(ui, scale):
        if color is not None:
            ui.text_colored(color, str(text))
        else:
            ui.text(str(text))
