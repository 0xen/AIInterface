// A child that never answers, and the kill that ends the wait.
//
// Review finding 2, M17.3. `ClaudeCodeClient::turn()` returns on one event and
// one event only -- the CLI's `result` -- and the interrupt it sends when
// `cancel` goes up is a `control_request` the child is free to ignore. A
// worker halfway through a Bash command does ignore it, and then
// `WorkerPool::stop()` froze in its join, `start_turn()` froze behind that, and
// the conversation was over for the rest of the run. `kill()` is the
// escalation; this is the proof that it ends a turn that nothing else would.
//
// ---------------------------------------------------------------------------
// The stand-in
// ---------------------------------------------------------------------------
//
// `cmd.exe`. Measured before it was relied on: given this app's whole command
// line -- `-p --input-format stream-json --output-format stream-json --verbose`
// and the rest -- cmd ignores every one of those switches, starts, reads its
// stdin and sits there. That is exactly the child this finding is about: a
// process that is alive, is attached to the same three pipes a real `claude`
// is, and will never in this life send a `result` event. It also echoes the
// JSON line written to it and complains about it on stdout, which the reader
// drops as unparseable, so the reader thread is exercised too.
//
// No `claude` process is started anywhere in this file and no model is called.
//
// Two layers are covered: `ClaudeCodeClient::kill()` itself, and
// `WorkerPool::stop()` on a worker built out of the same stand-in, which is
// the shape the finding was actually written about.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "core/worker_pool.h"
#include "llm/claude_code_client.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

std::string cmd_exe() {
  char buf[MAX_PATH] = {0};
  const UINT n = GetSystemDirectoryA(buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return "C:\\Windows\\System32\\cmd.exe";
  return std::string(buf, n) + "\\cmd.exe";
}

aii::ClaudeCodeClient::Options stand_in() {
  aii::ClaudeCodeClient::Options o;
  o.exe = cmd_exe();
  o.effort.clear();  // every flag is ignored by the stand-in; send fewer anyway
  return o;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main() {
  std::printf(
      "kill() on a child that will never send a result\n"
      "  (any \"is not recognized as an internal or external command\" lines below are the\n"
      "   stand-in child complaining about the JSON written to its stdin -- that is the point)\n");

  // ---- 1. a turn that hangs, ended by kill() -----------------------------
  {
    aii::ClaudeCodeClient client(stand_in());
    std::string err;
    if (!client.start(&err)) {
      std::printf("  FAIL could not start the stand-in child: %s\n", err.c_str());
      return 1;
    }
    check(true, "the stand-in child started");

    std::atomic<bool> cancel{false};
    std::atomic<bool> returned{false};
    aii::ChatResult result;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread turn([&] {
      result = client.turn("are you there?", nullptr, &cancel);
      returned = true;
    });

    // Half a second is far longer than a real `result` takes to arrive and is
    // the whole of the claim: nothing comes back on its own.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    check(!returned, "the turn is still waiting half a second in, as the finding says");

    // The interrupt, exactly as WorkerPool::pause() raises it. It is written
    // to a child that does not understand it, which is the case that used to
    // hang forever.
    cancel = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    check(!returned, "and it is still waiting half a second after the interrupt");

    const auto t_kill = std::chrono::steady_clock::now();
    client.kill();
    for (int i = 0; i < 200 && !returned; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const double after_kill = seconds_since(t_kill);
    turn.join();

    check(returned, "kill() ends the turn");
    check(after_kill < 1.0, "and it comes back inside a second (" +
                                std::to_string(after_kill) + " s)");
    check(!result.ok, "the turn reports as not ok");
    check(!result.error.empty(), "with an error saying what happened: " + result.error);
    // This is what makes a killed worker report as *stopped* rather than as
    // failed: WorkerPool::run() reads `ok` first and the cancel flag second,
    // and turn() marks a cancelled turn interrupted whatever ended it.
    check(result.stop_reason == "interrupted",
          "and interrupted, because the app raised cancel before it killed");
    check(seconds_since(t0) < 5.0, "the whole hang lasted only as long as the test allowed it to");
  }

  // ---- 2. kill() is safe in the shapes a caller will reach it in ---------
  {
    aii::ClaudeCodeClient never_started(stand_in());
    never_started.kill();
    check(true, "kill() on a client that was never started does nothing and returns");

    aii::ClaudeCodeClient client(stand_in());
    std::string err;
    if (client.start(&err)) {
      client.kill();
      client.kill();
      check(true, "kill() twice is harmless");
      aii::ChatResult r = client.turn("anything", nullptr, nullptr);
      check(!r.ok && !r.error.empty(),
            "and a turn started after a kill fails at once rather than waiting: " + r.error);
    } else {
      check(false, "could not start a second stand-in child: " + err);
    }
    // The destructor runs here on a client whose child is already gone. It
    // used to be the other half of the same hang; it must not wait.
    const auto t0 = std::chrono::steady_clock::now();
    {
      aii::ClaudeCodeClient third(stand_in());
      if (third.start(&err)) third.kill();
    }
    check(seconds_since(t0) < 2.0,
          "a destructor after a kill does not sit out the three-second wait (" +
              std::to_string(seconds_since(t0)) + " s)");
  }

  // ---- 3. the whole of finding 2, one layer up ---------------------------
  //
  // `WorkerPool::stop()` against a worker that will never answer: the same
  // stand-in, spawned through the pool exactly as a real worker is. Before
  // M17.3 this call did not return, and because the frame loop is what calls
  // it, neither did the conversation.
  std::printf("\nWorkerPool::stop() on a worker that will not answer\n");
  {
    aii::WorkerPool pool(cmd_exe(), false);
    std::atomic<int> reports{0};
    aii::WorkerPool::State reported = aii::WorkerPool::State::Starting;
    pool.set_on_report([&](const std::string&, aii::WorkerPool::State s, const std::string&,
                           const std::string&) {
      reported = s;
      ++reports;
    });

    std::string err;
    if (!pool.spawn("stand-in", ".", "this task is never read", &err)) {
      std::printf("  FAIL could not spawn the stand-in worker: %s\n", err.c_str());
      return 1;
    }
    // Let it get as far as it is ever going to get.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(reports == 0, "the worker is working and has reported nothing");

    const auto t0 = std::chrono::steady_clock::now();
    const bool stopped = pool.stop("stand-in");
    const double took = seconds_since(t0);
    check(stopped, "stop() returns true");
    check(took >= 2.0, "it gave the interrupt a real chance first (" + std::to_string(took) + " s)");
    check(took < aii::WorkerPool::kInterruptGrace.count() / 1000.0 + 2.0,
          "and it came back inside the grace period plus slack, not never");
    check(reports == 1, "the worker reported exactly once on its way out");
    // Finding 23's other half: the app ended this one on purpose, so it is
    // stopped, not failed. A Failed here would have the voice read out a
    // failure sentence for something the user asked for.
    check(reported == aii::WorkerPool::State::Paused,
          std::string("and it reports as stopped, not failed (") +
              aii::worker_state_name(reported) + ")");
  }

  std::printf(failures ? "\nFAILED: %d\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
