#include "llm/claude_client.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <string>

#include "json.hpp"
#include "llm/json_shape.h"

using json = nlohmann::json;

namespace aii {
namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() { if (h) WinHttpCloseHandle(h); }
};

}  // namespace

ClaudeClient::ClaudeClient(std::string api_key, std::string model)
    : api_key_(std::move(api_key)), model_(std::move(model)) {}

std::string ClaudeClient::build_body(const std::string& system_prompt,
                                     const std::vector<ChatMessage>& history) const {
  json body;
  body["model"] = model_;
  body["max_tokens"] = max_tokens_;
  body["stream"] = true;
  // Stable system prompt first, marked cacheable, so multi-turn sessions reuse the prefix.
  body["system"] = json::array({{{"type", "text"},
                                 {"text", system_prompt},
                                 {"cache_control", {{"type", "ephemeral"}}}}});
  json messages = json::array();
  for (const auto& m : history) messages.push_back({{"role", m.role}, {"content", m.text}});
  body["messages"] = messages;
  body["output_config"] = {{"effort", effort_}};
  if (fallbacks_) body["fallbacks"] = "default";
  return body.dump();
}

void ClaudeClient::handle_event(const std::string& event, const std::string& data, ChatResult& result,
                                const DeltaFn& on_delta) {
  if (data.empty()) return;
  json j;
  try {
    j = json::parse(data);
  } catch (...) {
    return;
  }
  // The same guard as `ClaudeCodeClient::handle_line` (M15.2, review finding
  // 1), for the same reason: every read below used to throw `type_error` on an
  // event of an unexpected shape, and this one runs on whichever thread called
  // `stream()`. The reads now go through `member()` and `field()`; the try is
  // the backstop. One dropped SSE event costs a number in a status line.
  try {
    if (event == "content_block_delta") {
      const json& d = member(j, "delta");
      if (field<std::string>(d, "type", "") == "text_delta") {
        std::string t = field<std::string>(d, "text", "");
        result.text += t;
        if (on_delta) on_delta(t);
      }
    } else if (event == "message_start") {
      const json& u = member(member(j, "message"), "usage");
      if (!u.is_null()) {
        result.input_tokens = field<int>(u, "input_tokens", 0);
        result.cache_read_tokens = field<int>(u, "cache_read_input_tokens", 0);
      }
    } else if (event == "message_delta") {
      const json& d = member(j, "delta");
      if (member(d, "stop_reason").is_string())
        result.stop_reason = field<std::string>(d, "stop_reason", "");
      const json& sd = member(d, "stop_details");
      if (!sd.is_null()) {
        result.error = "refusal: " + field<std::string>(sd, "category", std::string("?")) + " - " +
                       field<std::string>(sd, "explanation", std::string(""));
      }
      const json& u = member(j, "usage");
      if (!u.is_null()) result.output_tokens = field<int>(u, "output_tokens", 0);
    } else if (event == "error") {
      result.error = field<std::string>(member(j, "error"), "message", "unknown error");
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[claude-api] skipped an event of unexpected shape: %s\n", e.what());
  }
}

ChatResult ClaudeClient::stream(const std::string& system_prompt, const std::vector<ChatMessage>& history,
                                const DeltaFn& on_delta, std::atomic<bool>* cancel) {
  ChatResult result;
  const std::string body = build_body(system_prompt, history);

  Handle session, connection, request;
  session.h = WinHttpOpen(L"AIInterface/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session.h) { result.error = "WinHttpOpen failed"; return result; }
  // resolve, connect, send, receive (ms). Receive is long because the stream can be quiet while thinking.
  WinHttpSetTimeouts(session.h, 10000, 15000, 30000, 600000);
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
  WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

  connection.h = WinHttpConnect(session.h, L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!connection.h) { result.error = "WinHttpConnect failed"; return result; }

  request.h = WinHttpOpenRequest(connection.h, L"POST", L"/v1/messages", nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!request.h) { result.error = "WinHttpOpenRequest failed"; return result; }

  std::string headers = "Content-Type: application/json\r\n"
                        "Accept: text/event-stream\r\n"
                        "x-api-key: " + api_key_ + "\r\n"
                        "anthropic-version: 2023-06-01\r\n";
  if (fallbacks_) headers += "anthropic-beta: server-side-fallback-2026-07-01\r\n";
  std::wstring wheaders = widen(headers);

  if (!WinHttpSendRequest(request.h, wheaders.c_str(), (DWORD)wheaders.size(), (LPVOID)body.data(),
                          (DWORD)body.size(), (DWORD)body.size(), 0)) {
    result.error = "WinHttpSendRequest failed (" + std::to_string(GetLastError()) + ")";
    return result;
  }
  if (!WinHttpReceiveResponse(request.h, nullptr)) {
    result.error = "WinHttpReceiveResponse failed (" + std::to_string(GetLastError()) + ")";
    return result;
  }

  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  result.http_status = (int)status;

  std::string pending;  // unparsed SSE bytes
  std::string raw_body; // for non-200 error reporting
  std::string cur_event;
  std::string cur_data;
  char buf[16384];
  for (;;) {
    if (cancel && cancel->load()) { result.error = "cancelled"; break; }
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(request.h, &avail)) {
      result.error = "WinHttpQueryDataAvailable failed (" + std::to_string(GetLastError()) + ")";
      break;
    }
    if (avail == 0) break;  // end of response
    DWORD got = 0;
    DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
    if (!WinHttpReadData(request.h, buf, want, &got)) {
      result.error = "WinHttpReadData failed (" + std::to_string(GetLastError()) + ")";
      break;
    }
    if (got == 0) break;
    if (status != 200) { raw_body.append(buf, got); continue; }

    pending.append(buf, got);
    // Consume complete lines; a blank line ends an event.
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) {
        handle_event(cur_event, cur_data, result, on_delta);
        cur_event.clear();
        cur_data.clear();
        if (!result.error.empty() && result.stop_reason.empty()) {
          // 'error' event: nothing more useful will come
        }
        continue;
      }
      if (line.rfind("event:", 0) == 0) {
        cur_event = line.substr(6);
        while (!cur_event.empty() && cur_event.front() == ' ') cur_event.erase(0, 1);
      } else if (line.rfind("data:", 0) == 0) {
        std::string d = line.substr(5);
        while (!d.empty() && d.front() == ' ') d.erase(0, 1);
        if (!cur_data.empty()) cur_data += '\n';
        cur_data += d;
      }
    }
  }

  if (status != 200) {
    std::string msg = raw_body;
    try {
      json j = json::parse(raw_body);
      if (j.contains("error")) msg = j["error"].value("type", "") + ": " + j["error"].value("message", "");
    } catch (...) {
    }
    result.error = "HTTP " + std::to_string(status) + " " + msg;
    return result;
  }
  if (result.stop_reason == "refusal") return result;  // error already describes it
  result.ok = result.error.empty();
  return result;
}

ChatResult ApiLlmClient::turn(const std::string& user_text, const DeltaFn& on_delta,
                              std::atomic<bool>* cancel) {
  history_.push_back({"user", user_text});
  ChatResult r = client_.stream(system_, history_, on_delta, cancel);
  if (r.ok) history_.push_back({"assistant", r.text});
  else history_.pop_back();
  return r;
}

}  // namespace aii
