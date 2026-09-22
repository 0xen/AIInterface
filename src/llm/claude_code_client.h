#pragma once
// Backend that drives the locally installed Claude Code CLI (subscription billing).
// One long-lived `claude -p --input-format stream-json --output-format stream-json`
// process holds the conversation; each turn is a JSON line on its stdin and the
// reply streams back as JSON lines on its stdout.
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "llm/llm_client.h"

namespace aii {

class ClaudeCodeClient final : public LlmClient {
 public:
  struct Options {
    std::string exe;            // full path to claude.exe
    std::string system_prompt;  // replaces Claude Code's default system prompt
    std::string model;          // empty = the CLI's configured default
    std::string effort = "low";
    // Which built-in tools this instance gets, and — because the two cannot be
    // chosen independently here — how permission is settled for them. Nothing
    // in this app can answer a permission prompt, so every shape below has to
    // end at "runs" or "denied", never at "asks".
    //   ""         `--tools ""`: no tools at all. Nothing can prompt.
    //   "default"  every built-in tool; the flag is omitted entirely, which is
    //              what workers ran on before this was a string. Pair with
    //              `bypass_permissions`.
    //   a list     exactly those tools, e.g. "WebSearch,WebFetch" — and the
    //              same list is passed to `--allowedTools`, with
    //              `--permission-prompts none` behind it. See start().
    std::string tools;
    std::string cwd;            // working directory for the child (empty = inherit)
    // `tools == "default"` only: skip the CLI's permission prompts, which
    // nothing can answer from this app. Leave false for anything that touches
    // a directory the user has not agreed to hand over. A named tool list does
    // not use this — it pre-approves its own tools by name instead, so the
    // blanket bypass never has to be handed to an instance the user talks to.
    bool bypass_permissions = false;
    // M3.5. `--system-prompt` replaces the CLI's *default* prompt and suppresses
    // nothing the CLI discovers for itself: measured against 2.1.273, the
    // project's `CLAUDE.md`, the user's own Claude Code auto-memory and all of
    // their skills still reach the model. `--safe-mode` removes the first two;
    // `--disable-slash-commands` removes the third. This flag passes both.
    //
    // It is an option and not the default because the two kinds of instance
    // want opposite things. The conversational one must run on *our* prompts
    // alone. It used to lose nothing by it, having no tools at all; since M3.7
    // gave it `WebSearch`/`WebFetch` this flag is load-bearing a second way,
    // and measured so: `--tools` names members of the *built-in* set only, so
    // the user's MCP servers still attach their tools alongside a named
    // allowlist. Without this flag the same two-tool instance reported nine
    // extra `mcp__...` tools it had no business holding. `--safe-mode` is what
    // keeps them out. A worker wants all of those, and wants the
    // `CLAUDE.md` of the repo it was pointed at, which is exactly the file
    // that belongs in a coding agent's context. So: conversational on,
    // workers off.
    //
    // `--setting-sources user,local` looks like it does this and is a trap: it
    // drops the project `CLAUDE.md` and still leaks the auto-memory. `--bare`
    // suppresses more but authenticates strictly through `ANTHROPIC_API_KEY` /
    // `apiKeyHelper`, so it cannot be used on the subscription this app runs
    // on.
    bool suppress_cli_context = false;
    // M31. Claude in Chrome (the user's decision, 22 Sep 2026: "the workers as
    // well as the main AI have the ability to drive Claude in Chrome"). The
    // pinned CLI (2.1.280) has `--chrome` / `--no-chrome`; its tools are MCP
    // tools named `mcp__claude-in-chrome__<name>`. `start()` appends
    // `--chrome` when this is set, and, when `tools` is a restricted list,
    // also adds the server prefix `mcp__claude-in-chrome` to `--allowedTools`
    // -- `--chrome` alone is not enough there, because a named allowlist opts
    // *in* to exactly its own names and would otherwise leave the MCP tools
    // ungranted. When `tools == "default"` (workers) `--chrome` alone is
    // enough: `bypassPermissions` already covers whatever tools the flag adds.
    //
    // **Unmeasured from here.** Every other flag in this file was measured
    // against a running CLI by reading back what it actually did; this one
    // could not be, because nothing in this task can drive a browser or watch
    // one being driven. It is built to the CLI's own `--help` text and to the
    // tool-naming convention every other MCP server in this app already
    // follows. The coordinator measures it through the app.
    bool chrome = false;
  };

  explicit ClaudeCodeClient(Options opt) : opt_(std::move(opt)) {}
  ~ClaudeCodeClient() override;

  bool start(std::string* error);
  // Called on the reader thread. See LlmClient::set_on_activity.
  void set_on_activity(ActivityFn fn) override;
  const char* name() const override { return "claude-code"; }
  ChatResult turn(const std::string& user_text, const DeltaFn& on_delta,
                  std::atomic<bool>* cancel = nullptr) override;
  std::string status_line() const override;
  UsageStats usage() const override;
  std::string session_id() const { std::lock_guard<std::mutex> l(mutex_); return session_id_; }
  std::string model() const { std::lock_guard<std::mutex> l(mutex_); return model_; }

  // One line of the CLI's stream-json, already stripped of its newline. Called
  // on the reader thread, and public only so that it can be called off one:
  // this is the function that reads text written by another program, it is
  // where review finding 1 lived, and a client constructed and never started
  // is a perfectly good place to feed it recorded lines. See
  // `tests/json_shape_test.cpp`. It throws nothing, whatever it is given.
  void handle_line(const std::string& line);

  // M17.3, review finding 2. End the child now, from any thread.
  //
  // `turn()` returns on one event and one event only: the CLI's `result`. The
  // interrupt it sends when `cancel` goes up is a `control_request`, which the
  // CLI is free to take its time over or to ignore — a child halfway through a
  // Bash command does exactly that — and nothing escalated. So a worker that
  // would not stop froze `WorkerPool::stop()` in its join, and `start_turn()`
  // behind that, and the conversation was over for the rest of the run. The
  // destructor hung the same way at exit.
  //
  // This is the escalation, and it is deliberately brutal: close the child's
  // stdin, terminate the process, and finish the turn here with an error
  // rather than waiting for one to arrive. The reader thread then sees the
  // pipe break and unwinds on its own. Safe to call twice, safe to call on a
  // client that never started, and safe to call while the reader is mid-line
  // -- everything it touches is under `mutex_`, which is also why it must
  // never be called with a lock the activity callback takes already held.
  //
  // A killed turn comes back `ok == false`. When the caller raised `cancel`
  // first -- which is the only way this is reached inside this app -- it also
  // comes back `stop_reason == "interrupted"`, so a worker the app stopped on
  // purpose still reports as stopped rather than as failed.
  void kill();

 private:
  void reader_loop();
  // M26.1, finding 17. The child's stderr, on its own thread. Everything the
  // CLI has to say that is not stream-json arrives here -- "not signed in",
  // an unknown `--model`, a node crash -- and before this it was handed
  // `GetStdHandle(STD_ERROR_HANDLE)` and forgotten about, so a child that died
  // on any of them surfaced as "claude process exited: " with nothing after
  // the colon. A second thread rather than overlapped reads on the one reader:
  // the reader is blocked in `ReadFile` on stdout for minutes at a time, which
  // is exactly when stderr most needs draining, and a full stderr pipe would
  // block the child rather than merely delay a diagnostic.
  void stderr_loop();
  bool write_line(const std::string& line);
  // What to say about a child that has gone. Call with `mutex_` held: it reads
  // `stderr_tail_`. Returns "exit code 3: not signed in", or just the code, or
  // an empty string if neither is known yet.
  std::string exit_detail() const;

  Options opt_;
  HANDLE process_ = nullptr;
  HANDLE stdin_w_ = nullptr;
  HANDLE stdout_r_ = nullptr;
  HANDLE stderr_r_ = nullptr;
  std::thread reader_;
  std::thread err_reader_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool exited_ = false;
  bool err_done_ = false;  // M26.1: the stderr pipe has closed; the tail is all of it
  bool killed_ = false;  // M17.3: kill() has been here; the child is not coming back
  bool turn_done_ = false;
  bool turn_active_ = false;
  ChatResult current_;
  DeltaFn on_delta_;

  ActivityFn on_activity_;
  std::string session_id_, model_, last_error_;
  // M26.1: the last few kilobytes the child wrote to stderr. Bounded because a
  // CLI that has decided to be noisy can write for as long as it likes and
  // this is diagnostic text held for the length of a session; the tail is the
  // useful end of it anyway, since what killed the child is the last thing it
  // said.
  std::string stderr_tail_;
  double util_5h_ = -1, util_7d_ = -1;
  long long reset_5h_ = 0, reset_7d_ = 0;
  // Context-window fill: the newest message_start from the MAIN model (the
  // CLI also runs a small background model, whose usage must not count)
  // over the window size the result event reports for it.
  std::string canonical_model_;   // model_ without a "[1m]"-style suffix
  long long ctx_tokens_ = -1;
  long long ctx_window_ = 0;
  int request_counter_ = 0;
};

}  // namespace aii
