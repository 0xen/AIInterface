// avatar_children_test (M7.5): the departing child (`child_depart`) and the
// returning one (`child_merge`) -- not M2.3c's `depart`, which is the whole
// avatar leaving the band -- driven straight at AvatarController with
// hand-written snapshots.
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
#include <cstdint>
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

// One worker in one state, as the pool would report it. `id` defaults to a
// fresh one each call (WorkerPool::spawn() never hands out 0 or a repeat),
// which is what every case before M32 wants -- one worker, one identity. The
// M32 case below passes explicit ids because it is testing what happens when
// two rows share a name but not an id.
aii::WorkerPool::Snapshot worker(const char* name, aii::WorkerPool::State state,
                                 std::uint64_t id = 0) {
  static std::uint64_t next_id = 1;
  aii::WorkerPool::Snapshot w;
  w.id = id != 0 ? id : next_id++;
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
  check(def.find_trigger("child_depart") != nullptr,
        "the definition declares a `child_depart` trigger");
  check(def.find_trigger("child_merge") != nullptr,
        "the definition declares a `child_merge` trigger");
  if (const aii::AvatarTrigger* t = def.find_trigger("child_depart")) {
    check(t->variants.size() >= 2,
          "`child_depart` has " + std::to_string(t->variants.size()) +
              " variants, so it cannot read as a loop");
  }

  // ---- one worker, start to finish -----------------------------------------
  {
    aii::AvatarController ctl;
    ctl.note_definition(def);
    Run r{ctl, {}};
    r.frames(30, {});
    r.frames(kSettle, {worker("a", aii::WorkerPool::State::Working)});
    check(r.saw("child_depart"), "a worker starting sends a child: " + r.trail());
    check(r.seen.back() != "child_depart", "and the departure ends on its own");
    Run back{ctl, {}};
    back.frames(kSettle, {worker("a", aii::WorkerPool::State::Done)});
    check(back.saw("child_merge"), "a worker reporting brings one home: " + back.trail());
    check(back.saw("happy"), "and `happy` follows the arrival rather than replacing it");
    check(back.seen.back() != "child_merge" && back.seen.back() != "happy",
          "and both end on their own");
  }

  // ---- a reused name does not loop the merge clip (M32, 23 Sep 2026) -------
  //
  // The bug, reproduced directly: WorkerPool::spawn() lets a name be reused
  // the moment its earlier holder is Done or Failed, and before M32 it left
  // the finished row in place until the caller's own `spawn` line went in --
  // so a snapshot could carry a Done `bunpro` and a Working `bunpro` at once,
  // for as long as the model took to notice and spawn the next one. Keyed by
  // name, the two rows fought over one map entry every update() and re-armed
  // `pending_child_merge_` on every frame -- the clip the user watched loop
  // for minutes. Keyed by id (M32's fix), they do not: two rows, two keys,
  // neither overwrites the other's state.
  {
    aii::AvatarController ctl;
    ctl.note_definition(def);
    Run r{ctl, {}};
    r.frames(30, {});
    for (int i = 0; i < 200; ++i) {
      r.frames(1, {worker("bunpro", aii::WorkerPool::State::Done, 1),
                   worker("bunpro", aii::WorkerPool::State::Working, 2)});
    }
    check(r.count("child_merge") <= 1,
          "a finished worker and a freshly spawned one sharing a name arm "
          "child_merge at most once across 200 frames, not once per frame: " +
              r.trail());
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
    const int departs = r.count("child_depart");
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
    check(out.saw("child_depart"), "the child leaves with the worker");
    Run dead{ctl, {}};
    // Longer than kSettle: this case plays two clips back to back, and
    // `confused` carries the question mark, which holds the 0.80 s sprite
    // floor on the way out.
    dead.frames(kSettle * 3, {worker("a", aii::WorkerPool::State::Failed)});
    check(dead.saw("child_merge"),
          "a worker that dies without reporting still returns it: " +
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
    check(!r.saw("child_depart") && !r.saw("child_merge"),
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
    check(!r.saw("child_depart"),
          "a worker already in flight at startup sends nobody: " + r.trail());
  }

  // ---- the entrance the microphone summoned (19 Sep 2026) ------------------
  //
  // Not a child, but the same object and the same kind of ordering window, and
  // it is cheaper to assert here than to boot the app. M1f.5 latches the
  // microphone *below* the frame's snapshot, so the frame that summons the
  // avatar hands the controller a snapshot that still says Idle. The entrance
  // must not treat the next frame's Listening -- its own cause, one frame late
  // -- as an interruption.
  {
    const auto entrance_run = [&def](bool mic_open_at_summon) {
      aii::AvatarController ctl;
      ctl.note_definition(def);
      aii::VoiceSession::Snapshot snap;
      snap.state = aii::VoiceSession::State::Idle;
      snap.usage_stats.ctx = -1.0;
      ctl.update(snap, 1.0f / 60.0f);  // the seeding frame
      ctl.appear(mic_open_at_summon);
      // The summon frame: the snapshot is the stale one, as it is in main().
      ctl.update(snap, 1.0f / 60.0f);
      // Every frame after it, the microphone is open and the session says so.
      snap.state = aii::VoiceSession::State::Listening;
      int wake_frames = 0;
      for (int i = 0; i < 60; ++i) {
        if (ctl.update(snap, 1.0f / 60.0f).clip == "wake") ++wake_frames;
      }
      return wake_frames;
    };
    check(entrance_run(true) == 60,
          "an entrance the microphone summoned plays through the microphone (" +
              std::to_string(entrance_run(true)) + "/60 frames)");
    check(entrance_run(false) <= 2,
          "an entrance something else summoned still yields to the microphone (" +
              std::to_string(entrance_run(false)) + "/60 frames)");
  }

  // ---- M24.1: the one hex-colour parser ------------------------------------
  //
  // There were three of these, and they disagreed. main.cpp's `colourFromHex`
  // and bus_bindings.cpp's `colour_from_hex` were byte-for-byte the same
  // function under two names and accepted six digits only; avatar_def.cpp's
  // palette loader accepted six or eight and read the alpha of the eight. None
  // of the three ever accepted a three-digit short form, and all three treated
  // a leading `#` as optional. The eight-digit form is the only behavioural
  // difference and it is a widening, so it is what survives: the two callers
  // that used to reject `#rrggbbaa` both discard the alpha anyway
  // (`set_custom_colour` forces it opaque; the bus binding reads only r/g/b),
  // so nothing that used to work reads differently now.
  //
  // This test lives here because this is the one executable that links
  // avatar_def.cpp.
  {
    const auto parses = [](const char* text, std::uint32_t expect) {
      std::uint32_t got = 0;
      return aii::avatar_colour_from_hex(text, got) && got == expect;
    };
    const auto rejects = [](const char* text) {
      std::uint32_t got = 0;
      return !aii::avatar_colour_from_hex(text, got);
    };
    const std::uint32_t red = aii::avatar_rgba(0xFF, 0x00, 0x00, 0xFF);
    check(parses("#ff0000", red), "a six-digit colour with the hash parses");
    check(parses("ff0000", red), "and without it: the hash is optional");
    check(parses("FF0000", red), "upper case parses to the same colour");
    check(parses("#fF0000", red), "and so does a mixture of cases");
    check(parses("#ff0000ff", red),
          "an eight-digit value parses, which two of the three parsers refused");
    check(parses("#ff000080", aii::avatar_rgba(0xFF, 0x00, 0x00, 0x80)),
          "and its alpha is read rather than forced opaque");
    check(rejects("#f00"), "the three-digit short form is not accepted by any of them");
    check(rejects("#ff00000"), "seven digits is neither form");
    check(rejects("#ff00zz"), "a non-hex character fails the whole value");
    check(rejects("#ff 000"), "and so does a space, because nothing is trimmed");
    check(rejects(""), "an empty value parses as nothing, not as black");
    check(rejects("#"), "a bare hash likewise");
    check(rejects("##ff0000"), "only the first hash is stripped");
    {
      std::uint32_t got = 0x12345678u;
      aii::avatar_colour_from_hex("nonsense", got);
      check(got == 0x12345678u, "a value that does not parse leaves the output alone");
    }
  }

  std::printf("\n%s\n", failures ? "FAILURES" : "all good");
  return failures ? 1 : 0;
}
