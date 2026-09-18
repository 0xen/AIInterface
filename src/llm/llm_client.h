#pragma once
// Backend-neutral interface: one user turn in, streamed text out.
#include <atomic>
#include <functional>
#include <string>

namespace aii {

struct ChatResult {
  bool ok = false;
  std::string text;         // concatenated text deltas
  std::string stop_reason;  // end_turn, max_tokens, refusal, ...
  std::string error;        // set when !ok
  int http_status = 0;
  int input_tokens = 0;
  int output_tokens = 0;
  int cache_read_tokens = 0;
  double cost_usd = -1.0;   // reported by the Claude Code backend only
};

// What the Claude Code CLI reports about the session: how full the context
// window is and how much of each subscription window has been consumed. The
// fractions are 0..1; a negative value means the CLI has not said yet.
struct UsageStats {
  double ctx = -1.0;      // context window in use / its size
  long long ctx_window = 0;  // that window's size in tokens, 0 = unknown. A
                             // fraction alone cannot be compared with a token
                             // count, and M5.3's footer has to do exactly that.
  double session = -1.0;  // five-hour window utilisation
  double week = -1.0;     // seven-day window utilisation
  long long session_reset = 0;  // unix seconds, 0 = unknown
  long long week_reset = 0;
  std::string model;
};

using DeltaFn = std::function<void(const std::string&)>;

// What a tool-enabled instance is doing, as it happens: "WebSearch",
// "bash: cmake --build ...". Called from the backend's reader thread, mid-turn.
using ActivityFn = std::function<void(const std::string& what)>;

class LlmClient {
 public:
  virtual ~LlmClient() = default;
  virtual const char* name() const = 0;
  // Set before the first turn; safe to leave unset, and a backend with no
  // tools never calls it. It exists on the interface rather than on the one
  // backend that reports activity because the caller that most needs it — the
  // voice loop, which has to explain a silent several-second turn — holds an
  // `LlmClient`, not a `ClaudeCodeClient`.
  virtual void set_on_activity(ActivityFn /*fn*/) {}
  // Blocks until the reply is complete. on_delta may be called from another thread.
  virtual ChatResult turn(const std::string& user_text, const DeltaFn& on_delta,
                          std::atomic<bool>* cancel = nullptr) = 0;
  // One-line usage/quota summary for the UI, empty if unknown.
  virtual std::string status_line() const { return {}; }
  // The same numbers unformatted, for front-ends that lay them out
  // themselves. Negative = not known yet.
  virtual UsageStats usage() const { return {}; }
};

}  // namespace aii
