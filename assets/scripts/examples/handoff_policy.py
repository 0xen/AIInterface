"""M3.15 -- the handover, with the policy moved into Python.

Copy this one level up (into %APPDATA%\\AIInterface\\scripts) to have it run,
or pass it with --script. Set `handoff.threshold` to 0 in settings.json first,
or the app's own rule will fire before this one does; two policies racing is
not a demonstration of either.

----------------------------------------------------------------------------
Why this is an example and not a skill
----------------------------------------------------------------------------

The user asked for the handover "as a Python skill too". **M10.1 -- what a
skill even is in this app -- has not been decided**, and inventing an answer
here to get one feature shipped is how a framework ends up shaped by its first
customer's accident. So the C++ side was built properly and this is what the
Python side can honestly be today: a plain script, against the bus that
already existed, doing the whole job in twenty lines.

That it *is* twenty lines is the useful finding. The app publishes the
trigger (`session.usage` carries `ctx`, the CLI's own context fraction, and
has since M2.9) and now accepts the act (`aii.handoff()`), so nothing about
this feature needed a skill framework to be scriptable. What a skill would
add is not capability.

----------------------------------------------------------------------------
What M10.1 has to decide, with this as the worked example
----------------------------------------------------------------------------

Every one of these is a real fork this script had to walk past:

 1. **Does a skill own a decision, or offer one?** This script cannot stop the
    C++ rule firing -- it has to be switched off in settings.json by hand.
    Either a skill can claim a decision (and the built-in policy stands down
    while it is loaded, and something has to say so on screen), or it cannot,
    and every scripted policy ships with "first, turn the real one off".

 2. **What happens when two skills claim the same decision?** Two scripts both
    watching `ctx` is two handovers. The bus refuses the second, which is the
    right *mechanism* and no answer at all to the question of which script was
    supposed to win.

 3. **Is a skill loaded, or installed?** Scripts run because a `.py` file sits
    in one directory. A skill the assistant can be asked to use needs a name,
    a description the model reads, and somewhere for both to live -- which is
    the same problem `PromptKind::Skill` and its `triggers` field were parked
    against in M3.3 and never resolved.

 4. **Does the model know it is there?** If the assistant is to say "I can
    hand over early if you like", the skill's existence has to reach the
    system prompt, which is a launch argument. A skill loaded mid-session is
    then invisible to the model until the next restart -- or the act of
    loading one restarts the child, which for *this* skill would throw away
    the conversation it exists to preserve.

 5. **What does a skill get that a script does not?** Today: nothing. If the
    answer is still nothing when M10.1 is written, the honest shipping form of
    "as a Python skill" is this file with a better home, and M10 is about
    discovery and naming rather than about a new surface.
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
