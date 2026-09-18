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

 private:
  void reader_loop();
  void handle_line(const std::string& line);
  bool write_line(const std::string& line);

  Options opt_;
  HANDLE process_ = nullptr;
  HANDLE stdin_w_ = nullptr;
  HANDLE stdout_r_ = nullptr;
  std::thread reader_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool exited_ = false;
  bool turn_done_ = false;
  bool turn_active_ = false;
  ChatResult current_;
  DeltaFn on_delta_;

  ActivityFn on_activity_;
  std::string session_id_, model_, last_error_;
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
