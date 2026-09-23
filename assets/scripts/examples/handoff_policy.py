"""A handover policy, written in Python instead of the app's built-in rule.

Copy this one level up (into %APPDATA%\\AIInterface\\scripts) to have it run,
or pass it with --script. Set `handoff.threshold` to 0 in settings.json first,
or the app's own rule will fire before this one does; two policies racing is
not a demonstration of either.

The handover is the app's feature for replacing a session that is running out
of context with a fresh one, mid-conversation, without losing the thread: the
outgoing session leaves a note, the app speaks a short line while it swaps the
child process, and the new session opens with that note as its first turn.

The app publishes the trigger (`session.usage` carries `ctx`, the CLI's own
context fraction) and accepts the act (`aii.handoff()`), so the whole policy
is watch `ctx`, and call `handoff()` once it crosses a threshold -- twenty
lines below, with nothing this script has to work around. This is a script
rather than a built-in setting so the threshold, the logging and the arming
behaviour can be changed without touching the app itself.
"""
import aii

THRESHOLD = 0.40   # of the model's own context window -- 80k tokens of 200k, 400k of 1m
ARMED = True       # one handover per run of this script, to keep the example honest

aii.log("handoff policy: watching ctx, will hand over at %.0f%%" % (THRESHOLD * 100))

while not aii.should_quit():
    for e in aii.wait(0.5):
        t = e.get("t", "")
        if t == "session.handoff":
            aii.log("handover accepted" if e.get("ok") else "handover refused")
        elif t == "session.refused":
            aii.log("refused: %s" % e.get("reason", ""))
        elif t == "session.usage" and ARMED:
            ctx = e.get("ctx", -1.0)
            # -1.0 is "the child has not reported yet", not "nearly empty".
            # Reading it as a number is failure mode (3) in voice_session.cpp.
            if ctx >= 0.0 and ctx >= THRESHOLD:
                ARMED = False
                aii.log("context at %.0f%%: handing over" % (ctx * 100))
                # Accepted, not done. The app speaks the housekeeping line,
                # asks the outgoing session for a note, replaces the child and
                # carries the note into the first turn of the new one --
                # several seconds, and it waits for a gap before it starts.
                aii.handoff(echo="policy")
