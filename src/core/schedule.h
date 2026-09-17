#pragma once
// M2b.1: the schedule primitive — one-shot deferred actions inside the
// running session. Create, cancel, list; a timer is the first kind.
//
// ## Why this is C++ core and not Python, which is what the plan's title says
//
// The milestone is titled "Scheduled tasks, driven from Python", from the
// user's phrasing *"spawn a Python thread with a timer on it"*. Built that
// way it would not work on their machine. `ScriptHost::start()` returns
// immediately unless `discover()` found a script, `discover()` is a
// **non-recursive** scan of `%APPDATA%\AIInterface\scripts\*.py` plus
// `--script`, and the shipped example is seeded one level down into
// `scripts\examples\` *on purpose* so that a fresh install loads no
// interpreter at all (`script_host.h` says so in as many words). So a timer
// living in Python is a timer that does nothing until the user copies a file
// up a directory — and "set a timer for ten minutes" would fail as silence,
// which is this project's worst failure mode.
//
// The primitive therefore lives here, owned by the frame loop, and Python
// (M2b.2) and the ```aii``` block (M2b.3) become *clients* of it. The user's
// intent — "ask it to do something in ten minutes" — works with zero scripts,
// and a script can still schedule its own work through the bus.
//
// ## Why there is no timer thread
//
// Firing must happen on the frame loop. That is `AppBus::apply_pending()`'s
// rule and `VoiceSession::announce()`'s rule, and the reason both give is the
// same: a report that arrives off the frame loop does not fail loudly, it
// fails as a rare crash or a microphone that opens at the wrong moment.
//
// Given that, a thread per schedule (`std::thread`, `threading.Timer`, a
// waitable timer) is **strictly worse than no thread at all**. It would have
// to hand the event back to the frame loop anyway, so it buys no promptness
// the loop does not already have; it adds a kernel object, a join at
// shutdown and a second synchronisation point per schedule; and it turns
// "cancel" into a race with a callback that may already be running.
//
// So `tick()` is a due-time comparison on a sorted list, called once a frame
// by the thing that already runs at 60 Hz. Worst-case lateness is one frame
// period — ~17 ms against a feature whose unit is minutes.
//
// Two consequences, stated rather than discovered later:
//
//   * **Due times are `steady_clock`.** A ten-minute timer must not be moved
//     by an NTP correction or a daylight-saving change. The wall-clock due
//     time is carried alongside it for display only (M2b.5 wants to tell the
//     AI *when* something is due). The known limitation is machine sleep:
//     QPC does not necessarily advance across suspend, so a timer set before
//     the lid closes may be late by the time asleep. In-session and
//     non-persistent, that is the same class of thing as the app being shut
//     down, which the user already accepted.
//   * **If the frame loop stops, schedules stop.** They are in-session by
//     decision; the loop stopping means the session ending.
//
// ## Not persistent, and not silently so
//
// Schedules die with the app, per the user. `take_pending()` is how the app
// gets, at shutdown, exactly what it is about to drop, so it can say so.
// M2b.3/M2b.4 own the wording that reaches the user; this file owns the fact
// that the wording has something to read.
//
// ## The action space
//
// A fired schedule may do anything it could do live, workers included (the
// user's decision, 17 Sep 2026). So the action is **not** an enum of things
// this file knows how to do: it is a kind string plus a flat payload, routed
// by whoever consumes `tick()` — the same shape, and for the same reason, as
// `AppBus`'s family routing. Adding `worker` later is a new consumer branch,
// not a change to this header.
//
// `cwd` is captured at creation and never re-resolved at fire time, because a
// deferred worker runs `--permission-mode bypassPermissions` in the directory
// it was given and the user may be away from the desk when it fires. The
// directory it was *promised* is the only safe one.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aii {

// One book holds this many pending schedules. A bound rather than a policy:
// the AI, a script and the user share one book, and a script in a loop is the
// realistic way to fill it. Refusal is counted and reported, never fatal.
constexpr std::size_t kSchedulesMax = 64;
// Caps on what one schedule carries, matching the bus's own string cap so a
// message that arrives over the bus (M2b.2) cannot be truncated differently
// depending on which door it came through.
constexpr std::size_t kScheduleStringMax = 512;

// How the fired schedule reports back. **Chosen when the schedule is created,
// by whoever creates it, and never inferred at fire time** — the user's
// decision, 17 Sep 2026. A bare timer wants `Fixed`: instant, no subscription
// usage, cannot race a live turn. Anything with a result to describe wants
// `Phrased`: an injected turn, so the report can say something useful, at the
// cost of a few seconds and some usage.
enum class ReportGrade {
  Fixed,    // speak `report` verbatim through announce() (M2b.4)
  Phrased,  // inject a turn and let Claude say it in its own words (M2b.4)
};

const char* to_string(ReportGrade grade);
// Parses "fixed"/"phrased"; anything else is `Fixed`, because a bare timer is
// the cheap, unspendable answer and an unparseable grade must not silently
// start spending usage.
ReportGrade grade_from_string(const std::string& s);

// What the schedule does when it comes due. `kind` is the routing key and is
// the only part this file looks at (and it only looks at it to refuse an
// empty one). "timer" is the first kind; "worker" and anything else the AI
// can do live land as new consumer branches.
struct ScheduleAction {
  std::string kind;    // "timer" today
  std::string label;   // short human words: "the ten minute timer"
  std::string report;  // Fixed: the exact line to speak.
                       // Phrased: what to tell Claude happened.
  std::string task;    // the work itself, for kinds that have any (a worker's
                       // instruction). Empty for a bare timer.
  std::string name;    // a worker's name, a script's tag; free for the kind.
  std::string cwd;     // captured at creation, never re-resolved at fire time.
};

struct Schedule {
  std::uint64_t id = 0;
  std::chrono::steady_clock::time_point due{};      // what tick() compares
  std::chrono::system_clock::time_point due_wall{};  // display only
  std::chrono::steady_clock::time_point created{};
  ScheduleAction action;
  ReportGrade grade = ReportGrade::Fixed;

  // Seconds until due, from `now`; negative once overdue. For M2b.5's "what
  // is pending?", which has to answer in the units the user asked in.
  double seconds_until(std::chrono::steady_clock::time_point now) const;
};

// The book. Process-wide like `AppBus` and `ButtonRegistry`, and for the same
// reason: its producers are the frame loop, the turn thread (an ```aii```
// block runs there) and a script thread, and one store behind one mutex is
// the whole of the concurrency design.
//
// Every call is thread-safe. **`tick()` is frame-loop only** — not because it
// would corrupt anything (it takes the mutex like everything else) but
// because it is the call that *delivers*, and delivery off the frame loop is
// the bug this whole design exists to avoid. That is not left to a comment:
// the first `tick()` claims the book's owner thread and any later `tick()`
// from a different one is refused and recorded in `take_status()`, so a
// future consumer that ticks from a worker thread finds out in a log line
// instead of in a rare crash.
class ScheduleBook {
 public:
  // Constructible, so a test can hold its own book rather than fighting over
  // the singleton's owner thread.
  ScheduleBook() = default;
  static ScheduleBook& instance();

  // ---- create / cancel / list -----------------------------------------
  // Any thread. `delay` is clamped at zero (a schedule created already overdue
  // fires on the next tick rather than never). Returns the new id, or 0 with
  // `error` set when the book is full or the action has no kind. Ids are
  // monotonic within a run and never reused, so a cancel arriving after a fire
  // is a clean "no such schedule" rather than a cancel of somebody else's.
  std::uint64_t create(std::chrono::duration<double> delay, ScheduleAction action,
                       ReportGrade grade, std::string* error = nullptr);

  // Any thread. False when there is no pending schedule with that id — which
  // is also what a cancel that lost a race with the fire looks like, and is
  // the answer M2b.5 wants to report ("that one has already gone off").
  bool cancel(std::uint64_t id);

  // Any thread. Pending schedules, soonest first. A copy: the caller must not
  // hold the book's mutex while it decides what to say.
  std::vector<Schedule> list() const;
  std::size_t size() const;

  // **Frame loop only.** Everything due at `now`, removed from the book
  // before it is returned — so a schedule fires exactly once even if the
  // consumer throws, and a cancel arriving mid-fire cannot un-fire it.
  // Soonest-due first. Called with the frame's own clock reading so every
  // schedule in a frame agrees about when "now" is.
  std::vector<Schedule> tick(std::chrono::steady_clock::time_point now);
  std::vector<Schedule> tick();  // now = steady_clock::now()

  // Shutdown. Returns every pending schedule and empties the book, so the app
  // can say what it is dropping instead of dropping it silently — the plan
  // calls that out as a thing to design rather than accept. Any thread.
  std::vector<Schedule> take_pending();

  // Refusals, and any tick() from the wrong thread, since the last call.
  // Same contract as `AppBus::take_status()`: a producer with no console of
  // its own, and a dropped request is the thing that cannot be debugged later.
  std::vector<std::string> take_status();

  // The thread that owns delivery, claimed by the first tick(). Default-
  // constructed until then. This is what a test asserts against.
  std::thread::id owner_thread() const;

  // Test seam, and the reason `reset()` exists on AppBus too: forgets every
  // schedule, the owner thread, the status lines and the id counter.
  void reset();

 private:
  mutable std::mutex mutex_;
  std::vector<Schedule> pending_;  // kept sorted by `due`
  std::vector<std::string> status_;
  std::uint64_t next_id_ = 1;
  std::thread::id owner_{};
  bool owner_claimed_ = false;

  void note_locked(std::string line);
};

}  // namespace aii
