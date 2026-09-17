#include "voice_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "rend/core/log.h"

#include "core/language.h"
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
// The one stage M8.3 can skip, by index into the table above.
constexpr size_t kJapaneseVoiceStage = 3;

// The language selection, packed into one word (see langs_bits_).
constexpr unsigned kBitEn = 1u;
constexpr unsigned kBitJa = 2u;
unsigned pack_langs(LanguageSelection s) {
  return (s.english ? kBitEn : 0u) | (s.japanese ? kBitJa : 0u);
}
LanguageSelection unpack_langs(unsigned bits) {
  LanguageSelection s{(bits & kBitEn) != 0, (bits & kBitJa) != 0};
  // The invariant, once more at the boundary. Nothing should ever store a
  // selection with neither bit set, and if something did, every consumer below
  // would have to have an opinion about it.
  if (!s.english && !s.japanese) return LanguageSelection{};
  return s;
}

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
  langs_bits_ = pack_langs(cfg_.langs);
  // The bring-up plan for *this* run, decided before the thread starts. With
  // Japanese off the Japanese voice is not in it at all, so its weight never
  // enters the progress denominator and the bar reaches 100% at the right
  // moment rather than jumping the last fifth.
  for (size_t i = 0; i < kLoadStageCount; ++i) {
    if (i == kJapaneseVoiceStage && !cfg_.langs.japanese) continue;
    stage_ids_.push_back(i);
  }
  // Ready from the start when it was built at startup; otherwise Absent, which
  // is a deliberate skip rather than a failure.
  if (cfg_.langs.japanese) ja_started_ = true;
  set_status("loading engines...");
  loader_ = std::thread([this] { load(); });
}

VoiceSession::~VoiceSession() {
  cancel_ = true;
  if (speech_) speech_->clear();
  if (workers_) workers_->pause_all();
  if (loader_.joinable()) loader_.join();
  // Before speech_ and the engines go away: it is the thread that writes
  // eng_.voicevox. There is nothing to cancel it with — VOICEVOX's load is a
  // single blocking call — so quitting during the one second it takes waits
  // for it, the same as quitting during the startup load does.
  if (ja_loader_.joinable()) ja_loader_.join();
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

void VoiceSession::set_state_locked(State s) {
  if (s == state_) return;
  // Every transition is traced because the sequence is the only honest record
  // of this machine. The Speaking->Idle->Listening flutter that used to open
  // the microphone mid-reply held Idle for four milliseconds — too short to
  // read off the panel, and what the avatar's minimum dwell was masking — so
  // it was found here, and it is proved gone here.
  rend::log::trace("state: {} -> {}", state_name(state_), state_name(s));
  state_ = s;
}

void VoiceSession::set_state(State s) {
  std::lock_guard<std::mutex> l(mutex_);
  set_state_locked(s);
}

void VoiceSession::set_status(const std::string& s) {
  std::lock_guard<std::mutex> l(mutex_);
  status_ = s;
}

float VoiceSession::begin_load_stage(size_t index) {
  // `index` walks stage_ids_, not the table: with Japanese off the plan is one
  // stage shorter and both the numerator and the denominator have to agree
  // about that.
  float done = 0.0f;
  float total = 0.0f;
  for (size_t i = 0; i < stage_ids_.size(); ++i) {
    const float w = kLoadStages[stage_ids_[i]].weight;
    if (i < index) done += w;
    total += w;
  }
  std::lock_guard<std::mutex> l(mutex_);
  const bool past_end = index >= stage_ids_.size();
  load_progress_ = (past_end || total <= 0.0f) ? 1.0f : done / total;
  load_stage_ = past_end ? "" : kLoadStages[stage_ids_[index]].name;
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
  size_t step = 0;  // position in stage_ids_, which may be shorter than the table
  auto enter = [&](size_t table_index) {
    // A stage that is not in this run's plan is not entered at all, and does
    // not disturb the timing of the one before it. Called with the table index
    // so the call sites below read as the fixed sequence they are, rather than
    // as arithmetic over a list that varies.
    if (step >= stage_ids_.size() || stage_ids_[step] != table_index) return;
    const auto now = std::chrono::steady_clock::now();
    if (step > 0) {
      // Per-stage timings stay in at trace level: they are what the weights
      // above were measured from, and they are how to re-measure them.
      rend::log::trace("load: {} took {:.2f} s", kLoadStages[stage_ids_[step - 1]].name,
                       std::chrono::duration<double>(now - stage_began).count());
    }
    stage_began = now;
    const float progress = begin_load_stage(step);
    rend::log::trace("load: {:.0f}%  {}", progress * 100.0f,
                     step < stage_ids_.size() ? kLoadStages[stage_ids_[step]].name : "done");
    ++step;
  };
  auto finish_stages = [&] {
    step = stage_ids_.size();
    begin_load_stage(step);
    rend::log::trace("load: 100%  done");
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
  // M8.3: skipped entirely when Japanese is off, which is the second of
  // startup the user asked to save. `enter` knows it is not in the plan, so
  // the loading bar never reserves a slice for it.
  if (cfg_.langs.japanese) {
    enter(3);
    if (!build_voicevox(cfg_, eng_, logger, &err)) return fail(err);
  }

  enter(4);
  speaker_ = std::make_unique<AudioOut>();
  if (!speaker_->start(eng_.kokoro->sample_rate())) return fail("no playback device");
  enter(5);
  mic_ = std::make_unique<MicIn>();
  if (!mic_->open(kMicRate)) return fail("no capture device");
  finish_stages();
  // `eng_.voicevox` is null when Japanese is off; SpeechQueue takes that and
  // set_japanese() is how the on-demand load hands it one later.
  speech_ = std::make_unique<SpeechQueue>(eng_.kokoro.get(), eng_.voicevox.get(), speaker_.get());
  speech_->set_on_status([this](const std::string& s) { log("[tts] " + s); });
  workers_ = std::make_unique<WorkerPool>(cfg_.claude_exe, cfg_.worker_bypass);
  workers_->set_on_report([this](const std::string&, WorkerPool::State, const std::string& shown,
                                 const std::string& spoken) { announce(shown, spoken); });
  // M3.3. The same store the system prompt was composed from, read a second
  // time for its lazy half. Cheap (a few small files), and it keeps the
  // injector honest about *when* it sees the store: the global prompts left
  // this process at launch, and these have not been sent at all yet.
  if (std::string perr; !prompts_.load(&perr) && !perr.empty()) log("[prompts] " + perr);
  injector_.reset(prompts_);
  log("speaker: " + speaker_->device_name());
  log("mic:     " + mic_->device_name());
  set_status("ready. click the mic or press SPACE to speak.");
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

  // M8.3. Cheap (a string compare) and idempotent, and here rather than only
  // on the settings edge because the effective selection also changes when the
  // on-demand Japanese voice finishes loading on its own thread — which must
  // not reach into the recogniser itself while the frame loop is feeding it.
  apply_stt_language();
  // And the same for the voice, for a reason that is only a latent hole today:
  // set_languages() is level-based, so a switch-on that arrived while the
  // engines were still coming up would be recorded and never acted on. The
  // settings surface is withheld during loading so it cannot happen from the
  // panel, but a level that is only ever applied on its own edge is the shape
  // of a bug waiting for a second way in. Both calls are no-ops in the
  // ordinary case.
  ensure_japanese_voice();

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
    // A reply is over when the turn thread has stopped producing text *and*
    // the queue has run dry. `speech_->idle()` on its own is not that test.
    // The state turns to Speaking on the first text delta, but the splitter
    // holds the first chunk until a comma or twelve words and synthesis then
    // takes a few hundred milliseconds, so for that whole gap there is nothing
    // queued, nothing synthesising and nothing playing — which idle() has to
    // report as true, because from the queue's side it *is* true. The same gap
    // reopens every time the speaker catches up with a reply that is still
    // arriving. Read alone it meant "finished": the state dropped to Idle and
    // the branch below reopened the microphone into the middle of the reply.
    // On this machine the speakers face the C920, so the app then transcribed
    // Claude's own voice back in as the user — and begin_listening()'s
    // speech_->clear() threw away the rest of the reply on the way.
    //
    // `turn_running_` is the missing half and only the session can know it:
    // the queue cannot tell "nothing to do" from "more sentences are still
    // coming". Keeping the test here rather than teaching the queue to stay
    // busy from mark_new_reply() also cannot hang — a reply that speaks
    // nothing at all (an error, or a reply that was only a worker command
    // block) still ends its thread, where a queue-side promise would wait for
    // audio that is never coming.
    if (!turn_running_ && speech_->idle()) {
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
  // The only place the capture device is ever started, so this line answers
  // "did the microphone open while Claude was talking?" on its own. It is what
  // the speakers-into-the-C920 defect is checked against; keep it.
  rend::log::trace("mic: open (latch={})", mic_open_);
  // Start the silence clock now: without this the first frame would look
  // like a pause that had already run long enough to send.
  listen_began_ = std::chrono::steady_clock::now();
  last_voice_ = listen_began_;
  noise_floor_ = 0.0f;
  {
    std::lock_guard<std::mutex> l(mutex_);
    partial_.clear();
    status_ = "listening... pause sends; click the mic again to stop";
    set_state_locked(State::Listening);
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
  set_state_locked(State::Idle);
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
  // The gesture's verdict, on demand. Behind an environment variable because
  // it is a line per press and is only ever wanted by the harness that drives
  // the gesture matrix — which has no other way to see which of the two
  // readings a press got, since both of them end with the microphone shut.
  if (std::getenv("AII_TALK_DEBUG")) {
    log(std::string("[talk] verdict=") + (!hold_ ? "latch-release" : (held || !over_button) ? "dictate" : "latch-on") +
        " over_button=" + (over_button ? "1" : "0") + " held=" + (held ? "1" : "0") +
        " hold=" + (hold_ ? "1" : "0") + " mic_open=" + (mic_open_ ? "1" : "0"));
  }
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
  set_status("listening... pause sends; click the mic again to stop");
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

// ------------------------------------------------------------------ M8.3
LanguageSelection VoiceSession::requested_langs() const {
  return unpack_langs(langs_bits_.load(std::memory_order_acquire));
}

LanguageSelection VoiceSession::effective_langs() const {
  LanguageSelection sel = requested_langs();
  // The one place the checkboxes and the behaviour are allowed to differ: see
  // the header. Japanese counts as on only once there is a voice that can
  // actually speak it, so the ~1.1 s on-demand load is a second in which
  // nothing routes Japanese to the English voice, rather than a second in
  // which the app half-works.
  if (sel.japanese && !(speech_ && speech_->has_japanese())) sel.japanese = false;
  // English is never unavailable — Kokoro loads whatever the setting says,
  // because it is also the splitter's voice for every Latin run — so this can
  // only fire for a Japanese-only session in the load window, and it is here
  // so that "no language at all" is unreachable rather than merely unlikely.
  if (!sel.english && !sel.japanese) sel.english = true;
  return sel;
}

void VoiceSession::apply_stt_language() {
  if (!eng_.stt) return;
  // The hand override still wins; otherwise both -> auto, one -> that one.
  // `auto` is the measured failure mode, not the safe default: it deletes a
  // short Japanese insert inside an English sentence outright. Pinning is what
  // this setting buys.
  const std::string lang =
      cfg_.stt_lang.empty() ? std::string(stt_language_for(effective_langs())) : cfg_.stt_lang;
  eng_.stt->set_language(lang);
}

void VoiceSession::ensure_japanese_voice() {
  if (!loaded_) return;                       // the startup load owns the engines until it is done
  if (!requested_langs().japanese) return;
  if (ja_started_.exchange(true)) return;     // built at startup, or already loading, or done
  ja_loading_ = true;
  log("[lang] loading the Japanese voice...");
  ja_loader_ = std::thread([this] {
    const auto began = std::chrono::steady_clock::now();
    std::string err;
    LogFn logger = [this](const std::string& s) { log(s); };
    if (build_voicevox(cfg_, eng_, logger, &err)) {
      // Published only after the engine reports ok(), so nothing can be routed
      // to a half-built voice. Deliberately silent: the settings surface says
      // it arrived, and speaking a line here would be an announcement the mute
      // button never asked for.
      // The recogniser is deliberately *not* touched from this thread — it is
      // fed from the frame loop and is not thread-safe. update() re-applies
      // the language every frame, and that call is a no-op unless it changed,
      // so the pin follows the new voice within one frame.
      speech_->set_japanese(eng_.voicevox.get());
      rend::log::info("japanese voice loaded on demand in {:.2f} s",
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
    } else {
      ja_error_ = err;
      ja_failed_ = true;
      log("[lang] " + err);
    }
    ja_loading_ = false;
  });
}

void VoiceSession::set_languages(LanguageSelection sel) {
  if (!sel.english && !sel.japanese) sel = LanguageSelection{};
  const unsigned bits = pack_langs(sel);
  if (langs_bits_.exchange(bits) == bits) return;  // a level, not an edge
  log(std::string("[lang] ") + language_spec(sel));
  // Japanese first: it is what effective_langs() may still be waiting on, and
  // starting the load before the recogniser is told anything means the two are
  // never out of step in the wrong direction.
  ensure_japanese_voice();
  apply_stt_language();
}

void VoiceSession::set_muted(bool muted) {
  if (muted_.exchange(muted) == muted) return;  // a level, not an edge
  // Going on cuts what is already playing. The old silence() stopped here and
  // that was the whole of its shortcoming: the splitter goes on feeding the
  // queue from the turn thread, so the next sentence of the same reply started
  // speaking a moment later. Everything downstream of here is suppressed at
  // the enqueue instead — see run_turn() and flush_announcements() — so the
  // queue stays empty for as long as this is on, and the transcript, which is
  // written on a different path entirely, is untouched.
  if (muted && speech_) speech_->clear();
  log(muted ? "[mute] on (voice suppressed; text unaffected)" : "[mute] off");
}

void VoiceSession::stop() {
  State s;
  {
    std::lock_guard<std::mutex> l(mutex_);
    s = state_;
  }
  // Stop is a full stop: drop the mic latch too, or update() would reopen
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
    set_status(paused_workers ? "stopped (" + std::to_string(paused_workers) + " worker(s) paused). ready."
                              : "stopped. ready.");
  } else if (s == State::Listening) {
    mic_->stop();
    std::lock_guard<std::mutex> l(mutex_);
    partial_.clear();
    set_state_locked(State::Idle);
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
    set_state_locked(State::Thinking);
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
  // The one place a reply becomes sound, and therefore the only place mute can
  // honestly be applied. Clearing the queue alone (what silence() used to do)
  // stops the sentence that is playing and nothing else: this callback is on
  // the turn thread and keeps handing the queue the next sentence, so the
  // reply carries on speaking a beat later. The text side of this same loop —
  // `lines_.back().text += delta` above — is untouched, which is what makes
  // mute voice-only.
  SentenceSplitter splitter([this](const std::string& s) {
    if (muted_) {
      // Traced rather than silent: "the app said nothing" and "the app was
      // muted" look identical from outside, and this is the line that tells
      // them apart in a log.
      rend::log::trace("mute: dropped {} chars of speech", s.size());
      return;
    }
    speech_->enqueue(s);
  }, cfg_.early_words);
  bool first = true;
  // M3.3. `sent` is what Claude receives; `text` stays what the user said.
  // The transcript, the avatar and the mute path all work off the latter, so a
  // `<context>` block is never shown and never spoken — it is machine traffic
  // in the same sense the `aii` block is, just travelling the other way.
  // M8.3 rides on the same rail as M3.3 and is deliberately a separate,
  // stateless mechanism: the injector's loaded set exists so a prompt is sent
  // once and never again, and a setting the user can flip back needs the exact
  // opposite. This emits the block on every turn while a language is off, and
  // nothing at all when both are on — so the default configuration sends
  // byte-for-byte what it sent before this existed.
  const std::string injected = injector_.decorate(text);
  if (injected.size() != text.size()) {
    std::string names;
    for (const std::string& id : injector_.loaded()) names += (names.empty() ? "" : ", ") + id;
    log("[prompts] injected context; loaded this session: " + names);
  }
  const LanguageSelection eff = effective_langs();
  const std::string sent = decorate_language(injected, eff);
  if (!eff.both()) log("[lang] turn sent with the " + language_spec(eff) + "-only instruction");
  ChatResult r = eng_.llm->turn(sent, [&](const std::string& delta) {
    {
      std::lock_guard<std::mutex> l(mutex_);
      if (!lines_.empty() && !lines_.back().user) lines_.back().text += delta;
      if (first) {
        first = false;
        status_ = "speaking...";
        set_state_locked(State::Speaking);
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
      set_state_locked(State::Idle);
      ++turn_failed_seq_;
      return;
    }
    set_state_locked(State::Speaking);  // update() returns to Idle once the audio drains
    status_ = "speaking...";
  }
  // Worker commands ride in a fenced block, which is shown but never spoken.
  run_commands(r.text);
}

void VoiceSession::announce(const std::string& text) { announce(text, text); }

void VoiceSession::announce(const std::string& shown, const std::string& spoken) {
  if (shown.empty()) return;
  // Shown at once, but not spoken here. A worker can report at any moment,
  // including while the microphone is open, and speaking straight from this
  // call would put the app's own voice into the room with the mic still
  // listening — it would transcribe itself back in as the user. It is also
  // called from two threads (the worker poll on the frame loop, and the turn
  // thread via run_commands), neither of which may drive the microphone.
  // update() picks these up and takes the floor properly.
  std::lock_guard<std::mutex> l(mutex_);
  lines_.push_back({false, shown});
  if (lines_.size() > kMaxLines) lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - kMaxLines));
  if (!spoken.empty()) pending_announce_.push_back(spoken);
}

bool VoiceSession::flush_announcements() {
  std::vector<std::string> say_now;
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (pending_announce_.empty()) return false;
    // Muted: the report is already in the transcript (announce() put it there
    // the moment it arrived), so there is nothing left to do but drop the
    // spoken copy. Deliberately *not* held back for an unmute — a worker
    // report is about a moment, and a queue of them read out later would be
    // worse than not having heard them. It also must not take the floor:
    // returning true here would shut the microphone and push the state to
    // Speaking for a reply that is never going to make a sound.
    if (muted_) {
      pending_announce_.clear();
      return false;
    }
    say_now.swap(pending_announce_);
    partial_.clear();
    set_state_locked(State::Speaking);
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
  for (const Command& c : parse_commands(reply_text)) {
    // M3.4. Not a worker verb, so it is handled before the `workers_` guard
    // and does not need a pool. It only ever queues: the prompt arrives
    // prepended to the next user turn, because that is the whole point of
    // M3.3 — the system prompt is a launch argument and cannot be appended to
    // without restarting Claude and losing the prompt cache.
    //
    // A name that is not in the store is refused and logged, not announced.
    // The model reaching for a prompt that does not exist is its own
    // housekeeping; reading "I could not load that" aloud would spend a spoken
    // sentence on something the user never asked for, exactly as `button` does.
    if (c.verb == "load") {
      if (injector_.request(c.name)) log("[prompts] queued " + c.name + " for the next turn");
      else log("[prompts] refused load name=" + c.name + " (no such prompt in the store)");
      continue;
    }
    if (!workers_) continue;
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
  // M8.3, read before the lock for the same reason: none of it is behind
  // mutex_, and the settings surface needs all three every frame.
  const LanguageSelection eff = effective_langs();
  VoiceLoad ja = VoiceLoad::Absent;
  if (speech_ && speech_->has_japanese()) ja = VoiceLoad::Ready;
  else if (ja_failed_) ja = VoiceLoad::Failed;
  else if (ja_loading_) ja = VoiceLoad::Loading;
  std::lock_guard<std::mutex> l(mutex_);
  Snapshot s;
  s.effective_langs = eff;
  s.japanese_voice = ja;
  if (ja == VoiceLoad::Failed) s.japanese_voice_error = ja_error_;
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
