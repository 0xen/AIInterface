"""Say hello, as a worked example of what an action is.

Everything below the first line of this docstring is for whoever opens the
file; only the first line is ever put in the prompt, so it is the one that has
to say what this does.

An action is a `.py` in `scripts\\actions\\` that defines `run()`. Its name is
its filename -- this one is called `hello_action` -- and it is called by name
and only by name: nothing can hand the app a path or a body of code.

It is `exec`'d from source in a fresh namespace on **every** call, so editing
this file and saving it is the whole of "reloading" it. There is no cache to
clear and no restart to pay.

Two rules worth knowing before you write one of your own:

  * **Never call `aii.poll()`, `aii.wait()` or `aii.poll_lines()` here.** An
    action is called and returns; the one long-lived dispatcher thread owns the
    only event cursor, and a cursor left behind by a thread that has exited is
    a bug that reappears later on a completely different script, because
    Windows recycles thread ids.
  * **Nothing here comes back to the conversation as a return value.** An
    action runs beside the turn rather than inside it, so that a slow one
    cannot stall the reply. Speak, log, or set a status line instead -- all
    three reach the user, and the last two reach the model on the next turn.

A file that sits in `scripts\\` itself rather than in `scripts\\actions\\` is a
*policy*: it is started once at launch, on its own thread, and usually never
returns. That is a different animal and the directory is the whole of the
difference.
"""

import aii


def run():
    aii.log("hello from an action")
    aii.status("hello_action ran", True)
