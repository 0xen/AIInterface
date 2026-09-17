#include "core/worker_pool.h"

#include <algorithm>

#include "core/app_strings.h"
#include "core/button_registry.h"
#include "core/config.h"
#include "core/text_util.h"

namespace aii {
namespace {

constexpr size_t kRecent = 6;

// A worker's system prompt: it does real work with tools, and its final
// message is read aloud, so it must end with a one-sentence summary.
//
// The closing sentence obeys the same rules the user set for the voice in
// `assets/pre-prompt.md` -- short, and technical jargon (URLs, file paths,
// process names) kept to a minimum -- because it is heard by voice exactly as
// a reply is, and the pre-prompt reaches the conversational instance only.
// Not naming itself is part of that: the C++ below no longer speaks the
// worker's name, and a worker that opened with "counter here" would put it
// straight back in the model's own words.
const char* kWorkerPrompt =
    "You are a background worker instance driven by a voice assistant. Do the task you are given "
    "using your tools. You cannot ask questions: no one will answer, so make reasonable choices and "
    "state them. Your final message is read aloud, so end with ONE short plain sentence saying what "
    "you did and whether it worked. No markdown, no lists, no code in the final message. "
    "That sentence is heard, not read, so keep it short and keep technical jargon to a minimum: no "
    "file paths, no URLs, no process or command names, no error codes -- say what happened in "
    "ordinary words. Do not name or refer to yourself, and do not mention being a worker, an agent "
    "or a sub-agent: the listener is told which task this is by other means. Say what was done, not "
    "who did it.";

std::string first_sentence(const std::string& text, size_t limit = 220) {
  std::string t = trim(text);
  if (t.empty()) return t;
  // Prefer the last paragraph: the worker prompt asks for a closing summary.
  const size_t para = t.rfind("\n\n");
  if (para != std::string::npos && t.size() - para > 12) t = trim(t.substr(para + 2));
  if (t.size() > limit) {
    // Cut on a word boundary where there is one. Japanese has no spaces, so
    // that fallback is the *normal* path for half this app's output, and a bare
    // byte cut there lands mid-sequence -- hence clip_utf8 rather than substr.
    const size_t cut = t.rfind(' ', limit);
    t = (cut == std::string::npos ? clip_utf8(t, limit) : t.substr(0, cut)) + "...";
  }
  return t;
}

}  // namespace

const char* worker_state_name(WorkerPool::State s) {
  switch (s) {
    case WorkerPool::State::Starting: return "starting";
    case WorkerPool::State::Working: return "working";
    case WorkerPool::State::Done: return "done";
    case WorkerPool::State::Paused: return "paused";
    case WorkerPool::State::Failed: return "failed";
  }
  return "?";
}

WorkerPool::WorkerPool(std::string claude_exe, bool bypass_permissions)
    : exe_(std::move(claude_exe)), bypass_(bypass_permissions) {}

WorkerPool::~WorkerPool() {
  pause_all();
  std::vector<std::unique_ptr<Worker>> taken;
  {
    std::lock_guard<std::mutex> l(mutex_);
    taken.swap(workers_);
  }
  for (auto& w : taken) {
    if (w->thread.joinable()) w->thread.join();
  }
}

bool WorkerPool::spawn(const std::string& name, const std::string& cwd, const std::string& task,
                       std::string* error) {
  {
    std::lock_guard<std::mutex> l(mutex_);
    for (const auto& w : workers_) {
      if (w->name == name && w->state != State::Done && w->state != State::Failed) {
        if (error) *error = "a worker named " + name + " is already running";
        return false;
      }
    }
  }
  auto w = std::make_unique<Worker>();
  w->name = name;
  w->task = task;
  w->cwd = cwd;

  ClaudeCodeClient::Options o;
  o.exe = exe_;
  o.system_prompt = kWorkerPrompt;
  o.effort = "medium";
  o.tools = true;
  o.cwd = cwd;
  o.bypass_permissions = bypass_;  // nothing here can answer a permission prompt
  // Deliberately NOT suppress_cli_context (M3.5): a worker is a coding agent
  // running inside a repo the user pointed it at, so that repo's `CLAUDE.md`,
  // their skills, MCP servers, hooks, plugins and custom agents are all
  // capability it should have. Only the conversational instance is stripped.
  w->client = std::make_unique<ClaudeCodeClient>(o);
  if (!w->client->start(error)) return false;

  Worker* raw = w.get();
  raw->client->set_on_activity([this, raw](const std::string& what) {
    std::lock_guard<std::mutex> l(mutex_);
    raw->activity = what;
    ++raw->tool_calls;
    raw->recent.push_back(what);
    if (raw->recent.size() > kRecent) raw->recent.pop_front();
  });
  {
    std::lock_guard<std::mutex> l(mutex_);
    workers_.push_back(std::move(w));
  }
  raw->thread = std::thread([this, raw] { run(raw); });
  return true;
}

void WorkerPool::run(Worker* w) {
  {
    std::lock_guard<std::mutex> l(mutex_);
    w->state = State::Working;
    w->activity = "thinking";
  }
  ChatResult r = w->client->turn(w->task, nullptr, &w->cancel);
  State state;
  // `shown` keeps the worker's name, `spoken` never does -- see ReportFn.
  std::string shown, spoken;
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (w->cancel) {
      w->state = State::Paused;
      w->activity = "paused";
      shown = app_text(Msg::WorkerPausedShown, w->name);
      spoken = app_text(Msg::PausedSpoken);
    } else if (!r.ok) {
      w->state = State::Failed;
      w->activity = "failed";
      w->result = r.error;
      std::string why = first_sentence(r.error, 120);
      // A CLI error can end in a dangling "reason:" with nothing after it
      // ("claude process exited: "), which is read out as a colon-shaped
      // pause. Spoken, a full stop is the honest punctuation.
      while (!why.empty() && (why.back() == ':' || why.back() == ' ')) why.pop_back();
      if (!why.empty() && why.back() != '.' && why.back() != '!' && why.back() != '?') why += '.';
      shown = app_text(Msg::WorkerFailedShown, w->name, why);
      // The failure path loses the name too, not only the success path. A rule
      // with an exception is one the user hears break; the panel row and the
      // transcript still say which worker failed, and a failure is acted on by
      // looking, not by listening.
      //
      // And it loses the CLI's wording as well. `why` above is the raw reason
      // and it stays where it can be read: on the transcript line above, in
      // `w->result` and so in every `snapshot()` the panel is drawn from, and
      // in the log. What is *said* is one of the Fail* sentences, mapped from
      // the whole error rather than from the clipped first sentence -- the
      // clip is for the eye, and classifying what is left of a truncated
      // string would lose the very word that identifies it.
      spoken = app_text(Msg::TaskFailedSpoken, app_text(failure_reason(r.error)));
    } else {
      w->state = State::Done;
      w->activity = "done";
      w->result = trim(r.text);
      const std::string what = first_sentence(r.text);
      shown = app_text(Msg::WorkerFinishedShown, w->name, what);
      spoken = app_text(Msg::FinishedSpoken, what);
    }
    state = w->state;
  }
  w->finished = true;
  if (report_) report_(w->name, state, shown, spoken);
}

bool WorkerPool::pause(const std::string& name) {
  std::lock_guard<std::mutex> l(mutex_);
  for (auto& w : workers_) {
    if (w->name == name && (w->state == State::Working || w->state == State::Starting)) {
      w->cancel = true;
      w->activity = "pausing...";
      return true;
    }
  }
  return false;
}

void WorkerPool::pause_all() {
  std::lock_guard<std::mutex> l(mutex_);
  for (auto& w : workers_) {
    if (w->state == State::Working || w->state == State::Starting) {
      w->cancel = true;
      w->activity = "pausing...";
    }
  }
}

bool WorkerPool::stop(const std::string& name) {
  std::unique_ptr<Worker> taken;
  {
    std::lock_guard<std::mutex> l(mutex_);
    auto it = std::find_if(workers_.begin(), workers_.end(),
                           [&](const std::unique_ptr<Worker>& w) { return w->name == name; });
    if (it == workers_.end()) return false;
    (*it)->cancel = true;
    taken = std::move(*it);
    workers_.erase(it);
  }
  if (taken->thread.joinable()) taken->thread.join();
  return true;
}

void WorkerPool::update() {
  std::lock_guard<std::mutex> l(mutex_);
  for (auto& w : workers_) {
    if (w->finished && w->thread.joinable()) w->thread.join();
  }
}

std::vector<WorkerPool::Snapshot> WorkerPool::snapshot() const {
  std::lock_guard<std::mutex> l(mutex_);
  std::vector<Snapshot> out;
  out.reserve(workers_.size());
  for (const auto& w : workers_) {
    Snapshot s;
    s.name = w->name;
    s.task = w->task;
    s.cwd = w->cwd;
    s.state = w->state;
    s.activity = w->activity;
    s.result = w->result;
    s.tool_calls = w->tool_calls;
    s.recent.assign(w->recent.begin(), w->recent.end());
    out.push_back(std::move(s));
  }
  return out;
}

size_t WorkerPool::running() const {
  std::lock_guard<std::mutex> l(mutex_);
  size_t n = 0;
  for (const auto& w : workers_) {
    if (w->state == State::Working || w->state == State::Starting) ++n;
  }
  return n;
}

// ---------------------------------------------------------------- commands

std::vector<Command> parse_commands(const std::string& text) {
  std::vector<Command> out;
  size_t pos = 0;
  for (;;) {
    const size_t open = text.find("```aii", pos);
    if (open == std::string::npos) break;
    size_t body = text.find('\n', open);
    if (body == std::string::npos) break;
    ++body;
    const size_t close = text.find("```", body);
    const std::string block = text.substr(body, close == std::string::npos ? std::string::npos : close - body);
    pos = close == std::string::npos ? text.size() : close + 3;

    size_t line_start = 0;
    while (line_start < block.size()) {
      size_t nl = block.find('\n', line_start);
      if (nl == std::string::npos) nl = block.size();
      const std::string line = trim(block.substr(line_start, nl - line_start));
      line_start = nl + 1;
      if (line.empty()) continue;

      Command c;
      const size_t sp = line.find(' ');
      c.verb = sp == std::string::npos ? line : line.substr(0, sp);
      std::string rest = sp == std::string::npos ? "" : trim(line.substr(sp + 1));
      // key=value pairs. A value may be "quoted" when it contains spaces —
      // needed once `button` arrived, whose label and tooltip are prose and
      // whose path may sit under Program Files. Unquoted, `task=` and `path=`
      // still run to the end of the line: both are usually last, both usually
      // contain spaces, and requiring quotes there would break every reply the
      // assistant has been taught to write.
      while (!rest.empty()) {
        const size_t eq = rest.find('=');
        if (eq == std::string::npos) break;
        const std::string key = trim(rest.substr(0, eq));
        std::string value;
        if (eq + 1 < rest.size() && rest[eq + 1] == '"') {
          const size_t quote = rest.find('"', eq + 2);
          value = rest.substr(eq + 2, quote == std::string::npos ? std::string::npos : quote - eq - 2);
          rest = quote == std::string::npos ? "" : trim(rest.substr(quote + 1));
        } else if (key == "task" || key == "path" || key == "say") {
          value = trim(rest.substr(eq + 1));
          rest.clear();
        } else {
          const size_t end = rest.find(' ', eq + 1);
          value = rest.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1);
          rest = end == std::string::npos ? "" : trim(rest.substr(end + 1));
        }
        if (key == "name") c.name = value;
        else if (key == "cwd") c.cwd = value;
        else if (key == "task") c.task = value;
        else if (key == "id") c.id = value;
        else if (key == "label") c.label = value;
        else if (key == "tip") c.tip = value;
        else if (key == "path") c.path = value;
        else if (key == "in") c.in = value;
        else if (key == "say") c.say = value;
        else if (key == "grade") c.grade = value;
      }
      if (c.verb.empty()) continue;
      // App-owned verbs are applied here and dropped: see the header. A
      // refusal is not spoken and does not reach the transcript — the button
      // is the agent's own housekeeping, and reading "I could not add that
      // button" aloud would spend a spoken sentence on something the user
      // never asked for. It is recorded for the log instead.
      if (c.verb == "button") {
        ButtonRegistry::instance().add_path_button(c.id, c.label, c.tip, c.path, nullptr);
        continue;
      }
      // The `name` guard is a worker-verb guard: `spawn`, `pause` and `stop`
      // all address a worker by name and a nameless one is unrunnable. A bare
      // timer has nothing to name, so `schedule` is let through and validated
      // by its own handler, which can tell the user *why* it was refused.
      // M2b.5 adds `cancel`, which addresses a schedule by id and has no name
      // either. Both are validated by their own handler, which is what can tell
      // the user why it was refused; `spawn`, `pause` and `stop` are still
      // dropped without one, because a nameless worker verb is unrunnable.
      if (c.verb == "schedule" || c.verb == "cancel" || !c.name.empty())
        out.push_back(std::move(c));
    }
  }
  return out;
}

std::string strip_aii_blocks(const std::string& text) {
  std::string out;
  size_t pos = 0;
  for (;;) {
    const size_t open = text.find("```aii", pos);
    if (open == std::string::npos) {
      out += text.substr(pos);
      break;
    }
    out += text.substr(pos, open - pos);
    const size_t close = text.find("```", open + 6);
    if (close == std::string::npos) break;  // unterminated: drop the rest
    pos = close + 3;
  }
  return trim(out);
}

}  // namespace aii
