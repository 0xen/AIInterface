"""The one loop every `aii_ui` window runs: open, draw, sleep, repeat.

`Panel` is the base every other window in this package builds on
(`StatusPanel` is a `Panel` with a fixed drawing routine and some state of
its own). If you are adding a new kind of window, start here -- wrap a
`Panel` and give it your own `draw(ui)` function -- rather than calling
`aii.ui` directly from an action or a policy.

**This module never calls `aii.poll()` or `aii.wait()`.** That is the
bootstrap's rule for anything that is not the one dispatcher thread (see
`AII-MODULE.md`), and a panel is exactly the kind of long-running, own-thread
code that rule exists for: its inputs are the window's own widget results and
whatever `state.watch_json` reads off disk, never the event bus.
"""

import datetime
import threading
import time

import aii
import aii.ui as ui

# Every Panel window ends in a one-line footer saying when it was opened, in
# the dim hint colour: after a fix, it tells the user at a glance whether the
# window in front of them is the fresh copy or one left over from before. The
# script's own content is drawn in a child region that fills the window down
# to FOOTER_H pixels above the bottom, so the footer stays at the bottom and
# the content scrolls above it. FOOTER_H covers the gap after the region, a
# separator and one line of text at the app's 15 px font.
FOOTER_H = 30.0
BODY_ID = "##aii_panel_body"

# Opening call -> the call that closes it, for _Tracked below.
_CLOSERS = {
    "begin_child": "end_child", "begin_group": "end_group",
    "begin_disabled": "end_disabled", "begin_tab_bar": "end_tab_bar",
    "begin_tab_item": "end_tab_item", "begin_table": "end_table",
    "begin_node_editor": "end_node_editor", "begin_node": "end_node",
    "begin_node_title_bar": "end_node_title_bar",
    "begin_input_attribute": "end_input_attribute",
    "begin_output_attribute": "end_output_attribute",
    "begin_static_attribute": "end_static_attribute",
    "push_id": "pop_id", "push_style_color": "pop_style_color",
    "push_item_width": "pop_item_width", "push_node_color": "pop_node_color",
    "push_font_scale": "pop_font_scale",
}


class _Tracked:
    """`aii.ui` as handed to a Panel's draw(): every call goes straight
    through, and the openings not yet closed are remembered. If draw()
    raises part-way, Panel closes them innermost first, then the body region
    around them. The app's own end-of-frame clean-up closes a child region
    before the tab bars, tables and node editors inside it, which is out of
    order once the script's content sits inside the footer's body region."""

    def __init__(self, module):
        self._ui = module
        self._open = []

    def __getattr__(self, name):
        attr = getattr(self._ui, name)
        if not callable(attr):
            return attr
        if name == "tree_node":
            def tree_node(*a, **k):
                r = attr(*a, **k)
                if r:
                    self._open.append("tree_pop")
                return r
            return tree_node
        closer = _CLOSERS.get(name)
        if closer is not None:
            def opening(*a, **k):
                self._open.append(closer)
                return attr(*a, **k)
            return opening
        if name == "tree_pop" or name in _CLOSERS.values():
            def closing(*a, **k):
                for i in range(len(self._open) - 1, -1, -1):
                    if self._open[i] == name:
                        del self._open[i]
                        break
                return attr(*a, **k)
            return closing
        return attr

    def unwind(self):
        """Close what draw() left open, innermost first."""
        while self._open:
            getattr(self._ui, self._open.pop())()


class Panel:
    """One `aii.ui` window, identified by `key`, with a run loop around it.

    `draw(ui)` is called once per frame between `begin_frame`/`end_frame`; it
    gets the `aii.ui` module so it can call `ui.text(...)`, `ui.button(...)`,
    and so on directly, exactly as the raw API in `AII-UI.md` describes.
    """

    def __init__(self, key, title, w=360, h=240, hz=10):
        self.key = key
        self.title = title
        self.w = w
        self.h = h
        self.hz = hz
        self._last_error = None
        self.opened_at = datetime.datetime.now()   # reset by open()

    def set_title(self, title):
        """Re-title the window. `ui.open` on a key that is already open only
        updates its title and size, so this is safe to call at any time."""
        if title and title != self.title:
            self.title = title
            ui.open(self.key, self.title, self.w, self.h)

    def open(self):
        """Open the window if it is not already. Returns False (and logs)
        when the app refuses -- most often the six-window cap."""
        ok = ui.open(self.key, self.title, self.w, self.h)
        if not ok:
            aii.log("aii_ui: '%s' was refused (window cap or bad key?)" % self.key)
        else:
            self.opened_at = datetime.datetime.now()
        return ok

    def footer_text(self):
        return "Opened " + self.opened_at.strftime("%a %d %b %H:%M:%S")

    def close(self):
        ui.close(self.key)

    def frame(self, draw):
        """Record and submit exactly one frame. `run()` is this in a loop;
        call `frame` directly if you already have your own loop (a policy
        driving several panels, say) and just want one tick of this one."""
        ui.begin_frame(self.key)
        ui.begin_child(BODY_ID, 0.0, -FOOTER_H)
        tracked = _Tracked(ui)
        try:
            draw(tracked)
        except Exception as exc:
            # A typo in one frame's draw() must not take the window down --
            # the whole point of a panel is that it keeps showing whatever it
            # last drew successfully. Report once per distinct message so a
            # broken draw does not flood the settings panel every tick.
            text = str(exc) or exc.__class__.__name__
            if text != self._last_error:
                self._last_error = text
                aii.status("%s: %s" % (self.key, text), False)
            # draw() may have stopped inside a tab bar, table or node editor:
            # close those first, innermost first, then the body region.
            tracked.unwind()
        ui.end_child()
        ui.separator()
        ui.text_disabled(self.footer_text())
        ui.end_frame()

    def run(self, draw):
        """Open the window (if needed) and loop: begin_frame, draw(ui),
        end_frame, sleep 1/hz, until the user closes the window or the app
        is quitting. Call this on its own thread (or use `run_in_thread`) --
        it does not return until the window closes."""
        # Always open, even if a window with this key exists: opening an
        # existing key *takes it over*. A previous run of this script still
        # looping on the same key then sees is_open() go False and stops, so
        # the window shows only the new run instead of flickering between the
        # two. Re-running a rewritten script is the normal case, not an error.
        self.open()
        period = 1.0 / self.hz if self.hz > 0 else 0.1
        while ui.is_open(self.key) and not aii.should_quit():
            self.frame(draw)
            time.sleep(period)

    def run_in_thread(self, draw):
        """Start `run(draw)` on a daemon thread and return it. This is the
        usual way to show a panel from an action: open it, hand it a draw
        function, and let the action return while the thread keeps drawing."""
        t = threading.Thread(target=self.run, args=(draw,),
                              name="aii_ui:" + self.key, daemon=True)
        t.start()
        return t
