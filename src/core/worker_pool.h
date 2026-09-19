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

// One command parsed out of a fenced ```aii block in a reply.
//
// The block is this app's only agent→app command channel, and stays that way:
// M1c.2's toolbar buttons are a verb here rather than a second mechanism, and
// M2.5's bus is specced to reuse the same registry behind it.
struct Command {
  std::string verb;  // spawn | pause | stop | schedule (for the session)
  std::string name;
  std::string cwd;
  std::string task;
  // The `schedule` verb's fields (M2b.3). `in` is a spoken-shaped delay
  // ("10m", "90s") parsed by `aii::parse_delay`; `say` is the exact sentence a
  // bare timer speaks when it fires. `grade` is normally left empty and
  // derived from the shape — a line with `say=` is a fixed report, a line with
  // `task=` is a phrased one — and exists as a key so the bus (M2b.2) can be
  // explicit where a script has no shape to signal with.
  std::string in;
  std::string say;
  std::string grade;
  // The `button` verb's fields (M1c.2). Kept in the same struct rather than a
  // variant because the block is line-oriented key=value either way and one
  // parser is what makes a new verb a few lines instead of a format.
  std::string id;
  std::string label;
  std::string tip;
  std::string path;
  // The `setting` verb's fields (M3.14). `key` is a dotted `section.key` of
  // `settings.json` — the file's own vocabulary rather than a second one, so
  // that the words the model uses are the words the user reads.
  //
  // **`confirm` is a field and not prose, which is the whole of how the
  // question gets asked.** A change that restarts the `claude` child throws
  // the conversation away (M3.12), so warning about it has to survive being
  // forgotten — and a warning the model merely *says* can be skipped in the
  // same reply that acts, which is a question in grammar only. Making the
  // consent a token the model has to write turns an omission into a
  // commission, and `VoiceSession::apply_setting` then refuses it outright
  // unless the app asked on an earlier turn. See M3.13: the lever on this
  // model's behaviour was the syntax line, not the paragraph beside it.
  std::string key;
  std::string value;
  std::string confirm;
};

// Finds ```aii fenced blocks in `text` and parses their command lines.
//
// Values are unquoted single tokens, or `"quoted like this"` when they contain
// spaces; `task=` and `path=` run to the end of the line when unquoted, since
// both are routinely the last thing on it and both routinely contain spaces.
//
// **App-owned verbs are applied here and are not returned.** `button` has no
// worker to dispatch to — it registers with ButtonRegistry — and this is the
// one point every ```aii``` block in the process already flows through, exactly
// once per completed reply, which is precisely the cardinality registration
// wants. The caller keeps dispatching the worker verbs it owns and is not
// widened every time the app gains one of its own.
std::vector<Command> parse_commands(const std::string& text);

// The same text with every ```aii block removed, for display.
std::string strip_aii_blocks(const std::string& text);

}  // namespace aii
