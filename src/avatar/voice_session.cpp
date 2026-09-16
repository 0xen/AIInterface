#include "voice_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "rend/core/log.h"

#include "core/sentence_splitter.h"
#include "core/text_util.h"

namespace aii {

namespace {
constexpr size_t kMaxLines = 40;
constexpr int kMicRate = 16000;

// Engine bring-up, in the order load() performs it, with the share of the wait
// each stage costs. The shares are measured, not assumed even: on this machine
// a cold start is dominated by the two neural voices, and a bar that gave every
// stage an equal slice would race to 50% and then sit still for four seconds.
//
// Measured 16 Sep 2026 on the user's machine (Ryzen 9 7900, warm file cache,
// default Claude Code backend), averaged over two runs; the numbers are
// milliseconds, used as relative weights. Re-measure if the models or the
// backend change — the display is only as honest as these.
struct LoadStage {
  const char* name;  // shown on the loading screen
  float weight;      // relative cost; the table is normalised where it is used
};
constexpr LoadStage kLoadStages[] = {
    {"Claude", 520.0f},              // the CLI child process handshake
    {"Speech recognition", 1295.0f}, // sherpa-onnx + Nemotron, the longest single wait
    {"English voice", 995.0f},       // Kokoro
    {"Japanese voice", 1100.0f},     // VOICEVOX
    {"Speaker", 25.0f},              // device opens, over almost before they are shown
    {"Microphone", 25.0f},
};
constexpr size_t kLoadStageCount = sizeof(kLoadStages) / sizeof(kLoadStages[0]);

// Noise-gate shaping. The floor learns the room, and the gate sits a few times
// above it; anything louder counts as the user still talking.
//
// The floor is only ever adapted while the level is already below the gate —
// that is, during what is currently believed to be silence. Letting it track
// loud frames too would be self-defeating: over a few seconds of continuous
// speech the floor would climb until the gate passed the speaker's own voice,
// and the utterance would be cut off exactly as if they had stopped.
constexpr float kFloorRate = 0.05f;
constexpr float kGateOverFloor = 3.0f;
constexpr float kGateAbsMin = 0.004f;
// A noisy room is measured over the first moments after the mic opens, when
// the user has not had time to start. The cap keeps a speaker who talks over
// the calibration from teaching the gate to ignore their own voice.
constexpr float kCalibrateSec = 0.3f;
constexpr float kFloorMax = 0.02f;
// How long the room has to be quiet before a worker report may take the floor
// over an open microphone.
constexpr float kAnnounceGapSec = 0.4f;

float rms(const std::vector<float>& s) {
  if (s.empty()) return 0.0f;
  double sum = 0.0;
  for (float v : s) sum += double(v) * double(v);
  return static_cast<float>(std::sqrt(sum / double(s.size())));
}
}  // namespace

VoiceSession::VoiceSession(Config cfg) : cfg_(std::move(cfg)) {
  set_status("loading engines...");
  loader_ = std::thread([this] { load(); });
}

VoiceSession::~VoiceSession() {
  cancel_ = true;
  if (speech_) speech_->clear();
  if (workers_) workers_->pause_all();
  if (loader_.joinable()) loader_.join();
  if (turn_.joinable()) turn_.join();
  workers_.reset();  // interrupts and joins every worker
  if (mic_) mic_->close();
  speech_.reset();  // joins its worker before the engines go away
}

const char* VoiceSession::state_name(State s) {
  switch (s) {
    case State::Loading: return "loading";
    case State::Idle: return "idle";
    case State::Listening: return "listening";
    case State::Thinking: return "thinking";
    case State::Speaking: return "speaking";
    case State::Failed: return "failed";
  }
  return "?";
}

void VoiceSession::log(const std::string& s) {
  std::printf("  %s\n", s.c_str());
  std::fflush(stdout);
}

void VoiceSession::set_state(State s) {
  std::lock_guard<std::mutex> l(mutex_);
  state_ = s;
}

void VoiceSession::set_status(const std::string& s) {
  std::lock_guard<std::mutex> l(mutex_);
  status_ = s;
}

float VoiceSession::begin_load_stage(size_t index) {
  float done = 0.0f;
  float total = 0.0f;
  for (size_t i = 0; i < kLoadStageCount; ++i) {
    if (i < index) done += kLoadStages[i].weight;
    total += kLoadStages[i].weight;
  }
  std::lock_guard<std::mutex> l(mutex_);
  load_progress_ = (index >= kLoadStageCount || total <= 0.0f) ? 1.0f : done / total;
  load_stage_ = index < kLoadStageCount ? kLoadStages[index].name : "";
  return load_progress_;
}

void VoiceSession::load() {
  const auto load_began = std::chrono::steady_clock::now();
  auto stage_began = load_began;
  std::string err;
  LogFn logger = [this](const std::string& s) {
    log(s);
    set_status(s);
  };
  // Stage bookkeeping is a closure rather than a loop over the table because
  // the steps have no common signature: two of them are device opens, not
  // engine builders, and each has its own failure message.
  auto enter = [&](size_t index) {
    const auto now = std::chrono::steady_clock::now();
    if (index > 0) {
      // Per-stage timings stay in at trace level: they are what the weights
      // above were measured from, and they are how to re-measure them.
      rend::log::trace("load: {} took {:.2f} s", kLoadStages[index - 1].name,
                       std::chrono::duration<double>(now - stage_began).count());
    }
    stage_began = now;
    const float progress = begin_load_stage(index);
    rend::log::trace("load: {:.0f}%  {}", progress * 100.0f,
                     index < kLoadStageCount ? kLoadStages[index].name : "done");
  };
  auto fail = [&](const std::string& message) {
    log(message);
    set_status(message);
    load_failed_ = true;
  };

  enter(0);
  if (!build_llm(cfg_, eng_, logger, &err)) return fail(err);
  enter(1);
  if (!build_stt(cfg_, eng_, logger, &err)) return fail(err);
  enter(2);
  if (!build_kokoro(cfg_, eng_, logger, &err)) return fail(err);
  enter(3);
  if (!build_voicevox(cfg_, eng_, logger, &err)) return fail(err);

  enter(4);
  speaker_ = std::make_unique<AudioOut>();
  if (!speaker_->start(eng_.kokoro->sample_rate())) return fail("no playback device");
  enter(5);
  mic_ = std::make_unique<MicIn>();
  if (!mic_->open(kMicRate)) return fail("no capture device");
  enter(kLoadStageCount);
  speech_ = std::make_unique<SpeechQueue>(eng_.kokoro.get(), eng_.voicevox.get(), speaker_.get());
  speech_->set_on_status([this](const std::string& s) { log("[tts] " + s); });
  workers_ = std::make_unique<WorkerPool>(cfg_.claude_exe, cfg_.worker_bypass);
  workers_->set_on_report([this](const std::string&, WorkerPool::State, const std::string& summary) {
    announce(summary);
  });
  log("speaker: " + speaker_->device_name());
  log("mic:     " + mic_->device_name());
  set_status("ready. click Talk or press SPACE to speak.");
  rend::log::info("engines up in {:.1f} s",
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - load_began).count());
  loaded_ = true;
}

void VoiceSession::update() {
  State s;
  {
    std::lock_guard<std::mutex> l(mutex_);
    s = state_;
  }
  if (s == State::Loading) {
    if (load_failed_) set_state(State::Failed);
    else if (loaded_) set_state(State::Idle);
    return;
  }
  if (s == State::Failed) return;

  // Reap a finished turn thread.
  if (turn_.joinable() && !turn_running_) turn_.join();
  if (workers_) workers_->update();

  if (s == State::Listening) {
    chunk_.clear();
    mic_->drain(chunk_);
    if (!chunk_.empty()) {
      // Track how long the microphone has actually been quiet. The decoder's
      // own endpoint counts frames it decoded as blank, which it also does
      // mid-sentence when speech is slow, quiet or hesitant — trusting it
      // alone is what cuts the user off in the middle of a thought.
      const float level = rms(chunk_);
      const auto now = std::chrono::steady_clock::now();
      const float listening_for = std::chrono::duration<float>(now - listen_began_).count();
      float gate = std::max(noise_floor_ * kGateOverFloor, kGateAbsMin);
      if (listening_for < kCalibrateSec) {
        noise_floor_ = std::min(std::max(noise_floor_, level), kFloorMax);
        gate = std::max(noise_floor_ * kGateOverFloor, kGateAbsMin);
      } else if (level < gate) {
        noise_floor_ += (level - noise_floor_) * kFloorRate;
      }
      if (level > gate || listening_for < kCalibrateSec) last_voice_ = now;

      eng_.stt->feed(chunk_.data(), (int)chunk_.size(), kMicRate);
      std::string p = eng_.stt->partial();
      // Published against the gate rather than raw (see Snapshot::mic_level):
      // the gate already encodes what this room's silence sounds like, so the
      // avatar leans to the *voice* and not to the air conditioning.
      const float loud = std::min(1.0f, level / std::max(gate * 6.0f, 1e-6f));
      {
        std::lock_guard<std::mutex> l(mutex_);
        partial_ = p;
        mic_level_ = loud;
      }
      // A pause long enough to end the utterance sends it, so one click
      // carries a back-and-forth conversation rather than a single turn.
      // Three things all have to hold: there is something to send (silence
      // before any speech is just waiting), the decoder sees a boundary, and
      // the room has genuinely been quiet for the whole pause.
      const auto quiet_for = std::chrono::duration<float>(now - last_voice_).count();
      if (mic_open_ && !trim(p).empty() && quiet_for >= cfg_.endpoint_silence &&
          eng_.stt->is_endpoint()) {
        end_listening_and_send();
      } else if (!hold_ && trim(p).empty() && quiet_for >= kAnnounceGapSec) {
        // A worker reported while the mic was open. Wait for a gap rather
        // than cutting in: nothing has been said this utterance and the room
        // is quiet, so taking the floor now interrupts no one. Never during a
        // hold: the user has a button down, and taking the microphone out from
        // under them would end the dictation they are in the middle of. The
        // report waits the second or two the gesture lasts.
        flush_announcements();
      }
    }
  } else if (s == State::Speaking) {
    if (speech_->idle()) {
      set_state(State::Idle);
      set_status("ready.");
    }
  } else if (s == State::Idle) {
    // Anything a worker reported goes out before the mic reopens; otherwise
    // begin_listening() would put us back to listening with the report still
    // queued, and it would be spoken into an open microphone.
    if (!flush_announcements() && mic_open_) {
      // Still unmuted after a reply finished: reopen the mic for the next
      // turn. It stays shut while Claude speaks, so the speakers are never
      // transcribed back in as the user.
      begin_listening();
    }
  }
}

void VoiceSession::begin_listening() {
  speech_->clear();
  eng_.stt->begin();
  mic_->discard();
  if (!mic_->start()) {
    set_status("mic failed to start");
    return;
  }
  // Start the silence clock now: without this the first frame would look
  // like a pause that had already run long enough to send.
  listen_began_ = std::chrono::steady_clock::now();
  last_voice_ = listen_began_;
  noise_floor_ = 0.0f;
  {
    std::lock_guard<std::mutex> l(mutex_);
    partial_.clear();
    status_ = "listening... pause sends; click Mute to stop";
    state_ = State::Listening;
  }
}

std::string VoiceSession::finish_utterance() {
  mic_->stop();
  chunk_.clear();
  mic_->drain(chunk_);
  if (!chunk_.empty()) eng_.stt->feed(chunk_.data(), (int)chunk_.size(), kMicRate);
  return trim(eng_.stt->finish());
}

void VoiceSession::end_listening_and_send() {
  std::string text = finish_utterance();
  {
    std::lock_guard<std::mutex> l(mutex_);
    partial_.clear();
  }
  if (text.empty()) {
    set_state(State::Idle);
    set_status(mic_open_ ? "listening..." : "heard nothing. ready.");
    return;
  }
  start_turn(text);
}

void VoiceSession::end_listening_unsent() {
  // The same finalise the sending path uses — the decoder has to be flushed
  // either way, and a hold that ended on a half-decoded partial would leave
  // the user editing words the recogniser had already changed its mind about.
  // The text is published as `partial_` with the sequence bumped: the panel
  // writes it into the message field, and nothing here starts a turn.
  const std::string text = finish_utterance();
  std::lock_guard<std::mutex> l(mutex_);
  partial_ = text;
  ++dictated_seq_;
  state_ = State::Idle;
  status_ = text.empty() ? "heard nothing. ready."
                         : "in the message box. edit it, then press Enter.";
}

void VoiceSession::toggle_mic() { set_mic_open(!mic_open_); }

void VoiceSession::talk_pressed() {
  if (mic_open_) return;  // the latch is already on; the release mutes it
  State s;
  {
    std::lock_guard<std::mutex> l(mutex_);
    s = state_;
  }
  if (s == State::Loading || s == State::Failed) return;
  // Barge-in on the press, not on the release: the same rule as unmuting
  // mid-answer, and waiting for the release would mean talking over the reply
  // for as long as the gesture lasted before it was cut off.
  if (s == State::Thinking || s == State::Speaking) {
    cancel_ = true;
    speech_->clear();
  }
  if (s != State::Listening) begin_listening();
  hold_ = true;
  // begin_listening()'s line describes the latch, which this is not yet.
  set_status("listening...");
}

void VoiceSession::talk_released(bool over_button, bool held) {
  if (!hold_) {
    // The latch owned this press, or something took the microphone mid-gesture.
    // Over the button it means what it has always meant: mute, and send what
    // was captured. Released off it, the press is abandoned.
    if (over_button && mic_open_) toggle_mic();
    return;
  }
  hold_ = false;
  State s;
  {
    std::lock_guard<std::mutex> l(mutex_);
    s = state_;
  }
  if (s != State::Listening) return;
  if (held || !over_button) {
    // A hold, or a press dragged off the button and abandoned. Either way the
    // words belong in the field rather than in a turn.
    end_listening_unsent();
    return;
  }
  // Short and on target: a click. The microphone is already open, so this only
  // latches it — which is what makes the click path identical to the one that
  // predates the gesture, including the slow click that spent 300 ms deciding.
  set_mic_open(true);
  set_status("listening... pause sends; click Mute to stop");
}

void VoiceSession::set_mic_open(bool open) {
  if (open == mic_open_) return;  // a level, not an edge: nothing to do
  mic_open_ = open;

  State s;
  {
    std::lock_guard<std::mutex> l(mutex_);
    s = state_;
  }
  if (s == State::Loading || s == State::Failed) {
    mic_open_ = false;
    return;
  }

  if (open) {
    // The latch outranks a gesture in flight: with it on the utterance sends
    // itself on a pause, so there is nothing left for a release to finalise.
    hold_ = false;
    // Switched on. Barge-in if a reply is in flight: unmuting mid-answer is
    // how the user interrupts it.
    if (s == State::Thinking || s == State::Speaking) {
      cancel_ = true;
      speech_->clear();
    }
    if (s != State::Listening) begin_listening();
    return;
  }
  // Muted. Anything said but not yet sent goes now rather than being lost.
  if (s == State::Listening) end_listening_and_send();
}

void VoiceSession::say(const std::string& text) {
  State s;
  {
    std::lock_guard<std::mutex> l(mutex_);
    s = state_;
  }
  if (s == State::Loading || s == State::Failed || s == State::Listening) return;
  if (s != State::Idle) {
    cancel_ = true;
    speech_->clear();
  }
  start_turn(trim(text));
}

void VoiceSession::silence() {
  if (speech_) speech_->clear();
}

void VoiceSession::pause() {
  State s;
  {
    std::lock_guard<std::mutex> l(mutex_);
    s = state_;
  }
  // Pause is a full stop: drop the mic latch too, or update() would reopen
  // the mic on the very next frame. A Talk press still held goes with it —
  // its release must not then finalise an utterance this just cancelled.
  mic_open_ = false;
  hold_ = false;
  const size_t paused_workers = workers_ ? workers_->running() : 0;
  if (workers_) workers_->pause_all();
  if (s == State::Thinking || s == State::Speaking) {
    cancel_ = true;
    speech_->clear();
    set_state(State::Idle);
    set_status(paused_workers ? "paused (" + std::to_string(paused_workers) + " worker(s)). ready."
                              : "paused. ready.");
  } else if (s == State::Listening) {
    mic_->stop();
    std::lock_guard<std::mutex> l(mutex_);
    partial_.clear();
    state_ = State::Idle;
    status_ = "cancelled. ready.";
  }
}

bool VoiceSession::quitting_ok() const { return !turn_running_; }

void VoiceSession::start_turn(std::string text) {
  if (text.empty()) return;
  // Wait for a cancelled turn to unwind before reusing the thread.
  cancel_ = true;
  if (turn_.joinable()) turn_.join();
  cancel_ = false;
  {
    std::lock_guard<std::mutex> l(mutex_);
    lines_.push_back({true, text});
    lines_.push_back({false, ""});
    if (lines_.size() > kMaxLines) lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - kMaxLines));
    state_ = State::Thinking;
    status_ = "thinking...";
  }
  const unsigned gen = ++turn_generation_;
  turn_running_ = true;
  turn_ = std::thread([this, text = std::move(text), gen] {
    run_turn(text);
    (void)gen;
    turn_running_ = false;
  });
}

void VoiceSession::run_turn(std::string text) {
  speech_->mark_new_reply();
  SentenceSplitter splitter([this](const std::string& s) { speech_->enqueue(s); }, cfg_.early_words);
  bool first = true;
  ChatResult r = eng_.llm->turn(text, [&](const std::string& delta) {
    {
      std::lock_guard<std::mutex> l(mutex_);
      if (!lines_.empty() && !lines_.back().user) lines_.back().text += delta;
      if (first) {
        first = false;
        status_ = "speaking...";
        state_ = State::Speaking;
      }
    }
    splitter.feed(delta);
  }, &cancel_);
  if (cancel_) return;  // the caller already moved the state on
  splitter.flush();
  const std::string usage = eng_.llm->status_line();
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (!usage.empty()) usage_ = usage;
    // The command block is machine traffic: the worker strip shows what it did,
    // so keep it out of the transcript.
    if (!lines_.empty() && !lines_.back().user) lines_.back().text = strip_aii_blocks(lines_.back().text);
    if (!r.ok) {
      status_ = "error: " + r.error;
      state_ = State::Idle;
      ++turn_failed_seq_;
      return;
    }
    state_ = State::Speaking;  // update() returns to Idle once the audio drains
    status_ = "speaking...";
  }
  // Worker commands ride in a fenced block, which is shown but never spoken.
  run_commands(r.text);
}

void VoiceSession::announce(const std::string& text) {
  if (text.empty()) return;
  // Shown at once, but not spoken here. A worker can report at any moment,
  // including while the microphone is open, and speaking straight from this
  // call would put the app's own voice into the room with the mic still
  // listening — it would transcribe itself back in as the user. It is also
  // called from two threads (the worker poll on the frame loop, and the turn
  // thread via run_commands), neither of which may drive the microphone.
  // update() picks these up and takes the floor properly.
  std::lock_guard<std::mutex> l(mutex_);
  lines_.push_back({false, text});
  if (lines_.size() > kMaxLines) lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - kMaxLines));
  pending_announce_.push_back(text);
}

bool VoiceSession::flush_announcements() {
  std::vector<std::string> say_now;
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (pending_announce_.empty()) return false;
    say_now.swap(pending_announce_);
    partial_.clear();
    state_ = State::Speaking;
    status_ = "speaking...";
  }
  // Close the microphone for the same reason a reply does: nothing said into
  // it while the app is talking is the user. update() returns to Idle when
  // the audio drains, and reopens it from there if the latch is still on.
  mic_->stop();
  chunk_.clear();
  mic_->drain(chunk_);
  chunk_.clear();
  speech_->mark_new_reply();
  for (const std::string& s : say_now) speech_->enqueue(s);
  return true;
}

void VoiceSession::run_commands(const std::string& reply_text) {
  if (!workers_) return;
  for (const Command& c : parse_commands(reply_text)) {
    std::string err;
    if (c.verb == "spawn") {
      if (workers_->spawn(c.name, c.cwd, c.task, &err)) {
        log("[worker] spawned " + c.name + " in " + (c.cwd.empty()? std::string("(app dir)") : c.cwd));
      } else {
        announce("Could not start worker " + c.name + ". " + err);
      }
    } else if (c.verb == "pause") {
      if (!workers_->pause(c.name)) announce("No running worker called " + c.name + ".");
    } else if (c.verb == "stop") {
      if (!workers_->stop(c.name)) announce("No worker called " + c.name + ".");
    }
  }
}

VoiceSession::Snapshot VoiceSession::snapshot() const {
  // Ask the client before taking our own lock. Its reader thread calls back
  // into us (on_delta) while holding its lock, so locking in the other order
  // here would be the classic AB/BA deadlock.
  UsageStats stats;
  if (loaded_ && eng_.llm) stats = eng_.llm->usage();
  // An atomic on the device callback's side, so it is read here rather than
  // mirrored into a member the frame loop would have to remember to clear.
  const float speaking = (loaded_ && speaker_) ? speaker_->level() : 0.0f;
  std::lock_guard<std::mutex> l(mutex_);
  Snapshot s;
  s.state = state_;
  s.status = status_;
  s.usage = usage_;
  s.usage_stats = stats;
  s.partial = partial_;
  s.dictated_seq = dictated_seq_;
  s.turn_failed_seq = turn_failed_seq_;
  // Gated on the state here rather than zeroed wherever the microphone
  // closes: there are five paths out of Listening and only one of them would
  // have remembered, and a stale level would leave the avatar leaning at
  // something that stopped talking.
  s.mic_level = state_ == State::Listening ? mic_level_ : 0.0f;
  s.speak_level = speaking;
  s.load_progress = load_progress_;
  s.load_stage = load_stage_;
  s.lines = lines_;
  if (workers_) s.workers = workers_->snapshot();
  return s;
}

}  // namespace aii
