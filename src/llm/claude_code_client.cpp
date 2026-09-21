#include "llm/claude_code_client.h"

#include <chrono>
#include <ctime>
#include <cstdio>

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

// `resetsAt` is unix seconds, but be tolerant of a millisecond value: a
// seconds timestamp will not reach 1e11 for another thousand years.
long long to_unix_seconds(long long v) { return v > 100000000000LL ? v / 1000 : v; }

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
  // M17.3. The handles are taken out under the lock and used outside it. Under
  // it, because `kill()` may be running on another thread and two threads
  // closing one handle is a handle number that gets reused between them;
  // outside it, because the wait below is three seconds long and the reader
  // thread takes the same lock for every line the CLI sends.
  HANDLE proc = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stdin_w_) { CloseHandle(stdin_w_); stdin_w_ = nullptr; }  // EOF -> the CLI exits
    proc = process_;
    process_ = nullptr;  // nothing may terminate it after this line
  }
  if (proc) {
    if (WaitForSingleObject(proc, 3000) != WAIT_OBJECT_0) TerminateProcess(proc, 0);
    CloseHandle(proc);
  }
  // `stdout_r_` is deliberately left alone until the reader has been joined:
  // the reader loop is inside ReadFile on it, and closing a handle a thread is
  // blocked on is not how you stop that thread. Closing stdin, or terminating
  // above, is; the pipe breaks and ReadFile returns.
  if (reader_.joinable()) reader_.join();
  if (stdout_r_) CloseHandle(stdout_r_);
}

void ClaudeCodeClient::kill() {
  std::lock_guard<std::mutex> lock(mutex_);
  killed_ = true;
  // stdin first: on a child that is merely slow this alone is enough, and it
  // is the polite half of the pair.
  if (stdin_w_) { CloseHandle(stdin_w_); stdin_w_ = nullptr; }
  // TerminateProcess does not wait, so it is safe under the lock, and being
  // under the lock is what keeps the destructor from closing the handle
  // between the read and the call.
  if (process_) TerminateProcess(process_, 1);
  // Do not wait for the reader to notice. It will -- the pipe is broken -- but
  // a caller that has got this far has already waited as long as it means to,
  // and the whole point is that it stops waiting now.
  if (turn_active_ && !turn_done_) {
    current_.ok = false;
    current_.error = "the app stopped this instance";
    last_error_ = current_.error;
    turn_done_ = true;
  }
  cv_.notify_all();
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
  // Tools, and the permission flags that have to travel with them. See
  // Options::tools. The rule this encodes is that no configuration reachable
  // from here may leave the CLI *asking*: nothing in this app can answer, and a
  // question nobody answers is a conversation frozen forever.
  //
  // M3.7, measured against 2.1.275 rather than read off `--help`, which says
  // nothing about any of it: **both** `WebSearch` and `WebFetch` need
  // permission. Naming them in `--tools` only makes them exist; each one's
  // first call comes back as a `permission_denials` entry and the model
  // apologises that it has not been allowed to search. `--allowedTools` with
  // the same names is what actually grants them, and it grants `WebFetch` for
  // every domain — the per-domain approval the interactive CLI asks for does
  // not reappear here. A cross-host redirect is handed back to the model as
  // data ("REDIRECT DETECTED"), never as a prompt, and `file://` is refused by
  // URL validation, so `WebFetch` buys no local reach.
  //
  // `--permission-prompts none` is the backstop for everything not on that
  // list: anything that would prompt is denied outright. In stream-json mode
  // the CLI never sent a permission `control_request` to this host even
  // without it — it denied and carried on — but that is the SDK deciding we
  // have no handler, not a promise, and this turns the promise into a flag.
  if (opt_.tools != "default") cmd += " --tools " + quote_arg(opt_.tools);
  if (!opt_.tools.empty() && opt_.tools != "default")
    cmd += " --allowedTools " + quote_arg(opt_.tools) + " --permission-prompts none";
  if (opt_.tools == "default" && opt_.bypass_permissions)
    cmd += " --permission-mode bypassPermissions";
  if (!opt_.system_prompt.empty()) cmd += " --system-prompt " + quote_arg(opt_.system_prompt);
  // See Options::suppress_cli_context. Two flags because they suppress two
  // different things: --safe-mode takes the project CLAUDE.md and the user's
  // own auto-memory, --disable-slash-commands takes the skills.
  if (opt_.suppress_cli_context) cmd += " --safe-mode --disable-slash-commands";
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
  // M17.3. Under the lock, because `kill()` closes this handle and a write
  // racing that close is a write to a handle number the OS may already have
  // handed to something else. Held across the WriteFile rather than copying
  // the handle out, which would leave the same window open; the lines written
  // here are one JSON message each and the CLI drains its stdin continuously,
  // so the pipe does not fill and the call does not block. Both callers --
  // turn()'s send and its interrupt -- release `mutex_` before they get here.
  std::lock_guard<std::mutex> lock(mutex_);
  if (!stdin_w_) return false;
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
  // **This runs on the reader thread, where an escaped exception is the end of
  // the app** (M15.2, review finding 1). Only the parse above used to be
  // guarded; every read after it was a `value()` or an `operator[]` that throws
  // `type_error` on a top-level array, or on a field the CLI renamed, or on one
  // whose type changed between versions -- and there is no handler above this
  // frame, so `std::terminate` followed and the window vanished mid-sentence.
  //
  // Two layers, on purpose. The reads below now go through `member()` and
  // `field()` (llm/json_shape.h), which make a missing or wrongly-typed field a
  // fallback value instead of a throw; the try here is the backstop for the
  // next edit that forgets. A line this reader cannot make sense of is one
  // line of telemetry, so it is dropped and noted rather than being allowed to
  // take the conversation with it.
  try {
    const std::string type = field<std::string>(j, "type", "");
    std::lock_guard<std::mutex> lock(mutex_);

    if (type == "system") {
      if (field<std::string>(j, "subtype", "") == "init") {
        session_id_ = field<std::string>(j, "session_id", "");
        model_ = field<std::string>(j, "model", "");
        // "claude-opus-5[1m]" -> "claude-opus-5": message_start reports the
        // canonical id, while modelUsage is keyed by the decorated one.
        canonical_model_ = model_;
        if (const auto b = canonical_model_.find('['); b != std::string::npos)
          canonical_model_.erase(b);
        // Until a result event reports the real size, assume the documented
        // default (the "[1m]" suffix is the CLI's own long-context marker).
        if (ctx_window_ == 0)
          ctx_window_ = model_.find("[1m]") != std::string::npos ? 1000000 : 200000;
        cv_.notify_all();
      }
    } else if (type == "rate_limit_event") {
      const json& w = member(member(j, "rate_limit_info"), "unifiedWindows");
      const json& h5 = member(w, "five_hour");
      if (!h5.is_null()) {
        util_5h_ = field<double>(h5, "utilization", -1.0);
        reset_5h_ = to_unix_seconds(field<long long>(h5, "resetsAt", 0LL));
      }
      const json& d7 = member(w, "seven_day");
      if (!d7.is_null()) {
        util_7d_ = field<double>(d7, "utilization", -1.0);
        reset_7d_ = to_unix_seconds(field<long long>(d7, "resetsAt", 0LL));
      }
    } else if (type == "stream_event") {
      const json& ev = member(j, "event");
      const std::string et = field<std::string>(ev, "type", "");
      if (et == "content_block_delta") {
        const json& d = member(ev, "delta");
        if (field<std::string>(d, "type", "") == "text_delta") {
          std::string t = field<std::string>(d, "text", "");
          if (turn_active_ && !turn_done_) {
            current_.text += t;
            if (on_delta_) on_delta_(t);
          }
        }
      } else if (et == "content_block_start") {
        // A tool call starting: report what the instance is about to do.
        const json& b = member(ev, "content_block");
        if (field<std::string>(b, "type", "") == "tool_use" && on_activity_) {
          on_activity_(field<std::string>(b, "name", "tool"));
        }
      } else if (et == "message_start") {
        const json& msg = member(ev, "message");
        const json& u = member(msg, "usage");
        if (!u.is_null()) {
          current_.input_tokens = field<int>(u, "input_tokens", 0);
          current_.cache_read_tokens = field<int>(u, "cache_read_input_tokens", 0);
          // Everything handed to the model this request IS the context in use.
          // Only the main model counts: the CLI also drives a small background
          // model whose own message_start events pass through here.
          const std::string mm = field<std::string>(msg, "model", "");
          if (canonical_model_.empty() || mm == canonical_model_ || mm == model_) {
            ctx_tokens_ = field<long long>(u, "input_tokens", 0LL) +
                          field<long long>(u, "cache_read_input_tokens", 0LL) +
                          field<long long>(u, "cache_creation_input_tokens", 0LL);
          }
        }
      } else if (et == "message_delta") {
        const json& d = member(ev, "delta");
        if (member(d, "stop_reason").is_string())
          current_.stop_reason = field<std::string>(d, "stop_reason", "");
        const json& u = member(ev, "usage");
        if (!u.is_null()) current_.output_tokens = field<int>(u, "output_tokens", 0);
      }
    } else if (type == "result") {
      const bool is_error = field<bool>(j, "is_error", false);
      // `result` is a string on every shape this app has seen, and reading it
      // with `get<std::string>()` on the strength of that is what finding 1
      // is about. It is still guarded by `is_string()` here, as it always was,
      // and `field()` would have caught it anyway.
      const json& res = member(j, "result");
      if (current_.text.empty() && res.is_string()) {
        // No partial deltas arrived (e.g. tools disabled and short reply): use the final text.
        const std::string t = res.get<std::string>();
        if (!is_error) {
          current_.text = t;
          if (on_delta_) on_delta_(t);
        }
      }
      if (is_error) {
        current_.error = res.is_string() ? res.get<std::string>()
                                         : field<std::string>(j, "subtype", "error");
        last_error_ = current_.error;
      }
      if (member(j, "total_cost_usd").is_number())
        current_.cost_usd = field<double>(j, "total_cost_usd", -1.0);
      // The CLI states the real context size per model it used; take the main
      // model's so the guess made at init stops being used.
      const json& usage_by_model = member(j, "modelUsage");
      if (usage_by_model.is_object()) {
        for (const auto& entry : usage_by_model.items()) {
          const auto& mu = entry.value();
          if (!mu.is_object()) continue;
          if (entry.key() != model_ &&
              field<std::string>(mu, "canonicalModel", std::string()) != canonical_model_)
            continue;
          const long long win = field<long long>(mu, "contextWindow", 0LL);
          if (win > 0) ctx_window_ = win;
          break;
        }
      }
      if (current_.stop_reason.empty())
        current_.stop_reason = field<std::string>(j, "stop_reason", std::string(""));
      const json& u = member(j, "usage");
      if (!u.is_null()) {
        if (current_.output_tokens == 0) current_.output_tokens = field<int>(u, "output_tokens", 0);
        if (current_.cache_read_tokens == 0)
          current_.cache_read_tokens = field<int>(u, "cache_read_input_tokens", 0);
      }
      current_.ok = !is_error;
      turn_done_ = true;
      cv_.notify_all();
    }
  } catch (const std::exception& e) {
    // Nowhere better to put this: the client has no logger, and `last_error_`
    // is the text spoken when the child dies, which a malformed telemetry line
    // has no business rewriting. stderr is where the CLI's own diagnostics go.
    std::fprintf(stderr, "[claude] skipped a line of unexpected shape: %s\n", e.what());
  }
}

void ClaudeCodeClient::set_on_activity(ActivityFn fn) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_activity_ = std::move(fn);
}

ChatResult ClaudeCodeClient::turn(const std::string& user_text, const DeltaFn& on_delta,
                                  std::atomic<bool>* cancel) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // M17.3: `killed_` as well as `exited_`, because a kill is instant and the
    // reader may not have noticed the broken pipe yet. A turn started in that
    // window would otherwise be written to a closed stdin and then wait for a
    // result that can never come.
    if (exited_ || killed_) {
      ChatResult r;
      r.error = killed_ ? "the app stopped this instance"
                        : "claude process is not running: " + last_error_;
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

UsageStats ClaudeCodeClient::usage() const {
  std::lock_guard<std::mutex> lock(mutex_);
  UsageStats u;
  u.model = model_;
  u.session = util_5h_;
  u.week = util_7d_;
  u.session_reset = reset_5h_;
  u.week_reset = reset_7d_;
  u.ctx_window = ctx_window_;
  if (ctx_tokens_ >= 0 && ctx_window_ > 0)
    u.ctx = static_cast<double>(ctx_tokens_) / static_cast<double>(ctx_window_);
  return u;
}

}  // namespace aii
