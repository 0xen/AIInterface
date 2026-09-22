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

import threading
import time

import aii
import aii.ui as ui


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
        return ok

    def close(self):
        ui.close(self.key)

    def frame(self, draw):
        """Record and submit exactly one frame. `run()` is this in a loop;
        call `frame` directly if you already have your own loop (a policy
        driving several panels, say) and just want one tick of this one."""
        ui.begin_frame(self.key)
        try:
            draw(ui)
        except Exception as exc:
            # A typo in one frame's draw() must not take the window down --
            # the whole point of a panel is that it keeps showing whatever it
            # last drew successfully. Report once per distinct message so a
            # broken draw does not flood the settings panel every tick.
            text = str(exc) or exc.__class__.__name__
            if text != self._last_error:
                self._last_error = text
                aii.status("%s: %s" % (self.key, text), False)
        ui.end_frame()

    def run(self, draw):
        """Open the window (if needed) and loop: begin_frame, draw(ui),
        end_frame, sleep 1/hz, until the user closes the window or the app
        is quitting. Call this on its own thread (or use `run_in_thread`) --
        it does not return until the window closes."""
        if self.key not in ui.windows():
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
