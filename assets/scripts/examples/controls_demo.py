"""Working the app's own controls from a script.

Copy this one level up (into %APPDATA%\\AIInterface\\scripts) to have it run,
or pass it with --script.

Four controls, in order: mute and unmute, change the model, send a
prompt and get the reply, reset the context. Every call here lands on the same
C++ the control lands on -- `mic` is `toggle_mic()`, `mute` writes the flag the
mute button and the S key write, `say` is the message field, `reset` is the
transport row's fourth slot -- so there is nothing in this file that a hand on
the mouse could not do, and nothing the mouse can do that this file has to
work around.

Two shapes of answer, and the difference is worth reading:

  * **Levels answer as facts.** `session.muted` and `session.mic` are published
    the way `session.state` is -- on change, under their own key, with no
    `echo` -- because the mute may have come from this script, from the button,
    from the S key or from another script, and the same event has to arrive in
    all four cases. Setting one also forces the fact out on that frame even
    when nothing moved, so muting an already-muted app still answers.
  * **Discrete acts answer with `echo`.** `settings.changed`, `session.said`,
    `session.resetting` and every refusal carry back whatever `echo=` was sent,
    exactly as `schedule.created` does, so two scripts on one bus can tell
    their replies apart.

The model is put back at the end. This demo is not a reason for someone to
find their app on Haiku tomorrow.
"""
import aii

PROMPT = "Reply with exactly: the script is talking."
ECHO = "controls"


def until(want, timeout=20.0, **match):
    """Wait for one event of type `want` whose fields match, and return it.

    Every reply on this bus is an event -- there are no return values on a
    wire -- so this is the shape of every step below: post, then watch. A
    refusal on either new family ends the wait rather than running it out: the
    app has already said no, and sitting here for the rest of the timeout would
    only hide which line was wrong.
    """
    left = timeout
    while left > 0.0 and not aii.should_quit():
        for e in aii.wait(0.2):
            t = e.get("t", "")
            if t in ("session.refused", "settings.refused") and t != want:
                raise RuntimeError("refused: %s (%s)"
                                   % (e.get("reason", ""), e.get("verb") or e.get("key")))
            if t != want:
                continue
            if all(e.get(k) == v for k, v in match.items()):
                return e
        left -= 0.2
    raise RuntimeError("timed out waiting for %s %r" % (want, match))


def step(n, text):
    aii.log("[%d/4] %s" % (n, text))


# Wait for the engines. A script starts the moment the interpreter is up, which
# is several seconds before the session is, and `say` refuses while the app is
# still starting exactly as the message field does -- so a demo that did not
# wait would be demonstrating the refusal instead.
for _ in range(300):
    aii.session_info(echo="boot")
    if until("session.info", echo="boot")["state"] == "idle":
        break
else:
    raise RuntimeError("the session never came up")

# What is true before anything is touched, so every claim below is a change
# from something rather than an assertion about a default.
aii.session_info(echo=ECHO)
start = until("session.info", echo=ECHO)
aii.settings_info(echo=ECHO)
was = until("settings.info", echo=ECHO)
aii.log("before: state=%s muted=%s mic=%s model=%s"
        % (start["state"], start["muted"], start["mic"], was["model"]))

# ---- 1. mute and unmute ---------------------------------------------------
step(1, "mute")
aii.mute(True)
until("session.muted", on=True)
aii.log("muted: Claude's voice is suppressed at the source; text is untouched")
aii.mute(False)
until("session.muted", on=False)
aii.log("unmuted")

# ---- 2. change the model --------------------------------------------------
# By the settings.json key, not by a free-typed model string: an unknown
# --model starts a child in which every turn fails, so the app refuses the name
# rather than writing it.
step(2, "change the model")
want = "haiku" if was["model"] != "haiku" else "sonnet"
aii.model(want, echo=ECHO)
done = until("settings.changed", key="model", echo=ECHO)
aii.settings_info(echo=ECHO)
now = until("settings.info", echo=ECHO)
aii.log("model %s -> %s (settings.info agrees: %s)"
        % (was["model"], done["value"], now["model"]))
# And the refusal, because a picker that accepted anything would be the bug.
aii.model("gpt-4", echo=ECHO)
no = until("settings.refused", key="model", echo=ECHO)
aii.log("refused: %s" % no["reason"])

# ---- 3. prompt the AI, and hear back --------------------------------------
# `say` is the message field's own door, refusals included: it will not send
# while the microphone is open or while a reply is still coming, and it says
# so in the words the field puts under itself.
step(3, "send a prompt")
aii.say(PROMPT, echo=ECHO)
until("session.said", echo=ECHO)
until("session.state", timeout=30.0, value="thinking")
aii.log("Claude is thinking")
reply = until("session.state", timeout=120.0, value="idle")
aii.log("the turn finished (state=%s)" % reply["value"])

# ---- 4. reset the context -------------------------------------------------
# The button asks twice because it is 24 px wide and cannot be undone. This
# call is the confirmation; what a script still inherits is the part that is
# not about confirmation -- a second reset while one is running is refused, and
# so is a reset of a conversation with nothing in it.
step(4, "reset the context")
aii.reset(echo=ECHO)
until("session.resetting", ok=True, echo=ECHO)
aii.reset(echo="second")
again = until("session.refused", verb="reset", echo="second")
aii.log("a concurrent reset is refused: %s" % again["reason"])
# It takes a second or so. The conversation is gone when there is nothing left
# to reset -- resettable is "the transcript is not empty", which is the same
# rule that lights the button.
for _ in range(60):
    aii.session_info(echo=ECHO)
    after = until("session.info", echo=ECHO)
    if not after["resetting"] and not after["resettable"]:
        break
aii.log("reset done: state=%s resettable=%s (the transcript is empty)"
        % (after["state"], after["resettable"]))

# Put the model back where it was found.
aii.model(was["model"], echo=ECHO)
until("settings.changed", key="model", echo=ECHO)
aii.log("model put back to %s" % was["model"])
aii.status("controls demo: all four done", True)
