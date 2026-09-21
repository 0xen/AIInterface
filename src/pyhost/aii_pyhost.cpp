#include "aii_pyhost.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/app_bus.h"

namespace py = pybind11;

namespace {

// Everything the module talks to, filled in by aiiPyHostRegister before the
// interpreter exists. One host per process, like the engine's, and for the
// same reason: CPython is process-global and a second interpreter would fight
// over the GIL.
struct Host {
  aii::AppBus* bus = nullptr;
  std::vector<std::string> scripts;
  std::atomic<bool> quit{false};

  // **One queue, several readers.** `AppBus::drain_events()` empties the
  // queue, so whoever calls it first gets everything and everyone else gets
  // nothing. That is right for the app — the frame loop publishes, one hatch
  // consumes — and wrong the moment two scripts are running: the first run of
  // two scripts here had the second receive exactly zero events, silently.
  //
  // So the module drains once and fans out. Each Python thread gets its own
  // cursor the first time it polls, and `pump()` copies every drained line
  // into all of them. Capped and oldest-dropped, because a thread that
  // subscribes and then stops polling must cost a bounded amount and not the
  // rest of the run.
  std::mutex mutex;
  std::map<std::thread::id, std::deque<std::string>> subs;
};

constexpr std::size_t kSubMax = 256;

Host g_host;

// Drains the bus into every subscriber, then hands this thread its own. The
// caller holds the GIL throughout, which is what makes "one drainer wins the
// race" harmless: the loser simply finds nothing and reads its own queue.
std::vector<std::string> take_for_this_thread() {
  std::vector<std::string> drained;
  if (g_host.bus) drained = g_host.bus->drain_events();

  std::lock_guard<std::mutex> lock(g_host.mutex);
  std::deque<std::string>& mine = g_host.subs[std::this_thread::get_id()];
  for (auto& [id, queue] : g_host.subs) {
    for (const std::string& line : drained) {
      if (queue.size() >= kSubMax) queue.pop_front();
      queue.push_back(line);
    }
    (void)id;
  }
  std::vector<std::string> out(mine.begin(), mine.end());
  mine.clear();
  return out;
}

// A refused post is not an exception. The bus's whole contract is that bad
// input is counted and logged and never fatal, and a script that fell over
// because the inbox was briefly full would be a worse app than one that
// dropped a frame of decoration. The refusal still reaches the log through
// `AppBus::take_status()`, which the frame loop drains.
bool post_line(const std::string& line) {
  if (!g_host.bus) return false;
  return g_host.bus->post(line, nullptr);
}

// `hold` is only written when it was asked for: leaving it out is what tells
// the app "the default for this verb", which for a clip is the clip's own
// authored length. Writing 0.0 would say the same thing, but only by accident.
void add_hold(aii::BusLine& line, double hold) {
  if (hold > 0.0) line.num("hold", hold);
}

}  // namespace

// The bindings. Registered into CPython's inittab by pybind11 at DLL load,
// which is why this DLL is loaded before the interpreter is started and never
// unloaded afterwards.
// **What this module bounds, and what it does not** (M19.2, finding 12).
//
// Every binding below is a fixed verb over the bus: there is no "run this
// command", no "register a button that invokes something", and every path that
// comes back in is validated by the app, not by the script. That vocabulary is
// the boundary, and it is a real one — it is why a policy cannot ask this app
// to shell out, and why `button` takes a directory and not a command line.
//
// It is not a sandbox, and nothing here should be read as claiming one. The
// interpreter underneath is a full CPython inside `avatar.exe`: `os._exit()`
// ends the process, `ctypes` reaches whatever the process can, and an `import`
// of anything on the machine works. A script is therefore held to what it may
// *ask this app for*, and to nothing else; the gate that decides whether it
// runs at all is consent to the file, in `avatar/action_store.h`.
PYBIND11_EMBEDDED_MODULE(aii, m) {
  m.doc() = "The AIInterface app bus, from Python. See scripts/examples/.";

  // ---- the wire itself -------------------------------------------------
  m.def(
      "post", [](const std::string& line) { return post_line(line); }, py::arg("line"),
      "Post one raw JSON line to the app. False if the bus refused it.");

  m.def(
      "poll_lines", [] { return take_for_this_thread(); },
      "Every event queued since *this thread* last called, oldest first, as "
      "raw JSON lines. Each script thread has its own cursor, so two scripts "
      "both see everything. Prefer poll().");

  m.def(
      "wait_lines",
      [](double timeout) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
        for (;;) {
          std::vector<std::string> out = take_for_this_thread();
          if (!out.empty()) return out;
          if (g_host.quit.load() || std::chrono::steady_clock::now() >= deadline) {
            return std::vector<std::string>{};
          }
          // The GIL goes back while we sleep, so a second script thread runs
          // normally. Nothing here ever blocks the frame loop: the app's side
          // of the bus is a short mutex around a bounded deque and the
          // interpreter is on its own thread.
          py::gil_scoped_release release;
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      },
      py::arg("timeout") = 0.1,
      "Poll until something arrives or the timeout expires. Returns early on "
      "shutdown. Prefer wait().");

  // M10.2. **The other half of the fan-out, and it closes a real hole.**
  //
  // `g_host.subs` is written with `operator[]` on `std::this_thread::get_id()`
  // and, until this existed, was never erased anywhere in this file. Two
  // consequences, neither of which could bite while nothing was ever started
  // after launch — and actions are exactly the thing that starts threads after
  // launch:
  //
  //   * A finished thread left a cursor behind, and every later drain copied
  //     every event into it forever. Bounded in memory at kSubMax, but the
  //     per-event copy cost grew linearly in the number of dead cursors.
  //   * **Windows recycles thread ids.** A new thread handed a dead one's id
  //     inherited up to 256 stale events on its first poll — a silent
  //     wrong-data bug of exactly the shape M2.6 existed to fix, arriving with
  //     no error, no log line and no dropped counter.
  //
  // The structural answer is that only the dispatcher polls (see the bootstrap
  // below) and actions never do. This is the belt to that's braces: any thread
  // that ever polled can say it is done, and the bootstrap says it in a
  // `finally` for every thread it starts, so the hole is closed for every
  // future caller rather than only for actions.
  m.def(
      "unsubscribe",
      [] {
        std::lock_guard<std::mutex> lock(g_host.mutex);
        g_host.subs.erase(std::this_thread::get_id());
      },
      "Forget this thread's event cursor. Call it before a thread that has "
      "polled exits; the generated bootstrap already does this for every "
      "thread it starts.");

  m.def(
      "should_quit", [] { return g_host.quit.load(); },
      "True once the app has asked scripts to stop. **Every loop must poll "
      "this**: the app joins the script thread at exit, so a loop that never "
      "checks it is a loop that holds the app open.");

  // ---- saying something the user can see --------------------------------
  m.def(
      "log",
      [](const std::string& text) {
        post_line(aii::BusLine("script.log").str("text", text).done());
      },
      py::arg("text"), "Write one line into the app's log, prefixed [py].");

  m.def(
      "status",
      [](const std::string& text, bool ok) {
        post_line(aii::BusLine("script.status").str("text", text).flag("ok", ok).done());
      },
      py::arg("text"), py::arg("ok") = true,
      "Put one line under Scripts in the app's settings surface. ok=False "
      "colours it as a warning. This is where a script's own failures go.");

  // ---- the avatar -------------------------------------------------------
  m.def(
      "play",
      [](const std::string& clip, double hold) {
        aii::BusLine line("avatar.play");
        line.str("clip", clip);
        add_hold(line, hold);
        return post_line(line.done());
      },
      py::arg("clip"), py::arg("hold") = 0.0,
      "Ask for a clip. This is a LEASE inside the app's own state machine, "
      "not a seizure of the avatar: the user taking the microphone outranks "
      "it, and it expires (hold seconds, or the clip's authored length, "
      "capped at 30 s). Re-ask to hold it longer.");

  m.def(
      "release", [] { return post_line(aii::BusLine("avatar.release").done()); },
      "Hand the avatar straight back to the app's own policy.");

  m.def(
      "sprite",
      [](const std::string& name, bool on, double hold) {
        aii::BusLine line("avatar.sprite");
        line.str("name", name).flag("on", on);
        add_hold(line, hold);
        return post_line(line.done());
      },
      py::arg("name"), py::arg("on") = true, py::arg("hold") = 0.0,
      "Hold up one of the definition's accessories. Unlike a clip this keeps "
      "its lease through the microphone — the mic wants the body back, not "
      "the decoration — but the muted bubble still outranks it.");

  m.def(
      "cells",
      [](const std::vector<double>& cells, const std::string& layer, bool add, double hold) {
        // Built by hand rather than through BusLine, which has no array
        // field: the cell list is the one place the outbound vocabulary is
        // not flat scalars.
        std::string out = "{\"t\":\"avatar.cells\",\"layer\":\"";
        out += (layer == "base" ? "base" : "overlay");
        out += "\",\"add\":";
        out += add ? "true" : "false";
        if (hold > 0.0) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%.3f", hold);
          out += ",\"hold\":";
          out += buf;
        }
        out += ",\"cells\":[";
        for (std::size_t i = 0; i < cells.size(); ++i) {
          if (i) out += ',';
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%.0f", std::floor(cells[i]));
          out += buf;
        }
        out += "]}";
        return post_line(out);
      },
      py::arg("cells"), py::arg("layer") = "overlay", py::arg("add") = false,
      py::arg("hold") = 0.0,
      "Stamp cells over the composed avatar as a flat [x, y, rgba, ...] list. "
      "An overlay, never a write into the art, so a script cannot destroy the "
      "body. Leased like everything else here.");

  m.def(
      "clear_cells", [] { return post_line(aii::BusLine("avatar.clear").done()); },
      "Drop the cell overlay.");

  m.def(
      "load_avatar",
      [](const std::string& name) {
        return post_line(aii::BusLine("avatar.load").str("name", name).done());
      },
      py::arg("name"), "Switch to another definition under avatars\\, by name.");

  // ---- theme and toolbar ------------------------------------------------
  m.def(
      "theme",
      [](const std::string& name) {
        return post_line(aii::BusLine("theme.set").str("name", name).done());
      },
      py::arg("name"), "Select one of the definition's named palettes.");

  m.def(
      "colour",
      [](const std::string& value) {
        return post_line(aii::BusLine("theme.colour").str("value", value).done());
      },
      py::arg("value"), "Pick the body colour as #rrggbb; the features derive from it.");

  m.def(
      "button",
      [](const std::string& id, const std::string& label, const std::string& path,
         const std::string& tip) {
        return post_line(aii::BusLine("toolbar.button")
                             .str("id", id)
                             .str("label", label)
                             .str("tip", tip)
                             .str("path", path)
                             .done());
      },
      py::arg("id"), py::arg("label"), py::arg("path"), py::arg("tip") = "",
      "Add a toolbar button that opens a directory. The path must exist and "
      "be a directory; there is deliberately no way to register a command.");

  m.def(
      "clear_buttons", [] { return post_line(aii::BusLine("toolbar.clear").done()); },
      "Remove every registered toolbar button.");

  // ---- schedules (M2b.2) -------------------------------------------------
  m.def(
      "schedule",
      [](const std::string& in, const std::string& say, const std::string& task,
         const std::string& cwd, const std::string& name, const std::string& label,
         const std::string& grade, const std::string& echo) {
        return post_line(aii::BusLine("schedule.create")
                             .str("in", in)
                             .str("say", say)
                             .str("task", task)
                             .str("cwd", cwd)
                             .str("name", name)
                             .str("label", label)
                             .str("grade", grade)
                             .str("echo", echo)
                             .done());
      },
      py::arg("in_"), py::arg("say") = "", py::arg("task") = "", py::arg("cwd") = "",
      py::arg("name") = "", py::arg("label") = "", py::arg("grade") = "", py::arg("echo") = "",
      "Defer one thing inside this session. `in_` is a delay a person would "
      "say -- '90s', '10m', '1h30m' -- capped at a day, because a schedule "
      "lives only as long as the app and a promise past that is the failure.\n"
      "\n"
      "Either `say` (the exact words spoken when it fires) or `task` with an "
      "absolute `cwd` (a worker does the work then, and Claude reports it in "
      "its own words). `grade` overrides that default: 'fixed' speaks the "
      "line as it stands, 'phrased' spends a turn on Claude's own words. The "
      "assistant is not told about `grade` -- it picks by the shape of what "
      "was asked -- but a script has no shape to signal with, so it says so "
      "outright. `echo` comes back on the reply so several scripts can share "
      "one bus.\n"
      "\n"
      "Returns whether the line was accepted, not whether the schedule was "
      "made: watch for a `schedule.created` or `schedule.refused` event, and "
      "for `schedule.fired` when it comes due.");

  m.def(
      "cancel_schedule",
      [](double id, const std::string& echo) {
        return post_line(
            aii::BusLine("schedule.cancel").num("id", id, 0).str("echo", echo).done());
      },
      py::arg("id"), py::arg("echo") = "",
      "Cancel a pending schedule by the id `schedule.created` gave you. Also "
      "stops a worker one already started. Answered by `schedule.cancelled` "
      "with ok=False if it had already gone off -- and, unlike the "
      "assistant's own cancel, nothing is said aloud about it either way.");

  m.def(
      "list_schedules",
      [](const std::string& echo) {
        return post_line(aii::BusLine("schedule.list").str("echo", echo).done());
      },
      py::arg("echo") = "",
      "Ask what is pending. Answered by one `schedule.pending` event per item "
      "-- schedules not yet due *and* workers a schedule has already started "
      "-- then one `schedule.list` carrying the count.");

  // ---- the transport row (M2.9) -----------------------------------------
  // One helper per control, not a `press(button)`: half of these are levels
  // the panel owns rather than presses — `mute(False)` means the same thing
  // however many times it arrives, where "press mute" twice unmutes — and a
  // press-by-name verb would couple a script to the button registry, which is
  // the one door `ButtonActionKind::Invoke` is deliberately kept behind.
  m.def(
      "mic",
      [](bool on, const std::string& echo) {
        return post_line(aii::BusLine("session.mic").flag("on", on).str("echo", echo).done());
      },
      py::arg("on") = true, py::arg("echo") = "",
      "Latch the microphone on or off -- `toggle_mic()`, the same call a short "
      "click on the microphone button makes and the only path into the latch. "
      "Not hold-to-dictate, which is a gesture and is not on the bus.\n"
      "\n"
      "Answered by `session.mic` carrying `open`, which also arrives when the "
      "user clicks the button, when Stop drops the latch and when a timeout "
      "closes it. `session.refused` if the microphone cannot open yet.");

  m.def(
      "mute",
      [](bool on, const std::string& echo) {
        return post_line(aii::BusLine("session.mute").flag("on", on).str("echo", echo).done());
      },
      py::arg("on") = true, py::arg("echo") = "",
      "Mute or unmute Claude's *voice*. Voice only: the reply still arrives as "
      "text and the transcript is untouched. Turning it on cuts what is "
      "already being spoken, mid-sentence.\n"
      "\n"
      "This writes the same flag the mute button and the S key write, so it is "
      "remembered in settings.json exactly as a click is. Answered by "
      "`session.muted` carrying `on` -- a fact, published whoever changed it, "
      "and published even when your call changed nothing.");

  m.def(
      "stop",
      [](const std::string& echo) {
        return post_line(aii::BusLine("session.stop").str("echo", echo).done());
      },
      py::arg("echo") = "",
      "Stop: cancel the reply in flight, drop the microphone latch, pause "
      "every running worker. Answered by `session.stopped`.");

  m.def(
      "reset",
      [](const std::string& echo) {
        return post_line(aii::BusLine("session.reset").str("echo", echo).done());
      },
      py::arg("echo") = "",
      "Throw the conversation away and start a fresh one -- everything Stop "
      "does, then the `claude` child is ended and relaunched with the same "
      "options. Workers and schedules survive it; mute, language and the "
      "avatar are untouched.\n"
      "\n"
      "**This call is the confirmation.** The button asks twice because it is "
      "24 px wide and cannot be undone; a line of Python is already "
      "deliberate. What you do inherit is the guard that is not about "
      "confirmation: a second reset while one is running is refused, and so is "
      "a reset of an empty conversation.\n"
      "\n"
      "Answered by `session.resetting` with ok=True when it was *accepted*. It "
      "takes a second or so; wait for `session.state` to come back.");

  m.def(
      "handoff",
      [](const std::string& echo) {
        return post_line(aii::BusLine("session.handoff").str("echo", echo).done());
      },
      py::arg("echo") = "",
      "M3.15. Hand the conversation over to a fresh session of itself: the "
      "housekeeping line is spoken first, the outgoing session writes a short "
      "note about where the conversation had got to, the `claude` child is "
      "replaced, and that note rides in with the new session's first turn. "
      "The wording is forgotten; the direction is not.\n"
      "\n"
      "The app does this by itself when the context window passes "
      "`handoff.threshold` in settings.json (0.40 by default, the user's own "
      "40%). This call is for a script that wants its own rule instead: "
      "`session.usage` publishes `ctx` on every change, so the policy is "
      "yours to write and this is the act.\n"
      "\n"
      "It does not interrupt anything. A reply in flight finishes, a sentence "
      "being spoken is finished, an utterance being dictated is finished, and "
      "only then does the handover start -- so `ok=True` means accepted, not "
      "done, and it can be several seconds before `session.state` settles. "
      "Refused while a restart or another handover is already running.");

  m.def(
      "say",
      [](const std::string& text, const std::string& echo) {
        return post_line(aii::BusLine("session.say").str("text", text).str("echo", echo).done());
      },
      py::arg("text"), py::arg("echo") = "",
      "Send text as the user's turn, exactly as typing it and pressing Enter "
      "does. Refused for the reasons the message field refuses, in the same "
      "words -- 'the microphone is open', 'Claude is still replying' -- as a "
      "`session.refused` event. Answered by `session.said`, and the reply "
      "arrives as `session.state` and (with --bus-text) `turn.text`.");

  m.def(
      "session_info",
      [](const std::string& echo) {
        return post_line(aii::BusLine("session.get").str("echo", echo).done());
      },
      py::arg("echo") = "",
      "Ask what the session is doing. Answered by one `session.info` -- state, "
      "mic, muted, resetting, resettable, status -- plus the two level facts.");

  // ---- the settings surface (M2.9) --------------------------------------
  m.def(
      "model",
      [](const std::string& name, const std::string& echo) {
        return post_line(aii::BusLine("settings.model").str("name", name).str("echo", echo).done());
      },
      py::arg("name"), py::arg("echo") = "",
      "Pick the conversational instance's base model, by its settings.json "
      "key: 'default', 'opus', 'sonnet' or 'haiku'. Anything else is refused "
      "rather than written, because an unknown --model starts a child in which "
      "every turn fails.\n"
      "\n"
      "The same write the picker makes, with the same reach: it is remembered, "
      "and it reaches Claude when the child next starts. Workers are unaffected "
      "-- they are separate processes with their own grant.\n"
      "\n"
      "Answered by `settings.changed` with key='model'.");

  m.def(
      "tools",
      [](const std::string& group, bool on, const std::string& echo) {
        return post_line(aii::BusLine("settings.tools")
                             .str("group", group)
                             .flag("on", on)
                             .str("echo", echo)
                             .done());
      },
      py::arg("group"), py::arg("on") = true, py::arg("echo") = "",
      "Grant or withhold one tool group for the conversational instance: "
      "'web', 'file_read' or 'file_write'. An enabled group is *granted*, not "
      "offered -- nothing in this app can answer a permission prompt -- so "
      "'file_write' lets Claude change files on this PC without asking. Reaches "
      "the child when it next starts, like the model.");

  m.def(
      "language",
      [](bool english, bool japanese, const std::string& echo) {
        return post_line(aii::BusLine("settings.language")
                             .flag("english", english)
                             .flag("japanese", japanese)
                             .str("echo", echo)
                             .done());
      },
      py::arg("english") = true, py::arg("japanese") = false, py::arg("echo") = "",
      "Which languages are recognised and spoken. Takes effect at once, on an "
      "utterance already in flight. Both false is refused, the way the "
      "checkboxes lock the last one on.");

  m.def(
      "auto_listen",
      [](bool on, const std::string& echo) {
        return post_line(
            aii::BusLine("settings.auto_listen").flag("on", on).str("echo", echo).done());
      },
      py::arg("on") = true, py::arg("echo") = "",
      "Whether the app latches the microphone for itself at startup. Read at "
      "launch, so this is for the next run.");

  m.def(
      "listen_timeout",
      [](double seconds, const std::string& echo) {
        return post_line(aii::BusLine("settings.listen_timeout")
                             .num("seconds", seconds, 0)
                             .str("echo", echo)
                             .done());
      },
      py::arg("seconds"), py::arg("echo") = "",
      "How long a latched microphone may hear nothing before it closes itself. "
      "0 is never; otherwise 15 to 600 seconds, the control's own clamp. "
      "Honoured by a latch that is already open -- there is no restart.");

  m.def(
      "chat",
      [](bool on, const std::string& echo) {
        return post_line(aii::BusLine("settings.chat").flag("on", on).str("echo", echo).done());
      },
      py::arg("on") = true, py::arg("echo") = "", "Open or close the transcript panel.");

  m.def(
      "open_settings",
      [](bool on, const std::string& echo) {
        return post_line(aii::BusLine("settings.open").flag("on", on).str("echo", echo).done());
      },
      py::arg("on") = true, py::arg("echo") = "",
      "Open or close the settings surface -- the cog in the sidebar.");

  m.def(
      "avatar_mode",
      [](const std::string& value, const std::string& echo) {
        return post_line(
            aii::BusLine("settings.avatar_mode").str("value", value).str("echo", echo).done());
      },
      py::arg("value"), py::arg("echo") = "",
      "Cycle target for the avatar-visibility disc: 'always', 'when_talking' "
      "or 'hidden'.");

  m.def(
      "settings_info",
      [](const std::string& echo) {
        return post_line(aii::BusLine("settings.get").str("echo", echo).done());
      },
      py::arg("echo") = "",
      "Ask what every setting is. Answered by one `settings.info`, read off "
      "the panel rather than off the file, so it is right on the frame before "
      "the debounced write has happened.");

  m.attr("scripts") = g_host.scripts;

  // The two calls a script actually uses are parsed JSON, not lines. Written
  // in Python because `json` already does it and a second parser in C++ would
  // be a second set of bugs about the transcript's UTF-8.
  py::exec(R"PY(
import json as _json

def _parse(lines):
    out = []
    for line in lines:
        try:
            out.append(_json.loads(line))
        except ValueError:
            pass
    return out

def poll():
    """Every event queued since the last call, as dicts. Never blocks."""
    return _parse(poll_lines())

def wait(timeout=0.1):
    """poll(), but sleeps up to `timeout` for the first event. Returns early
    on shutdown, so `while not should_quit(): for e in wait(0.2): ...` is the
    loop this module is shaped for."""
    return _parse(wait_lines(timeout))

def send(t, **fields):
    """Post any message, including one this module has no helper for."""
    return post(_json.dumps(dict(fields, t=t)))
)PY",
           m.attr("__dict__"));
}

namespace {

// The bootstrap. It exists for one reason: **a script that fails must say so
// where the user will see it.** Handing the user's files straight to the host
// would put a syntax error in a console nobody is looking at, and would run
// them one after another, so the first script with a loop in it would stop
// the second from ever starting.
constexpr const char* kBootSource = R"PY(# Generated by AIInterface each run. Edits here are overwritten.
import runpy, sys, threading, time, traceback
import aii

# M19.3, finding 33. **A report that the bus refuses is retried, and what is
# still lost is counted into the next one that gets through.**
#
# `aii.status()` returns False when the inbox is full -- which is a transient
# state by construction, since the frame loop drains it every frame -- and the
# old `_report` threw that answer away. The one message in the app whose whole
# job is to say that something broke was the one message that could vanish
# without trace: a script that failed during a busy second reported nothing,
# and the Scripts row said it was running.
#
# The retry is bounded (three tries inside a quarter of a second, on the
# script's own dying thread) because a report is only worth so much delay, and
# the counter is what keeps the rest honest: the next report that does land
# carries "+N dropped", so the row can never imply it is telling the whole
# story when it is not.
_dropped = 0
_dropped_lock = threading.Lock()

def _status(text, ok=False):
    global _dropped
    with _dropped_lock:
        carried = _dropped
    if carried:
        text = "%s (+%d earlier report%s dropped)" % (
            text, carried, "" if carried == 1 else "s")
    for delay in (0.0, 0.05, 0.2):
        if delay:
            time.sleep(delay)
        if aii.status(text, ok):
            with _dropped_lock:
                _dropped -= carried
            return True
    with _dropped_lock:
        _dropped += 1
    sys.stderr.write("aii: the bus refused this status, so it is only here: %s\n" % text)
    return False

def _report(label, path):
    # The last line first: it is the one that names the mistake, and the
    # settings surface has room for a line, not a traceback.
    text = traceback.format_exc()
    last = [x for x in text.strip().splitlines() if x.strip()][-1]
    _status("%s: %s" % (label, last), False)
    sys.stderr.write(text)

def _run(path):
    try:
        runpy.run_path(path, run_name="__main__")
    except BaseException:
        if aii.should_quit():
            # The app is closing and this is how it broke the script's loop
            # (KeyboardInterrupt, or on Windows sometimes an OSError about a
            # signal race). Not a fault, and not something to report as one.
            return
        _report(path.replace("\\", "/").rsplit("/", 1)[-1], path)
    else:
        aii.log("'%s' finished" % path)
    finally:
        # M10.2. A policy's loop polls, so it has a cursor, and a cursor whose
        # thread has died is the recycled-id hole. Said here rather than left
        # to the policy author, because the author of a policy that crashed is
        # in no position to say anything.
        aii.unsubscribe()

# ---- actions (M10.2) ------------------------------------------------------
#
# **An action is exec'd from source in a fresh namespace on every call.** There
# is no cache, no importlib.reload, no sub-interpreter and no unload: re-reading
# the file per call *is* the reload, so there is nothing left in sys.modules to
# go stale and no half-reloaded state to be in. The cost is a compile per call,
# which for tens of lines called conversationally is not worth a cache that
# could be wrong.
#
# (The one real limitation, stated rather than fixed: a helper module an action
# `import`s IS cached by CPython and IS stale until the app restarts. Fixing
# that is the unload problem again, which is the thing this design exists to
# not have.)
def _run_action(name, path):
    try:
        ns = runpy.run_path(path, run_name="__aii_action__")
        fn = ns.get("run")
        if not callable(fn):
            _status("%s: no run() to call" % name, False)
            return
        fn()
    except BaseException:
        if aii.should_quit():
            return
        _report(name, path)
    else:
        aii.log("action '%s' finished" % name)
    finally:
        # An action must never poll -- see _dispatch -- so in the ordinary case
        # there is no cursor here to erase. It is called anyway, because "no
        # cursor" is a property of code somebody else wrote and this is the one
        # line that makes it not matter if they got it wrong.
        aii.unsubscribe()

# **One thread owns the only cursor, and actions never poll.** This is the
# structural half of the fix above: whatever an action does, it cannot register
# a per-thread cursor that outlives it, because the thread it runs on never
# calls poll/wait at all -- this one does, once, for the life of the app.
#
# Each call gets its own daemon thread. A Python thread cannot be killed, so a
# hung action must be *contained* rather than timed out: on its own thread it
# costs one leaked thread and never the dispatcher, and the app still exits,
# because daemon threads do not hold the process open.
def _dispatch():
    try:
        while not aii.should_quit():
            for e in aii.wait(0.2):
                if e.get("t") != "script.dispatch":
                    continue
                name = e.get("name") or ""
                path = e.get("path") or ""
                if not name or not path:
                    continue
                threading.Thread(target=_run_action, args=(name, path),
                                 name="action:" + name, daemon=True).start()
    except (KeyboardInterrupt, OSError):
        pass
    finally:
        aii.unsubscribe()

_threads = []
# The dispatcher first, and always -- there may be no policies at all now that
# an app with only actions still wants a host. It is a daemon like the rest and
# is not counted in `_threads`: the bootstrap's own loop must not stay alive
# just because the dispatcher is, or the app would never exit.
_dispatcher = threading.Thread(target=_dispatch, name="aii-dispatch", daemon=True)
_dispatcher.start()

for _path in aii.scripts:
    # One thread each, not one after another: these are independent policies
    # reacting to the same events, and every useful one ends in a loop that
    # never returns. aii.post/poll are safe from any thread.
    _t = threading.Thread(target=_run, args=(_path,), name=_path, daemon=True)
    _t.start()
    _threads.append(_t)

# Daemon threads, so a script that ignores should_quit() cannot hold the app
# open at exit; this loop is what keeps the interpreter alive until they are
# genuinely done or the app says stop.
#
# It sleeps rather than polling the bus: polling would subscribe this thread
# to the event stream and then throw every event away, which costs a copy per
# event for nothing. The try is for the shutdown itself — the host raises
# KeyboardInterrupt into this thread to break the sleep, and Windows sometimes
# delivers that as "OSError: Signal 2 ignored due to race condition" instead.
try:
    # The dispatcher is in this condition, and with no policies at all it is
    # the whole of it: `any([])` is False, so without it an app whose only
    # scripting is actions would tear the interpreter down on its first frame
    # and every later call would find no host.
    while not aii.should_quit() and (_dispatcher.is_alive() or
                                     any(t.is_alive() for t in _threads)):
        time.sleep(0.05)
except (KeyboardInterrupt, OSError):
    pass
)PY";

}  // namespace

extern "C" {

bool aiiPyHostRegister(aii::AppBus* bus, const char* const* scriptPaths, int scriptCount) {
  // M10.2. **Zero scripts is now legal.** It used to mean "nothing to do", and
  // that was right when a policy was the only thing Python was for. An app
  // whose only scripting is actions has no policies at all and still needs the
  // host up, because the dispatcher lives in it.
  if (!bus || scriptCount < 0 || (scriptCount > 0 && !scriptPaths)) return false;
  if (g_host.bus) return false;
  if (scriptCount > 0) g_host.scripts.assign(scriptPaths, scriptPaths + scriptCount);
  g_host.quit.store(false);
  g_host.bus = bus;
  return true;
}

void aiiPyHostUnregister(void) {
  // Symmetrical with the registration above and nothing more. The scripts list
  // is cleared too, so a retry that discovers a different set does not inherit
  // the old one through `aii.scripts`.
  g_host.bus = nullptr;
  g_host.scripts.clear();
  g_host.quit.store(false);
}

void aiiPyHostQuit(void) { g_host.quit.store(true); }

const char* aiiPyHostBootName(void) { return "_aii_boot.py"; }

const char* aiiPyHostBootSource(void) { return kBootSource; }
}
