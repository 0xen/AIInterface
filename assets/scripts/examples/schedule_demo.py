"""M2b.2 -- scheduling from a script, with no conversational instance involved.

Copy this one level up (into %APPDATA%\\AIInterface\\scripts) to have it run,
or pass it with --script.

It does the whole family in one pass: creates two schedules, lists what is
pending, cancels one of them, and waits for the other to fire. Everything the
app says back arrives as an event, because a bus has no return values -- so
every call here is "post and then watch for the answer", and `echo=` is how a
reply is recognised when more than one script shares the bus.

The grade is the part worth reading. The assistant chooses it by the *shape*
of what it was asked -- words to say, or work to do -- and is never told the
word "grade". A script has no shape to signal with, so it says it outright:

    aii.schedule("30m", task="build main and say if it broke",
                 cwd=r"C:\\github\\AIInterface", name="build", grade="fixed")

is "do the work in half an hour, then say exactly what comes back" -- which is
the one thing the assistant's own verb cannot express.
"""
import aii

TIMER = "6s"      # short enough to watch, long enough to cancel something first
DOOMED = "10m"    # created only to be cancelled

aii.log("schedule demo: creating two schedules")
aii.schedule(TIMER, say="The script set this timer, and it has just gone off.",
             label="the demo timer", echo="demo")
aii.schedule(DOOMED, say="Nobody will ever hear this.", label="the doomed one",
             echo="doomed")

ids = {}
listed = False
doomed_cancelled = False
done = False

while not aii.should_quit() and not done:
    for e in aii.wait(0.2):
        t = e.get("t", "")
        if t == "schedule.created":
            ids[e.get("echo")] = int(e["id"])
            aii.log("created id=%d kind=%s grade=%s in %.0fs"
                    % (e["id"], e["kind"], e["grade"], e["in"]))
            # Both ids in hand: ask what is pending, then cancel one.
            if len(ids) == 2 and not listed:
                listed = True
                aii.list_schedules(echo="demo")
        elif t == "schedule.refused":
            aii.status("schedule refused: %s" % e.get("reason", ""), False)
        elif t == "schedule.pending":
            aii.log("pending id=%d kind=%s grade=%s in %.0fs -- %s"
                    % (e["id"], e["kind"], e["grade"], e["in"], e.get("label", "")))
        elif t == "schedule.list":
            aii.log("that is all of it: %d pending" % e["count"])
            if not doomed_cancelled and "doomed" in ids:
                doomed_cancelled = True
                aii.cancel_schedule(ids["doomed"], echo="doomed")
        elif t == "schedule.cancelled":
            # ok=False means it had already fired. Data, not a sentence: the
            # app says nothing aloud about a cancel a script asked for.
            aii.log("cancel id=%d ok=%s" % (e["id"], e["ok"]))
        elif t == "schedule.fired":
            aii.log("fired id=%d kind=%s grade=%s -- %s"
                    % (e["id"], e["kind"], e["grade"], e.get("label", "")))
            aii.status("the demo timer fired", True)
            done = True
