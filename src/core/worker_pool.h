#pragma once
// Multiple Claude instances working in the background. Each worker is its own
// Claude Code child process with tools enabled and its own working directory;
// the voice interface spawns them, watches what they are doing and reports
// back. The conversational instance in VoiceSession is separate and never has
// tools.
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/aii_block.h"
#include "llm/claude_code_client.h"

namespace aii {

class WorkerPool {
 public:
  enum class State { Starting, Working, Done, Paused, Failed };

  struct Snapshot {
    std::string name;
    std::string task;
    std::string cwd;
    State state = State::Starting;
    std::string activity;               // what it is doing right now
    std::string result;                 // final text once it is Done
    std::vector<std::string> recent;    // last few activity lines, oldest first
    int tool_calls = 0;
  };

  // Called from a worker thread when an instance finishes (Done or Failed).
  // Two texts, deliberately different: `shown` names the worker and goes to
  // the transcript, `spoken` does not and is what the voice reads out. The
  // user asked not to hear the worker's name when work comes back; it costs
  // them nothing on screen, where the panel row and the transcript both keep
  // it, so only the spoken copy loses it.
  using ReportFn = std::function<void(const std::string& name, State state,
                                      const std::string& shown, const std::string& spoken)>;

  WorkerPool(std::string claude_exe, bool bypass_permissions);
  ~WorkerPool();
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  void set_on_report(ReportFn fn) { report_ = std::move(fn); }

  // Starts an instance on `task` in `cwd` (empty = this process's directory).
  // Returns false and fills `error` when the process cannot start or the name
  // is already taken by a running worker.
  bool spawn(const std::string& name, const std::string& cwd, const std::string& task,
             std::string* error);
  // Interrupts the worker's current turn; it stays in the list as Paused.
  bool pause(const std::string& name);
  // Interrupts every running worker.
  void pause_all();
  // Removes a finished (or paused) worker and its process.
  bool stop(const std::string& name);

  std::vector<Snapshot> snapshot() const;
  size_t running() const;
  // Reaps worker threads that have finished; call from the frame loop.
  void update();

 private:
  struct Worker {
    std::string name, task, cwd;
    std::unique_ptr<ClaudeCodeClient> client;
    std::thread thread;
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    State state = State::Starting;
    std::string activity, result;
    std::deque<std::string> recent;
    int tool_calls = 0;
  };

  void run(Worker* w);

  std::string exe_;
  bool bypass_ = true;
  ReportFn report_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<Worker>> workers_;
};

const char* worker_state_name(WorkerPool::State s);

// M11.1: has this agent stopped for good? One definition, here beside the enum,
// because three surfaces now ask the question — the chat's worker rows, the
// worker strip and the agent menu — and a disagreement between them would put
// the same agent in two places at once or in neither.
//
// **`Paused` is not finished.** It is a turn that was interrupted and an entry
// that is still in the pool on purpose; `stop` is what ends one. Filing it with
// the finished agents would hide a worker the user deliberately held, which is
// the opposite of what pausing it was for.
inline bool agent_finished(WorkerPool::State s) {
  return s == WorkerPool::State::Done || s == WorkerPool::State::Failed;
}

// Parsing a reply's ```aii blocks lives in `core/aii_block.h` (M15.1), and
// `Command` moved there with it: it is the one piece of this app that reads
// untrusted model output and decides what to do about it, and it could not be
// given a test while it shared a translation unit with a class that spawns
// processes.
//
// The two wrappers below are all that stays here. They bind the app-owned
// `button` verb to ButtonRegistry, which cannot live in a standard-library-only
// file because it opens folders through <shellapi.h>.

// Parses `text` and applies any `button` lines. `problems` collects what the
// parser refused — today, an unterminated block — for the caller to log; the
// one-argument form discards them.
std::vector<Command> parse_commands(const std::string& text, std::vector<std::string>* problems);
std::vector<Command> parse_commands(const std::string& text);

}  // namespace aii
