#include "llm/claude_code_client.h"

#include <chrono>
#include <ctime>
#include <cstdio>

#include "json.hpp"

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

// Quote one argument the way CommandLineToArgvW expects.
std::string quote_arg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
  std::string out = "\"";
  size_t backslashes = 0;
  for (char c : a) {
    if (c == '\\') { ++backslashes; continue; }
    if (c == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, '\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, '\\');
  out.push_back('"');
  return out;
}

std::string fmt_reset(long long epoch) {
  if (epoch <= 0) return "?";
  long long now = (long long)std::time(nullptr);
  long long d = epoch - now;
  if (d < 0) d = 0;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lldh%02dm", d / 3600, (int)((d % 3600) / 60));
  return buf;
}

}  // namespace

ClaudeCodeClient::~ClaudeCodeClient() {
  if (stdin_w_) { CloseHandle(stdin_w_); stdin_w_ = nullptr; }  // EOF -> the CLI exits
  if (process_) {
    if (WaitForSingleObject(process_, 3000) != WAIT_OBJECT_0) TerminateProcess(process_, 0);
    CloseHandle(process_);
  }
  if (reader_.joinable()) reader_.join();
  if (stdout_r_) CloseHandle(stdout_r_);
}

bool ClaudeCodeClient::start(std::string* error) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE stdin_r = nullptr, stdout_w = nullptr;
  if (!CreatePipe(&stdin_r, &stdin_w_, &sa, 0) || !CreatePipe(&stdout_r_, &stdout_w, &sa, 1 << 20)) {
    if (error) *error = "CreatePipe failed";
    return false;
  }
  SetHandleInformation(stdin_w_, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stdout_r_, HANDLE_FLAG_INHERIT, 0);

  std::string cmd = quote_arg(opt_.exe) +
                    " -p --input-format stream-json --output-format stream-json --verbose"
                    " --include-partial-messages --no-session-persistence";
  if (!opt_.effort.empty()) cmd += " --effort " + opt_.effort;
  if (!opt_.model.empty()) cmd += " --model " + quote_arg(opt_.model);
  if (!opt_.tools) cmd += " --tools \"\"";
  if (!opt_.system_prompt.empty()) cmd += " --system-prompt " + quote_arg(opt_.system_prompt);
  std::wstring wcmd = widen(cmd);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = stdin_r;
  si.hStdOutput = stdout_w;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  std::wstring wcwd = widen(opt_.cwd);
  BOOL okay = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
  CloseHandle(stdin_r);
  CloseHandle(stdout_w);
  if (!okay) {
    if (error) *error = "CreateProcess failed (" + std::to_string(GetLastError()) + "): " + cmd;
    return false;
  }
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  reader_ = std::thread([this] { reader_loop(); });

  // In stream-json input mode the CLI sends its init event only once the first
  // message arrives, so just make sure the process survived launch.
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return exited_; });
  if (exited_) {
    if (error) *error = "claude exited during startup: " + last_error_;
    return false;
  }
  return true;
}

bool ClaudeCodeClient::write_line(const std::string& line) {
  std::string data = line + "\n";
  DWORD written = 0;
  return WriteFile(stdin_w_, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
}

void ClaudeCodeClient::reader_loop() {
  std::string pending;
  char buf[65536];
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(stdout_r_, buf, sizeof(buf), &got, nullptr) || got == 0) break;
    pending.append(buf, got);
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) handle_line(line);
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  exited_ = true;
  if (turn_active_ && !turn_done_) {
    current_.error = "claude process exited: " + last_error_;
    turn_done_ = true;
  }
  cv_.notify_all();
}

void ClaudeCodeClient::handle_line(const std::string& line) {
  json j;
  try {
    j = json::parse(line);
  } catch (...) {
    return;
  }
  const std::string type = j.value("type", "");
  std::lock_guard<std::mutex> lock(mutex_);

  if (type == "system") {
    if (j.value("subtype", "") == "init") {
      session_id_ = j.value("session_id", "");
      model_ = j.value("model", "");
      cv_.notify_all();
    }
  } else if (type == "rate_limit_event") {
    const auto& info = j["rate_limit_info"];
    if (info.contains("unifiedWindows")) {
      const auto& w = info["unifiedWindows"];
      if (w.contains("five_hour")) {
        util_5h_ = w["five_hour"].value("utilization", -1.0);
        reset_5h_ = w["five_hour"].value("resetsAt", 0LL);
      }
      if (w.contains("seven_day")) {
        util_7d_ = w["seven_day"].value("utilization", -1.0);
        reset_7d_ = w["seven_day"].value("resetsAt", 0LL);
      }
    }
  } else if (type == "stream_event") {
    const auto& ev = j["event"];
    const std::string et = ev.value("type", "");
    if (et == "content_block_delta") {
      const auto& d = ev["delta"];
      if (d.value("type", "") == "text_delta") {
        std::string t = d.value("text", "");
        if (turn_active_ && !turn_done_) {
          current_.text += t;
          if (on_delta_) on_delta_(t);
        }
      }
    } else if (et == "message_start") {
      if (ev.contains("message") && ev["message"].contains("usage")) {
        const auto& u = ev["message"]["usage"];
        current_.input_tokens = u.value("input_tokens", 0);
        current_.cache_read_tokens = u.value("cache_read_input_tokens", 0);
      }
    } else if (et == "message_delta") {
      if (ev.contains("delta") && ev["delta"].contains("stop_reason") && !ev["delta"]["stop_reason"].is_null())
        current_.stop_reason = ev["delta"]["stop_reason"].get<std::string>();
      if (ev.contains("usage")) current_.output_tokens = ev["usage"].value("output_tokens", 0);
    }
  } else if (type == "result") {
    bool is_error = j.value("is_error", false);
    if (current_.text.empty() && j.contains("result") && j["result"].is_string()) {
      // No partial deltas arrived (e.g. tools disabled and short reply): use the final text.
      std::string t = j["result"].get<std::string>();
      if (!is_error) {
        current_.text = t;
        if (on_delta_) on_delta_(t);
      }
    }
    if (is_error) {
      current_.error = j.contains("result") && j["result"].is_string() ? j["result"].get<std::string>()
                                                                        : j.value("subtype", "error");
      last_error_ = current_.error;
    }
    if (j.contains("total_cost_usd")) current_.cost_usd = j.value("total_cost_usd", -1.0);
    if (current_.stop_reason.empty()) current_.stop_reason = j.value("stop_reason", std::string(""));
    if (j.contains("usage")) {
      const auto& u = j["usage"];
      if (current_.output_tokens == 0) current_.output_tokens = u.value("output_tokens", 0);
      if (current_.cache_read_tokens == 0) current_.cache_read_tokens = u.value("cache_read_input_tokens", 0);
    }
    current_.ok = !is_error;
    turn_done_ = true;
    cv_.notify_all();
  }
}

ChatResult ClaudeCodeClient::turn(const std::string& user_text, const DeltaFn& on_delta,
                                  std::atomic<bool>* cancel) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (exited_) {
      ChatResult r;
      r.error = "claude process is not running: " + last_error_;
      return r;
    }
    current_ = ChatResult{};
    on_delta_ = on_delta;
    turn_active_ = true;
    turn_done_ = false;
  }
  json msg = {{"type", "user"}, {"message", {{"role", "user"}, {"content", user_text}}}};
  if (!write_line(msg.dump())) {
    std::lock_guard<std::mutex> lock(mutex_);
    turn_active_ = false;
    ChatResult r;
    r.error = "failed to write to claude stdin";
    return r;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  bool interrupted = false;
  while (!turn_done_) {
    cv_.wait_for(lock, std::chrono::milliseconds(100));
    if (cancel && cancel->load() && !interrupted) {
      interrupted = true;
      json req = {{"type", "control_request"},
                  {"request_id", "int-" + std::to_string(++request_counter_)},
                  {"request", {{"subtype", "interrupt"}}}};
      lock.unlock();
      write_line(req.dump());
      lock.lock();
    }
  }
  turn_active_ = false;
  on_delta_ = nullptr;
  ChatResult r = current_;
  if (interrupted) r.stop_reason = "interrupted";
  return r;
}

std::string ClaudeCodeClient::status_line() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (util_5h_ < 0 && util_7d_ < 0) return {};
  char buf[220];
  std::snprintf(buf, sizeof(buf),
                "%s | usage: 5-hour window %.0f%% used (resets in %s), 7-day %.0f%% (resets in %s)",
                model_.empty() ? "model ?" : model_.c_str(), util_5h_ * 100.0, fmt_reset(reset_5h_).c_str(),
                util_7d_ * 100.0, fmt_reset(reset_7d_).c_str());
  return buf;
}

}  // namespace aii
