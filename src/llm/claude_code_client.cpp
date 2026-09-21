#include "llm/claude_code_client.h"

#include <chrono>
#include <ctime>
#include <cstdio>

#include "core/text_util.h"
#include "json.hpp"
#include "llm/json_shape.h"

using json = nlohmann::json;

namespace aii {
namespace {

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

// M26.1. How much of the child's stderr is kept, and how much of that is
// allowed into a sentence the user sees. The first is generous because it is
// only memory and a stack trace is worth having in the log; the second is
// tight because it goes in a status line beside everything else.
constexpr std::size_t kStderrTailBytes = 8192;
constexpr std::size_t kStderrInErrorBytes = 200;

// The useful end of what the child said. A CLI that fails on startup prints
// its reason last, and usually prints blank lines and a banner before it, so
// take the final non-empty lines rather than the final N bytes -- a byte cut
// would just as happily hand back the tail of a stack frame.
std::string last_words(const std::string& tail) {
  std::string out;
  std::size_t end = tail.size();
  while (end > 0 && out.size() < kStderrInErrorBytes) {
    while (end > 0 && (tail[end - 1] == '\n' || tail[end - 1] == '\r' || tail[end - 1] == ' ' ||
                       tail[end - 1] == '\t'))
      --end;
    if (end == 0) break;
    std::size_t begin = tail.find_last_of("\r\n", end - 1);
    begin = begin == std::string::npos ? 0 : begin + 1;
    std::string line = trim(tail.substr(begin, end - begin));
    if (!line.empty()) out = out.empty() ? line : line + " / " + out;
    end = begin;
  }
  return clip_utf8(out, kStderrInErrorBytes);
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
  // M26.1: the stderr reader unwinds for the same reason and under the same
  // rule -- its pipe breaks when the child goes, and the handle is closed only
  // after the thread that is inside `ReadFile` on it has been joined.
  if (err_reader_.joinable()) err_reader_.join();
  if (stderr_r_) CloseHandle(stderr_r_);
}

std::string ClaudeCodeClient::exit_detail() const {
  std::string code;
  DWORD status = 0;
  if (process_ && GetExitCodeProcess(process_, &status) && status != STILL_ACTIVE)
    code = "exit code " + std::to_string(static_cast<long long>(static_cast<int>(status)));
  const std::string said = last_words(stderr_tail_);
  if (code.empty()) return said;
  if (said.empty()) return code;
  return code + ": " + said;
}

void ClaudeCodeClient::stderr_loop() {
  char buf[4096];
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(stderr_r_, buf, sizeof(buf), &got, nullptr) || got == 0) break;
    // Still to the app's own stderr as well, which `main.cpp` has already
    // pointed at `avatar.log`. Piping it here is about getting the text into
    // `last_error_` where the user can be told; it must not also mean the log
    // stops carrying what the CLI reports.
    std::fwrite(buf, 1, got, stderr);
    std::lock_guard<std::mutex> lock(mutex_);
    stderr_tail_.append(buf, got);
    if (stderr_tail_.size() > kStderrTailBytes)
      stderr_tail_.erase(0, stderr_tail_.size() - kStderrTailBytes);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  err_done_ = true;
  cv_.notify_all();
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
  HANDLE stdin_r = nullptr, stdout_w = nullptr, stderr_w = nullptr;
  if (!CreatePipe(&stdin_r, &stdin_w_, &sa, 0) ||
      !CreatePipe(&stdout_r_, &stdout_w, &sa, 1 << 20) ||
      !CreatePipe(&stderr_r_, &stderr_w, &sa, 1 << 16)) {
    if (error) *error = "CreatePipe failed";
    return false;
  }
  SetHandleInformation(stdin_w_, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stdout_r_, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(stderr_r_, HANDLE_FLAG_INHERIT, 0);

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

  // M26.2, finding 21. `CreateProcessW` writes the command line into the new
  // process's environment block and caps it at 32,767 characters including the
  // terminating NUL -- and almost all of what is on this one is
  // `--system-prompt`, which grows every time a prompt file or a digest does.
  // Nobody was checking. Over the limit, `CreateProcessW` fails with a plain
  // "the parameter is incorrect", which is not a sentence anybody could act on.
  //
  // Measured on 21 Sep 2026 against a freshly seeded prompt tree, with no
  // memories and one action: **20,278 of 32,767 characters**, of which the
  // system prompt is 19,943 bytes. About 12,000 characters of headroom, which
  // sounds comfortable and is not: it is roughly the size of two more prompt
  // files, and `memories.md` alone is allowed 4,000 characters of it. Logged
  // on every start, because the only way this stops being a surprise is if the
  // number is written down every run.
  //
  // The pinned CLI (2.1.278) has `--system-prompt-file <path>`, which would
  // take the prompt off the command line altogether and end the ceiling as a
  // concern. Not used here: it is a change of how the prompt reaches the child
  // and it wants its own milestone, with the file's lifetime, its location
  // under `%APPDATA%` and its removal all decided rather than assumed.
  constexpr std::size_t kCommandLineMax = 32767;
  std::fprintf(stderr, "[claude] command line: %zu of %zu characters (system prompt %zu)\n",
               wcmd.size() + 1, kCommandLineMax, opt_.system_prompt.size());
  // stderr is redirected onto the log file in the windowed app, where it is a
  // block-buffered FILE* and not the unbuffered console stream this would
  // otherwise be. Without this the line is still sitting in the buffer when
  // the run people are reading the log of has finished.
  std::fflush(stderr);
  if (wcmd.size() + 1 > kCommandLineMax) {
    if (error)
      *error = "the command line for claude is " + std::to_string(wcmd.size() + 1) +
               " characters, over Windows' limit of " + std::to_string(kCommandLineMax) +
               "; the system prompt (" + std::to_string(opt_.system_prompt.size()) +
               " bytes) is what has grown";
    return false;
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = stdin_r;
  si.hStdOutput = stdout_w;
  // M26.1, finding 17. Was `GetStdHandle(STD_ERROR_HANDLE)`, which is whatever
  // the windowed app happened to have -- nothing at all, before `main.cpp`
  // started redirecting the streams, and the log file afterwards. Either way
  // the text went somewhere this class could not read, so the one question the
  // user actually asks of a dead child ("why?") had no answer here. A pipe of
  // our own, drained by `stderr_loop` and echoed on to the app's stderr so the
  // log keeps it too.
  si.hStdError = stderr_w;
  PROCESS_INFORMATION pi{};
  std::wstring wcwd = widen(opt_.cwd);
  BOOL okay = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
  CloseHandle(stdin_r);
  CloseHandle(stdout_w);
  CloseHandle(stderr_w);  // the child holds the only write end now, so EOF means "it is gone"
  if (!okay) {
    // M26.2, finding 20. This used to append `cmd`, which is the whole command
    // line -- and the whole command line is a 20 KB system prompt. It went
    // into the status line, where it is a wall of text where a reason should
    // be, and into the log, where it is the app's own prompts written out in
    // full every time a path is wrong. The exe and the error code are what
    // identifies the fault; the prompt is in `assets/` for anyone who wants it.
    if (error)
      *error = "CreateProcess failed (" + std::to_string(GetLastError()) + ") for " + opt_.exe;
    return false;
  }
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  reader_ = std::thread([this] { reader_loop(); });
  err_reader_ = std::thread([this] { stderr_loop(); });

  // In stream-json input mode the CLI sends its init event only once the first
  // message arrives, so just make sure the process survived launch.
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return exited_; });
  if (exited_) {
    // M26.1. The stdout pipe breaks first and the stderr thread may still be
    // holding the sentence that explains why, so give it a moment -- the wait
    // is against `err_done_`, which the stderr reader sets when *its* pipe
    // closes, and that has already happened by the time a child has exited.
    cv_.wait_for(lock, std::chrono::milliseconds(250), [&] { return err_done_; });
    const std::string why = exit_detail();
    if (error)
      *error = why.empty() ? "claude exited during startup, saying nothing"
                           : "claude exited during startup: " + why;
    if (!why.empty()) last_error_ = why;
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
  std::unique_lock<std::mutex> lock(mutex_);
  exited_ = true;
  // M26.1. Same reason as in `start()`: stdout is closed and stderr may not be
  // yet, and the half-second of text on stderr is the entire content of the
  // message about to be composed. Bounded, and on a thread whose only
  // remaining job is this.
  cv_.wait_for(lock, std::chrono::milliseconds(250), [&] { return err_done_; });
  if (turn_active_ && !turn_done_) {
    // "claude process exited: " with nothing after the colon was finding 17
    // itself. Every branch here now ends in something a person can act on: the
    // CLI's own last words, or the exit code, or -- when there is genuinely
    // neither -- a sentence that says so rather than trailing off.
    std::string why = exit_detail();
    if (why.empty()) why = last_error_;
    current_.error = why.empty() ? "claude process exited without saying why"
                                 : "claude process exited: " + why;
    last_error_ = current_.error;
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
      // The same dangling colon as finding 17, one branch along: `last_error_`
      // is empty whenever the child died without a `result` event and without
      // a word on stderr.
      std::string why = last_error_.empty() ? exit_detail() : last_error_;
      r.error = killed_ ? "the app stopped this instance"
                        : why.empty() ? "claude process is not running"
                                      : "claude process is not running: " + why;
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
