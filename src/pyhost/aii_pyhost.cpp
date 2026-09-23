#include "aii_pyhost.h"

#include <pybind11/embed.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/app_bus.h"
#include "core/ui_bridge.h"

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
  // M28. The window store, handed over the same way as `bus` and for the
  // same reason (see the header comment on `aiiPyHostSetUiBridge`). Null
  // until the caller sets it; `aii.ui`'s functions all treat that as "no
  // window host" rather than a null-pointer fault.
  aii::UiBridge* ui = nullptr;
  // M28. `scripts\lib`, the parent of the helper package the bootstrap adds
  // to `sys.path`. Empty when the caller never set it (an old script_host
  // against a new DLL), in which case the bootstrap simply does not add it.
  std::string lib_dir;

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

// ---- aii.ui (M28) ---------------------------------------------------------
//
// One *recording* per thread, matching `ui_bridge.h`'s statement that "any
// thread may record for any key": nothing here associates a thread with one
// window, so two threads driving two panels never collide, and the same
// thread can drive two panels in series as long as it end_frame()s between.
struct UiRecording {
  std::string key;
  aii::UiFrame frame;
  std::vector<std::string> id_stack;
  std::map<std::string, aii::UiResult> results;
  bool active = false;
};
thread_local UiRecording g_rec;
// Per thread: the epoch at which this thread last open()ed each key. See
// ui_bridge.h, "Takeover". Thread-local on purpose -- the point is to tell
// the thread that was superseded apart from the one that superseded it.
thread_local std::map<std::string, std::uint64_t> g_owned;

// A refusal reaches the log once per distinct message per key, not once per
// frame: a window host that never appears (an old build, or one the app
// chose not to give this process) would otherwise fill the log at whatever
// rate the script records.
std::mutex g_ui_log_mutex;
std::map<std::string, std::set<std::string>> g_ui_logged;

void ui_log_once(const std::string& key, const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(g_ui_log_mutex);
    if (!g_ui_logged[key].insert(text).second) return;
  }
  post_line(aii::BusLine("script.log").str("text", "ui(" + key + "): " + text).done());
}

void ui_require_active() {
  if (!g_rec.active) throw std::runtime_error("call begin_frame() first");
}

aii::UiCommand& ui_push(aii::UiOp op) {
  ui_require_active();
  g_rec.frame.cmds.emplace_back();
  aii::UiCommand& c = g_rec.frame.cmds.back();
  c.op = op;
  return c;
}

std::string ui_id(const std::string& label) { return aii::ui_compose_id(g_rec.id_stack, label); }

const aii::UiResult* ui_find(const std::string& label) {
  auto it = g_rec.results.find(ui_id(label));
  return it == g_rec.results.end() ? nullptr : &it->second;
}

// `rgba` is any Python sequence of 3 or 4 floats; a missing alpha is 1.0, as
// in any colour picker. Anything else -- a scalar, a mapping, the wrong
// length -- is a script bug and raises TypeError rather than silently
// drawing black or bright magenta.
void ui_set_rgba(float out[4], const py::object& rgba) {
  if (!py::isinstance<py::sequence>(rgba))
    throw py::type_error("rgba must be a sequence of 3 or 4 floats");
  py::sequence seq = rgba.cast<py::sequence>();
  const std::size_t n = seq.size();
  if (n != 3 && n != 4) throw py::type_error("rgba must have 3 or 4 components");
  for (std::size_t i = 0; i < n; ++i) out[i] = seq[i].cast<float>();
  if (n == 3) out[3] = 1.0f;
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
      [](const std::string& id, const std::string& label, const std::string& tip,
         const std::string& path, const std::string& run) {
        return post_line(aii::BusLine("toolbar.button")
                             .str("id", id)
                             .str("label", label)
                             .str("tip", tip)
                             .str("path", path)
                             .str("run", run)
                             .done());
      },
      py::arg("id"), py::arg("label"), py::arg("tip"), py::arg("path") = "",
      py::arg("run") = "",
      "Add a toolbar button: exactly one of `path=` (opens a directory, which "
      "must exist and be one) or `run=` (runs one of your actions by name -- "
      "the same door a `run name=` line already opens; whether it resolves "
      "or is armed is checked at the click, not here). There is deliberately "
      "no way to register a command string.");

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

  // ---- workers (M32) ------------------------------------------------------
  m.def(
      "tell",
      [](const std::string& name, const std::string& text, const std::string& echo) {
        return post_line(
            aii::BusLine("worker.tell").str("name", name).str("text", text).str("echo", echo).done());
      },
      py::arg("name"), py::arg("text"), py::arg("echo") = "",
      "Pass a note to a worker by name, without starting a new one. A worker "
      "still Working or Starting reads it once its current turn ends; one "
      "that has already finished (Done) is handed a fresh turn on the same "
      "child, so it keeps whatever it had already found. Paused or Failed, "
      "or no worker by that name, is refused.\n"
      "\n"
      "Answered by `worker.told` (`ok`, `reason`, `echo`) and, once the "
      "worker actually replies, the same `worker.state` events a task "
      "produces.");

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
      "and it reaches Claude when the child next starts. Workers have their own "
      "model -- see model_worker() -- and are unaffected by this one.\n"
      "\n"
      "Answered by `settings.changed` with key='model'.");

  // M31.
  m.def(
      "model_worker",
      [](const std::string& name, const std::string& echo) {
        return post_line(
            aii::BusLine("settings.model_worker").str("name", name).str("echo", echo).done());
      },
      py::arg("name"), py::arg("echo") = "",
      "Pick the model background workers run on, by the same settings.json "
      "keys as model(): 'default', 'opus', 'sonnet' or 'haiku'. Unlike the "
      "conversational model this is read fresh at each worker's spawn, so it "
      "reaches the next worker started rather than waiting for a restart, and "
      "the shipped default is 'opus' rather than 'default' -- a worker is "
      "graded on getting the work done, not on how quickly, so it runs on a "
      "cheaper, quicker model unless told otherwise.\n"
      "\n"
      "Answered by `settings.changed` with key='model_worker'.");

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
      "'web', 'file_read', 'file_write' or 'browser'. An enabled group is "
      "*granted*, not offered -- nothing in this app can answer a permission "
      "prompt -- so 'file_write' lets Claude change files on this PC without "
      "asking, and 'browser' (M31, off by default) lets it drive your Chrome "
      "through the Claude in Chrome extension. Reaches the child when it next "
      "starts, like the model; 'browser' also reaches background workers, "
      "which take their Chrome grant from this same switch.");

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

  // ---- windows (M28) ------------------------------------------------------
  //
  // `ui_bridge.h`'s "Results latch" and "The override" sections are the
  // contract these functions read against; `docs/design-script-ui.md` §3-4
  // is the exact command/result shape per op. Every widget function below
  // requires an active recording (`begin_frame()`..`end_frame()`); the ones
  // that start or end one are the two exceptions with their own rules.
  py::module_ ui = m.def_submodule(
      "ui", "ImGui-shaped windows, recorded here and replayed by the app. See "
            "AII-UI.md and docs/design-script-ui.md.");

  ui.def(
      "open",
      [](const std::string& key, const std::string& title, unsigned w, unsigned h) {
        if (!g_host.ui) {
          ui_log_once(key, "no window host");
          return false;
        }
        aii::UiWindowSpec spec;
        spec.key = key;
        spec.title = title;
        spec.w = w;
        spec.h = h;
        std::string err;
        const bool ok = g_host.ui->open(spec, &err);
        if (!ok) ui_log_once(key, err.empty() ? "open refused" : err);
        // This thread now owns the key at this epoch. A later open() of the
        // same key from another thread moves the epoch on, and this thread's
        // is_open() then answers false -- ui_bridge.h, "Takeover".
        if (ok) g_owned[key] = g_host.ui->epoch_of(key);
        return ok;
      },
      py::arg("key"), py::arg("title"), py::arg("w") = 360, py::arg("h") = 240,
      "Open, or re-title and re-size, a window. False (and one aii.log line "
      "per distinct reason) when refused or there is no window host.");

  ui.def(
      "close",
      [](const std::string& key) {
        if (g_host.ui) g_host.ui->close(key);
      },
      py::arg("key"), "Ask the app to destroy this window. A later open() makes a fresh one.");

  ui.def(
      "is_open",
      [](const std::string& key) {
        if (!g_host.ui || !g_host.ui->is_open(key)) return false;
        const auto owned = g_owned.find(key);
        // Superseded: another thread opened this key after we did. Answer
        // false so the old loop ends by itself; the new run owns the window.
        if (owned != g_owned.end() && owned->second != g_host.ui->epoch_of(key)) return false;
        return true;
      },
      py::arg("key"),
      "True while the window exists, the user has not closed it, and no later "
      "open() of the same key from another thread has taken it over.");

  ui.def(
      "epoch", [](const std::string& key) { return g_host.ui ? g_host.ui->epoch_of(key) : 0ull; },
      py::arg("key"),
      "How many times this key has been opened. Moves on when a newer script "
      "takes the window over; a loop that records under an old value is stale.");

  ui.def(
      "windows", [] { return g_host.ui ? g_host.ui->keys() : std::vector<std::string>{}; },
      "Every key with a window right now.");

  ui.def(
      "begin_frame",
      [](const std::string& key) {
        if (!g_host.ui) throw std::runtime_error("no window host");
        if (g_rec.active)
          throw std::runtime_error("begin_frame() already active; call end_frame() first");
        g_rec.results.clear();
        for (const aii::UiResult& r : g_host.ui->take_results(key)) g_rec.results[r.id] = r;
        g_rec.key = key;
        g_rec.frame = aii::UiFrame{};
        g_rec.id_stack.clear();
        g_rec.active = true;
      },
      py::arg("key"),
      "Start recording a frame for this window: takes its latched results, "
      "then clears the recording. Raises RuntimeError with no window host, "
      "or if a recording is already active on this thread.");

  ui.def(
      "end_frame",
      [] {
        if (!g_rec.active) throw std::runtime_error("call begin_frame() first");
        const int count = static_cast<int>(g_rec.frame.cmds.size());
        std::string err;
        // A frame from a thread whose epoch is stale is dropped, not drawn:
        // this is the half of the takeover that stops the flicker on the very
        // frame the new run opens, before the old loop has noticed.
        const auto owned = g_owned.find(g_rec.key);
        const bool stale = g_host.ui && owned != g_owned.end() &&
                           owned->second != g_host.ui->epoch_of(g_rec.key);
        const bool ok = stale || (g_host.ui && g_host.ui->submit(g_rec.key, std::move(g_rec.frame), &err));
        if (!ok && err.empty()) err = "no window host";
        if (!err.empty()) ui_log_once(g_rec.key, err);
        g_rec.active = false;
        g_rec.frame = aii::UiFrame{};
        g_rec.id_stack.clear();
        return count;
      },
      "Submit the recording. Returns the number of commands recorded. Raises "
      "RuntimeError if begin_frame() was not called.");

  ui.def(
      "push_id",
      [](const std::string& s) {
        aii::UiCommand& c = ui_push(aii::UiOp::PushId);
        c.label = s;
        g_rec.id_stack.push_back(s);
      },
      py::arg("s"), "Push an id onto the stack every later label in this frame is composed under.");

  ui.def(
      "pop_id",
      [] {
        ui_push(aii::UiOp::PopId);
        if (!g_rec.id_stack.empty()) g_rec.id_stack.pop_back();
      },
      "Pop the last id pushed by push_id().");

  ui.def(
      "text", [](const std::string& s) { ui_push(aii::UiOp::Text).label = s; }, py::arg("s"),
      "One line of plain text.");
  ui.def(
      "text_colored",
      [](const py::object& rgba, const std::string& s) {
        aii::UiCommand& c = ui_push(aii::UiOp::TextColored);
        c.label = s;
        ui_set_rgba(c.f, rgba);
      },
      py::arg("rgba"), py::arg("s"), "One line of text in the given colour.");
  ui.def(
      "text_wrapped", [](const std::string& s) { ui_push(aii::UiOp::TextWrapped).label = s; },
      py::arg("s"), "Text that wraps at the window's edge.");
  ui.def(
      "text_disabled", [](const std::string& s) { ui_push(aii::UiOp::TextDisabled).label = s; },
      py::arg("s"), "Text in the disabled-text colour.");
  ui.def(
      "bullet_text", [](const std::string& s) { ui_push(aii::UiOp::BulletText).label = s; },
      py::arg("s"), "One bulleted line of text.");
  ui.def(
      "label_text",
      [](const std::string& label, const std::string& value) {
        aii::UiCommand& c = ui_push(aii::UiOp::LabelText);
        c.label = label;
        c.text = value;
      },
      py::arg("label"), py::arg("value"), "A right-aligned label with a value beside it.");

  ui.def(
      "separator", [] { ui_push(aii::UiOp::Separator); }, "A thin horizontal line.");
  ui.def(
      "separator_text", [](const std::string& s) { ui_push(aii::UiOp::SeparatorText).label = s; },
      py::arg("s"), "A horizontal line with a label in it.");
  ui.def(
      "same_line",
      [](float offset, float spacing) {
        aii::UiCommand& c = ui_push(aii::UiOp::SameLine);
        c.f[0] = offset;
        c.f[1] = spacing;
      },
      py::arg("offset") = 0.0f, py::arg("spacing") = -1.0f,
      "Keep the next item on the same line as the last.");
  ui.def(
      "new_line", [] { ui_push(aii::UiOp::NewLine); }, "Move to the next line.");
  ui.def(
      "spacing", [] { ui_push(aii::UiOp::Spacing); }, "A small vertical gap.");
  ui.def(
      "dummy",
      [](float w, float h) {
        aii::UiCommand& c = ui_push(aii::UiOp::Dummy);
        c.f[0] = w;
        c.f[1] = h;
      },
      py::arg("w"), py::arg("h"), "An invisible item of the given size, for spacing layouts.");
  ui.def(
      "indent", [](float w) { ui_push(aii::UiOp::Indent).f[0] = w; }, py::arg("w") = 0.0f,
      "Indent the following items.");
  ui.def(
      "unindent", [](float w) { ui_push(aii::UiOp::Unindent).f[0] = w; }, py::arg("w") = 0.0f,
      "Undo the last indent().");

  ui.def(
      "button",
      [](const std::string& label, float w, float h) {
        aii::UiCommand& c = ui_push(aii::UiOp::Button);
        c.label = label;
        c.f[0] = w;
        c.f[1] = h;
        const aii::UiResult* r = ui_find(label);
        return r ? r->clicked : false;
      },
      py::arg("label"), py::arg("w") = 0.0f, py::arg("h") = 0.0f,
      "A push button. True on the recording after it was clicked.");
  ui.def(
      "small_button",
      [](const std::string& label) {
        ui_push(aii::UiOp::SmallButton).label = label;
        const aii::UiResult* r = ui_find(label);
        return r ? r->clicked : false;
      },
      py::arg("label"), "A button sized to its label, for inline use.");
  ui.def(
      "checkbox",
      [](const std::string& label, bool value) {
        aii::UiCommand& c = ui_push(aii::UiOp::Checkbox);
        c.label = label;
        c.b = value;
        const aii::UiResult* r = ui_find(label);
        return r ? r->b : value;
      },
      py::arg("label"), py::arg("value"), "A checkbox. Returns the possibly user-changed value.");
  ui.def(
      "radio_button",
      [](const std::string& label, bool active) {
        aii::UiCommand& c = ui_push(aii::UiOp::RadioButton);
        c.label = label;
        c.b = active;
        const aii::UiResult* r = ui_find(label);
        return r ? r->clicked : false;
      },
      py::arg("label"), py::arg("active"), "A radio button. True when clicked.");
  ui.def(
      "selectable",
      [](const std::string& label, bool selected) {
        aii::UiCommand& c = ui_push(aii::UiOp::Selectable);
        c.label = label;
        c.b = selected;
        const aii::UiResult* r = ui_find(label);
        return r ? r->clicked : false;
      },
      py::arg("label"), py::arg("selected") = false, "A selectable row. True when clicked.");

  ui.def(
      "slider_float",
      [](const std::string& label, float v, float lo, float hi, const std::string& fmt) {
        aii::UiCommand& c = ui_push(aii::UiOp::SliderFloat);
        c.label = label;
        c.f[0] = v;
        c.f[1] = lo;
        c.f[2] = hi;
        c.text = fmt;
        const aii::UiResult* r = ui_find(label);
        return r ? r->f[0] : v;
      },
      py::arg("label"), py::arg("v"), py::arg("lo"), py::arg("hi"), py::arg("fmt") = "%.3f",
      "A float slider between lo and hi.");
  ui.def(
      "slider_int",
      [](const std::string& label, int v, int lo, int hi) {
        aii::UiCommand& c = ui_push(aii::UiOp::SliderInt);
        c.label = label;
        c.i[0] = v;
        c.i[1] = lo;
        c.f[2] = static_cast<float>(hi);
        const aii::UiResult* r = ui_find(label);
        return r ? r->i : v;
      },
      py::arg("label"), py::arg("v"), py::arg("lo"), py::arg("hi"),
      "An int slider between lo and hi.");
  ui.def(
      "drag_float",
      [](const std::string& label, float v, float speed, float lo, float hi,
         const std::string& fmt) {
        aii::UiCommand& c = ui_push(aii::UiOp::DragFloat);
        c.label = label;
        c.f[0] = v;
        c.f[1] = lo;
        c.f[2] = hi;
        c.f[3] = speed;
        c.text = fmt;
        const aii::UiResult* r = ui_find(label);
        return r ? r->f[0] : v;
      },
      py::arg("label"), py::arg("v"), py::arg("speed") = 1.0f, py::arg("lo") = 0.0f,
      py::arg("hi") = 0.0f, py::arg("fmt") = "%.3f",
      "A float value the user drags rather than slides; lo==hi means unbounded.");
  ui.def(
      "drag_int",
      [](const std::string& label, int v, float speed, int lo, int hi) {
        aii::UiCommand& c = ui_push(aii::UiOp::DragInt);
        c.label = label;
        c.i[0] = v;
        c.i[1] = lo;
        c.f[2] = static_cast<float>(hi);
        c.f[3] = speed;
        const aii::UiResult* r = ui_find(label);
        return r ? r->i : v;
      },
      py::arg("label"), py::arg("v"), py::arg("speed") = 1.0f, py::arg("lo") = 0,
      py::arg("hi") = 0, "An int value the user drags; lo==hi means unbounded.");

  ui.def(
      "input_text",
      [](const std::string& label, const std::string& text, const std::string& hint, int flags) {
        aii::UiCommand& c = ui_push(aii::UiOp::InputText);
        c.label = label;
        c.text = hint;
        c.i[1] = flags;
        c.items = {text};
        const aii::UiResult* r = ui_find(label);
        return r ? r->s : text;
      },
      py::arg("label"), py::arg("text"), py::arg("hint") = "", py::arg("flags") = 0,
      "A single-line text field. Its current text is the second argument, not a "
      "default -- the script owns the value between recordings.");
  ui.def(
      "input_text_multiline",
      [](const std::string& label, const std::string& text, float w, float h) {
        aii::UiCommand& c = ui_push(aii::UiOp::InputTextMultiline);
        c.label = label;
        c.f[0] = w;
        c.f[1] = h;
        c.items = {text};
        const aii::UiResult* r = ui_find(label);
        return r ? r->s : text;
      },
      py::arg("label"), py::arg("text"), py::arg("w") = 0.0f, py::arg("h") = 0.0f,
      "A multi-line text field, sized w by h (0 = auto).");
  ui.def(
      "input_int",
      [](const std::string& label, int v, int step) {
        aii::UiCommand& c = ui_push(aii::UiOp::InputInt);
        c.label = label;
        c.i[0] = v;
        c.i[1] = step;
        const aii::UiResult* r = ui_find(label);
        return r ? r->i : v;
      },
      py::arg("label"), py::arg("v"), py::arg("step") = 1, "An integer field with +/- steppers.");
  ui.def(
      "input_float",
      [](const std::string& label, float v, float step, const std::string& fmt) {
        aii::UiCommand& c = ui_push(aii::UiOp::InputFloat);
        c.label = label;
        c.f[0] = v;
        c.f[1] = step;
        c.text = fmt;
        const aii::UiResult* r = ui_find(label);
        return r ? r->f[0] : v;
      },
      py::arg("label"), py::arg("v"), py::arg("step") = 0.0f, py::arg("fmt") = "%.3f",
      "A float field with +/- steppers when step > 0.");

  ui.def(
      "combo",
      [](const std::string& label, int index, const std::vector<std::string>& items) {
        aii::UiCommand& c = ui_push(aii::UiOp::Combo);
        c.label = label;
        c.i[0] = index;
        c.items = items;
        const aii::UiResult* r = ui_find(label);
        return r ? r->i : index;
      },
      py::arg("label"), py::arg("index"), py::arg("items"),
      "A drop-down over items. Returns the selected index.");
  ui.def(
      "list_box",
      [](const std::string& label, int index, const std::vector<std::string>& items,
         int height_in_items) {
        aii::UiCommand& c = ui_push(aii::UiOp::ListBox);
        c.label = label;
        c.i[0] = index;
        c.i[1] = height_in_items;
        c.items = items;
        const aii::UiResult* r = ui_find(label);
        return r ? r->i : index;
      },
      py::arg("label"), py::arg("index"), py::arg("items"), py::arg("height_in_items") = -1,
      "A scrolling list over items. Returns the selected index.");
  ui.def(
      "color_edit",
      [](const std::string& label, const py::object& rgba, int flags) {
        aii::UiCommand& c = ui_push(aii::UiOp::ColorEdit);
        c.label = label;
        ui_set_rgba(c.f, rgba);
        c.i[1] = flags;
        const aii::UiResult* r = ui_find(label);
        const float* f = r ? r->f : c.f;
        return py::make_tuple(f[0], f[1], f[2], f[3]);
      },
      py::arg("label"), py::arg("rgba"), py::arg("flags") = 0,
      "A colour swatch that opens a picker. Returns a 4-tuple.");

  ui.def(
      "progress_bar",
      [](float fraction, float w, float h, const std::string& overlay) {
        aii::UiCommand& c = ui_push(aii::UiOp::ProgressBar);
        c.f[0] = fraction;
        c.f[1] = w;
        c.f[2] = h;
        c.text = overlay;
      },
      py::arg("fraction"), py::arg("w") = -1.0f, py::arg("h") = 0.0f, py::arg("overlay") = "",
      "A fraction-filled bar. Not interactive; no result.");
  ui.def(
      "progress_ring",
      [](const py::object& fraction, float radius, float thickness, const py::object& color,
         const py::object& track, const std::string& label, const std::string& caption,
         const py::object& label_colors) {
        // `values` layout is in ui_bridge.h at UiOp::ProgressRing.
        aii::UiCommand& c = ui_push(aii::UiOp::ProgressRing);
        c.f[0] = radius;
        c.f[1] = thickness;
        c.label = label;
        c.text = caption;
        float rgba[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
        if (!track.is_none()) {
          ui_set_rgba(rgba, track);
          c.i[0] |= 1;
        }
        c.values.assign(rgba, rgba + 4);
        const auto push_segment = [&c](float frac, const py::object& col) {
          float seg[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
          if (!col.is_none()) ui_set_rgba(seg, col);
          c.values.push_back(frac);
          c.values.insert(c.values.end(), seg, seg + 4);
        };
        c.values.push_back(0.0f);  // segment count, filled in below
        float count = 0.0f;
        if (py::isinstance<py::sequence>(fraction) && !py::isinstance<py::str>(fraction)) {
          for (const py::handle part : fraction.cast<py::sequence>()) {
            py::sequence p = part.cast<py::sequence>();
            if (p.size() != 2)
              throw py::type_error("each part must be a (fraction, rgba) pair");
            push_segment(p[0].cast<float>(), py::reinterpret_borrow<py::object>(p[1]));
            count += 1.0f;
          }
        } else {
          push_segment(fraction.cast<float>(), color);
          count = 1.0f;
        }
        c.values[4] = count;
        if (!label_colors.is_none()) {
          for (const py::handle lc : label_colors.cast<py::sequence>()) {
            float col[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
            if (!lc.is_none()) ui_set_rgba(col, py::reinterpret_borrow<py::object>(lc));
            c.values.insert(c.values.end(), col, col + 4);
          }
        }
      },
      py::arg("fraction"), py::arg("radius") = 32.0f, py::arg("thickness") = 6.0f,
      py::arg("color") = py::none(), py::arg("track") = py::none(), py::arg("label") = "",
      py::arg("caption") = "", py::arg("label_colors") = py::none(),
      "A ring gauge filled clockwise from 12 o'clock. fraction is 0..1, or a list of "
      "(fraction, rgba) parts drawn in order. label is centred inside ('\\n' for more "
      "lines, label_colors colours each), caption sits beneath. Not interactive; no result.");
  ui.def(
      "plot_lines",
      [](const std::string& label, const std::vector<float>& values, float lo, float hi, float w,
         float h, const std::string& overlay) {
        aii::UiCommand& c = ui_push(aii::UiOp::PlotLines);
        c.label = label;
        c.f[0] = lo;
        c.f[1] = hi;
        c.f[2] = w;
        c.f[3] = h;
        c.text = overlay;
        c.values = values;
      },
      py::arg("label"), py::arg("values"), py::arg("lo") = FLT_MAX, py::arg("hi") = FLT_MAX,
      py::arg("w") = 0.0f, py::arg("h") = 0.0f, py::arg("overlay") = "",
      "A line plot over values; lo/hi default to the data's own range.");
  ui.def(
      "plot_histogram",
      [](const std::string& label, const std::vector<float>& values, float lo, float hi, float w,
         float h, const std::string& overlay) {
        aii::UiCommand& c = ui_push(aii::UiOp::PlotHistogram);
        c.label = label;
        c.f[0] = lo;
        c.f[1] = hi;
        c.f[2] = w;
        c.f[3] = h;
        c.text = overlay;
        c.values = values;
      },
      py::arg("label"), py::arg("values"), py::arg("lo") = FLT_MAX, py::arg("hi") = FLT_MAX,
      py::arg("w") = 0.0f, py::arg("h") = 0.0f, py::arg("overlay") = "",
      "A bar histogram over values; lo/hi default to the data's own range.");

  ui.def(
      "collapsing_header",
      [](const std::string& label, bool default_open, int flags) {
        aii::UiCommand& c = ui_push(aii::UiOp::CollapsingHeader);
        c.label = label;
        c.b = default_open;
        c.i[1] = flags;
        const aii::UiResult* r = ui_find(label);
        return r ? r->b : default_open;
      },
      py::arg("label"), py::arg("default_open") = false, py::arg("flags") = 0,
      "A collapsible section header. Returns whether it is open; the script "
      "decides what to record after it, exactly as ImGui does.");
  ui.def(
      "tree_node",
      [](const std::string& label) {
        ui_push(aii::UiOp::TreeNode).label = label;
        // `b`, not `clicked`: ImGui's TreeNode answers "is it open", and a
        // script records the children only when this says so.
        const aii::UiResult* r = ui_find(label);
        return r ? r->b : false;
      },
      py::arg("label"), "A tree node. True when open; pair with tree_pop() when it is.");
  ui.def(
      "tree_pop", [] { ui_push(aii::UiOp::TreePop); }, "Close a tree_node() that was open.");

  ui.def(
      "begin_child",
      [](const std::string& id, float w, float h, bool border, int flags) {
        aii::UiCommand& c = ui_push(aii::UiOp::BeginChild);
        c.label = id;
        c.f[0] = w;
        c.f[1] = h;
        c.i[1] = flags;
        c.b = border;
        return true;  // as begin_tab_bar(): ImGui-shaped, always yes while recording
      },
      py::arg("id"), py::arg("w") = 0.0f, py::arg("h") = 0.0f, py::arg("border") = false,
      py::arg("flags") = 0, "Begin a scrolling child region. Always True while recording.");
  ui.def(
      "end_child", [] { ui_push(aii::UiOp::EndChild); }, "End a begin_child() region.");
  ui.def(
      "begin_group", [] { ui_push(aii::UiOp::BeginGroup); },
      "Group the following items as one for layout purposes.");
  ui.def(
      "end_group", [] { ui_push(aii::UiOp::EndGroup); }, "End a begin_group().");
  ui.def(
      "begin_disabled", [](bool disabled) { ui_push(aii::UiOp::BeginDisabled).b = disabled; },
      py::arg("disabled") = true, "Grey out and block input to the following items.");
  ui.def(
      "end_disabled", [] { ui_push(aii::UiOp::EndDisabled); }, "End a begin_disabled().");

  ui.def(
      "begin_tab_bar",
      [](const std::string& id) {
        ui_push(aii::UiOp::BeginTabBar).label = id;
        // True, as ImGui's returns a bool and a script written from ImGui
        // habit puts this in an `if`. Recording cannot know whether the bar
        // will draw, so it always says yes; the replayer balances the end.
        return true;
      },
      py::arg("id"), "Begin a row of tabs. Always True while recording.");
  ui.def(
      "end_tab_bar", [] { ui_push(aii::UiOp::EndTabBar); }, "End a begin_tab_bar().");
  ui.def(
      "begin_tab_item",
      [](const std::string& label) {
        ui_push(aii::UiOp::BeginTabItem).label = label;
        // `b`: the selected tab, which the first one is by default. A click
        // answer here left the first recording with no tab ever selected.
        const aii::UiResult* r = ui_find(label);
        return r ? r->b : false;
      },
      py::arg("label"), "One tab. True while it is the selected tab.");
  ui.def(
      "end_tab_item", [] { ui_push(aii::UiOp::EndTabItem); },
      "End a begin_tab_item() that returned True.");

  ui.def(
      "begin_table",
      [](const std::string& id, int columns, int flags, float w, float h) {
        aii::UiCommand& c = ui_push(aii::UiOp::BeginTable);
        c.label = id;
        c.i[0] = columns;
        c.i[1] = flags;
        c.f[0] = w;
        c.f[1] = h;
        return true;
      },
      py::arg("id"), py::arg("columns"), py::arg("flags") = 0, py::arg("w") = 0.0f,
      py::arg("h") = 0.0f, "Begin a table. Always True in a recording; a real refusal is the app's.");
  ui.def(
      "end_table", [] { ui_push(aii::UiOp::EndTable); }, "End a begin_table().");
  ui.def(
      "table_next_row", [] { ui_push(aii::UiOp::TableNextRow); }, "Start the next table row.");
  ui.def(
      "table_next_column", [] { ui_push(aii::UiOp::TableNextColumn); },
      "Move to the next table column.");
  ui.def(
      "table_setup_column",
      [](const std::string& label, int flags, float width) {
        aii::UiCommand& c = ui_push(aii::UiOp::TableSetupColumn);
        c.label = label;
        c.i[1] = flags;
        c.f[0] = width;
      },
      py::arg("label"), py::arg("flags") = 0, py::arg("width") = 0.0f,
      "Declare one table column before table_headers_row().");
  ui.def(
      "table_headers_row", [] { ui_push(aii::UiOp::TableHeadersRow); },
      "Draw the header row from the declared columns.");

  ui.def(
      "columns",
      [](int count, bool border) {
        aii::UiCommand& c = ui_push(aii::UiOp::Columns);
        c.i[0] = count;
        c.b = border;
      },
      py::arg("count") = 1, py::arg("border") = true, "The old-style column layout.");
  ui.def(
      "next_column", [] { ui_push(aii::UiOp::NextColumn); }, "Move to the next old-style column.");

  ui.def(
      "push_style_color",
      [](int idx, const py::object& rgba) {
        aii::UiCommand& c = ui_push(aii::UiOp::PushStyleColor);
        c.i[0] = idx;
        ui_set_rgba(c.f, rgba);
      },
      py::arg("idx"), py::arg("rgba"), "Override one ImGuiCol_ for the following items.");
  ui.def(
      "pop_style_color", [](int count) { ui_push(aii::UiOp::PopStyleColor).i[0] = count; },
      py::arg("count") = 1, "Undo the last count push_style_color() calls.");

  ui.def(
      "push_item_width", [](float w) { ui_push(aii::UiOp::PushItemWidth).f[0] = w; }, py::arg("w"),
      "Set the width of the following widgets.");
  ui.def(
      "pop_item_width", [] { ui_push(aii::UiOp::PopItemWidth); }, "End a push_item_width().");
  ui.def(
      "set_next_item_width", [](float w) { ui_push(aii::UiOp::SetNextItemWidth).f[0] = w; },
      py::arg("w"), "Set the width of the very next widget only.");

  ui.def(
      "set_tooltip", [](const std::string& s) { ui_push(aii::UiOp::SetTooltip).label = s; },
      py::arg("s"), "A tooltip shown while the item recorded just before this call is hovered.");
  ui.def(
      "set_scroll_here_y", [](float center) { ui_push(aii::UiOp::SetScrollHereY).f[0] = center; },
      py::arg("center") = 0.5f, "Scroll the current window so this point is at center (0..1).");

  // ---- aii.ui node graphs (M30, imnodes) -----------------------------
  //
  // Same recording shape as everything above: a command per call, a result
  // read back after end_frame()'s next begin_frame(). Node-graph results are
  // not one widget's id (`ui_compose_id`) -- they are the synthetic strings
  // `ui_bridge.h`'s BeginNodeEditor/EndNodeEditor comment names
  // (`node_pos:<id>`, `node_selected:<id>`, `link_created:<start>:<end>`,
  // `link_destroyed:<id>`), which `script_window.cpp` writes directly into
  // the results map, so the four reader functions below scan `g_rec.results`
  // for those prefixes rather than looking one up by composed id.
  ui.def(
      "begin_node_editor", [] { ui_push(aii::UiOp::BeginNodeEditor); },
      "Begin a node-graph editor. One per frame; a second Begin while one is "
      "open is ignored by the app.");
  ui.def(
      "end_node_editor", [] { ui_push(aii::UiOp::EndNodeEditor); },
      "End the node-graph editor. This is where links_created(), "
      "links_destroyed(), node_pos() and selected_nodes() get their answers "
      "for *this* window, next time you call begin_frame().");
  ui.def(
      "begin_node", [](int id) { ui_push(aii::UiOp::BeginNode).i[0] = id; }, py::arg("id"),
      "Begin one node. Ids are the script's own, unique within this editor.");
  ui.def(
      "end_node", [] { ui_push(aii::UiOp::EndNode); }, "End a begin_node().");
  ui.def(
      "begin_node_title_bar", [] { ui_push(aii::UiOp::BeginNodeTitleBar); },
      "Begin a node's title bar; put text() (or similar) between this and "
      "end_node_title_bar(). Must come before any attribute in the node.");
  ui.def(
      "end_node_title_bar", [] { ui_push(aii::UiOp::EndNodeTitleBar); },
      "End a begin_node_title_bar().");
  ui.def(
      "begin_input_attribute",
      [](int id, int shape) {
        aii::UiCommand& c = ui_push(aii::UiOp::BeginInputAttribute);
        c.i[0] = id;
        c.i[1] = shape;
      },
      py::arg("id"), py::arg("shape") = 1,
      "Begin an input attribute (pin on the left). shape is one of the "
      "PIN_* constants.");
  ui.def(
      "end_input_attribute", [] { ui_push(aii::UiOp::EndInputAttribute); },
      "End a begin_input_attribute().");
  ui.def(
      "begin_output_attribute",
      [](int id, int shape) {
        aii::UiCommand& c = ui_push(aii::UiOp::BeginOutputAttribute);
        c.i[0] = id;
        c.i[1] = shape;
      },
      py::arg("id"), py::arg("shape") = 1,
      "Begin an output attribute (pin on the right). shape is one of the "
      "PIN_* constants.");
  ui.def(
      "end_output_attribute", [] { ui_push(aii::UiOp::EndOutputAttribute); },
      "End a begin_output_attribute().");
  ui.def(
      "begin_static_attribute", [](int id) { ui_push(aii::UiOp::BeginStaticAttribute).i[0] = id; },
      py::arg("id"), "Begin an attribute with no pin -- can't be linked, but can hold widgets.");
  ui.def(
      "end_static_attribute", [] { ui_push(aii::UiOp::EndStaticAttribute); },
      "End a begin_static_attribute().");
  ui.def(
      "link",
      [](int id, int start_attr, int end_attr) {
        aii::UiCommand& c = ui_push(aii::UiOp::NodeLink);
        c.i[0] = id;
        c.f[0] = static_cast<float>(start_attr);
        c.f[1] = static_cast<float>(end_attr);
      },
      py::arg("id"), py::arg("start_attr"), py::arg("end_attr"),
      "Draw a link between two attribute ids used in begin_input_attribute() "
      "/ begin_output_attribute() calls. id is the link's own, unique id.");
  ui.def(
      "set_node_pos",
      [](int id, float x, float y, bool force) {
        aii::UiCommand& c = ui_push(aii::UiOp::SetNodePos);
        c.i[0] = id;
        c.i[1] = force ? 1 : 0;
        c.f[0] = x;
        c.f[1] = y;
      },
      py::arg("id"), py::arg("x"), py::arg("y"), py::arg("force") = false,
      "Place a node in grid space. Applied once per node id unless force=True "
      "-- a script that keeps re-recording a position does not fight the "
      "user's drag.");
  ui.def(
      "mini_map",
      [](float fraction, int location) {
        aii::UiCommand& c = ui_push(aii::UiOp::NodeMiniMap);
        c.f[0] = fraction;
        c.i[0] = location;
      },
      py::arg("fraction") = 0.2f, py::arg("location") = 1,
      "A navigable minimap of the editor. Record after every node and link "
      "in the frame; the app ignores it if recorded outside "
      "begin_node_editor()/end_node_editor().");
  ui.def(
      "push_node_color",
      [](int idx, const py::object& rgba) {
        aii::UiCommand& c = ui_push(aii::UiOp::PushNodeColor);
        c.i[0] = idx;
        ui_set_rgba(c.f, rgba);
      },
      py::arg("idx"), py::arg("rgba"),
      "Override one NODE_COL_* for the following node-editor items.");
  ui.def(
      "pop_node_color", [] { ui_push(aii::UiOp::PopNodeColor); },
      "Undo the last push_node_color() call.");

  ui.def(
      "links_created",
      [] {
        std::vector<std::pair<int, int>> out;
        for (const auto& [id, r] : g_rec.results) {
          if (!r.clicked || id.rfind("link_created:", 0) != 0) continue;
          const std::size_t sep = id.find(':', 13);
          if (sep == std::string::npos) continue;
          try {
            out.emplace_back(std::stoi(id.substr(13, sep - 13)), std::stoi(id.substr(sep + 1)));
          } catch (const std::exception&) {
          }
        }
        return out;
      },
      "Every link the user finished dragging since the last begin_frame(), "
      "as (start_attr, end_attr) tuples.");
  ui.def(
      "links_destroyed",
      [] {
        std::vector<int> out;
        for (const auto& [id, r] : g_rec.results) {
          if (!r.clicked || id.rfind("link_destroyed:", 0) != 0) continue;
          try {
            out.push_back(std::stoi(id.substr(15)));
          } catch (const std::exception&) {
          }
        }
        return out;
      },
      "Every link id the user detached since the last begin_frame().");
  ui.def(
      "node_pos",
      [](int id) -> py::object {
        auto it = g_rec.results.find("node_pos:" + std::to_string(id));
        if (it == g_rec.results.end()) return py::none();
        return py::make_tuple(it->second.f[0], it->second.f[1]);
      },
      py::arg("id"),
      "This node's grid-space position as of the last begin_frame(), or None "
      "if it was not drawn that frame.");
  ui.def(
      "node_size",
      [](int id) -> py::object {
        auto it = g_rec.results.find("node_size:" + std::to_string(id));
        if (it == g_rec.results.end()) return py::none();
        return py::make_tuple(it->second.f[0], it->second.f[1]);
      },
      py::arg("id"),
      "This node's drawn (width, height) in pixels as of the last "
      "begin_frame(), or None if it was not drawn that frame.");
  ui.def(
      "selected_nodes",
      [] {
        std::vector<int> out;
        for (const auto& [id, r] : g_rec.results) {
          if (!r.b || id.rfind("node_selected:", 0) != 0) continue;
          try {
            out.push_back(std::stoi(id.substr(14)));
          } catch (const std::exception&) {
          }
        }
        return out;
      },
      "Every node id selected in the editor as of the last begin_frame().");

  // ImNodesPinShape_ values (imnodes.h), for begin_input_attribute() /
  // begin_output_attribute()'s `shape`.
  ui.attr("PIN_CIRCLE") = 0;
  ui.attr("PIN_CIRCLE_FILLED") = 1;
  ui.attr("PIN_TRIANGLE") = 2;
  ui.attr("PIN_TRIANGLE_FILLED") = 3;
  ui.attr("PIN_QUAD") = 4;
  ui.attr("PIN_QUAD_FILLED") = 5;
  // ImNodesMiniMapLocation_ values, for mini_map()'s `location`.
  ui.attr("MINIMAP_BOTTOM_LEFT") = 0;
  ui.attr("MINIMAP_BOTTOM_RIGHT") = 1;
  ui.attr("MINIMAP_TOP_LEFT") = 2;
  ui.attr("MINIMAP_TOP_RIGHT") = 3;
  // ImNodesCol_ values, mirrored by hand for the same reason the ImGuiCol_
  // block below is: this DLL does not include imnodes.h (only
  // `script_window.cpp` does). third_party/imnodes is pinned to Nelarius/
  // imnodes @ master as vendored for M30; these are that commit's
  // ImNodesCol_ ordinals and only change if the pin moves and the enum
  // changed under it.
  ui.attr("NODE_COL_NODE_BACKGROUND") = 0;
  ui.attr("NODE_COL_TITLE_BAR") = 4;
  ui.attr("NODE_COL_LINK") = 7;
  ui.attr("NODE_COL_PIN") = 10;

  // ImGuiCol_ / ImGuiInputTextFlags_ / ImGuiTreeNodeFlags_ / ImGuiTableFlags_
  // values, mirrored by hand because this DLL does not include imgui.h (only
  // `script_window.cpp` does). Dear ImGui v1.91.8, the tag `FetchImgui.cmake`
  // pins; these are the ordinal / bit values from `imgui.h`'s `ImGuiCol_` and
  // flag enums as of that tag, and only change if the pin moves and the enum
  // changed under it.
  ui.attr("COL_TEXT") = 0;
  ui.attr("COL_TEXT_DISABLED") = 1;
  ui.attr("COL_WINDOW_BG") = 2;
  ui.attr("COL_CHILD_BG") = 3;
  ui.attr("COL_BORDER") = 5;
  ui.attr("COL_FRAME_BG") = 7;
  ui.attr("COL_BUTTON") = 21;
  ui.attr("COL_BUTTON_HOVERED") = 22;
  ui.attr("COL_BUTTON_ACTIVE") = 23;
  ui.attr("COL_HEADER") = 24;
  ui.attr("COL_SEPARATOR") = 27;
  ui.attr("COL_PLOT_LINES") = 40;
  ui.attr("COL_PLOT_HISTOGRAM") = 42;
  ui.attr("INPUT_TEXT_READ_ONLY") = 1 << 9;
  ui.attr("INPUT_TEXT_PASSWORD") = 1 << 10;
  ui.attr("TREE_DEFAULT_OPEN") = 1 << 5;
  ui.attr("TABLE_BORDERS") = (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10);
  ui.attr("TABLE_ROW_BG") = 1 << 6;
  ui.attr("TABLE_RESIZABLE") = 1 << 0;

  m.attr("scripts") = g_host.scripts;
  m.attr("lib_dir") = g_host.lib_dir;

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

# M28. `aii.lib_dir` is `scripts\lib`, seeded beside `scripts\examples`; this
# is what makes `import aii_ui` work from a script or an action without every
# one of them repeating the path. As with the `import` a helper module gets
# below (see `_run_action`'s comment on caching): once CPython has imported a
# module from here, that module is cached in sys.modules until this process
# restarts, same as any other import.
if aii.lib_dir and aii.lib_dir not in sys.path:
    sys.path.insert(0, aii.lib_dir)

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

void aiiPyHostSetUiBridge(aii::UiBridge* bridge) { g_host.ui = bridge; }

void aiiPyHostSetLibDir(const char* dir) { g_host.lib_dir = dir ? dir : ""; }

const char* aiiPyHostBootName(void) { return "_aii_boot.py"; }

const char* aiiPyHostBootSource(void) { return kBootSource; }
}
