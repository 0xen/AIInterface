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

using DeltaFn = std::function<void(const std::string&)>;

class LlmClient {
 public:
  virtual ~LlmClient() = default;
  virtual const char* name() const = 0;
  // Blocks until the reply is complete. on_delta may be called from another thread.
  virtual ChatResult turn(const std::string& user_text, const DeltaFn& on_delta,
                          std::atomic<bool>* cancel = nullptr) = 0;
  // One-line usage/quota summary for the UI, empty if unknown.
  virtual std::string status_line() const { return {}; }
};

}  // namespace aii
