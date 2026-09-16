#pragma once
// Streaming client for the Claude Messages API over WinHTTP (API-key billing).
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "llm/llm_client.h"

namespace aii {

struct ChatMessage {
  std::string role;  // "user" | "assistant"
  std::string text;
};

class ClaudeClient {
 public:
  ClaudeClient(std::string api_key, std::string model);

  void set_effort(std::string effort) { effort_ = std::move(effort); }
  void set_max_tokens(int n) { max_tokens_ = n; }
  void set_fallbacks(bool on) { fallbacks_ = on; }

  // Blocks until the stream ends. on_delta is called on this thread for each text delta.
  ChatResult stream(const std::string& system_prompt, const std::vector<ChatMessage>& history,
                    const DeltaFn& on_delta, std::atomic<bool>* cancel = nullptr);

 private:
  std::string build_body(const std::string& system_prompt, const std::vector<ChatMessage>& history) const;
  void handle_event(const std::string& event, const std::string& data, ChatResult& result,
                    const DeltaFn& on_delta);

  std::string api_key_;
  std::string model_;
  std::string effort_ = "low";
  int max_tokens_ = 8000;
  bool fallbacks_ = true;
};

// LlmClient adapter: keeps the conversation history in memory and calls the API.
class ApiLlmClient final : public LlmClient {
 public:
  ApiLlmClient(std::string api_key, std::string model, std::string effort, std::string system_prompt)
      : client_(std::move(api_key), std::move(model)), system_(std::move(system_prompt)) {
    client_.set_effort(std::move(effort));
  }
  const char* name() const override { return "api"; }
  ChatResult turn(const std::string& user_text, const DeltaFn& on_delta,
                  std::atomic<bool>* cancel = nullptr) override;

 private:
  ClaudeClient client_;
  std::string system_;
  std::vector<ChatMessage> history_;
};

}  // namespace aii
