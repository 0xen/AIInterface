# The app's own avatar policy, written out in Python.
#
# Copy this file up one directory — into %APPDATA%\AIInterface\scripts\ — and
# it runs at the next start. Nothing in scripts\examples\ runs; that is what
# keeps a machine with no scripts from loading Python at all.
#
# It is here as documentation as much as a starting point. What it does is
# what the built-in C++ policy (AvatarController) already does: map the
# session's state to a clip. Running it changes nothing you can see, which is
# the point — you can delete a rule and watch exactly that rule stop working,
# rather than guessing which half of a new policy broke.
#
# ---------------------------------------------------------------------------
# The two rules worth knowing before you change anything
#
# 1. **A clip request is a lease, not a seizure.** `aii.play("talk")` asks the
#    app's own state machine for the clip and gets it for a while: the hold you
#    give, or the clip's authored length, capped at 30 seconds. It outranks the
#    app's automatic reactions (the wake, the happy on a finished worker) — a
#    deliberate ask beats a reflex — and it loses, every time, to the user
#    taking the microphone. Do not fight that. A script that re-asked on every
#    frame to hold the avatar through someone trying to speak would be a script
#    the app is designed to defeat, and the app wins.
#
#    One visible consequence of that same rule, seen the first time this ran:
#    the wake animation at startup is a *reaction*, and a lease outranks a
#    reaction, so asking for `idle` on the first tick cuts the wake short. If
#    you want the app's entrance, do not ask for anything until the first
#    session.state that is not "loading" has had a second to play out.
#
# 2. **Nothing here runs on the frame loop.** This is a Python thread; the app
#    applies what you post at one defined point in its frame. So you cannot
#    tear a frame and you cannot stall one — but you also cannot assume a post
#    has taken effect by the next line.
#
# ---------------------------------------------------------------------------
# The events (aii.poll() / aii.wait() give you these as dicts)
#
#   {"t": "session.state",  "value": "listening"}   idle | listening |
#                                                   thinking | speaking |
#                                                   loading | failed
#   {"t": "session.level",  "mic": 0.31, "speaker": 0.0}      ~10 Hz
#   {"t": "session.usage",  "ctx": 0.82, "session": 0.44, "week": 0.1}
#   {"t": "worker.state",   "name": "counter", "state": "done",
#                           "activity": "Bash"}
#   {"t": "turn.text",      "role": "assistant", "text": "..."}  (--bus-text)
#
# What you can ask for: aii.play / release / sprite / cells / clear_cells /
# load_avatar / theme / colour / button / clear_buttons / status / log, and
# aii.send(t, **fields) for anything this module has no helper for.

import time

import aii

# state -> clip. The same table as the C++ default, and the same omissions:
# `teaching` has no trigger because nothing in the session distinguishes
# Claude explaining something from Claude replying, and a keyword heuristic
# over the reply text would be guesswork dressed as policy. It is reachable
# from here, which is the whole reason it stays authored — see the note at the
# top of avatar_controller.h.
#
# `listening` is in the table and asking for it is still not a fight: while the
# user has the microphone the app drops every script lease on sight, so this
# line asks for the clip the built-in policy is already showing. That is the
# difference between agreeing with the precedence rule and losing to it.
CLIPS = {
    "listening": "listen",
    "speaking": "talk",
    "thinking": "think",
    "failed": "confused",
    "idle": "idle",
}

# Thinking has to last this long before we ask for the think clip. The first
# token often arrives in 0.2–0.6 s, and a thought bubble for a pause that short
# is a bubble that gets yanked back; filtering at entry beats retracting.
THINK_DELAY = 0.35

# Idle this long and the slime droops. Two minutes, as the plan says.
SLEEPY_AFTER = 120.0

# Context fullness, with hysteresis, so a reading sitting on the line does not
# switch the steam on and off once a second.
CTX_ENTER, CTX_EXIT = 0.85, 0.78
FRUSTRATED_PERIOD = 20.0

TICK = 0.2

state = "loading"
entered = time.monotonic()  # when `state` began; wait() can return early, so
                            # this is a clock and not a count of ticks
ctx_high = False
frustrated_wait = 0.0
last = time.monotonic()


def ask(clip, hold=0.0):
    """Take (or renew) the lease on `clip`.

    Renewing every pass rather than only on a change is deliberate: the lease
    is a duration, so an ambient clip has to be re-asked or it lapses and the
    built-in policy shows through for a frame. `hold` of just over one tick is
    the shortest renewal that never gaps — and it means killing this script
    hands the avatar back inside half a second, which is exactly the property
    the lease exists for. A one-shot (`happy`, `confused`) passes no hold at
    all and gets the clip's own authored length, once.
    """
    aii.play(clip, hold)


aii.status("default_policy.py running")
aii.log("default policy: taking the avatar from Python")

while not aii.should_quit():
    # wait() returns as soon as anything arrives, or after TICK, or at once on
    # shutdown — so this loop is both event-driven and a clock, and it always
    # leaves promptly when the app is closing.
    for event in aii.wait(TICK):
        t = event.get("t")
        if t == "session.state":
            value = event.get("value", "")
            if value != state:
                state, entered = value, time.monotonic()
        elif t == "session.usage":
            ctx = event.get("ctx", -1.0)
            if ctx >= 0.0:
                if not ctx_high and ctx >= CTX_ENTER:
                    ctx_high, frustrated_wait = True, 1.0
                elif ctx_high and ctx < CTX_EXIT:
                    ctx_high = False
        elif t == "worker.state":
            # A finished worker is good news and a broken one is the same news
            # as a broken turn: the avatar has one vocabulary for it. Both are
            # one play of the clip, so no hold is given — the lease is then the
            # clip's own authored length and the policy has the avatar back the
            # moment it ends.
            if event.get("state") == "done":
                ask("happy")
            elif event.get("state") == "failed":
                ask("confused")

    now = time.monotonic()
    dt, last = now - last, now
    state_age = now - entered

    if state == "loading":
        # The loading overlay owns the window and the avatar is drawn at zero
        # alpha behind it. Nothing to decide.
        continue

    if state == "thinking" and state_age < THINK_DELAY:
        # Hold whatever is up — usually `listen`, which reads as attention
        # persisting past the end of the sentence.
        continue

    if state == "idle":
        if ctx_high:
            frustrated_wait -= dt
            if frustrated_wait <= 0.0:
                frustrated_wait = FRUSTRATED_PERIOD
                # A burst, not a permanent state: shaking and steaming forever
                # stops being an expression and becomes a fault.
                ask("frustrated")
                continue
        if state_age >= SLEEPY_AFTER:
            ask("sleepy", TICK * 2)
            continue

    clip = CLIPS.get(state)
    if clip:
        ask(clip, TICK * 2)

aii.status("default_policy.py stopped")
