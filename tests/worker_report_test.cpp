// worker_report_test: what a finished worker actually hands to the speech path.
//
// The window is not needed to see this and should not be used for it: the two
// strings a worker produces are built in `WorkerPool::run()` and handed
// straight to `VoiceSession::announce(shown, spoken)`, so running the pool
// directly captures exactly the bytes the voice reads.
//
// It prints both:
//   SHOWN  -- the transcript line, which still names the worker (and is
//             byte-identical to what used to be spoken)
//   SPOKEN -- what the voice says now, which must not contain the name
//
// Usage: worker_report_test <name> <cwd> <task...>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/worker_pool.h"

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: worker_report_test <name> <cwd> <task...>\n");
    return 2;
  }
  const std::string name = argv[1];
  const std::string cwd = argv[2];
  std::string task = argv[3];
  for (int i = 4; i < argc; ++i) task += std::string(" ") + argv[i];

  aii::Config cfg = aii::Config::from_env();
  aii::WorkerPool pool(cfg.claude_exe, cfg.worker_bypass);

  std::atomic<bool> done{false};
  pool.set_on_report([&](const std::string& n, aii::WorkerPool::State s, const std::string& shown,
                         const std::string& spoken) {
    std::printf("NAME   %s\n", n.c_str());
    std::printf("STATE  %s\n", aii::worker_state_name(s));
    std::printf("SHOWN  %s\n", shown.c_str());
    std::printf("SPOKEN %s\n", spoken.c_str());
    std::fflush(stdout);
    done = true;
  });

  std::string err;
  if (!pool.spawn(name, cwd, task, &err)) {
    std::fprintf(stderr, "spawn failed: %s\n", err.c_str());
    return 1;
  }
  std::printf("spawned %s in %s\n", name.c_str(), cwd.c_str());
  std::fflush(stdout);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (!done && std::chrono::steady_clock::now() < deadline) {
    pool.update();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!done) {
    std::fprintf(stderr, "timed out waiting for the worker\n");
    return 1;
  }
  // Panel row, as `avatar_ui.cpp` draws it, to show the name is still there.
  for (const auto& s : pool.snapshot()) {
    std::printf("PANEL  %s [%s] %s\n", s.name.c_str(), aii::worker_state_name(s.state),
                s.activity.c_str());
  }
  std::fflush(stdout);
  return 0;
}
