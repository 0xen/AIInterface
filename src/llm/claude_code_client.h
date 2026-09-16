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
    bool tools = false;         // false = `--tools ""` (pure chat)
    std::string cwd;            // working directory for the child (empty = inherit)
    // Tool-enabled instances only: skip the CLI's permission prompts, which
    // nothing can answer from this app. Leave false for anything that touches
    // a directory the user has not agreed to hand over.
    bool bypass_permissions = false;
  };

  // What a tool-enabled instance is doing, as it happens: "read main.cpp",
  // "bash: cmake --build ...". Called on the reader thread.
  using ActivityFn = std::function<void(const std::string& what)>;

  explicit ClaudeCodeClient(Options opt) : opt_(std::move(opt)) {}
  ~ClaudeCodeClient() override;

  bool start(std::string* error);
  // Set before the first turn; safe to leave unset.
  void set_on_activity(ActivityFn fn);
  const char* name() const override { return "claude-code"; }
  ChatResult turn(const std::string& user_text, const DeltaFn& on_delta,
                  std::atomic<bool>* cancel = nullptr) override;
  std::string status_line() const override;
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
  int request_counter_ = 0;
};

}  // namespace aii
