#pragma once
// Multiple Claude instances working in the background. Each worker is its own
// Claude Code child process with tools enabled and its own working directory;
// the voice interface spawns them, watches what they are doing and reports
// back. The conversational instance in VoiceSession is separate and never has
// tools.
#include <atomic>
#include <chrono>
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

  // M31. The model a worker runs on when a `spawn` names none of its own — a
  // CLI alias ("opus", "sonnet", "haiku") or empty for "no --model flag,
  // whatever the CLI would pick". See `model_choice.h` for why an alias and
  // not a dated id: the same reasoning applies here, and this is not a second
  // table, just a second reader of the one that already exists.
  //
  // **Why a lesser model by default.** The user's decision (22 Sep 2026): "by
  // default the AI uses lesser AI models such as Opus for the default worker
  // model" -- cost and speed. The conversation's model is chosen for talking
  // to a person, who is waiting and hears every second of it; a worker's task
  // is graded on getting the work done, not on how quickly, and nobody is
  // listening to it think. Opus is still a capable model, and the shipped
  // default -- it is a *lesser* choice only next to what the conversational
  // instance can be set to, not a weak one on its own.
  //
  // `Live`: read fresh at each `spawn()`, so changing it costs nothing and
  // reaches the next worker started, never the ones already running.
  void set_model(std::string model) { model_ = std::move(model); }
  std::string model() const { return model_; }

  // M31. Whether a worker gets Claude in Chrome (`--chrome`), mirroring
  // whatever the `tools.browser` switch says for the conversational instance
  // -- see main.cpp. `Live`, like `model_` above.
  void set_chrome(bool on) { chrome_ = on; }
  bool chrome() const { return chrome_; }

  // Starts an instance on `task` in `cwd` (empty = this process's directory).
  // Returns false and fills `error` when the process cannot start or the name
  // is already taken by a running worker.
  //
  // `model_override` (M31): non-empty wins over `model_` for this one worker
  // -- the assistant asking for `sonnet` on a trivial task through the `aii`
  // block's `spawn ... model=` field. Empty (the ordinary case) means "use
  // whatever the pool is set to".
  bool spawn(const std::string& name, const std::string& cwd, const std::string& task,
             std::string* error, const std::string& model_override = {});
  // Interrupts the worker's current turn; it stays in the list as Paused.
  //
  // M17.3. Returns straight away, as it always did -- the interrupt is a line
  // on the child's stdin and the worker thread is what notices it. What is new
  // is what happens when it is *not* noticed: the moment the interrupt goes
  // out a clock starts, and update() ends the child itself once it runs out.
  // See `kInterruptGrace`.
  bool pause(const std::string& name);
  // Interrupts every running worker. Same escalation as pause().
  void pause_all();
  // Removes a finished (or paused) worker and its process.
  //
  // **This one blocks**, because it joins the worker thread before it returns:
  // the caller's next act is usually to say the work has stopped, and it must
  // be true by then. Before M17.3 it blocked for as long as the child felt
  // like taking, which for a child halfway through a Bash command that ignores
  // the interrupt was forever -- and the frame loop, and with it the whole
  // conversation, was gone with it. It now waits `kInterruptGrace` for the
  // interrupt to land and then ends the process, so the worst case is a known
  // number of seconds rather than the rest of the run.
  bool stop(const std::string& name);

  // How long the polite interrupt is given before the child is simply ended.
  //
  // Three seconds, which is the number `~ClaudeCodeClient` has always waited
  // for a child to exit after its stdin closed: that is the one figure in this
  // app that has been through a shutdown on this machine several hundred times
  // without anybody noticing it, so it is the honest answer to "how long is a
  // child allowed to take over ending a turn". It is also the worst case a
  // stop can now freeze the frame loop for, which puts it at the edge of what
  // is tolerable -- a window that stops compositing for three seconds looks
  // stuck, and one that never comes back *is* stuck.
  static constexpr std::chrono::milliseconds kInterruptGrace{3000};

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
    // M17.3. When the interrupt went out, and whether the child has since been
    // ended for ignoring it. Both are written under `mutex_` by the frame loop
    // (pause, pause_all, stop, update) and read there; the worker thread
    // touches neither.
    std::chrono::steady_clock::time_point cancel_at{};
    bool killed = false;
    State state = State::Starting;
    std::string activity, result;
    std::deque<std::string> recent;
    int tool_calls = 0;
  };

  void run(Worker* w);
  // M17.3. Raise the interrupt and start its clock. Caller holds `mutex_`.
  static void request_cancel(Worker* w);
  // M17.3. Wait up to `kInterruptGrace` for `w` to finish on its own, and end
  // its child if it does not. **Must be called with `mutex_` released**: the
  // client's activity callback takes `mutex_` while holding the client's own
  // lock, so killing under `mutex_` is that pair of locks in the other order.
  static void wait_then_kill(Worker* w);

  std::string exe_;
  bool bypass_ = true;
  // M31. Defaults: opus for the model (see set_model's comment) and Chrome on
  // -- the same-machine default `AII_WORKER_CHROME` seeds -- because a worker
  // is untouched by the conversational instance's own tool toggles and the
  // browser switch has to have *some* answer before main.cpp's first frame
  // mirrors `tools.browser` into it.
  std::string model_ = "opus";
  bool chrome_ = true;
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
