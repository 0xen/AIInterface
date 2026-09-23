#include "core/worker_pool.h"

#include <algorithm>
#include <cstdio>

#include "core/app_strings.h"
#include "core/button_registry.h"
#include "core/config.h"
#include "core/cwd_policy.h"
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

void WorkerPool::request_cancel(Worker* w) {
  if (w->cancel) return;  // the clock belongs to the first interrupt, not the third
  w->cancel = true;
  w->cancel_at = std::chrono::steady_clock::now();
  w->activity = "pausing...";
}

void WorkerPool::wait_then_kill(Worker* w) {
  // Nothing is ever killed that was not first asked politely. It is what makes
  // `stop_reason == "interrupted"` true of everything this ends, and so what
  // makes a killed worker report as stopped rather than as failed; it is also
  // the guard for the one frame between a worker setting its own state and
  // setting `finished`, where a destructor could otherwise terminate a child
  // whose turn had already come back.
  if (!w->cancel) return;
  const auto deadline = w->cancel_at + kInterruptGrace;
  while (!w->finished && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (w->finished || w->killed) return;
  w->killed = true;
  // No logger here, and deliberately no callback for it either: stderr is
  // routed into the app's log file (see main.cpp, routeDiagnostics), which is
  // where `claude_code_client.cpp` already writes the one other line this
  // layer has to say for itself.
  std::fprintf(stderr, "[worker] %s did not answer the interrupt within %lld ms; ending its process\n",
               w->name.c_str(), (long long)kInterruptGrace.count());
  if (w->client) w->client->kill();
}

WorkerPool::~WorkerPool() {
  pause_all();
  std::vector<std::unique_ptr<Worker>> taken;
  {
    std::lock_guard<std::mutex> l(mutex_);
    taken.swap(workers_);
  }
  // M17.3. One grace period for all of them, not one each: they were all
  // interrupted at the same moment above, and quitting is the one path where
  // the cost of waiting is paid by somebody watching a window that will not
  // close. Then whatever is left is ended outright.
  //
  // The `taken` vector keeps every Worker alive while its thread runs, so the
  // activity callbacks still have something to write into.
  for (auto& w : taken) wait_then_kill(w.get());
  for (auto& w : taken) {
    if (w->thread.joinable()) w->thread.join();
  }
}

bool WorkerPool::spawn(const std::string& name, const std::string& cwd, const std::string& task,
                       std::string* error, const std::string& model_override) {
  std::unique_ptr<Worker> stale;
  {
    std::lock_guard<std::mutex> l(mutex_);
    for (const auto& w : workers_) {
      if (w->name == name && w->state != State::Done && w->state != State::Failed) {
        if (error) *error = "a worker named " + name + " is already running";
        return false;
      }
    }
    // A finished row under this name is taken out before the new one is
    // added, rather than left beside it. Its report was already delivered
    // (that is what made it Done or Failed), so nothing is lost -- and two
    // snapshot rows sharing one name is exactly what fed AvatarController's
    // old name-keyed map two different states for one entry and made it flip
    // every frame (23 Sep 2026: a finished `bunpro` and a fresh one looping
    // `child_merge` for minutes). `Snapshot::id` is the other half of that
    // fix; this half is what stops the ambiguity ever reaching a snapshot.
    auto it = std::find_if(workers_.begin(), workers_.end(), [&](const std::unique_ptr<Worker>& w) {
      return w->name == name && (w->state == State::Done || w->state == State::Failed);
    });
    if (it != workers_.end()) {
      stale = std::move(*it);
      workers_.erase(it);
    }
  }
  // Joined outside the lock, matching stop()'s reasoning: a finished worker's
  // thread is on its way out through the same mutex, so joining it while
  // holding that mutex is a wait for a thread waiting on the lock. In
  // practice this join is instant -- update() already joins finished threads
  // every frame -- but the ordering has to hold regardless.
  if (stale && stale->thread.joinable()) stale->thread.join();

  auto w = std::make_unique<Worker>();
  w->name = name;
  w->task = task;
  // The last stop on the way to a bypassPermissions process: whatever the
  // callers did or did not decide, a worker never starts in "wherever the
  // child happens to inherit". An empty cwd becomes the app's own folder here,
  // explicitly, so the panel row and the log name a real directory instead of
  // a blank -- see core/cwd_policy.h for why this is the app's decision and
  // not the model's.
  w->cwd = cwd.empty() ? app_dir() : cwd;

  ClaudeCodeClient::Options o;
  o.exe = exe_;
  o.system_prompt = kWorkerPrompt;
  o.effort = "medium";
  o.tools = "default";  // every built-in tool; the conversational instance gets two
  o.cwd = w->cwd;
  o.bypass_permissions = bypass_;  // nothing here can answer a permission prompt
  // M31. `model_override` wins when this one spawn named a model of its own;
  // otherwise the pool's own setting, which defaults to `opus` and is read
  // fresh here so a change reaches the next worker started without a restart.
  o.model = model_override.empty() ? model_ : model_override;
  o.chrome = chrome_;
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
    w->id = next_id_++;
    workers_.push_back(std::move(w));
  }
  raw->thread = std::thread([this, raw] { run(raw, false); });
  return true;
}

namespace {
// M32. "note waiting: <first 60 chars>" -- clipped on a UTF-8 boundary the
// same way first_sentence() clips a reply, because a note is exactly as
// likely to be Japanese as anything else typed at this app.
std::string clip_note(const std::string& text, size_t limit) {
  const std::string t = trim(text);
  return t.size() <= limit ? t : clip_utf8(t, limit) + "...";
}
}  // namespace

void WorkerPool::run(Worker* w, bool resume) {
  if (!resume) {
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
      // **`r.ok` is asked first** (M17.3, review finding 23). The cancel flag used
      // to be, and it is the wrong question to ask first because it is raised
      // from another thread at a moment nobody chose: a worker that finished its
      // turn properly a millisecond before somebody pressed Stop was filed as
      // Paused, and the panel showed an agent still holding work it had already
      // handed back. A turn that came back `ok` came back; whatever happened
      // afterwards happened to nothing.
      //
      // `r.stop_reason == "interrupted"` is not used for this, although turn()
      // sets it on exactly this path: it is set *because* the cancel flag was
      // seen, so it is the same question in different words and it answers 23 no
      // better. What it does is arrive with the same meaning after a kill, which
      // is why the order below is also what makes M17.3's escalation report as
      // stopped: `kill()` finishes the turn with `ok == false`, the cancel flag
      // is up because this app only ever kills what it has already interrupted,
      // and so a worker the app ended reads as Paused and not as Failed.
      if (r.ok) {
        w->state = State::Done;
        w->activity = "done";
        w->result = trim(r.text);
        const std::string what = first_sentence(r.text);
        shown = app_text(Msg::WorkerFinishedShown, w->name, what);
        spoken = app_text(Msg::FinishedSpoken, what);
      } else if (w->cancel) {
        w->state = State::Paused;
        w->activity = "paused";
        shown = app_text(Msg::WorkerPausedShown, w->name);
        spoken = app_text(Msg::PausedSpoken);
      } else {
        w->state = State::Failed;
        w->activity = "failed";
        w->result = r.error;
        std::string why = first_sentence(r.error, 120);
        // A CLI error can end in a dangling "reason:" with nothing after it
        // ("claude process exited: "), which is read out as a colon-shaped
        // pause. Spoken, a full stop is the honest punctuation.
        //
        // M26.1 fixed the source of that particular string -- the client now
        // puts the child's exit code and its last words after the colon, or says
        // it has neither -- so this no longer has a known caller. It stays
        // because it is three lines and because the error text here comes from
        // another program: a `result` event carrying "Error:" and nothing else
        // would land in exactly the same shape.
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
      }
      state = w->state;
    }
    if (report_) report_(w->name, state, shown, spoken);
    // Paused or Failed ends the thread here, exactly as before M32: a note
    // waiting behind a turn that did not come back Done has nothing left to
    // reach. Only success falls through to the notes loop below.
    if (state != State::Done) {
      w->finished = true;
      return;
    }
  }

  // M32. The notes loop: a worker that came back Done, or was just restarted
  // on a note (`resume == true`), works through whatever is waiting for it
  // one turn at a time. `State` stays Done between notes -- the panel and the
  // model both see a finished worker the whole time, which is honest: nothing
  // is running until a note actually starts a turn.
  for (;;) {
    std::string note;
    {
      std::lock_guard<std::mutex> l(mutex_);
      if (w->notes.empty()) {
        w->finished = true;
        return;
      }
      note = w->notes.front();
      w->notes.pop_front();
      w->state = State::Working;
      w->activity = "reading a note";
    }
    ChatResult r = w->client->turn(note, nullptr, &w->cancel);
    State state;
    std::string shown, spoken;
    {
      std::lock_guard<std::mutex> l(mutex_);
      if (r.ok) {
        w->state = State::Done;
        w->activity = "done";
        w->result = trim(r.text);
        const std::string what = first_sentence(r.text);
        shown = app_text(Msg::WorkerRepliedShown, w->name, what);
        spoken = app_text(Msg::RepliedSpoken, what);
      } else if (w->cancel) {
        // A cancelled note-turn follows the same Paused branch a cancelled
        // task does: it is the same fact in the same words, and the panel and
        // the transcript already know how to show it.
        w->state = State::Paused;
        w->activity = "paused";
        shown = app_text(Msg::WorkerPausedShown, w->name);
        spoken = app_text(Msg::PausedSpoken);
      } else {
        w->state = State::Failed;
        w->activity = "failed";
        w->result = r.error;
        std::string why = first_sentence(r.error, 120);
        while (!why.empty() && (why.back() == ':' || why.back() == ' ')) why.pop_back();
        if (!why.empty() && why.back() != '.' && why.back() != '!' && why.back() != '?') why += '.';
        shown = app_text(Msg::WorkerFailedShown, w->name, why);
        spoken = app_text(Msg::TaskFailedSpoken, app_text(failure_reason(r.error)));
      }
      state = w->state;
    }
    if (report_) report_(w->name, state, shown, spoken);
    if (state != State::Done) {
      w->finished = true;
      return;
    }
    // Done: loop back and see whether another note arrived while this one was
    // being answered.
  }
}

bool WorkerPool::pause(const std::string& name) {
  std::lock_guard<std::mutex> l(mutex_);
  for (auto& w : workers_) {
    if (w->name == name && (w->state == State::Working || w->state == State::Starting)) {
      request_cancel(w.get());
      return true;
    }
  }
  return false;
}

void WorkerPool::pause_all() {
  std::lock_guard<std::mutex> l(mutex_);
  for (auto& w : workers_) {
    if (w->state == State::Working || w->state == State::Starting) request_cancel(w.get());
  }
}

bool WorkerPool::stop(const std::string& name) {
  // M17.3, review finding 2. Three steps, and the middle one is the fix: raise
  // the interrupt, wait a bounded time for it to be answered and end the child
  // if it is not, and only then take the entry out and join.
  //
  // The wait cannot be done while holding `mutex_`. The worker thread takes it
  // on its way out of run(), so a join -- or a sleep -- under the lock is a
  // wait for a thread that is waiting for the lock.
  Worker* target = nullptr;
  {
    std::lock_guard<std::mutex> l(mutex_);
    auto it = std::find_if(workers_.begin(), workers_.end(),
                           [&](const std::unique_ptr<Worker>& w) { return w->name == name; });
    if (it == workers_.end()) return false;
    request_cancel(it->get());
    target = it->get();
  }
  wait_then_kill(target);

  std::unique_ptr<Worker> taken;
  {
    std::lock_guard<std::mutex> l(mutex_);
    // By pointer, not by name: the entry cannot have moved -- every caller of
    // this and of update() is the frame loop -- but finding it again by the
    // thing that identifies it costs nothing and does not depend on that.
    auto it = std::find_if(workers_.begin(), workers_.end(),
                           [&](const std::unique_ptr<Worker>& w) { return w.get() == target; });
    if (it == workers_.end()) return true;
    taken = std::move(*it);
    workers_.erase(it);
  }
  if (taken->thread.joinable()) taken->thread.join();
  return true;
}

bool WorkerPool::tell(const std::string& name, const std::string& text, std::string* error) {
  // Only the Done branch falls out of the lock block below: the Working /
  // Starting case and the not-found case both return from inside it.
  Worker* target = nullptr;
  {
    std::lock_guard<std::mutex> l(mutex_);
    Worker* live = nullptr;
    Worker* done = nullptr;
    Worker* other = nullptr;
    for (auto& w : workers_) {
      if (w->name != name) continue;
      if (w->state == State::Working || w->state == State::Starting) { live = w.get(); break; }
      if (w->state == State::Done) done = w.get();
      else other = w.get();
    }
    if (live) {
      live->notes.push_back(text);
      live->activity = "note waiting: " + clip_note(text, 60);
      return true;
    }
    if (!done) {
      if (error) {
        if (other && other->state == State::Paused)
          *error = name + " is paused; a paused worker cannot take a note";
        else if (other && other->state == State::Failed)
          *error = name + " failed";
        else
          *error = "no worker named " + name;
      }
      return false;
    }
    // `done`: the child is still alive with the conversation in it, so the
    // note becomes its next turn. The note is queued here, under the lock,
    // before anything below can look at it -- run(w, /*resume=*/true) starts
    // straight into the notes loop and expects to find it.
    done->notes.push_back(text);
    target = done;
  }

  // The old thread already returned (that is what made the worker Done);
  // update() joins finished threads every frame, but this may run on the
  // same frame that state was set, so the join is done here too rather than
  // assumed to have already happened.
  if (target->thread.joinable()) target->thread.join();
  {
    std::lock_guard<std::mutex> l(mutex_);
    target->cancel = false;
    target->killed = false;
    target->finished = false;
    target->state = State::Working;
    target->activity = "reading a note";
  }
  target->thread = std::thread([this, target] { run(target, /*resume=*/true); });
  return true;
}

void WorkerPool::update() {
  // M17.3. The escalation for pause() and pause_all(), which return straight
  // away and so have nowhere of their own to put a wait. Collected under the
  // lock and acted on outside it: `kill()` takes the client's own lock, and the
  // client's activity callback takes *this* lock while holding that one, so
  // doing it the other way round is the two locks in both orders.
  std::vector<Worker*> overdue;
  {
    std::lock_guard<std::mutex> l(mutex_);
    const auto now = std::chrono::steady_clock::now();
    for (auto& w : workers_) {
      if (w->finished && w->thread.joinable()) w->thread.join();
      if (!w->finished && w->cancel && !w->killed && now - w->cancel_at >= kInterruptGrace)
        overdue.push_back(w.get());
    }
  }
  for (Worker* w : overdue) wait_then_kill(w);
}

std::vector<WorkerPool::Snapshot> WorkerPool::snapshot() const {
  std::lock_guard<std::mutex> l(mutex_);
  std::vector<Snapshot> out;
  out.reserve(workers_.size());
  for (const auto& w : workers_) {
    Snapshot s;
    s.id = w->id;
    s.name = w->name;
    s.task = w->task;
    s.cwd = w->cwd;
    s.state = w->state;
    s.activity = w->activity;
    s.result = w->result;
    s.tool_calls = w->tool_calls;
    s.recent.assign(w->recent.begin(), w->recent.end());
    s.notes_waiting = static_cast<int>(w->notes.size());
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
//
// The parser itself is in `core/aii_block.cpp`. What is left here is the one
// thing it could not take with it: applying the `button` verb. ButtonRegistry
// opens folders through <shellapi.h>, so a file that called it could not be
// linked into a test without <windows.h> and the rest of the app behind it —
// which is the whole reason the parser moved. It takes a callback instead, and
// this is the callback.

std::vector<Command> parse_commands(const std::string& text, std::vector<std::string>* problems) {
  return parse_commands(text, problems, [](const Command& c) {
    ButtonRegistry::instance().add_path_button(c.id, c.label, c.tip, c.path, nullptr);
  });
}

std::vector<Command> parse_commands(const std::string& text) {
  return parse_commands(text, nullptr);
}

}  // namespace aii
