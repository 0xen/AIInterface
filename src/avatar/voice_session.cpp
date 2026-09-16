#include "voice_session.h"

#include <cstdio>

#include "core/sentence_splitter.h"
#include "core/text_util.h"

namespace aii {

namespace {
constexpr size_t kMaxLines = 40;
constexpr int kMicRate = 16000;
}  // namespace

VoiceSession::VoiceSession(Config cfg) : cfg_(std::move(cfg)) {
  set_status("loading engines...");
  loader_ = std::thread([this] { load(); });
}

VoiceSession::~VoiceSession() {
  cancel_ = true;
  if (speech_) speech_->clear();
  if (loader_.joinable()) loader_.join();
  if (turn_.joinable()) turn_.join();
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

void VoiceSession::load() {
  std::string err;
  LogFn logger = [this](const std::string& s) {
    log(s);
    set_status(s);
  };
  if (!build_llm(cfg_, eng_, logger, &err) || !build_speech(cfg_, eng_, logger, &err)) {
    log(err);
    set_status(err);
    load_failed_ = true;
    return;
  }
  speaker_ = std::make_unique<AudioOut>();
  if (!speaker_->start(eng_.kokoro->sample_rate())) {
    set_status("no playback device");
    load_failed_ = true;
    return;
  }
  mic_ = std::make_unique<MicIn>();
  if (!mic_->open(kMicRate)) {
    set_status("no capture device");
    load_failed_ = true;
    return;
  }
  speech_ = std::make_unique<SpeechQueue>(eng_.kokoro.get(), eng_.voicevox.get(), speaker_.get());
  speech_->set_on_status([this](const std::string& s) { log("[tts] " + s); });
  log("speaker: " + speaker_->device_name());
  log("mic:     " + mic_->device_name());
  set_status("ready. SPACE or Talk to speak.");
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

  if (s == State::Listening) {
    chunk_.clear();
    mic_->drain(chunk_);
    if (!chunk_.empty()) {
      eng_.stt->feed(chunk_.data(), (int)chunk_.size(), kMicRate);
      std::string p = eng_.stt->partial();
      std::lock_guard<std::mutex> l(mutex_);
      partial_ = p;
    }
  } else if (s == State::Speaking) {
    if (speech_->idle()) {
      set_state(State::Idle);
      set_status("ready.");
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
  {
    std::lock_guard<std::mutex> l(mutex_);
    partial_.clear();
    status_ = "listening... SPACE or Talk to send";
    state_ = State::Listening;
  }
}

void VoiceSession::toggle_talk() {
  State s;
  {
    std::lock_guard<std::mutex> l(mutex_);
    s = state_;
  }
  switch (s) {
    case State::Loading:
    case State::Failed:
      return;
    case State::Idle:
      begin_listening();
      return;
    case State::Thinking:
    case State::Speaking:
      // Barge-in: drop the reply in flight and listen.
      cancel_ = true;
      speech_->clear();
      begin_listening();
      return;
    case State::Listening: {
      mic_->stop();
      chunk_.clear();
      mic_->drain(chunk_);
      if (!chunk_.empty()) eng_.stt->feed(chunk_.data(), (int)chunk_.size(), kMicRate);
      std::string text = trim(eng_.stt->finish());
      {
        std::lock_guard<std::mutex> l(mutex_);
        partial_.clear();
      }
      if (text.empty()) {
        set_state(State::Idle);
        set_status("heard nothing. ready.");
        return;
      }
      start_turn(text);
      return;
    }
  }
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
  if (s == State::Thinking || s == State::Speaking) {
    cancel_ = true;
    speech_->clear();
    set_state(State::Idle);
    set_status("paused. ready.");
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
  std::string usage = eng_.llm->status_line();
  std::lock_guard<std::mutex> l(mutex_);
  if (!usage.empty()) usage_ = usage;
  if (!r.ok) {
    status_ = "error: " + r.error;
    state_ = State::Idle;
    return;
  }
  state_ = State::Speaking;  // update() returns to Idle once the audio drains
  status_ = "speaking...";
}

VoiceSession::Snapshot VoiceSession::snapshot() const {
  std::lock_guard<std::mutex> l(mutex_);
  Snapshot s;
  s.state = state_;
  s.status = status_;
  s.usage = usage_;
  s.partial = partial_;
  s.lines = lines_;
  return s;
}

}  // namespace aii
