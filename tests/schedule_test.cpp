// M2b.1's harness. Not "it compiles": a simulated 60 Hz frame loop on a
// thread of its own, with every timing case run as **repeats** rather than one
// sample — this project has twice declared a race fixed on one sample and been
// wrong twice (MANAGER-HANDOFF.md, "Method lessons").
//
//   schedule_test [reps]        default 40
//
// What it checks, and why each one is here:
//
//   1. fire-once     a schedule fires exactly once, at roughly the right
//                    time. Lateness is measured every rep and the
//                    distribution printed, because a tolerance nobody
//                    measured is a tolerance nobody can defend.
//   2. cancel        a schedule cancelled before its due time never fires.
//   3. cancel-race   a cancel issued *at* the due instant, from another
//                    thread, is exactly one of "cancelled" or "fired" —
//                    never both, never neither. This is the case a
//                    `threading.Timer` design gets wrong, and it is why the
//                    book removes a fired schedule under the same lock that
//                    cancel takes.
//   4. list          list() reflects a create, a cancel and a fire.
//   5. thread        every fire is delivered on the loop thread, including
//                    schedules created from four other threads at once.
//   6. off-loop tick tick() from a thread that is not the owner delivers
//                    nothing and says so, rather than delivering there.
//   7. cap           the 65th pending schedule is refused with a reason.
//   8. shutdown      take_pending() hands back exactly what is being dropped.
//  10. no folder     a deferred worker with no cwd= runs where the app was
//                    launched from rather than being refused (M3.12).
#include <windows.h>
// WIN32_LEAN_AND_MEAN drops mmsystem.h, and timeBeginPeriod lives there.
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/cwd_policy.h"
#include "core/schedule.h"

using namespace aii;
using Clock = std::chrono::steady_clock;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL  %s\n", what.c_str());
  }
}

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

// The stand-in for `avatar.exe`'s frame loop: one thread, ~60 Hz, calling
// tick() once per pass and recording what came back and on which thread.
class FrameLoop {
 public:
  struct Fire {
    std::uint64_t id;
    Clock::time_point at;
    std::thread::id thread;
    double late_ms;
  };

  explicit FrameLoop(ScheduleBook& book) : book_(book) {
    thread_ = std::thread([this] { run(); });
  }
  ~FrameLoop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  std::thread::id thread_id() const { return thread_.get_id(); }

  std::vector<Fire> fires() const {
    std::lock_guard<std::mutex> l(mutex_);
    return fires_;
  }
  void clear() {
    std::lock_guard<std::mutex> l(mutex_);
    fires_.clear();
  }
  // Worst gap between two consecutive ticks: the loop's own honesty about
  // what "roughly the right time" can mean.
  double worst_tick_gap_ms() const {
    std::lock_guard<std::mutex> l(mutex_);
    return worst_gap_ms_;
  }
  std::size_t ticks() const {
    std::lock_guard<std::mutex> l(mutex_);
    return ticks_;
  }

 private:
  void run() {
    auto next = Clock::now();
    auto prev = next;
    while (!stop_) {
      const auto now = Clock::now();
      // One clock reading for the whole pass, exactly as the frame loop hands
      // the frame's own `now` to everything that needs it.
      std::vector<Schedule> fired = book_.tick(now);
      {
        std::lock_guard<std::mutex> l(mutex_);
        ++ticks_;
        if (ticks_ > 1) worst_gap_ms_ = std::max(worst_gap_ms_, ms(now - prev));
        for (const Schedule& s : fired)
          fires_.push_back({s.id, now, std::this_thread::get_id(), ms(now - s.due)});
      }
      prev = now;
      next += std::chrono::microseconds(16667);
      std::this_thread::sleep_until(next);
    }
  }

  ScheduleBook& book_;
  mutable std::mutex mutex_;
  std::vector<Fire> fires_;
  std::size_t ticks_ = 0;
  double worst_gap_ms_ = 0.0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

ScheduleAction timer_action(const char* label) {
  ScheduleAction a;
  a.kind = "timer";
  a.label = label;
  a.report = std::string("Your ") + label + " is done.";
  a.cwd = "C:\\github\\AIInterface";
  return a;
}

void percentiles(std::vector<double> v, const char* name) {
  if (v.empty()) {
    std::printf("  %s: no samples\n", name);
    return;
  }
  std::sort(v.begin(), v.end());
  const auto at = [&](double p) { return v[static_cast<std::size_t>(p * (v.size() - 1))]; };
  double sum = 0;
  for (double x : v) sum += x;
  std::printf("  %s over %zu reps: min %.2f  median %.2f  p90 %.2f  max %.2f  mean %.2f (ms)\n",
              name, v.size(), v.front(), at(0.5), at(0.9), v.back(), sum / v.size());
}

}  // namespace

int main(int argc, char** argv) {
  // Windows' default timer resolution is 15.6 ms, which would make a 60 Hz
  // sleep loop tick at 15.6-31 ms and charge the schedule for the harness's
  // own jitter. The real frame loop is paced by the presenter, not by sleep.
  timeBeginPeriod(1);
  const int reps = argc > 1 ? std::atoi(argv[1]) : 40;
  std::printf("schedule_test: %d reps per timing case\n\n", reps);

  ScheduleBook book;
  FrameLoop loop(book);

  // ---- 1. fires once, and roughly on time -------------------------------
  {
    std::vector<double> late;
    int wrong_count = 0;
    for (int i = 0; i < reps; ++i) {
      loop.clear();
      const std::uint64_t id =
          book.create(std::chrono::duration<double>(0.200), timer_action("two hundred ms timer"),
                      ReportGrade::Fixed);
      check(id != 0, "create returned an id");
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      const auto f = loop.fires();
      if (f.size() != 1) {
        ++wrong_count;
        continue;
      }
      check(f[0].id == id, "the fired id is the created id");
      late.push_back(f[0].late_ms);
    }
    std::printf("case 1  fire-once: %d/%d reps fired exactly once\n", reps - wrong_count, reps);
    check(wrong_count == 0, "every rep fired exactly once");
    percentiles(late, "lateness past due");
    double worst = 0;
    for (double x : late) worst = std::max(worst, x);
    // The tolerance, stated and defended: a due-time check on a 60 Hz loop
    // can be late by up to one tick interval plus whatever the OS scheduler
    // adds. Never early, because the comparison is `due <= now`.
    check(worst <= 50.0, "worst lateness within 50 ms of due");
    for (double x : late) check(x >= 0.0, "never fired before its due time");
    std::printf("  loop: %zu ticks, worst tick gap %.2f ms\n\n", loop.ticks(),
                loop.worst_tick_gap_ms());
  }

  // ---- 2. cancelled before due never fires ------------------------------
  {
    int leaked = 0;
    for (int i = 0; i < reps; ++i) {
      loop.clear();
      const std::uint64_t id = book.create(std::chrono::duration<double>(0.200),
                                           timer_action("cancelled timer"), ReportGrade::Fixed);
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      check(book.cancel(id), "cancel before due succeeded");
      check(!book.cancel(id), "cancelling twice is refused");
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      if (!loop.fires().empty()) ++leaked;
    }
    std::printf("case 2  cancel: %d/%d reps never fired\n\n", reps - leaked, reps);
    check(leaked == 0, "a cancelled schedule never fires");
  }

  // ---- 3. cancel racing the fire ----------------------------------------
  // Deliberately aimed at the due instant, from a thread that is not the
  // loop. Exactly one of the two outcomes, every time.
  {
    const int race_reps = reps * 5;
    int both = 0, neither = 0, cancelled = 0, fired = 0;
    for (int i = 0; i < race_reps; ++i) {
      loop.clear();
      const std::uint64_t id = book.create(std::chrono::duration<double>(0.030),
                                           timer_action("race timer"), ReportGrade::Fixed);
      // The schedule's *own* due instant, read back rather than recomputed,
      // so the cancel is aimed at the moment the loop's comparison flips
      // rather than at a moment near it.
      Clock::time_point target = Clock::now();
      for (const Schedule& s : book.list())
        if (s.id == id) target = s.due;
      // Jittered across a whole tick interval and a half. Aimed at the due
      // instant exactly, the cancel wins every time — the loop only looks
      // every ~17 ms, so "exactly due" is reliably before the tick that would
      // fire it, and 200 identical wins would prove nothing. Spreading the
      // cancel across the window puts it on both sides of the tick, which is
      // the only way to see the case where the fire got there first.
      target += std::chrono::microseconds(std::rand() % 25000);
      while (Clock::now() < target) { /* spin onto the chosen instant */ }
      const bool c = book.cancel(id);
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      const bool f = !loop.fires().empty();
      if (c && f) ++both;
      if (!c && !f) ++neither;
      if (c) ++cancelled;
      if (f) ++fired;
    }
    std::printf("case 3  cancel-race over %d reps: cancelled %d, fired %d, both %d, neither %d\n\n",
                race_reps, cancelled, fired, both, neither);
    check(both == 0, "a schedule is never both cancelled and fired");
    check(neither == 0, "a schedule is never both missed and uncancelled");
  }

  // ---- 4. list reflects create, cancel and fire -------------------------
  {
    for (int i = 0; i < reps; ++i) {
      loop.clear();
      const std::uint64_t later = book.create(std::chrono::duration<double>(0.300),
                                              timer_action("later"), ReportGrade::Phrased);
      const std::uint64_t sooner = book.create(std::chrono::duration<double>(0.120),
                                               timer_action("sooner"), ReportGrade::Fixed);
      auto l = book.list();
      check(l.size() == 2, "list holds both");
      if (l.size() == 2) {
        check(l[0].id == sooner && l[1].id == later, "list is soonest-due first");
        check(l[0].grade == ReportGrade::Fixed && l[1].grade == ReportGrade::Phrased,
              "the grade written at creation survives");
        check(l[0].action.label == "sooner", "the payload survives");
        check(l[0].action.cwd == "C:\\github\\AIInterface", "the cwd is carried, not re-resolved");
        check(l[0].seconds_until(Clock::now()) > 0.0, "seconds_until is positive before due");
      }
      check(book.cancel(later), "cancel the later one");
      l = book.list();
      check(l.size() == 1 && l[0].id == sooner, "list reflects the cancel");
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      check(book.list().empty(), "list reflects the fire");
      check(loop.fires().size() == 1, "only the uncancelled one fired");
    }
    std::printf("case 4  list: %d reps\n\n", reps);
  }

  // ---- 5. every fire lands on the loop thread ---------------------------
  // Including schedules created from four threads at once, which is the shape
  // M2b.2 has: a script thread asking for a timer.
  {
    int off_thread = 0;
    for (int i = 0; i < reps; ++i) {
      loop.clear();
      std::vector<std::thread> makers;
      std::atomic<int> made{0};
      for (int t = 0; t < 4; ++t) {
        makers.emplace_back([&book, &made] {
          for (int k = 0; k < 3; ++k) {
            if (book.create(std::chrono::duration<double>(0.100), timer_action("from a thread"),
                            ReportGrade::Fixed) != 0)
              ++made;
          }
        });
      }
      for (auto& t : makers) t.join();
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      const auto f = loop.fires();
      check(static_cast<int>(f.size()) == made.load(), "every schedule created fired once");
      for (const auto& x : f)
        if (x.thread != loop.thread_id()) ++off_thread;
    }
    std::printf("case 5  thread: %d reps, %d fires off the loop thread\n\n", reps, off_thread);
    check(off_thread == 0, "no fire was delivered off the frame loop");
    check(book.owner_thread() == loop.thread_id(), "the book's owner is the loop thread");
  }

  // ---- 6. a tick from the wrong thread delivers nothing ------------------
  {
    book.take_status();
    const std::uint64_t id =
        book.create(std::chrono::duration<double>(0.0), timer_action("wrong thread"),
                    ReportGrade::Fixed);
    // This is the main thread; the loop claimed the book long ago.
    const auto got = book.tick(Clock::now() + std::chrono::seconds(10));
    check(got.empty(), "an off-loop tick delivers nothing");
    const auto notes = book.take_status();
    check(!notes.empty(), "an off-loop tick is reported");
    std::printf("case 6  off-loop tick: delivered %zu, status \"%s\"\n\n", got.size(),
                notes.empty() ? "" : notes.front().c_str());
    // It is still pending, so the loop will pick it up: refused, not lost.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(book.cancel(id) == false, "the refused-tick schedule still fired on the loop");
  }

  // ---- 7. the cap ---------------------------------------------------------
  {
    book.take_pending();
    loop.clear();
    book.take_status();
    std::size_t made = 0;
    std::string err;
    for (std::size_t i = 0; i < kSchedulesMax + 8; ++i) {
      std::string e;
      if (book.create(std::chrono::duration<double>(60.0), timer_action("capped"),
                      ReportGrade::Fixed, &e) != 0)
        ++made;
      else if (err.empty())
        err = e;
    }
    check(made == kSchedulesMax, "the book caps at kSchedulesMax");
    check(!err.empty(), "a refusal carries a reason");
    std::string kindless_err;
    ScheduleAction bad;
    check(book.create(std::chrono::duration<double>(1.0), bad, ReportGrade::Fixed,
                      &kindless_err) == 0,
          "an action with no kind is refused");
    std::printf("case 7  cap: %zu accepted, then \"%s\"; kindless: \"%s\"\n\n", made, err.c_str(),
                kindless_err.c_str());
  }

  // ---- 8. shutdown reports what it drops ---------------------------------
  {
    const auto dropped = book.take_pending();
    check(dropped.size() == kSchedulesMax, "take_pending hands back everything pending");
    check(book.list().empty(), "the book is empty afterwards");
    check(!dropped.empty() && dropped.front().action.label == "capped",
          "a dropped schedule still carries its payload, so it can be described");
    std::printf("case 8  shutdown: %zu schedules handed back for reporting\n\n", dropped.size());
  }

  // ---- 9. M2b.3: the delay the model writes, and the block it writes it in -
  // The prompt asks for `in=10m`, not for seconds, because "in ten minutes" is
  // what the user said. So the reading of that string is part of the feature
  // and not a convenience: an `in=` this cannot read is a timer the user
  // believes exists.
  {
    struct { const char* text; double want; } good[] = {
        {"600", 600.0},   {"90s", 90.0},     {"10m", 600.0},   {"10min", 600.0},
        {"10 min", 600.0},{"2h", 7200.0},    {"3H", 10800.0},  {"1h30m", 5400.0},
        {"1m30s", 90.0},  {"0.5m", 30.0},    {"1d", 86400.0},
    };
    for (const auto& g : good) {
      double s = -1.0;
      const bool ok = parse_delay(g.text, &s);
      check(ok && std::abs(s - g.want) < 1e-6,
            std::string("parse_delay(\"") + g.text + "\") == " + std::to_string(g.want));
    }
    const char* bad[] = {"", "soon", "ten minutes", "m", "-5m", "0", "0s", "2d", "10x", "1h30"};
    for (const char* b : bad) {
      double s = -1.0;
      // "1h30" is deliberately in this list: a trailing bare number after a
      // unit is read as seconds, so it parses -- 3630. It is here to record
      // that, not to assert a refusal.
      const bool ok = parse_delay(b, &s);
      if (std::string(b) == "1h30") check(ok && std::abs(s - 3630.0) < 1e-6,
                                          "\"1h30\" reads as 1h plus 30 seconds");
      else check(!ok, std::string("parse_delay refuses \"") + b + "\"");
    }
    // The block parser itself is deliberately *not* exercised here, and since
    // M15.1 it does not need to be: `parse_commands` moved out of
    // `worker_pool.cpp` -- which drags in ClaudeCodeClient and ButtonRegistry --
    // into the standard-library-only `core/aii_block.cpp`, and
    // `aii_block_test` is where its cases now live. This target keeps the one
    // property it exists to have: no engine DLLs beside it, so a timing harness
    // cannot fail for a reason that is not timing.
    std::printf("case 9  M2b.3: every delay string the prompt teaches parses\n\n");
  }

  // ---- 10. a deferred worker with no folder ----------------------------
  // The deferred path is the one that worried us: a `schedule` line creates a
  // worker that starts minutes later at bypassPermissions, possibly with
  // nobody at the desk. It used to *refuse* a request with no `cwd=`, which
  // read as caution and worked as pressure -- it gave the model a reason to
  // put something on the line, and what it put there was invented. The user's
  // decision ("use same folder as primary agent") replaces the refusal with
  // the app's own folder, and this is where that is nailed down. The
  // corroboration rule that decides a folder *was* named lives one door out,
  // in core/cwd_policy.h, and has its own test.
  {
    aii::set_app_dir("C:\\github\\AIInterface");
    aii::ScheduleRequest r;
    r.in = "10m";
    r.task = "check the build and say how it went";
    r.name = "build";
    ScheduleAction a;
    ReportGrade grade = ReportGrade::Fixed;
    double secs = 0.0;
    std::string detail;
    check(aii::build_schedule(r, &a, &grade, &secs, &detail) == aii::ScheduleRefusal::None,
          "a deferred worker with no cwd= is no longer refused");
    check(a.cwd == "C:\\github\\AIInterface", "it runs in the folder the app was launched from");
    check(grade == ReportGrade::Phrased, "and it is still a phrased report");

    r.cwd = "C:\\github\\Renderer";
    check(aii::build_schedule(r, &a, &grade, &secs, &detail) == aii::ScheduleRefusal::None &&
              a.cwd == "C:\\github\\Renderer",
          "a folder that was given is still the folder it runs in");

    r.cwd = "src\\core";
    check(aii::build_schedule(r, &a, &grade, &secs, &detail) ==
              aii::ScheduleRefusal::RelativeFolder,
          "a relative cwd= is still refused, because it is not a folder at all");
    std::printf("case 10 deferred worker: no folder -> the app's own\n\n");
  }

  timeEndPeriod(1);
  std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
