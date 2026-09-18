#include "voice_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "rend/core/log.h"

#include "core/app_strings.h"
#include "core/language.h"
#include "core/schedule.h"
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

// M1f.1. The lowest positive auto-listen timeout the mechanism will honour.
// This is *not* the floor a person is allowed to type — that one belongs to
// M1f.2's control, where a value below it can be refused visibly instead of
// being silently rewritten. This one exists so that a timeout small enough to
// be meaningless (or a stray 0.001 from a hand-edited file) cannot make the
// latch close the moment it opens, and so that the harness can measure the
// mechanism at five seconds instead of sitting through a real minute.
constexpr float kListenTimeoutFloorSec = 1.0f;

// M1f.1, and the one number in this feature that was measured rather than
// chosen. How long the noise gate has to stay open, continuously, before the
// auto-listen timeout accepts it as *a voice* and restarts its clock.
//
// The gate itself is unchanged and is still the only detector: this is a
// duration on the same signal, not a second threshold and not a second
// opinion about loudness. It exists because the gate is tuned for a job that
// lasts about a second — deciding whether a pause has ended an utterance —
// where reacting to a single loud frame is exactly right and costs nothing.
// Read over a minute it means something else entirely. Measured on the user's
// machine with a C920 on a quiet desk (six 20 s runs, 17 Sep 2026), the raw
// gate opened on a transient roughly every 1-3 s: a keyboard press, a chair,
// the GPU fan changing note. A timeout restarted by each of those never
// elapses, and the whole feature would have shipped looking correct and
// silently never firing — the most expensive way to be wrong.
//
// 0.25 s is below any syllable the recogniser can decode and far above the
// transients: a click is tens of milliseconds, a vowel is hundreds. A word so
// short it does not clear this bar still decodes, still endpoints and still
// sends, and the turn resets the clock by leaving the Listening state — so no
// utterance the user actually gets a reply to can be missed by this.
constexpr float kVoiceRunSec = 0.25f;

float rms(const std::vector<float>& s) {
  if (s.empty()) return 0.0f;
  double sum = 0.0;
  for (float v : s) sum += double(v) * double(v);
  return static_cast<float>(std::sqrt(sum / double(s.size())));
}
}  // namespace

VoiceSession::VoiceSession(Config cfg) : cfg_(std::move(cfg)) {
  langs_bits_ = pack_langs(cfg_.langs);
  // M1f.1. The configured default; M1f.2's control overwrites it as a level
  // once there is a settings surface to read one from.
  set_listen_timeout(cfg_.listen_timeout);
  timeout_blocked_at_ = std::chrono::steady_clock::now();
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
  // M3.7. A web search adds seconds to a turn in which the microphone is shut
  // and nothing is spoken, and from outside that is indistinguishable from the
  // app having died. The avatar already covers the shape of it — the turn is
  // `Thinking`, so the thought bubble goes up on its own once the delay passes
  // — but the bubble says "still here", not "looking something up", and it is
  // the same bubble a two-second reply raises. This is the line that says
  // which. It is only ever set mid-turn; `run_turn` puts "ready." back at the
  // end, and a turn with no tool call never touches it.
  if (eng_.llm) {
    eng_.llm->set_on_activity([this](const std::string& what) {
      const bool web = what == "WebSearch" || what == "WebFetch";
      rend::log::info("[tool] {}", what);
      set_status(web ? "searching the web..." : "thinking... (" + what + ")");
    });
  }
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
  // The canned lines need the setting from the first second, not from the
  // first turn: a schedule restored before anyone has spoken can fire, and a
  // worker can fail, with the table still on its default.
  set_enabled_languages(effective_langs());
  workers_ = std::make_unique<WorkerPool>(cfg_.claude_exe, cfg_.worker_bypass);
  workers_->set_on_report([this](const std::string& name, WorkerPool::State state,
                                 const std::string& shown, const std::string& spoken) {
    // M2b.4 sent a *scheduled* worker's report to the conversational instance
    // and let a live worker's own sentence be spoken as 517c241 settled it.
    // **M2c.1 makes the live path do the same thing**, because the argument
    // M2b.4 made for the deferred one was never actually about the delay: a
    // worker writes its closing sentence for a reader. It carries paths,
    // identifiers, file names, counts and CLI phrasing, and it is written in
    // English whatever language this conversation is being held in. None of
    // that survives being heard. So the raw sentence keeps the two places a
    // person can *read* it — the transcript line and the panel row, both of
    // which are `shown` and are untouched here — and the voice gets the
    // conversational instance's own phrasing of the same result instead.
    //
    // What does **not** route through the model is written below, at the two
    // early returns: a worker that was stopped on purpose, and a worker that
    // failed. Both already have a plain, short, localised sentence.
    // M2b.5. Stopped on purpose: the user asked for it and has already been
    // told it is stopping. The pool is right to report a cancelled turn as
    // Paused; saying so out loud, or spending a turn on it, would be the app
    // narrating its own bookkeeping.
    if (take_silenced_worker(name)) {
      log("[worker] " + name + " stopped on request; nothing reported");
      // The list entry goes too, or the pending list keeps offering a worker
      // that is not there any more.
      take_scheduled_worker(name, nullptr);
      return;
    }
    // The spoken line no longer carries the client's own words, so the raw
    // report has to be somewhere a person can go and look. It is on the
    // transcript line (`shown`) and on the pool's snapshot; this puts it in
    // the log too, which is the one of the three that outlives the session.
    //
    // M2c.1 widened this from failures to every report, and that is not
    // tidiness. Now that what is *said* is the model's paraphrase, the raw
    // sentence and the spoken one are two different strings for the first
    // time on the success path as well, and a log holding only one of them
    // cannot answer the only question worth asking of this feature — whether
    // the paraphrase was faithful. Next to the `[speak]` line that
    // flush_announcements() and the splitter write, this gives a run both
    // halves in one file. HANDOFF lists "the avatar logs no spoken text" as a
    // harness blindness; this is the other half of the same hole.
    log("[worker] " + shown);
    bool phrased = true;
    const bool scheduled = take_scheduled_worker(name, &phrased);
    // **A worker that did not finish keeps its canned line, scheduled or not.**
    // 5ad6381 already maps every failure reason to one plain sentence in both
    // languages ("It stopped before it finished."), and Paused has had one
    // since 517c241. Those sentences are short, jargon-free and already in the
    // language the app is speaking, so there is nothing for a rephrasing to
    // improve — and there is something to lose: a turn costs seconds and usage
    // and can itself fail, which would mean answering "the work failed" with
    // silence. The failure path is the one place the app cannot afford a
    // second thing that might not come back.
    //
    // M2b.4's Phrased grade is the deliberate exception: a *scheduled* worker
    // that failed is still phrased, because the user asked for it ten minutes
    // ago in a language the canned table may not be set to and is owed the
    // report in the shape the promise was made. That behaviour is unchanged.
    if (state != WorkerPool::State::Done) {
      announce(shown, scheduled && phrased ? std::string() : spoken);
      if (scheduled && phrased) queue_injected_turn(scheduled_report_prompt(state, shown), spoken);
      return;
    }
    if (!scheduled) {
      // Muted: nothing said here is going to be heard, so there is nothing for
      // a rephrasing to be better *at*. flush_announcements() already drops
      // the spoken copy and keeps the transcript line, which is the whole of
      // what a muted report can be; spending a turn to write a sentence the
      // splitter will then throw away is usage bought for nobody. This is the
      // ordinary path, unchanged, and it is deliberately decided here rather
      // than at the flush: the flush point is minutes later and the question
      // "is this worth a model call?" is asked about the moment it arrived.
      if (muted_) {
        announce(shown, spoken);
        return;
      }
      // The live path, M2c.1. The transcript keeps the worker's own words; the
      // voice waits for the instance to say what happened. `spoken` rides
      // along as the fallback, so the worst case is exactly the behaviour this
      // replaced rather than silence.
      announce(shown, "");
      queue_worker_report(shown, spoken);
      return;
    }
    // M2b.5. Every schedule-started worker is now registered, not only the
    // Phrased ones, so that "what is pending?" can see it and a cancel has
    // something to address. A Fixed one (the bus's, M2b.2) still reports the
    // ordinary way once it is off the list.
    if (!phrased) {
      announce(shown, spoken);
      return;
    }
    announce(shown, "");
    // M2c.1: the raw sentence as the fallback here too. Before this the only
    // thing said when a report turn failed was Msg::ScheduledReportLost, which
    // tells the user a promise was broken without telling them the outcome
    // that was sitting right there.
    queue_injected_turn(scheduled_report_prompt(state, shown), spoken);
  });
  // M3.3. The same store the system prompt was composed from, read a second
  // time for its lazy half. Cheap (a few small files), and it keeps the
  // injector honest about *when* it sees the store: the global prompts left
  // this process at launch, and these have not been sent at all yet.
  // The return value and the problems are two different questions, and asking
  // only the first is what let a declared prompt go missing in silence: a body
  // that cannot be read leaves `load()` returning true. Both are logged.
  if (std::string perr; !prompts_.load(&perr))
    log("[prompts] " + (perr.empty() ? std::string("the prompt store could not be read") : perr));
  for (const std::string& p : prompts_.problems()) log("[prompts] " + p);
  injector_.reset(prompts_);
  // M5.2. First publication: the store has just been read, so the inspector
  // can stop saying "still loading" and start naming the global prompts. It
  // happens here rather than in the constructor because "the store has not
  // been read yet" and "the store has been read and is empty" are different
  // things to tell the user, and the flag that distinguishes them is set by
  // this call.
  publish_inventory();
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

  // M1f.1. Every frame this session is doing something other than listening —
  // thinking, speaking, or idle with the microphone shut — is a frame that
  // must not be banked as silence. Stamping here as well as in the Listening
  // branch below means the stamp is unconditional: there is no state, and no
  // early return past this point, in which the clock quietly keeps running.
  if (s != State::Listening) timeout_blocked_at_ = std::chrono::steady_clock::now();

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
      const bool voiced = level > gate || listening_for < kCalibrateSec;
      if (voiced) last_voice_ = now;
      // M1f.1. The same gate, read over a longer window. `last_voice_` above
      // is endpointing's and is deliberately untouched — it has to react to
      // the first loud frame or an utterance would be cut off. The auto-listen
      // timeout wants "somebody is in the room and talking", which a single
      // loud frame is not; see kVoiceRunSec.
      if (voiced) {
        if (voice_run_began_.time_since_epoch().count() == 0) voice_run_began_ = now;
        if (std::chrono::duration<float>(now - voice_run_began_).count() >= kVoiceRunSec)
          last_sustained_voice_ = now;
      } else {
        voice_run_began_ = {};
      }

      eng_.stt->feed(chunk_.data(), (int)chunk_.size(), kMicRate);
      std::string p = eng_.stt->partial();
      // Published against the gate rather than raw (see Snapshot::mic_level):
      // the gate already encodes what this room's silence sounds like, so the
      // avatar leans to the *voice* and not to the air conditioning.
      // M1f.1. The second thing that restarts the auto-listen clock, and the
      // one that makes failure mode "the latch closed mid-sentence"
      // unreachable rather than merely unlikely: the decoder's hypothesis
      // *changed*. New words are being produced, so somebody is talking, and
      // no tuning of the gate can disagree with that — it is the recogniser's
      // own opinion, which is the thing the gate exists to stay in step with.
      //
      // Changed, not merely non-empty, on purpose. A partial that is sitting
      // still is not somebody talking; it is a hypothesis the decoder has
      // stopped adding to. Keying on "non-empty" would mean an utterance the
      // endpointer never accepts (which is exactly what off-prompt speech
      // does here — it decodes as blanks and never endpoints) could hold the
      // latch open forever, which is the bug this whole task exists to close.
      if (p != timeout_partial_) {
        timeout_partial_ = p;
        last_sustained_voice_ = now;
      }
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
        //
        // M2b.4: a scheduled report takes the same gap and under the same
        // three conditions, because the risk it runs is the same one — the
        // app's voice arriving in a room with the microphone listening. The
        // canned line goes first when both are waiting: it is instant and
        // already written, where a turn spends seconds and usage.
        if (!flush_announcements()) flush_injected_turns();
      }
    }
    // M1f.1. The auto-listen timeout, evaluated *outside* the chunk block on
    // purpose: a capture device that stops delivering is the one case where a
    // latch could stay open forever with nothing to notice it, and a check
    // that only runs when audio arrives would be blind to exactly that.
    //
    // Three deliberate choices, all of them visible in this one expression:
    //
    //  1. **No second detector.** The clock is restarted by two signals and
    //     both of them already existed: the session's own RMS gate (the same
    //     threshold on the same samples that end-of-utterance detection uses,
    //     read over a longer window — see kVoiceRunSec) and the recogniser's
    //     hypothesis changing. Nothing here measures audio for itself, and
    //     that is the point: a detector of its own would eventually disagree
    //     with the recogniser about whether the user is talking, and the
    //     moment it did, the latch would close mid-sentence.
    //
    //  2. **Any voice activity resets it, not a completed utterance.** The gate
    //     opening is enough. The user's words were "if it's not heard any
    //     voice", and the difference is a real one: someone thinking aloud in
    //     fragments never finishes an utterance the endpointer will accept, and
    //     an utterance-based timer would cut them off precisely while they were
    //     working out what to say. The cost is that a noisy room holds the latch
    //     open — which is the correct failure, because a noisy room is one that
    //     somebody is in.
    //
    //  3. **The latch only.** `mic_open_ && !hold_` — a Talk press is held by
    //     the user's own finger, and a gesture that expired under it would be
    //     the app deciding it knew better than the hand on the button.
    //
    // `turn_running_` and a busy speaker are folded in through
    // timeout_blocked_at_ rather than merely suppressing the fire, which is
    // the difference between "never counts the app's own time as silence" and
    // "fires the instant a long reply ends". See the member's comment.
    {
      const auto now = std::chrono::steady_clock::now();
      const float timeout = listen_timeout_.load(std::memory_order_relaxed);
      const bool eligible = mic_open_ && !hold_ && timeout > 0.0f && !turn_running_ &&
                            (!speech_ || speech_->idle());
      if (!eligible) {
        timeout_blocked_at_ = now;
      } else {
        const auto since = std::max(last_sustained_voice_, timeout_blocked_at_);
        const float quiet_for = std::chrono::duration<float>(now - since).count();
        if (quiet_for >= timeout) close_latch_after_silence(quiet_for);
      }
      // Behind an environment variable for the same reason [talk] verdict is:
      // a line a second, wanted only by the harness that has to prove the
      // clock is running and to distinguish "it did not fire" from "it was
      // never allowed to run". Once a second, not per frame.
      if (std::getenv("AII_LISTEN_DEBUG")) {
        static auto last_dbg = std::chrono::steady_clock::time_point{};
        if (std::chrono::duration<float>(now - last_dbg).count() >= 1.0f) {
          last_dbg = now;
          rend::log::info(
              "[listen-timeout] eligible={} quiet={:.1f}s raw_quiet={:.1f}s timeout={:.1f}s "
              "latch={} hold={} turn={} speech_idle={} floor={:.4f}",
              eligible ? 1 : 0,
              std::chrono::duration<float>(now - std::max(last_sustained_voice_, timeout_blocked_at_)).count(),
              std::chrono::duration<float>(now - std::max(last_voice_, timeout_blocked_at_)).count(),
              timeout, mic_open_ ? 1 : 0, hold_ ? 1 : 0, turn_running_ ? 1 : 0,
              (!speech_ || speech_->idle()) ? 1 : 0, noise_floor_);
        }
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
    // M2b.4 adds the injected turn to the same chain, and its position in the
    // chain is the design: an announcement first (instant, already written),
    // a scheduled report next, and only then the microphone. Reopening the mic
    // before either would put the app's own voice into an open microphone,
    // which is the defect announce() exists to avoid.
    if (!flush_announcements() && !flush_injected_turns() && mic_open_) {
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
  // M1f.1. Every reopen is a fresh window, which is what makes the stretch in
  // which the microphone was shut for a reply a *reset* rather than a pause.
  last_sustained_voice_ = listen_began_;
  voice_run_began_ = {};
  timeout_partial_.clear();
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

// M1f.1. The latch has heard no voice for the configured time. Close it.
//
// Deliberately *not* set_mic_open(false). That path exists for a user
// deciding to stop, and it sends whatever was captured but not yet sent —
// which is right for a click and wrong for this. By construction the gate has
// heard nothing for the whole window, so anything still sitting in the
// decoder is at least `timeout` seconds old and was never enough to endpoint;
// sending it would spend a turn on a fragment and then speak Claude's answer
// into a room the user has left. The premise of this whole feature is that
// they are not there. So it is dropped, exactly as Stop drops it.
//
// Nothing is spoken here and nothing should be: the reaction the user learns
// this from is M1f.3's, and it is silent by design.
void VoiceSession::close_latch_after_silence(float quiet_for) {
  mic_open_ = false;
  hold_ = false;
  mic_->stop();
  // The one line that proves this fired, and when. Deliberately at info and
  // deliberately carrying the measured seconds and the configured value:
  // "the latch closed" and "the latch closed on time" are different claims,
  // and only the second one is worth anything.
  rend::log::info("[listen-timeout] latch closed after {:.1f} s without voice (timeout {:.1f} s)",
                  quiet_for, listen_timeout_.load(std::memory_order_relaxed));
  std::lock_guard<std::mutex> l(mutex_);
  partial_.clear();
  mic_level_ = 0.0f;
  ++listen_timeout_seq_;
  set_state_locked(State::Idle);
  // M1f.3. The wording is the whole job of this line, and M1f.2's review is
  // why it changed: the placeholder read "stopped listening: nothing heard.
  // ready.", which is one word away from line 640's "heard nothing. ready." --
  // the ordinary empty utterance -- and ends in the same "ready." that the
  // idle line and the cancel line end in. Somebody coming back to the desk
  // would have read it as the app sitting where they left it.
  //
  // Three things are deliberate. **"by itself"** is the fact that has to
  // survive: the question the user has on walking back is *who* stopped it,
  // and the three answers -- I clicked Stop, a reply finished, it gave up on
  // me -- are otherwise indistinguishable in this line. **The number** is the
  // setting quoted back, so the line explains the behaviour rather than merely
  // reporting it; this is the one feature in the panel that can never teach
  // itself through use (M1f.2), and the only moment it is on screen is the
  // moment the user is asking why. **No "ready."** -- the word every idle line
  // here ends with, and the word that made the placeholder look ordinary.
  //
  // English, like every other line in this panel and unlike the spoken table
  // in core/app_strings (7c51862), which excludes the status line by name.
  // Nothing here is spoken, and one Japanese sentence among twenty English
  // ones is not a localised panel, it is an inconsistent one.
  status_ = "stopped listening by itself: no voice for " +
            std::to_string(static_cast<int>(listen_timeout_.load(std::memory_order_relaxed) +
                                            0.5f)) +
            " s.";
}

// M1f.1. A level, the same shape as set_muted()/set_languages().
void VoiceSession::set_listen_timeout(float seconds) {
  // "Never" is a first-class value, not a disabled feature: anything at or
  // below zero stores exactly 0, so every reader has one spelling to test.
  const float v = seconds <= 0.0f ? 0.0f : std::max(seconds, kListenTimeoutFloorSec);
  const float was = listen_timeout_.exchange(v, std::memory_order_relaxed);
  if (was == v) return;  // no-op unless it changed; this is called every frame
  if (v <= 0.0f) log("[listen-timeout] never");
  else log("[listen-timeout] " + std::to_string((int)(v + 0.5f)) + " s");
}

float VoiceSession::listen_timeout() const {
  return listen_timeout_.load(std::memory_order_relaxed);
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
  // And the app's own sentences, for the same reason the recogniser is told
  // here: the checkbox has moved and the next thing said must already know.
  set_enabled_languages(effective_langs());
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
  // M2c.2. Anything a live worker finished while the user was talking rides
  // *with* this turn rather than waiting to become a turn of its own. Taken
  // here, after the cancel-and-join above, so the batch cannot be claimed by a
  // turn that is being torn down; a report that lands between this line and
  // the send simply stays queued and is delivered the ordinary way.
  std::vector<PendingTurn> rider = take_riding_reports();
  if (!rider.empty()) {
    log("[report] " + std::to_string(rider.size()) +
        " report(s) riding on the user's turn");
  }
  const unsigned gen = ++turn_generation_;
  turn_running_ = true;
  turn_ = std::thread([this, text = std::move(text), gen, rider = std::move(rider)]() mutable {
    run_turn(text, false, std::string(), std::move(rider));
    (void)gen;
    turn_running_ = false;
  });
}

void VoiceSession::start_injected_turn(std::string sent, std::string fallback) {
  // No user line: nobody said this. The transcript already carries what the
  // worker reported (announce(shown, "") put it there the moment it arrived),
  // so the only thing missing from the record is Claude's answer, and that is
  // the empty line below filling up. A `{true, ...}` line here would put words
  // in the user's mouth that they never spoke.
  //
  // Deliberately **not** cancelling a turn in flight, which is the one thing
  // start_turn() does that this must not: the caller has already established
  // that the floor is free, and a report ten minutes late must never be the
  // reason a live reply is cut off.
  if (turn_.joinable()) turn_.join();
  cancel_ = false;
  {
    std::lock_guard<std::mutex> l(mutex_);
    lines_.push_back({false, ""});
    if (lines_.size() > kMaxLines) lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - kMaxLines));
    set_state_locked(State::Thinking);
    status_ = "thinking...";
  }
  turn_running_ = true;
  turn_ = std::thread([this, sent = std::move(sent), fallback = std::move(fallback)] {
    run_turn(sent, true, fallback);
    turn_running_ = false;
  });
}

void VoiceSession::run_turn(std::string text, bool is_injected, std::string fallback,
                            std::vector<PendingTurn> rider) {
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
    // Same record as flush_announcements()' — a reply's chunks as the splitter
    // hands them over, which for an injected turn is the only place the AI's
    // own words for a scheduled report can be read back.
    rend::log::info("[speak] {}", s);
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
    // M5.2. Stamp the ones that fired on *this* turn and republish, so the
    // inspector's list changes while it is open. This is the whole of "the
    // list is live": nothing polls, and nothing is rebuilt at startup and then
    // believed for the rest of the session.
    const double now =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - session_began_).count();
    for (const std::string& id : injector_.loaded()) {
      bool known = false;
      for (const auto& seen : prompt_times_) known = known || seen.first == id;
      if (!known) prompt_times_.emplace_back(id, now);
    }
    publish_inventory();
  }
  // M2b.5. What the user is still waiting for, composed into the turn rather
  // than written into the system prompt (which is a launch argument and cannot
  // show live state) or fetched with a verb (which costs a round trip and only
  // works if the model decides to ask). See pending_context() for the full
  // argument and for what this choice cannot do.
  //
  // **User turns only.** An injected turn is the app telling Claude that one
  // deferred thing has finished, and its whole job is one or two sentences
  // about that one thing; handing it the rest of the list at the same moment
  // invites a report that recites the queue. The user is not at the keyboard
  // for it and did not ask.
  const std::string pending = is_injected ? std::string() : pending_context();
  if (!pending.empty()) log("[schedule] this turn carries the pending list");
  // M2c.2. The aside. It goes in front of the user's own words for the same
  // reason the pending block does — it is context for the answer, not part of
  // the question — and it is the only thing in the composed prompt that asks
  // for something to be said *after* the answer.
  std::string rider_block;
  if (!rider.empty()) {
    std::vector<std::string> shown;
    shown.reserve(rider.size());
    for (const PendingTurn& t : rider) shown.push_back(t.report);
    rider_block = rider_report_prompt(shown);
  }
  const LanguageSelection eff = effective_langs();
  // Which language the app's *own* sentences speak in (core/app_strings.h).
  // Same two facts the rest of this function already works from: the resolved
  // settings, and -- only when both languages are on -- whether the user's own
  // words had Japanese in them. An injected turn is the app talking to itself,
  // so it is not evidence of anything and is not counted.
  set_enabled_languages(eff);
  if (!is_injected) note_user_language(text);
  const std::string sent = decorate_language(pending + rider_block + injected, eff);
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
  if (cancel_) {
    // M2c.2. A cancelled turn says nothing, so anything riding on it was not
    // delivered and goes straight back into the queue. This runs *before* the
    // thread exits and therefore before the join in start_turn() returns,
    // which is what lets a report survive being cancelled by the very turn
    // that is about to carry it: the next turn picks it up again.
    requeue_riding_reports(std::move(rider));
    return;  // the caller already moved the state on
  }
  splitter.flush();
  const std::string usage = eng_.llm->status_line();
  bool injected_turn_failed = false;
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
      // M2b.4. A failed *user* turn is visible — they are at the keyboard,
      // they just spoke, the status line says error. A failed **injected**
      // turn is silence where a report was promised ten minutes ago, with
      // nobody at the desk to see the status line, and silence is this app's
      // worst failure. So the promise is kept with a canned line instead.
      injected_turn_failed = is_injected;
    } else {
      set_state_locked(State::Speaking);  // update() returns to Idle once the audio drains
      status_ = "speaking...";
    }
  }
  if (injected_turn_failed) {
    // **M2c.1. The fallback is the raw report, and the canned line is what is
    // left when there is not one.**
    //
    // Everything that routes a result through the model arrives here holding
    // the sentence it was going to say before it decided to ask for a better
    // one. That sentence is already written, already localised by the string
    // table, and already correct — it is only *worse*, not wrong. So a model
    // call that does not come back costs the user the phrasing and nothing
    // else, which is the property that makes routing reports through a turn
    // safe to do at all. Silence here would not be a degraded report; it
    // would be the app forgetting it had been asked to do something.
    //
    // Msg::ScheduledReportLost stays for the callers that have no raw copy —
    // a bare reminder, which never had words of its own to fall back to. It
    // says nothing about what failed, on purpose: a client error string is
    // jargon the user cannot act on, and the status line and the log have it.
    log("[report] the report turn failed: " + r.error);
    announce(fallback.empty() ? app_text(Msg::ScheduledReportLost) : fallback);
    return;
  }
  if (!r.ok) {
    // M2c.2. The user's turn failed, so the aside was never said either. Back
    // in the queue rather than announced here: the user is at the keyboard and
    // can see the error, and the next gap will try the report again through
    // its own turn — which has the raw sentence as *its* fallback, so a model
    // that stays down still ends in the plain line rather than in silence.
    requeue_riding_reports(std::move(rider));
    return;
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
      // Traced for the same reason the splitter's drop is: "the app said
      // nothing" and "the app was muted" look identical from outside, and a
      // fired schedule that is never heard because mute was left on is
      // otherwise indistinguishable from a timer that never fired at all.
      rend::log::info("[mute] {} report(s) shown in the chat but not spoken",
                      pending_announce_.size());
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
  for (const std::string& s : say_now) {
    // The one record of what the app's own voice actually said. HANDOFF lists
    // "the avatar logs no spoken text" as a harness blindness, and it is the
    // reason a scheduled report cannot otherwise be verified at all: the
    // transcript shows the *shown* string, which is deliberately not the
    // spoken one, and a screenshot of a Japanese sentence cannot say which
    // voice read it. Info rather than trace, so a scripted run captures it.
    rend::log::info("[speak] {}", s);
    speech_->enqueue(s);
  }
  return true;
}

// ---------------------------------------------------------------- M2b.4
//
// ## Floor-taking, which is the hard part of this task and not the plumbing
//
// Four things can be happening when a schedule comes due, and the loop checks
// a sorted list every frame, so all four are ordinary rather than exotic:
//
//   * **Claude is mid-reply.** Neither grade may make a sound. The canned one
//     goes through announce(), which never speaks from the call — it queues,
//     and flush_announcements() is reached only from Idle or from a verified
//     gap in Listening, so Thinking and Speaking simply are not flush points.
//     The phrased one queues here and start_injected_turn() refuses to run
//     while `turn_running_`; it also, unlike start_turn(), never sets
//     `cancel_`, so a report ten minutes late can never be the reason a live
//     reply is truncated.
//   * **The microphone is open.** Same two queues, and the Listening flush
//     point already carries the three conditions this needs: nothing said this
//     utterance, the room quiet for kAnnounceGapSec, and no Talk button held.
//     Both flushes then close the microphone before making a sound — the
//     phrased one through start_injected_turn()'s caller here, for exactly the
//     reason written on announce().
//   * **Another schedule is firing on the same frame.** main.cpp calls
//     deliver_schedule() once per fired schedule and both land in a queue, so
//     two canned lines are spoken in one floor-take, in due order, and two
//     phrased ones run as consecutive turns. Nothing races, because neither
//     queue is drained anywhere but the frame loop.
//   * **The user has muted.** Handled where mute is already handled, and
//     deliberately not specially: flush_announcements() drops the spoken copy
//     and the transcript keeps the line announce() wrote at fire time. So a
//     fired schedule still *lands*, in the chat, which is the answer to a
//     promise that cannot be kept aloud. See deliver_schedule() for the one
//     case where mute changes what is worth spending.
//
// The ordering between the two queues is also a decision: announcements are
// flushed first everywhere. A canned line is already written and costs
// nothing, where a turn costs seconds and subscription usage, so the thing
// that can be said now is said now.

void VoiceSession::queue_injected_turn(std::string sent, std::string fallback) {
  if (sent.empty()) return;
  PendingTurn t;
  t.sent = std::move(sent);
  t.fallback = std::move(fallback);
  std::lock_guard<std::mutex> l(mutex_);
  pending_turns_.push_back(std::move(t));
}

void VoiceSession::queue_worker_report(std::string shown, std::string spoken) {
  if (shown.empty()) return;
  PendingTurn t;
  t.report = std::move(shown);
  t.fallback = std::move(spoken);
  t.live_worker = true;
  std::lock_guard<std::mutex> l(mutex_);
  pending_turns_.push_back(std::move(t));
}

// ---------------------------------------------------------------- M2c.2
//
// ## Riding the tail of the answer, and why it is opportunistic
//
// A report that lands while the user is talking used to wait for a gap and
// then be delivered as a turn of its own — correct, never interrupting, and
// still two utterances back to back: the answer to the question, then a
// separate little speech about a worker. What the user asked for is one reply
// that answers them and then says "by the way, I heard back".
//
// The mechanism is deliberately *pull*, not push. Nothing is ever held back
// waiting for a user turn to ride on. A finished report sits in
// `pending_turns_` exactly as it did before, and the frame loop's flush points
// will deliver it on its own at the next gap exactly as they did before; the
// only new thing is that start_turn() looks in the queue on its way past and
// takes what is ready. If no question ever comes, nothing changes. That is the
// whole answer to "a report lost waiting for a turn that never came" — the
// waiting state does not exist.
//
// The two edges that could still drop one are both cancellation-shaped, and
// both go back to the queue rather than to the floor:
//
//   * A report landing **between** the take and the send stays queued. It was
//     never taken, so it is simply the next flush's business.
//   * A turn that carried a rider and then was cancelled or failed puts it
//     back, from the turn thread, before that thread exits — which is before
//     start_turn()'s join returns, so the turn that cancelled it can pick the
//     same report up itself.
//
// Live worker reports only, and not scheduled ones. A scheduled report is
// already a *composed* turn by the time it reaches the queue, written for a
// user who is not at the desk ("picking the conversation back up after a
// gap"), and that framing is wrong for an aside — it would need a third
// prompt, not a plumbing change. It also merges reports the existing design
// deliberately keeps apart: two schedules are two promises made at two
// moments. Live workers are the case the user described and the case where
// merging is already the rule.

bool VoiceSession::reports_waiting() const {
  std::lock_guard<std::mutex> l(mutex_);
  for (const PendingTurn& t : pending_turns_) {
    if (t.live_worker) return true;
  }
  return false;
}

std::vector<VoiceSession::PendingTurn> VoiceSession::take_riding_reports() {
  std::vector<PendingTurn> rider;
  std::lock_guard<std::mutex> l(mutex_);
  // The same leading run flush_injected_turns() would have taken, and for the
  // same reason: a scheduled report sitting in front of a live one keeps its
  // place, because reordering it would be this mechanism deciding which
  // promise the user hears about first.
  while (!pending_turns_.empty() && pending_turns_.front().live_worker) {
    rider.push_back(std::move(pending_turns_.front()));
    pending_turns_.erase(pending_turns_.begin());
  }
  return rider;
}

void VoiceSession::requeue_riding_reports(std::vector<PendingTurn> rider) {
  if (rider.empty()) return;
  {
    std::lock_guard<std::mutex> l(mutex_);
    // At the head, which is where they came from. Anything that arrived while
    // the turn was running is newer and belongs behind them.
    pending_turns_.insert(pending_turns_.begin(), std::make_move_iterator(rider.begin()),
                          std::make_move_iterator(rider.end()));
  }
  log("[report] the turn carrying " + std::to_string(rider.size()) +
      " report(s) said nothing; they are back in the queue");
}

bool VoiceSession::flush_injected_turns() {
  // Not while a turn is running. This is checked outside the lock and then
  // again by the queue being drained on this one thread: `turn_running_` is
  // only ever cleared by the turn thread itself and only ever set here and in
  // start_turn(), which is also frame loop only.
  if (turn_running_) return false;
  std::string sent, fallback;
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (pending_turns_.empty()) return false;
    if (pending_turns_.front().live_worker) {
      // **M2c.1. Live worker reports are merged, and scheduled ones are not.**
      //
      // The rule below for schedules — one turn each, because the user asked
      // for them at two different moments and experiences them as two promises
      // — inverts for live workers. Three workers the user started in one
      // breath and that finish within a second of each other are one piece of
      // news, and three consecutive model calls about them would be the app
      // holding the floor for the better part of a minute to say what fits in
      // a sentence. So every *consecutive* live report waiting at this moment
      // goes into one turn. Consecutive, not all of them: a scheduled report
      // sitting between two live ones keeps its place in the queue, because
      // reordering it would be this mechanism deciding which promise the user
      // hears about first.
      std::vector<std::string> batch;
      while (!pending_turns_.empty() && pending_turns_.front().live_worker) {
        batch.push_back(std::move(pending_turns_.front().report));
        if (!pending_turns_.front().fallback.empty()) {
          if (!fallback.empty()) fallback += " ";
          fallback += pending_turns_.front().fallback;
        }
        pending_turns_.erase(pending_turns_.begin());
      }
      sent = live_report_prompt(batch);
    } else {
      // One at a time. Two reports that came due together are two turns, not one
      // turn describing both: the second is started from the Idle the first one
      // returns to, so the user hears them as two things because they were two
      // things.
      sent = std::move(pending_turns_.front().sent);
      fallback = std::move(pending_turns_.front().fallback);
      pending_turns_.erase(pending_turns_.begin());
    }
    partial_.clear();
  }
  // The microphone closes before the turn starts, not when the reply begins
  // to arrive. A turn takes a few seconds to say anything, and a microphone
  // left open across that gap is one that hears the room, decodes it as the
  // user and sends it — which is the same defect announce() was written to
  // avoid, just with a longer fuse.
  mic_->stop();
  chunk_.clear();
  mic_->drain(chunk_);
  chunk_.clear();
  speech_->clear();
  log("[report] reporting back through Claude");
  start_injected_turn(std::move(sent), std::move(fallback));
  return true;
}

bool VoiceSession::take_scheduled_worker(const std::string& name, bool* phrased) {
  std::lock_guard<std::mutex> l(mutex_);
  for (auto it = scheduled_workers_.begin(); it != scheduled_workers_.end(); ++it) {
    if (it->name == name) {
      if (phrased) *phrased = it->phrased;
      scheduled_workers_.erase(it);
      return true;
    }
  }
  return false;
}

void VoiceSession::silence_worker(const std::string& name) {
  if (name.empty()) return;
  std::lock_guard<std::mutex> l(mutex_);
  silenced_workers_.push_back(name);
}

bool VoiceSession::take_silenced_worker(const std::string& name) {
  std::lock_guard<std::mutex> l(mutex_);
  for (auto it = silenced_workers_.begin(); it != silenced_workers_.end(); ++it) {
    if (*it == name) {
      silenced_workers_.erase(it);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------- M2b.5
//
// ## How the AI comes to know what is pending, and why it is not a verb
//
// The plan offered two doors — surface it into the prompt, or add a verb to
// fetch it — and both are wrong here.
//
// **The system prompt cannot carry it.** A system prompt in this app is a
// *launch argument*: `ClaudeCodeClient` hands it to the child process once,
// when the process is created, which is why `PromptStore` deliberately does
// not hot-reload (HANDOFF says so in as many words). A list of pending
// schedules written into it would be the list as it stood at startup, which is
// always empty, forever.
//
// **A verb costs a round trip and a decision.** "What have you got pending?"
// would become: a turn that emits a fetch, an app that answers it, and a
// second turn that speaks. Two turns of usage and several seconds for a
// question whose answer the app already holds — and worse, it only works if
// the model *decides* to ask. A model that thinks it remembers will answer
// from memory and be wrong, and "cancel that" will address a schedule that
// fired four minutes ago. The failure is silent and sounds confident.
//
// **The third door is the one M3.3 already built: a turn is composed, not
// just sent.** `run_turn()` decorates the user's words with the lazy project
// prompts and, since M8.3, with the language directive — machine traffic in a
// `<context>` block, never shown and never spoken. Pending schedules are the
// same kind of thing and ride the same rail. The model sees them *with* the
// question, so "what is pending?" is one turn and no decision, and "cancel the
// stretch reminder" resolves against a list that is at most one turn old.
//
// What this choice cannot do, stated rather than discovered later:
//
//   * **It is a snapshot at turn start, not a live feed.** A schedule that
//     fires while the model is composing its reply is still in the block the
//     model is reading. That is why a cancel that misses is *spoken* — see
//     apply_cancels() — rather than assumed to be impossible.
//   * **It only reaches the model when the user speaks.** Nothing wakes the
//     model to tell it a timer went off; M2b.4's report is what does that.
//   * **It costs tokens on every turn that has anything pending.** Bounded by
//     `kSchedulesMax` and a line each. When nothing is pending it emits
//     nothing at all, so a session that never schedules anything sends
//     byte-for-byte what it sent before this existed — the same property M8.3
//     gave the language directive, and for the same reason.
//
// ## What "pending" means, and it is not the book
//
// `ScheduleBook::list()` is the obvious answer and it is the wrong one. A
// `Phrased` worker schedule leaves the book *the instant it fires*, and what
// it starts then runs for minutes. Answering from the book alone would say
// "nothing is pending" to a user who is sitting there waiting for the build
// result they asked for — the app would be telling them, confidently, that it
// had forgotten.
//
// So pending means **what the user is still waiting for**, which is three
// things: schedules not yet due, workers a schedule started that are still
// running, and reports that are finished but not yet spoken. The user does not
// experience those as three mechanisms. They experience one promise, and it is
// not kept until they hear it.
std::string VoiceSession::pending_context() const {
  const std::vector<Schedule> book = ScheduleBook::instance().list();
  std::vector<ScheduledWorker> running;
  std::size_t ready = 0;
  {
    std::lock_guard<std::mutex> l(mutex_);
    running = scheduled_workers_;
    ready = pending_turns_.size();
  }
  if (book.empty() && running.empty() && ready == 0) return std::string();

  const auto now = std::chrono::steady_clock::now();
  std::string b;
  b += "<context name=\"Pending\" kind=\"state\">\n";
  b += "Things you promised the user earlier and have not delivered yet. This list is the "
       "app's, not your memory: trust it over anything you recall.\n\n";
  if (!book.empty()) {
    b += "Still to come:\n";
    for (const Schedule& s : book) {
      b += "- id=" + std::to_string(s.id) + ", " + describe_delay(s.seconds_until(now)) + ": ";
      const std::string& label = s.action.label.empty() ? s.action.report : s.action.label;
      b += label.empty() ? std::string("something you set") : label;
      if (s.action.kind == "worker") b += " (work to be done in " + s.action.cwd + ")";
      b += "\n";
    }
    b += "\n";
  }
  if (!running.empty()) {
    b += "Started already, still working, and you owe them the outcome:\n";
    for (const ScheduledWorker& w : running) {
      const double elapsed =
          std::chrono::duration<double>(now - w.started).count();
      b += "- id=" + std::to_string(w.id) + ", running for ";
      b += elapsed < 60.0 ? std::string("less than a minute")
                          : (std::to_string(static_cast<long>(elapsed / 60.0 + 0.5)) + " minutes");
      b += ": " + (w.label.empty() ? w.name : w.label) + "\n";
    }
    b += "\n";
  }
  if (ready > 0) {
    // No id, and that is honest rather than an omission: the work is done and
    // the words are written. There is nothing left to cancel, only something
    // left to say, and it will be said as soon as there is a gap to say it in.
    b += "Finished, and waiting for a gap to tell them about: " + std::to_string(ready) +
         (ready == 1 ? " thing\n\n" : " things\n\n");
  }
  b += "If they ask what is pending, say it the way a person would - what it is and roughly "
       "how long, in the language you are speaking. Never read out an id or say the word id; "
       "those are for the cancel line only, and they are not words the user has ever heard. "
       "If they ask you to cancel something, work out which ones they mean and put a cancel "
       "line for each in your block. If two could be meant, ask which rather than guessing.\n";
  b += "</context>\n\n";
  return b;
}

// ## Cancelling, and why it is queued rather than done where it is asked
//
// `run_commands()` runs on the turn thread. `tick()` and `deliver_schedule()`
// run on the frame loop. Calling `ScheduleBook::cancel()` straight from the
// turn thread is *safe* — one mutex, and M2b.1 measured 500 jittered races
// with zero double-outcomes — but safe is not the same as correct here,
// because a fired worker schedule lives in two places in succession: it leaves
// the book inside `tick()` and appears in `scheduled_workers_` inside
// `deliver_schedule()`, and `spawn()` sits between the two for as long as it
// takes Windows to create a process. A cancel landing in that gap would find
// the schedule in neither place and tell the user it had already gone off,
// while the worker it was supposed to stop started anyway.
//
// So the cancel is queued and applied **on the frame loop, immediately after
// the tick**, which is the same discipline `AppBus::apply_pending()` and
// `announce()` already follow for the same reason. Two consequences fall out
// and neither is a check that could be got wrong:
//
//   * A schedule that came due on this frame has already been delivered and
//     registered before any cancel is looked up. The gap is not narrow; it
//     does not exist.
//   * `deliver_schedule()` pushes the record *before* it spawns, so even
//     inside that call there is no frame on which the schedule is nowhere.
//     A cancel that arrives while the spawn is in flight marks the record and
//     `deliver_schedule()` stops the worker the moment it has one to stop.
void VoiceSession::request_cancel(std::vector<std::uint64_t> ids) {
  if (ids.empty()) return;
  std::lock_guard<std::mutex> l(mutex_);
  for (std::uint64_t id : ids) pending_cancels_.push_back(id);
}

// M2b.2. One id, cancelled, with nothing said about it. Lifted out of
// apply_cancels() unchanged so the bus and the ```aii``` verb cancel through
// exactly the same two lookups in the same order; the sentence stays behind in
// apply_cancels(), because only one of the two callers has already promised
// the user out loud that this worked.
bool VoiceSession::cancel_schedule(std::uint64_t id) {
  // The book first. This is the ordinary case and it is exact: ids are
  // monotonic and never reused, so a cancel that arrives after the schedule
  // fired is a clean miss and can never take somebody else's timer with it.
  if (ScheduleBook::instance().cancel(id)) {
    log("[schedule] cancelled id=" + std::to_string(id));
    return true;
  }
  // Then the workers a schedule already started. Killing one of these is the
  // honest meaning of "cancel that" when the thing has moved on from being a
  // timer to being work in progress: the user asked for it, it has not
  // reported, and they have changed their mind.
  std::string kill;
  bool handled = false;
  {
    std::lock_guard<std::mutex> l(mutex_);
    for (auto it = scheduled_workers_.begin(); it != scheduled_workers_.end(); ++it) {
      if (it->id != id) continue;
      handled = true;
      if (it->running) {
        kill = it->name;
        scheduled_workers_.erase(it);
      } else {
        // Still inside spawn(). Marked rather than erased: deliver_schedule()
        // is holding this record's other end and will stop the process the
        // moment it exists. Reported as stopped because it will be, and the
        // frame loop is the only thread that can act on it either way.
        it->cancelled = true;
      }
      break;
    }
  }
  if (!handled) {
    log("[schedule] nothing to cancel for id=" + std::to_string(id));
    return false;
  }
  // Recorded before the stop, not after: stop() joins the worker thread and
  // the report runs inside that join, so a mark set afterwards would arrive
  // too late to suppress the very report it exists to suppress.
  if (!kill.empty()) {
    silence_worker(kill);
    if (workers_) workers_->stop(kill);
  }
  log("[schedule] cancelled id=" + std::to_string(id) + " by stopping the work it started");
  return true;
}

// M2b.2. The same three sources pending_context() reads, handed back as data
// for the bus to publish. It is deliberately *not* refactored into the shared
// body of pending_context(): that function's product is a paragraph of English
// aimed at the model, with hedged units and an instruction about ids, and none
// of that belongs on a wire a script reads.
std::vector<VoiceSession::PendingItem> VoiceSession::pending_items() const {
  const auto now = std::chrono::steady_clock::now();
  std::vector<PendingItem> out;
  for (const Schedule& s : ScheduleBook::instance().list()) {
    PendingItem it;
    it.id = s.id;
    it.kind = s.action.kind;
    it.label = s.action.label.empty() ? s.action.name : s.action.label;
    it.phrased = s.grade == ReportGrade::Phrased;
    it.seconds = s.seconds_until(now);
    out.push_back(std::move(it));
  }
  std::lock_guard<std::mutex> l(mutex_);
  for (const ScheduledWorker& w : scheduled_workers_) {
    PendingItem it;
    it.id = w.id;
    it.kind = "running";
    it.label = w.label;
    it.phrased = w.phrased;
    it.seconds = std::chrono::duration<double>(now - w.started).count();
    out.push_back(std::move(it));
  }
  return out;
}

void VoiceSession::apply_cancels() {
  std::vector<std::uint64_t> ids;
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (pending_cancels_.empty()) return;
    ids.swap(pending_cancels_);
  }
  int stopped = 0, missed = 0;
  for (std::uint64_t id : ids) {
    if (cancel_schedule(id)) {
      ++stopped;
      continue;
    }
    ++missed;
  }
  // **A cancel that misses must be heard.** This is the failure the whole task
  // exists to avoid: the model has already said "done, cancelled" out loud by
  // the time this block runs, so a miss that only reached the log would leave
  // the user believing a timer is gone when it is not. Same reasoning, and the
  // same shape, as M2b.3's spoken refusal.
  //
  // One sentence for the whole block, however many missed, because "cancel
  // everything" is one request and hearing the same line four times is not an
  // answer. No ids, no names, no numbers beyond the ones a person would use.
  std::string line;
  if (stopped == 0)
    line = app_text(missed == 1 ? Msg::CancelMissedOne : Msg::CancelMissedMany);
  else
    line = app_text(missed == 1 ? Msg::CancelPartialOne : Msg::CancelPartialMany);
  announce(line);
}

std::string VoiceSession::scheduled_report_prompt(WorkerPool::State state,
                                                  const std::string& shown) {
  // Machine traffic, in the same wrapper PromptInjector and the language
  // directive use, so from the model's side this is a kind of message it
  // already knows how to read.
  //
  // What it must not do is decide the wording. The user's rules about how the
  // app sounds — short, heard and not read, no jargon — reach the
  // conversational instance through the composed prompt already, and the whole
  // reason this is a turn rather than a string is that the model applies them,
  // in the language the conversation is being held in, to a result nobody had
  // when the schedule was created.
  const bool failed = state == WorkerPool::State::Failed;
  std::string body =
      "Something the user asked you to do later has just finished. They are not "
      "waiting at the keyboard and they have not said anything; this is the app "
      "telling you the outcome so that you can tell them.\n\n"
      "What came back:\n";
  body += shown;
  body += "\n\n";
  body += failed ? "It did not succeed. Say so plainly, in one short sentence, and say what "
                   "little is worth saying about why.\n"
                 : "Tell them how it went, in one or two short sentences.\n";
  body +=
      "Speak as if picking the conversation back up after a gap, because that is what this "
      "is. Do not mention this message, the app, the worker, its name, or any file path.";
  return "<context name=\"Deferred task\" kind=\"report\">\n" + body + "\n</context>\n\n" +
         "Tell me how that went.";
}

std::string VoiceSession::live_report_prompt(const std::vector<std::string>& shown) {
  // M2c.1. The same wrapper and the same division of labour as
  // scheduled_report_prompt(): the app supplies the fact, the model supplies
  // the wording, and the rules about how this app sounds reach it through the
  // composed system prompt rather than being restated here.
  //
  // What differs is the framing, and it is not cosmetic. The user is at the
  // desk. They asked for this work a few minutes ago, in this conversation,
  // and the model can see that it asked for the worker itself — so "picking
  // the conversation back up after a gap" would be wrong, and so would a
  // recap of what the task was. They know. They want the outcome.
  const bool many = shown.size() > 1;
  std::string body =
      many ? "Several of the background tasks you started have just finished, together.\n\n"
           : "The background task you started has just finished.\n\n";
  body += "What came back";
  body += many ? ", one line each:\n" : ":\n";
  for (const std::string& s : shown) body += (many ? "- " : "") + s + "\n";
  body += "\n";
  body +=
      "That text was written to be read, not heard: it may carry file names, paths, "
      "identifiers or counts. Say what it amounts to, out loud, in the language you have "
      "been speaking";
  body += many ? ", in one or two short sentences covering all of them together.\n"
               : ", in one short sentence.\n";
  body +=
      "Do not read the text back, do not spell out paths or names, and do not mention this "
      "message, the app or the worker.";
  return "<context name=\"Task finished\" kind=\"report\">\n" + body + "\n</context>\n\n" +
         "That's back — what happened?";
}

std::string VoiceSession::rider_report_prompt(const std::vector<std::string>& shown) {
  // M2c.2. The same facts as live_report_prompt() and the same division of
  // labour, with one thing added and one thing taken away.
  //
  // Added: *where in the reply this goes*. The user has just asked something
  // and is waiting for the answer; the report is news they did not ask for at
  // this moment. So the answer comes first, whole, and the news arrives on the
  // tail of it the way a person drops something in as they finish speaking.
  //
  // Taken away: any suggestion that this is the reply. An aside that grows
  // into a second report buries the answer the user actually wanted, which is
  // the cost of doing it this way at all, so the length is stated as a limit
  // relative to the answer rather than as a sentence count in the abstract.
  const bool many = shown.size() > 1;
  std::string body =
      many ? "Several of the background tasks you started have just finished, while the user "
             "was talking to you.\n\n"
           : "The background task you started has just finished, while the user was talking "
             "to you.\n\n";
  body +=
      "They have not heard about it yet and they did not ask about it just now. This block is "
      "the app telling you; it is not something they said.\n\n";
  body += "What came back";
  body += many ? ", one line each:\n" : ":\n";
  for (const std::string& s : shown) body += (many ? "- " : "") + s + "\n";
  body += "\n";
  body +=
      "Answer what they actually said first, properly and in full. Then, at the end of that "
      "same reply, mention this - the way a person adds something on the way out: \"by the "
      "way, I heard back about...\". Not a heading, not a new subject, and in the language you "
      "have been speaking.\n";
  body += many ? "One or two short sentences covering all of them together, and never longer "
                 "than the answer itself.\n"
               : "One short sentence, and never longer than the answer itself.\n";
  body +=
      "That text was written to be read, not heard: say what it amounts to. Do not read it "
      "back, do not spell out paths, names, identifiers or counts, and do not mention this "
      "message, the app or the worker.";
  return "<context name=\"Task finished\" kind=\"report\">\n" + body + "\n</context>\n\n";
}

void VoiceSession::deliver_schedule(const Schedule& s) {
  const ScheduleAction& a = s.action;
  // A kind that carries work. Starting it takes no floor at all — a worker is
  // its own process — so this happens the moment it comes due even if Claude
  // is mid-reply or the user is mid-sentence. Only the *report* has to wait,
  // and that is what the queues above are for.
  if (a.kind == "worker") {
    std::string err;
    const std::string name = a.name.empty() ? std::string("task") : a.name;
    // `cwd` exactly as it was captured at creation and never re-resolved: a
    // deferred worker runs with permissions bypassed, ten minutes after the
    // conversation that could have caught a wrong folder, quite possibly with
    // nobody at the desk. The directory it was promised is the only safe one.
    // M2b.5. Registered **before** the spawn, not after it, and erased again if
    // the spawn fails. `spawn()` creates a Windows process and takes as long as
    // that takes; registering afterwards would leave a window in which the
    // schedule had left the book and not yet arrived here, and a cancel landing
    // there would report "already gone off" while the worker started anyway.
    // With the push first there is no such moment: the schedule is in the book,
    // or it is in this list, and apply_cancels() runs on this same thread.
    //
    // `phrased` is a field rather than a reason not to register: Fixed on a
    // worker is reachable only through the bus (M2b.2), which can set `grade=`
    // where the model cannot, and it means "do the work, report it the ordinary
    // way" — but the user is waiting for it either way, so it belongs in the
    // pending list and it must be cancellable.
    {
      std::lock_guard<std::mutex> l(mutex_);
      ScheduledWorker w;
      w.id = s.id;
      w.name = name;
      w.label = a.label.empty() ? name : a.label;
      w.started = std::chrono::steady_clock::now();
      w.phrased = s.grade == ReportGrade::Phrased;
      scheduled_workers_.push_back(std::move(w));
    }
    if (workers_ && workers_->spawn(name, a.cwd, a.task, &err)) {
      log("[schedule] started deferred worker " + name + " in " + a.cwd);
      // A cancel that arrived while the spawn was in flight. **Today this is
      // unreachable, and deliberately written anyway.** `request_cancel()` only
      // queues, and `apply_cancels()` runs on this same thread immediately
      // after the tick loop this call sits inside, so nothing can act on the
      // record between the push above and here. It is belt and braces against
      // the one change that would break that — a future consumer (the bus,
      // M2b.2) applying a cancel from its own thread — because the failure it
      // would cause is the silent one: a worker the user cancelled running
      // anyway, with the app having said it stopped. Cheap here, invisible
      // there.
      bool kill = false;
      {
        std::lock_guard<std::mutex> l(mutex_);
        for (auto it = scheduled_workers_.begin(); it != scheduled_workers_.end(); ++it) {
          if (it->id != s.id) continue;
          if (it->cancelled) {
            kill = true;
            scheduled_workers_.erase(it);
          } else {
            it->running = true;
          }
          break;
        }
      }
      if (kill) {
        silence_worker(name);
        if (workers_) workers_->stop(name);
        log("[schedule] stopped deferred worker " + name + ": cancelled while it was starting");
      }
      return;
    }
    {
      std::lock_guard<std::mutex> l(mutex_);
      for (auto it = scheduled_workers_.begin(); it != scheduled_workers_.end(); ++it) {
        if (it->id == s.id) {
          scheduled_workers_.erase(it);
          break;
        }
      }
    }
    // **The promise is now broken and the user is owed a sentence.** Ten
    // minutes ago they were told this would happen; the worst outcome here is
    // silence, so this is canned and immediate rather than another turn that
    // could fail the same way. It names nothing — not the worker, not the
    // folder, not the reason, which is a CLI error string and is jargon. The
    // transcript line and the log carry all three.
    log("[schedule] deferred worker " + name + " in " + a.cwd + " could not start: " + err);
    // The reason stays in the log and out of the chat on purpose: a spawn
    // failure's `error` is the whole command line, system prompt included, and
    // a transcript is a place the user reads, not a place to dump 900
    // characters of argv. What the chat needs is which one and where.
    announce(app_text(Msg::DeferredStartFailedShown, name, a.cwd),
             app_text(Msg::DeferredStartFailedSpoken));
    return;
  }

  // A bare timer. `report` is a finished spoken sentence, written at creation
  // by the model, **in the user's own language** — a Japanese request produces
  // a Japanese say=. So it is spoken exactly as it stands: a prefix here would
  // be an English word in front of a Japanese sentence, and a decoration would
  // be the app talking over the words the user was promised.
  std::string report = a.report.empty() ? a.label : a.report;
  if (report.empty()) report = app_text(Msg::TimerNoWords);
  if (s.grade == ReportGrade::Fixed) {
    announce(report, report);
    return;
  }
  // Phrased with no work to do: the bus again, or a schedule that wants the
  // AI's own words about something it already knows. Same floor rules.
  queue_injected_turn("<context name=\"Reminder\" kind=\"report\">\n"
                      "A reminder the user set earlier has just come due. What they asked to be "
                      "reminded of:\n" + report +
                      "\nTell them, in one short sentence, in the language you have been "
                      "speaking. Do not mention this message or the app.\n</context>\n\n"
                      "It is time.");
}

void VoiceSession::drop_schedules(const std::vector<Schedule>& dropped) {
  if (dropped.empty()) return;
  // **One line, and it is not spoken — which is a fact here, not a policy.**
  // This runs during teardown: the frame loop has stopped, so nothing will
  // ever call flush_announcements() again, and the speech queue and its audio
  // device are being pulled down in the next few statements. A sentence per
  // dropped schedule would be a sentence per schedule that nobody can hear.
  //
  // The honest place for this promise is therefore *before* it is broken, and
  // M2b.3 already put it there: the prompt makes the model say the lifetime
  // out loud when it accepts ("as long as this is still running"), and
  // parse_delay() refuses anything past a day outright, because past a day the
  // promise is itself the failure. This is the record that it was broken, for
  // the chat and the log, and it names what is being dropped because a shown
  // form may.
  std::string shown = app_text(dropped.size() == 1 ? Msg::ClosingWithOne : Msg::ClosingWithMany,
                               std::to_string(dropped.size()));
  for (size_t i = 0; i < dropped.size(); ++i) {
    const std::string& label = dropped[i].action.label.empty() ? dropped[i].action.name
                                                               : dropped[i].action.label;
    shown += (i ? "; " : "") + (label.empty() ? std::string("(unnamed)") : label);
  }
  shown += ".";
  // The log as well as the transcript, and the log is the half that survives:
  // the window is a second from being torn down, so the chat line is real but
  // nobody will read it. main.cpp keeps the per-schedule detail beside this.
  log("[schedule] " + shown);
  announce(shown, "");
}

namespace {

// M2b.3. Turns one `schedule` line into a pending schedule, or into a sentence
// the user hears.
//
// A refusal here is unlike `button`'s and `load`'s, which are the model's own
// housekeeping and are logged rather than spoken. A schedule is something the
// user asked for out loud and is then waiting on, and the model has *already*
// said "I'll tell you in ten minutes" by the time this runs — a block is the
// end of a reply. So a refusal that only reached the log would leave the user
// believing a timer exists, which is precisely the failure the plan names. It
// is spoken, in the register the user asked for: no ids, no field names, no
// "schedule refused".
//
// Returns an empty string on success, or the sentence to say.
std::string create_schedule(const Command& c, std::string* detail) {
  // M2b.2. The mapping itself now lives in `build_schedule()`, beside the book,
  // because the bus is a second door onto the same policy and the shape-is-the-
  // grade rule is the part that must not be written twice. What stays here is
  // the half that is this door's alone: the *words*. A refusal the model
  // triggered is spoken in the user's register; the bus's is a line in the log.
  ScheduleRequest req;
  req.in = c.in;
  req.say = c.say;
  req.task = c.task;
  req.cwd = c.cwd;
  req.name = c.name;
  req.label = c.label;
  req.grade = c.grade;
  ScheduleAction action;
  ReportGrade grade = ReportGrade::Fixed;
  double seconds = 0.0;
  switch (build_schedule(req, &action, &grade, &seconds, detail)) {
    case ScheduleRefusal::Delay: return app_text(Msg::RefuseDelay);
    case ScheduleRefusal::NoFolder: return app_text(Msg::RefuseNoFolder);
    case ScheduleRefusal::RelativeFolder: return app_text(Msg::RefuseRelativeFolder);
    case ScheduleRefusal::NothingToDo: return app_text(Msg::RefuseNothingToDo);
    case ScheduleRefusal::None: break;
  }

  std::string err;
  const std::uint64_t id = ScheduleBook::instance().create(
      std::chrono::duration<double>(seconds), action, grade, &err);
  if (id == 0) {
    *detail = err;
    // The one refusal the book itself makes is "full" (64 pending). Said
    // without the number, because the user did not ask for a number.
    return app_text(Msg::RefuseTooMany);
  }
  char buf[160];
  std::snprintf(buf, sizeof buf, "created id=%llu kind=%s grade=%s in %.1fs",
                static_cast<unsigned long long>(id), action.kind.c_str(), to_string(grade),
                seconds);
  *detail = buf;
  return {};
}

}  // namespace

void VoiceSession::run_commands(const std::string& reply_text) {
  // M2b.5. Every cancel in one block is collected and handed over together, so
  // "cancel everything" is one request with one answer rather than one
  // sentence per item. Applied on the frame loop; see apply_cancels().
  std::vector<std::uint64_t> cancels;
  for (const Command& c : parse_commands(reply_text)) {
    if (c.verb == "cancel") {
      // The id comes from the list this app gave the model a moment ago, so a
      // value it cannot read is the model inventing one. Logged, not spoken:
      // apply_cancels() is what owes the user a sentence, and it will say the
      // honest thing when the cancel misses.
      char* end = nullptr;
      const unsigned long long n = std::strtoull(c.id.c_str(), &end, 10);
      if (c.id.empty() || (end && *end != '\0') || n == 0) {
        log("[schedule] refused cancel: could not read id=\"" + c.id + "\"");
        // Still counted as a miss, because the user asked for something to
        // stop and nothing did. Queuing a 0 makes apply_cancels() say so
        // through the one path that says it.
        cancels.push_back(0);
        continue;
      }
      cancels.push_back(static_cast<std::uint64_t>(n));
      continue;
    }
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
    // M2b.3. Not a worker verb either, so it needs no pool — a scheduled
    // worker only wants one when it fires, which is M2b.4's problem.
    if (c.verb == "schedule") {
      std::string detail;
      const std::string refusal = create_schedule(c, &detail);
      log("[schedule] " + std::string(refusal.empty() ? "" : "refused: ") + detail);
      if (!refusal.empty()) announce(refusal);
      continue;
    }
    if (!workers_) continue;
    std::string err;
    if (c.verb == "spawn") {
      if (workers_->spawn(c.name, c.cwd, c.task, &err)) {
        log("[worker] spawned " + c.name + " in " + (c.cwd.empty()? std::string("(app dir)") : c.cwd));
      } else {
        // `err` is a client error string ("CreateProcess failed (2): claude
        // --flags ...") and is spoken, so it goes through the same mapping the
        // failure report uses rather than being read out as it stands.
        log("[worker] could not spawn " + c.name + ": " + err);
        announce(app_text(Msg::SpawnFailed, c.name, app_text(failure_reason(err))));
      }
    } else if (c.verb == "pause") {
      if (!workers_->pause(c.name)) announce(app_text(Msg::NoRunningWorker, c.name));
    } else if (c.verb == "stop") {
      // M2b.5. Same suppression as a cancel, and for the same reason: this is a
      // stop the user asked for out loud, so the pool reporting it as Paused a
      // moment later is not news. It also clears the pending list if what was
      // stopped happened to be a schedule-started worker the model addressed by
      // name instead of by id.
      silence_worker(c.name);
      if (!workers_->stop(c.name)) {
        take_silenced_worker(c.name);
        announce(app_text(Msg::NoWorker, c.name));
      }
    }
  }
  request_cancel(std::move(cancels));
}

void VoiceSession::publish_inventory() {
  // Built here, on a thread that owns `prompts_` and `injector_`, and handed
  // over as a value. The lock covers the handover only: build_inventory does
  // file work (it asks whether `pre-prompt.md` has anything in it) and holding
  // `mutex_` across that would stall the frame loop on a disk read.
  PromptInventory inv = build_inventory(prompts_, injector_, prompt_times_);
  std::lock_guard<std::mutex> l(mutex_);
  inventory_ = std::move(inv);
}

PromptInventory VoiceSession::prompt_inventory() const {
  // Read the clock before the lock, for no better reason than that nothing
  // else in this class holds `mutex_` across anything it does not have to.
  const double up =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - session_began_).count();
  // M5.3: the real context fullness travels with the list it is compared
  // against. Asked for before `mutex_` is taken, for the reason snapshot()
  // gives above its own call -- the client's reader thread calls back into us
  // while holding its lock, so taking ours first is the AB/BA deadlock.
  UsageStats stats;
  if (loaded_ && eng_.llm) stats = eng_.llm->usage();
  std::lock_guard<std::mutex> l(mutex_);
  PromptInventory inv = inventory_;
  inv.uptime = up;
  inv.ctx = stats.ctx;
  inv.ctx_window = stats.ctx_window;
  return inv;
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
  s.listen_timeout_seq = listen_timeout_seq_;
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
