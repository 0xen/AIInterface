// avatar_children_test (M7.5): the departing child and the returning merge,
// driven straight at AvatarController with hand-written snapshots.
//
// The window is not needed to see any of this and should not be used for it.
// Everything M7.5 has to get right is decided inside `AvatarController::update()`,
// which is a pure function of a `VoiceSession::Snapshot` plus its own timers —
// so a fake worker list run at a fixed 60 Hz reproduces exactly what a real
// pool would make the policy do, and does it in milliseconds instead of
// minutes of real Claude processes.
//
// It asserts the two rules the milestone asks to be stated:
//
//   * **concurrency** — children are counted, not named, and at most one
//     departure or return is on screen at a time; workers that start together
//     send one child between them.
//   * **death without a report** — a worker that stops being a worker for any
//     reason is a fall in the running count and therefore a return, so there
//     is never a child that left and did not come back. The case the plan
//     names by hand is the last one here: a worker that goes Failed having
//     said nothing.
//
// Usage: avatar_children_test [<avatar dir>]  (default: the repo's assets copy)
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "avatar/avatar_controller.h"
#include "avatar/avatar_def.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

// One worker in one state, as the pool would report it.
aii::WorkerPool::Snapshot worker(const char* name, aii::WorkerPool::State state) {
  aii::WorkerPool::Snapshot w;
  w.name = name;
  w.state = state;
  return w;
}

// A run of frames at 60 Hz with the worker list held fixed, collecting every
// clip the policy put on screen. The whole test is written in terms of this:
// nothing here cares *when* a clip appeared, only that it did and that it
// stopped again.
struct Run {
  aii::AvatarController& ctl;
  std::vector<std::string> seen;

  void frames(int n, const std::vector<aii::WorkerPool::Snapshot>& workers) {
    aii::VoiceSession::Snapshot snap;
    snap.state = aii::VoiceSession::State::Idle;
    snap.usage_stats.ctx = -1.0;  // unknown: no steam, no frustration
    snap.workers = workers;
    for (int i = 0; i < n; ++i) {
      const aii::AvatarPose pose = ctl.update(snap, 1.0f / 60.0f);
      if (seen.empty() || seen.back() != pose.clip) seen.push_back(pose.clip);
    }
  }

  bool saw(const std::string& clip) const {
    for (const std::string& s : seen) {
      if (s == clip) return true;
    }
    return false;
  }
  int count(const std::string& clip) const {
    int n = 0;
    for (const std::string& s : seen) {
      if (s == clip) ++n;
    }
    return n;
  }
  std::string trail() const {
    std::string out;
    for (const std::string& s : seen) out += (out.empty() ? "" : " -> ") + s;
    return out;
  }
};

// Two seconds is longer than any clip in the set, so "and then it stopped" is
// always inside the window a case runs for.
constexpr int kSettle = 150;

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path dir =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::path(AII_ASSETS_DIR) / "avatars" / "default";
  aii::AvatarDefinition def;
  std::string error;
  if (!aii::load_avatar_definition(dir, "", def, &error)) {
    std::fprintf(stderr, "cannot load %s: %s\n", dir.string().c_str(), error.c_str());
    return 2;
  }
  std::printf("definition: %s\n\n", dir.string().c_str());

  // The art has to exist before the policy asking for it means anything:
  // clip_length() returns 0 for a clip the definition does not declare and
  // start_oneshot() then quietly does nothing, so a missing file would make
  // every case below pass by never animating at all.
  check(def.find_trigger("depart") != nullptr, "the definition declares a `depart` trigger");
  check(def.find_trigger("merge") != nullptr, "the definition declares a `merge` trigger");
  if (const aii::AvatarTrigger* t = def.find_trigger("depart")) {
    check(t->variants.size() >= 2,
          "`depart` has " + std::to_string(t->variants.size()) +
              " variants, so it cannot read as a loop");
  }

  // ---- one worker, start to finish -----------------------------------------
  {
    aii::AvatarController ctl;
    ctl.note_definition(def);
    Run r{ctl, {}};
    r.frames(30, {});
    r.frames(kSettle, {worker("a", aii::WorkerPool::State::Working)});
    check(r.saw("depart"), "a worker starting sends a child: " + r.trail());
    check(r.seen.back() != "depart", "and the departure ends on its own");
    Run back{ctl, {}};
    back.frames(kSettle, {worker("a", aii::WorkerPool::State::Done)});
    check(back.saw("merge"), "a worker reporting brings one home: " + back.trail());
    check(back.saw("happy"), "and `happy` follows the arrival rather than replacing it");
    check(back.seen.back() != "merge" && back.seen.back() != "happy",
          "and both end on their own");
  }

  // ---- three at once -------------------------------------------------------
  {
    aii::AvatarController ctl;
    ctl.note_definition(def);
    Run r{ctl, {}};
    r.frames(30, {});
    r.frames(kSettle, {worker("a", aii::WorkerPool::State::Working),
                       worker("b", aii::WorkerPool::State::Working),
                       worker("c", aii::WorkerPool::State::Working)});
    const int departs = r.count("depart");
    check(departs == 1,
          "three workers starting on one frame send one child, not three (got " +
              std::to_string(departs) + "): " + r.trail());
  }

  // ---- a worker that dies without ever reporting ---------------------------
  //
  // The failure the plan names: a child that left and is stranded off screen
  // because the thing that was supposed to bring it back never happened. The
  // worker goes Working -> Failed with no report, which is what a child
  // process dying looks like from here.
  {
    aii::AvatarController ctl;
    ctl.note_definition(def);
    Run out{ctl, {}};
    out.frames(30, {});
    out.frames(kSettle, {worker("a", aii::WorkerPool::State::Working)});
    check(out.saw("depart"), "the child leaves with the worker");
    Run dead{ctl, {}};
    // Longer than kSettle: this case plays two clips back to back, and
    // `confused` carries the question mark, which holds the 0.80 s sprite
    // floor on the way out.
    dead.frames(kSettle * 3, {worker("a", aii::WorkerPool::State::Failed)});
    check(dead.saw("merge"), "a worker that dies without reporting still returns it: " +
                                 dead.trail());
    check(dead.saw("confused"), "and the news it brings back is `confused`, not `happy`");
    check(dead.seen.back() == "idle" || dead.seen.back() == "blink",
          "and the avatar is back to resting, with nothing outstanding: " + dead.trail());
  }

  // ---- a spawn that never starts -------------------------------------------
  //
  // WorkerPool::spawn() returning false never puts the worker in the list at
  // all, so the count never rises. Nothing leaves, and therefore nothing can
  // be stranded.
  {
    aii::AvatarController ctl;
    ctl.note_definition(def);
    Run r{ctl, {}};
    r.frames(30, {});
    r.frames(kSettle, {});
    check(!r.saw("depart") && !r.saw("merge"),
          "a spawn that fails animates nothing: " + r.trail());
  }

  // ---- workers already running when the controller is built ----------------
  //
  // The first snapshot is recorded, not reacted to, which is the same rule the
  // failure counters have kept since M2.4.
  {
    aii::AvatarController ctl;
    ctl.note_definition(def);
    Run r{ctl, {}};
    r.frames(kSettle, {worker("a", aii::WorkerPool::State::Working)});
    check(!r.saw("depart"), "a worker already in flight at startup sends nobody: " + r.trail());
  }

  std::printf("\n%s\n", failures ? "FAILURES" : "all good");
  return failures ? 1 : 0;
}
