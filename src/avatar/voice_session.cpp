#include "voice_session.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "rend/core/log.h"

#include "settings.h"

#include "audio/device_pick.h"
#include "core/app_bus.h"
#include "core/app_strings.h"
#include "core/cwd_policy.h"
#include "core/directive_filter.h"
#include "core/handoff_policy.h"
#include "core/language.h"
#include "core/model_choice.h"
#include "core/schedule.h"
#include "core/sentence_splitter.h"
#include "core/text_util.h"
#include "core/user_paths.h"

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

// M12.1. How long the running-worker count has to hold at zero before the wait
// is declared over and the remembered latch state is thrown away.
//
// Not zero, and the reason is a race rather than a preference. `running()` is
// read on the frame loop, but a worker's report arrives on the pool's own
// thread; the two are not ordered, so a frame that sees the count reach zero
// may be a millisecond ahead of the report that needed the memory. Two seconds
// is far longer than that gap and far shorter than the pause between one piece
// of work and the next, so the only behaviour it changes is that a worker
// started within two seconds of the last one finishing joins the same wait
// instead of re-reading the latch. Over two seconds the latch has not moved.
constexpr float kWaitEndGraceSec = 2.0f;

// M12.2. How long one passive wake segment may run before the recogniser is
// restarted on a fresh stream.
//
// The wake state can now be open for hours, where every other use of this
// recogniser lasts one utterance. `Recognizer` keeps the segment's own PCM for
// M8.4's re-decode and its hypothesis grows with the stream, so a stream that
// is never restarted is a slow leak and a decoder whose hypothesis is mostly
// old news. Eight seconds is several times the longest wake phrase anyone
// would type and short enough that the buffer stays small. The segment is also
// restarted on every endpoint, which is what happens in practice — this is the
// ceiling for a room with continuous noise that never endpoints at all.
constexpr float kWakeSegmentSec = 8.0f;

// M12.2, and the two numbers that make always-on matching affordable at all.
//
// **This was measured before it was chosen, and the first version failed.**
// Feeding the recogniser continuously — which is what "always-on" naively
// means — cost **175% of one core, sustained**, against 2.1% for the same app
// with the phrase cleared (30 s runs, 20 Sep 2026, measured as process CPU
// time over a 15 s window). The wall-clock real-time factor looked harmless at
// 0.095, and that is exactly the trap: `build_stt` gives the recogniser eight
// threads, so it keeps up with the microphone easily while burning nearly two
// cores to do it. On this desktop that is a fan that never stops; on a laptop
// it is the battery. Shipping it would have been a feature that worked and
// quietly ruined the machine it worked on.
//
// The fix is not a second recogniser and not fewer threads — it is to decode
// only when there is something to decode. The RMS gate this session already
// runs for endpointing and for the listen timeout says whether anybody is
// making a noise; below it, nothing is fed to the decoder at all, and a quiet
// room costs what an idle app costs. **No second detector**, which is the same
// rule kVoiceRunSec is written under: one gate, one threshold, one opinion
// about whether the room is quiet.
//
// `kWakeTailSec` keeps decoding for a moment after the gate shuts, so the end
// of a word is not cut off by the silence that follows it. `kWakePrerollSec`
// is the other end of the same problem: the gate opens on the first loud
// frame, by which time the first consonant is already past, so the last
// fraction of a second of audio is kept and handed over when the gate opens.
// Without it "Aria" reliably decodes as "ria".
// The measurement did not stop at the gate, and the second half of it is the
// same lesson kVoiceRunSec learned for the listen timeout. With a bare gate in
// front of the decoder the cost fell from 175% of a core to 31%, which is
// better and still wrong: the log said 8.6 s of the 22 s the microphone had
// been open was reaching the decoder, in a room with nobody in it. That is not
// speech. It is the transients the kVoiceRunSec comment already names -- a
// keyboard press, a chair, the GPU fan changing note -- each one opening the
// gate for an instant and buying itself the whole `kWakeTailSec` tail.
//
// So the gate has to be open *continuously* for kWakeOnsetSec before anything
// is decoded. A click is tens of milliseconds and a syllable is hundreds, so
// 0.12 s sits well clear of both; what it costs is that the phrase's first
// 0.12 s arrives late, which is exactly what the pre-roll is for, and the
// pre-roll is sized to cover it several times over.
constexpr float kWakeOnsetSec = 0.12f;
constexpr float kWakeTailSec = 0.8f;
constexpr float kWakePrerollSec = 0.5f;

// M18.2/M18.3. How much of the barge watch's audio is kept back, so that the
// syllable which interrupted the reply is still there to hand to the fresh
// recogniser stream when the rule fires.
//
// It is longer than `kBargeOnsetSec` (0.30 s) and for a different reason than
// `kWakePrerollSec` is longer than `kWakeOnsetSec`: the wake pre-roll is sized
// to cover the gate's late opening several times over, while this one is only
// ever *trimmed back* to the onset run's start before it is used. The extra
// 0.2 s is slack for a frame that arrives long, not audio anyone intends to
// decode -- everything earlier than the run is the reply's own leakage and the
// room, and `docs/bargein-measurements.md` shows the recogniser inventing
// words ("Sorry") out of exactly that. So it is bounded here and trimmed
// there, and the two together are why a barge cannot put the app's own voice
// into the user's next turn.
constexpr float kBargePrerollSec = 0.5f;

float rms(const std::vector<float>& s) {
  if (s.empty()) return 0.0f;
  double sum = 0.0;
  for (float v : s) sum += double(v) * double(v);
  return static_cast<float>(std::sqrt(sum / double(s.size())));
}

// M13.2. Hand the queue its secondary voices, having first thrown out any the
// engines do not actually have.
//
// Validation is here, at load, rather than at synthesis, because the two
// engines fail an unknown id in opposite directions and **neither failure is
// recoverable once the utterance has been dequeued**. Kokoro does not refuse a
// speaker it lacks: it prints to stderr and speaks the line in `af_alloy`, so a
// typo becomes a third voice nobody chose. VOICEVOX returns an error and the
// line is simply silent. A bad entry dropped here costs one log line and falls
// back to the primary, which is the behaviour M13 specifies.
//
// Japanese is validated only when VOICEVOX exists. It arrives late or not at
// all, so this runs again when it is handed over.
void apply_voice_lists(const Config& cfg, Engines& eng, SpeechQueue& speech,
                       const std::function<void(const std::string&)>& log) {
  std::vector<int> en, ja;
  if (eng.kokoro) {
    const int speakers = eng.kokoro->speaker_count();
    for (const int sid : cfg.voices_en) {
      if (sid < 0 || (speakers > 0 && sid >= speakers)) {
        log("[voices] English v" + std::to_string(int(en.size()) + 2) + ": speaker " +
            std::to_string(sid) + " is not in this model (" + std::to_string(speakers) +
            " speakers); falling back to the primary");
        continue;
      }
      // Ids 28 and up are Kokoro's other languages. Allowed, because somebody
      // may want one deliberately, but said out loud: it will be phonemised
      // with the English lexicon and will not sound like that language.
      if (sid > 27)
        log("[voices] English v" + std::to_string(int(en.size()) + 2) + ": speaker " +
            std::to_string(sid) + " is outside the English range (0-27)");
      en.push_back(sid);
    }
  }
  if (eng.voicevox && eng.voicevox->ok()) {
    for (const int style : cfg.voices_ja) {
      if (style < 0 || !eng.voicevox->has_style(static_cast<uint32_t>(style))) {
        log("[voices] Japanese v" + std::to_string(int(ja.size()) + 2) + ": style " +
            std::to_string(style) +
            " is not in the loaded voice model; falling back to the primary");
        continue;
      }
      ja.push_back(style);
    }
  }
  log("[voices] English " + std::to_string(en.size() + 1) + ", Japanese " +
      std::to_string(ja.size() + 1) + " (v1 is the configured primary)");
  speech.set_voices(std::move(en), std::move(ja));
}
}  // namespace

VoiceSession::VoiceSession(Config cfg) : cfg_(std::move(cfg)), memory_(memories_path()) {
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
  // Before `turn_`, because the reset thread is the one joining it, and before
  // the engines go away, because it is the thread that writes `eng_.llm`.
  // Quitting during the second a reset takes waits for it, the same as
  // quitting during the startup load does.
  if (reset_.joinable()) reset_.join();
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

// Through rend::log rather than straight at stdout, which it used to be.
//
// These are the `[setting]` and `[handoff]` lines -- the ones a run is read
// against -- and they were the odd ones out in their own stream: `[reply]`,
// `[speak]` and `[tool]` a few lines away already go through rend::log and so
// already carry a timestamp and a level. Now these do too, which is worth
// having on exactly the lines you end up reconstructing a session from.
//
// It also means they reach the log file in *both* the modes routeDiagnostics()
// can end up in (see main.cpp): when a console is attached the file is a
// rend::log mirror, and a raw printf would have gone only to the console.
void VoiceSession::log(const std::string& s) { rend::log::info("{}", s); }

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
  // M3.12. The first child, recorded the way every later one is: what is in
  // force is what a `build_llm` that returned true was given, and nothing
  // else. The settings surface reads this out of the snapshot.
  {
    std::lock_guard<std::mutex> l(mutex_);
    model_in_force_ = cfg_.model_override;
    tools_in_force_ = cfg_.tools;
  }
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

  // M18.4. `AII_SPEAKER` and `AII_MIC` name an endpoint by a substring of its
  // name; unset, both are the Windows default, as they always were. They exist
  // because the machine this is tuned on has a dead microphone as its default
  // input and the loudspeakers are not its default output, and a tuning run
  // has to be able to say "the webcam, and the loudspeakers" without touching
  // Windows sound settings in between. A name that matches nothing is a hard
  // failure with the list of what was there: silently falling back to the
  // default would make a mistyped name look like a microphone that hears
  // nothing, which is exactly the fault this is for telling apart.
  auto pick = [&](bool playback, const char* var, ma_device_id* id, std::string* err) {
    const char* want = std::getenv(var);
    if (!want || !*want) return false;
    std::string name;
    if (find_audio_device(playback, want, id, &name)) {
      rend::log::info("[audio] {}={} picked {}", var, want, name);
      return true;
    }
    std::string list;
    for (const auto& d : list_audio_devices(playback))
      list += "\n    " + std::string(d.is_default ? "* " : "  ") + d.name;
    *err = std::string("no ") + (playback ? "playback" : "capture") + " device contains \"" +
           want + "\" (" + var + "); available:" + list;
    return false;
  };
  ma_device_id speaker_id{};
  ma_device_id mic_id{};
  std::string pick_err;
  enter(4);
  const bool named_speaker = pick(true, "AII_SPEAKER", &speaker_id, &pick_err);
  if (!pick_err.empty()) return fail(pick_err);
  speaker_ = std::make_unique<AudioOut>();
  if (!speaker_->start(eng_.kokoro->sample_rate(), named_speaker ? &speaker_id : nullptr))
    return fail("no playback device");
  enter(5);
  const bool named_mic = pick(false, "AII_MIC", &mic_id, &pick_err);
  if (!pick_err.empty()) return fail(pick_err);
  mic_ = std::make_unique<MicIn>();
  if (!mic_->open(kMicRate, named_mic ? &mic_id : nullptr)) return fail("no capture device");
  finish_stages();
  // `eng_.voicevox` is null when Japanese is off; SpeechQueue takes that and
  // set_japanese() is how the on-demand load hands it one later.
  speech_ = std::make_unique<SpeechQueue>(eng_.kokoro.get(), eng_.voicevox.get(), speaker_.get());
  speech_->set_on_status([this](const std::string& s) { log("[tts] " + s); });
  apply_voice_lists(cfg_, eng_, *speech_, [this](const std::string& s) { log(s); });
  // The canned lines need the setting from the first second, not from the
  // first turn: a schedule restored before anyone has spoken can fire, and a
  // worker can fail, with the table still on its default.
  set_enabled_languages(effective_langs());
  workers_ = std::make_unique<WorkerPool>(cfg_.claude_exe, cfg_.worker_bypass);
  // M31. Seeded from Config the same way `worker_bypass` already was --
  // AII_WORKER_MODEL / AII_WORKER_CHROME for a run with no panel (voiceloop),
  // and the panel's own `model.worker` / `tools.browser` rows override them
  // every frame from here on through set_worker_model()/set_worker_chrome().
  workers_->set_model(cfg_.worker_model);
  workers_->set_chrome(cfg_.worker_chrome);
  workers_->set_on_report([this](const std::string& name, WorkerPool::State state,
                                 const std::string& shown, const std::string& spoken) {
    // M12.1, and the first line of the callback on purpose: **a worker has
    // come back**, which is the whole of what the restore is waiting for. It
    // is set before any of the early returns below because none of them is a
    // reason to leave the user without the microphone they had -- a worker
    // that failed, or one they stopped themselves, has still stopped being the
    // thing the app went quiet for.
    //
    // This runs on the pool's report thread, so it does exactly one thing: set
    // a flag. Every decision, and the microphone itself, belongs to the frame
    // loop -- see tick_listen_restore().
    worker_reported_.store(true, std::memory_order_release);
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

  // A reset is running: the reset thread owns `turn_` and `eng_.llm` until it
  // clears the flag, and everything below this line either joins that thread's
  // thread (the reap immediately after) or starts a turn on that thread's
  // client. Two joins of one `std::thread` is undefined behaviour, not a race
  // to be lost occasionally, so this is a hard gate rather than a try.
  //
  // Workers are still serviced, because a worker is a separate process that
  // reset did not kill and its report callbacks only ever queue — see
  // queue_worker_report(), which takes `mutex_` and pushes. Nothing here takes
  // the floor, and nothing needs to: the microphone is shut and the state is
  // Idle for the whole of it.
  if (resetting()) {
    // M12.2. The passive capture goes down with everything else: update()
    // stops draining the microphone for the second the rebuild takes, and a
    // capture device nothing is draining is one that overruns. It comes back
    // on the first Idle frame afterwards, which is the same frame the latch
    // comes back on -- and if the latch is the one that comes back, tick_wake()
    // is not reached at all, which is correct.
    if (wake_open_) end_wake();
    // M18.2. And the barge watch with it, for the same reason and with the
    // same consequence: update() is the only thing that drains the capture
    // device, and it is about to stop for the second the rebuild takes.
    end_barge_watch();
    if (workers_) workers_->update();
    return;
  }
  // The first frame after a reset finished, and the latch it closed is owed
  // back. Here rather than at the end of run_reset() because the microphone is
  // the frame loop's: begin_listening() starts the capture device and update()
  // is the only thing that drains it. The status is rewritten afterwards
  // because begin_listening() has its own, and "listening..." on its own would
  // lose the one line that says what the button just did.
  if (relatch_after_reset_) {
    relatch_after_reset_ = false;
    if (s == State::Idle) {
      // The latch is a *level*, and the Idle branch at the bottom of this
      // function is what turns it into an open capture device — the same path
      // that reopens the microphone after every reply. So the level is what is
      // restored here. Calling set_mic_open() instead opened the device on
      // this line and the Idle branch opened it again on the same frame,
      // because `s` was read before either of them ran; the log showed two
      // `mic: open` lines a microsecond apart, which is the one trace in this
      // file that is supposed to answer "did the microphone open?" exactly
      // once per opening.
      mic_open_ = true;
      // The same clause run_reset() just wrote, with a different tail. It used
      // to be this literal, which was already wrong for a settings restart the
      // moment M3.12 shipped — a user with auto-listen on who moved the model
      // picker was told the conversation had been cleared, in the one place
      // that was supposed to explain what had happened — and would have been
      // wrong in the opposite direction for a handoff, telling a session that
      // *has* carried its direction over that it remembers nothing.
      set_status(restart_note_ + " listening...");
      log("[reset] the microphone latch was on; it is on again");
    }
  }

  // Reap a finished turn thread.
  if (turn_.joinable() && !turn_running_) turn_.join();
  if (workers_) workers_->update();

  // M12. The harnesses first, so an event they post this frame is seen by the
  // restore on the same frame rather than the next one; then the restore,
  // which may set the latch level that the Idle branch at the bottom of this
  // function turns into an open microphone.
  tick_harnesses();
  tick_listen_restore(s);

  // M12.2. The passive capture belongs to the Idle branch and to nothing else.
  // Unconditional here rather than at each of the paths that take the
  // microphone away, because there are five of them and a capture device with
  // two owners is the one way this feature could break the app it is bolted
  // on to. begin_listening() closes it too, for the paths that never come back
  // through here first.
  if (wake_open_ && (s != State::Idle || mic_open_ || hold_)) end_wake();

  // M18.2. The barge watch's half of the same rule, and unconditional for the
  // same reason: the watch belongs to Thinking and Speaking and to nothing
  // else, and every other way out of them -- an error, a Stop, a reply that
  // simply ended -- would otherwise leave a capture device running with
  // nobody draining it. tick_barge() below is what opens it.
  if (barge_watch_ && s != State::Thinking && s != State::Speaking) end_barge_watch();

  // M3.15. Before the state branches, because the stage that ends in a
  // restart is reached while the session is Thinking and Thinking has no
  // branch below. Arming, which is the half that must only happen when
  // everything has settled, is not here: it is in the Idle branch, after the
  // two flushes and before the microphone reopens.
  tick_handoff(s);

  // M1f.1. Every frame this session is doing something other than listening —
  // thinking, speaking, or idle with the microphone shut — is a frame that
  // must not be banked as silence. Stamping here as well as in the Listening
  // branch below means the stamp is unconditional: there is no state, and no
  // early return past this point, in which the clock quietly keeps running.
  if (s != State::Listening) timeout_blocked_at_ = std::chrono::steady_clock::now();

  // M18.2. The two states in which the app is doing the talking. Before the
  // branches rather than inside them because Thinking has no branch below at
  // all, and because a fire moves the state to Listening -- which the stale
  // `s` read at the top of this function must not then be used to service.
  // It is not: the Speaking branch below only ever drops the state to Idle,
  // and it cannot, because the turn is still running.
  if (s == State::Thinking || s == State::Speaking) tick_barge();

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
    // M3.15 extends the same chain by one link, in the one place where it can
    // be sure of all four things a handover must not interrupt: no turn is
    // running (Idle), nothing is half-spoken (Speaking is a different branch),
    // nothing is queued to say (the two flushes above just said so), and the
    // microphone is shut — it is reopened on the line below, and taking the
    // frame here is what stops that happening. A user who is mid-sentence is
    // in Listening, which never reaches this branch at all.
    // M17.2 puts one more link in that chain, ahead of the reports and behind
    // the announcements: a question the user asked during the last handover
    // outranks a worker's aside — they are waiting for an answer to it — and
    // still comes after a canned line, which is instant and already written.
    // start_turn() will pick up any waiting live-worker report as a rider, so
    // ordering it first costs the reports nothing.
    if (!flush_announcements() && !flush_queued_user_turn() && !flush_injected_turns() &&
        !begin_handoff_if_due()) {
      // M17.2. **The microphone stays shut for the length of a handover.**
      // Without this the latch reopened it the moment the housekeeping line
      // finished, and everything said into it for the next several seconds
      // went to a child that was about to be replaced. It is not lost — that
      // is what the queue above is for — but the honest shape is not to invite
      // a sentence the app cannot answer for five seconds. The latch itself is
      // untouched: `relatch_after_reset_` is taken from it in begin_restart()
      // and hands it straight back when the new child is up.
      if (handing_off()) {
        // Nothing. Neither the latch nor the wake watch may take the
        // microphone while the session is being replaced.
      } else if (mic_open_) {
        // Still unmuted after a reply finished: reopen the mic for the next
        // turn. It stays shut while Claude speaks, so the speakers are never
        // transcribed back in as the user.
        begin_listening();
      } else if (!hold_) {
        // M12.2. The latch is off and there is nothing waiting to be said, so
        // the microphone is free: listen passively for the wake phrase. It is
        // the *last* link in the same chain for the same reason begin_listening()
        // is -- an announcement or a report has to go out before any microphone
        // opens, or the app speaks into its own open mic. A no-op when no
        // phrase is set, which is the default.
        tick_wake();
      }
    }
  }
}

void VoiceSession::begin_listening(std::vector<float> preroll) {
  // M12.2. The passive capture and this one are the same device and the same
  // recogniser, and they must never both be up. Here rather than only in
  // update() because talk_pressed() reaches this function directly -- a SPACE
  // hold never passes through the Idle branch at all.
  end_wake();
  // M18.2. And the same for the barge watch, which is the third owner this
  // device now has. `end_barge_watch()` stops it; the fire path in
  // tick_barge() has already cleared the flag by the time it calls here, so a
  // barge hands the device over without the stop-and-start a shut mic would
  // cost -- `MicIn::start()` below is then a no-op and the capture never
  // actually pauses.
  end_barge_watch();
  speech_->clear();
  eng_.stt->begin();
  mic_->discard();
  if (!mic_->start()) {
    set_status("mic failed to start");
    return;
  }
  // The only place the capture device is ever started for a turn, so this line
  // answers "did the microphone open while Claude was talking?" on its own. It
  // is what the speakers-into-the-C920 defect is checked against; keep it.
  // M18.2 opens the same device for the barge watch, which has a line of its
  // own saying so and which can never send a word -- so this one still means
  // what it has always meant: the microphone is open *and what it hears goes
  // to Claude*.
  rend::log::trace("mic: open (latch={})", mic_open_);
  // M18.3. The onset run that fired the barge, handed to the stream that was
  // just begun. Fed before anything else so it is the first audio the decoder
  // sees, which is the whole point: without it the user's first syllable is
  // the one the gate spent on deciding they were talking.
  if (!preroll.empty()) {
    eng_.stt->feed(preroll.data(), (int)preroll.size(), kMicRate);
    // The length, never the audio and never the words. The same rule
    // discard_utterance() writes its line under.
    rend::log::trace("mic: {:.2f} s of barge pre-roll fed to the new stream",
                     float(preroll.size()) / float(kMicRate));
  }
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

std::string VoiceSession::finish_utterance(bool may_watch) {
  // M18.2. The microphone is not stopped when a reply is about to be spoken
  // into a room the user may talk over: the device stays up and the barge
  // watch takes it. The *decode* still ends here either way -- the stream is
  // finished on the next line and the watch feeds the recogniser nothing at
  // all -- so the utterance that was just spoken is closed exactly as it was
  // before, and what the watch hears afterwards belongs to no utterance.
  const bool watch = may_watch && barge_watch_wanted();
  if (!watch) mic_->stop();
  chunk_.clear();
  mic_->drain(chunk_);
  if (!chunk_.empty()) eng_.stt->feed(chunk_.data(), (int)chunk_.size(), kMicRate);
  const std::string text = trim(eng_.stt->finish());
  // After the decode, so the watch's pre-roll starts empty and the words that
  // were just sent cannot be handed back to a later turn as a pre-roll.
  if (watch) begin_barge_watch();
  return text;
}

void VoiceSession::end_listening_and_send() {
  std::string text = finish_utterance(true);
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
  const std::string text = finish_utterance(false);
  std::lock_guard<std::mutex> l(mutex_);
  partial_ = text;
  ++dictated_seq_;
  set_state_locked(State::Idle);
  status_ = text.empty() ? "heard nothing. ready."
                         : "in the message box. edit it, then press Enter.";
}

void VoiceSession::discard_utterance() {
  const std::string dropped = finish_utterance(false);
  {
    std::lock_guard<std::mutex> l(mutex_);
    partial_.clear();
    set_state_locked(State::Idle);
  }
  // The length and not the words. Everything else this app logs about speech is
  // something the user chose to send; this is the one line about speech they
  // decided against, and writing it down would make the log a record of things
  // said in the room and not sent.
  if (!dropped.empty())
    rend::log::trace("mic: discarded {} characters, the keyboard had the turn", dropped.size());
}

void VoiceSession::toggle_mic() { set_mic_open(!mic_open_); }

void VoiceSession::talk_pressed() {
  if (mic_open_) return;  // the latch is already on; the release mutes it
  // Not while the conversation is being replaced: update() is handed to the
  // reset thread for that second, so a microphone opened here would never be
  // drained. This reaches begin_listening() directly rather than through
  // set_mic_open(), so it needs its own guard — and it is the one that catches
  // the SPACE hold, which never touches the button the panel disables.
  if (resetting()) return;
  State s;
  {
    std::lock_guard<std::mutex> l(mutex_);
    s = state_;
  }
  if (s == State::Loading || s == State::Failed) return;
  // Barge-in on the press, not on the release: the same rule as unmuting
  // mid-answer, and waiting for the release would mean talking over the reply
  // for as long as the gesture lasted before it was cut off.
  //
  // M17.2. **Not the handover's summary turn.** That turn is Thinking like any
  // other and is the one turn here that is not a reply to anybody: it is the
  // outgoing session writing the note its replacement reads. Cancelling it
  // costs the new session everything it was going to be told, and the user
  // pressing Talk asked for the microphone, not for that. The gesture still
  // opens the microphone below; what they then say is held by start_turn()
  // for the new session.
  if ((s == State::Thinking || s == State::Speaking) && !handing_off()) {
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
  // Not while the conversation is being replaced. update() is handed over to
  // the reset thread for that second, so a latch opened here would never be
  // drained and a hold would never be finalised; the one guard covers the
  // button, the SPACE gesture and toggle_mic() alike. Closing is always
  // allowed — it is what reset() itself does on the way in.
  if (open && resetting()) {
    mic_open_ = false;
    return;
  }
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

  // M12.1. **This is the user's own hand on the microphone**, and it is the
  // one thing that outranks the remembered state: the button, the SPACE
  // gesture, the bus and the panel all arrive here. Shutting it forgets the
  // memory for the rest of the wait, so no worker still out can reopen what
  // they closed; opening it re-arms, because "listening" is now the state they
  // chose. Neither of those is what close_latch_after_silence() does, and that
  // asymmetry is the whole of M12.1 -- see core/listen_restore.h.
  if (open) listen_restore_.user_opened_the_mic();
  else listen_restore_.user_shut_the_mic();

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

// M31. `workers_` is only ever touched from the frame loop (spawn, pause,
// stop, this) so no lock is needed here beyond what WorkerPool itself takes;
// see its header. Both are no-ops when `workers_` does not exist yet.
void VoiceSession::set_worker_model(std::string model_arg) {
  if (workers_) workers_->set_model(std::move(model_arg));
}

void VoiceSession::set_worker_chrome(bool on) {
  if (workers_) workers_->set_chrome(on);
}

// ---------------------------------------------------------------------------
// M12.1. Listening resumes after a worker reports -- if it was listening first
// ---------------------------------------------------------------------------
//
// The rule, and every case it has to get right, is in `core/listen_restore.h`;
// it is a separate header precisely so that the negative case -- mic shut on
// purpose, worker returns, mic stays shut -- is checked by `wake_word_test`
// rather than by somebody reproducing it with three background agents. This is
// only the frame loop's side: two edges to spot and one flag to drain.
//
// **The microphone is the frame loop's.** The report callback runs on the
// pool's thread and does nothing but set `worker_reported_`; every decision and
// every write to `mic_open_` happens here.
//
// `mic_open_` is set as a *level*, never by calling set_mic_open(). The Idle
// branch at the bottom of update() is what turns the level into an open
// capture device, and it is the same path that reopens the microphone after
// every reply -- which matters because a report usually arrives while the
// session is speaking the report itself. Setting the level mid-reply means the
// microphone opens when the reply ends, which is the behaviour, and it is the
// same lesson `relatch_after_reset_` learned the hard way.
void VoiceSession::tick_listen_restore(State s) {
  const auto now = std::chrono::steady_clock::now();
  const size_t out = (workers_ ? workers_->running() : 0) + sim_workers_out_;

  // The wait begins: workers went from none to some. `wait_began()` ignores
  // this while a wait is already open, so a second worker joins the first
  // one's wait rather than re-reading a latch that has since timed out.
  if (out > 0 && workers_out_prev_ == 0) {
    listen_restore_.wait_began(mic_open_);
    rend::log::info(
        "[listen-restore] a worker went out; the microphone was {} - a report {} reopen it",
        mic_open_ ? "LISTENING" : "SHUT", listen_restore_.armed() ? "WILL" : "will NOT");
  }
  workers_out_prev_ = out;
  if (out > 0) all_back_since_ = {};
  else if (all_back_since_.time_since_epoch().count() == 0) all_back_since_ = now;

  if (worker_reported_.exchange(false)) {
    const bool restore = listen_restore_.worker_reported();
    // The one line that proves this feature either way, and it says which of
    // the two it did rather than only the interesting one: "the mic stayed
    // shut" is the claim that is worth evidence, because it is the one a naive
    // "reopen on report" implementation would get wrong while looking fine.
    rend::log::info("[listen-restore] a worker reported; {} -> the microphone {}",
                    !listen_restore_.waiting()  ? "no wait was open"
                    : restore                   ? "it was listening when the wait began"
                                                : "it was shut when the wait began, or the user "
                                                  "has taken it since",
                    restore ? "is being reopened" : "STAYS SHUT");
    if (restore && !mic_open_ && !hold_ && s != State::Loading && s != State::Failed &&
        !resetting()) {
      mic_open_ = true;
      // Hold the wait open past this frame: the count may already be zero and
      // the grace below must not end the wait on the same frame a report used
      // it, or a second worker's report would find nothing to restore.
      all_back_since_ = {};
    }
  }

  // The wait ends once the count has held at zero for the grace. See
  // kWaitEndGraceSec for why it is not simply "the count reached zero".
  if (listen_restore_.waiting() && out == 0 && all_back_since_.time_since_epoch().count() != 0 &&
      std::chrono::duration<float>(now - all_back_since_).count() >= kWaitEndGraceSec) {
    listen_restore_.wait_ended();
    rend::log::info("[listen-restore] every worker is back; the remembered state is cleared");
  }
}

// ---------------------------------------------------------------------------
// M12.2. The wake phrase
// ---------------------------------------------------------------------------

std::string VoiceSession::wake_phrase_locked_copy() const {
  std::lock_guard<std::mutex> l(mutex_);
  return wake_phrase_;
}

std::string VoiceSession::wake_phrase() const { return wake_phrase_locked_copy(); }

void VoiceSession::set_wake_phrase(std::string phrase) {
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (wake_phrase_ == phrase) return;  // a level: called every frame
    wake_phrase_ = phrase;
  }
  // Said in the log rather than out loud. Changing this setting is a thing the
  // user did on purpose in a panel that is showing them the result; a spoken
  // confirmation would be the app narrating a button press.
  if (wake_phrase_armed(phrase)) {
    log("[wake] listening for \"" + phrase + "\" whenever the microphone is not latched");
  } else if (const std::string why = wake_phrase_problem(phrase); !why.empty()) {
    log("[wake] \"" + phrase + "\" is off: " + why);
  } else {
    log("[wake] off");
  }
}

// The passive capture goes up. Deliberately its own pair of functions rather
// than a flag inside begin_listening(): the two states want the microphone for
// opposite reasons and only one of them may hold the capture device at a time,
// and a single function with a mode is how that invariant gets lost.
void VoiceSession::begin_wake(const std::string& phrase) {
  if (wake_open_) return;
  eng_.stt->begin();
  mic_->discard();
  if (!mic_->start()) {
    set_status("mic failed to start");
    return;
  }
  wake_open_ = true;
  wake_segment_began_ = std::chrono::steady_clock::now();
  wake_open_at_ = wake_segment_began_;
  // A fresh calibration of the room every time the passive capture comes up,
  // exactly as begin_listening() takes one: the noise floor that was right an
  // hour ago is not evidence about the room now.
  wake_floor_ = 0.0f;
  wake_last_voice_ = {};
  wake_run_began_ = {};
  wake_decoding_ = false;
  wake_preroll_.clear();
  wake_said_.clear();
  // At info, and worded for somebody auditing the microphone rather than for
  // somebody debugging the feature: this line is the log's record that the
  // capture device was opened without anyone pressing anything, and why.
  rend::log::info(
      "[wake] passive listening is on for \"{}\" - the microphone is OPEN, every word is decoded "
      "on this machine, and nothing leaves the process until the phrase matches",
      phrase);
  set_status("listening for \"" + phrase + "\" only - nothing is sent until you say it");
}

void VoiceSession::end_wake() {
  if (!wake_open_) return;
  wake_open_ = false;
  mic_->stop();
  // Flushed and thrown away, not read. Everything decoded while passive is
  // dropped where it stands -- including the segment the wake phrase was
  // *in*. See wake_heard() for why that is a decision and not an oversight.
  (void)eng_.stt->finish();
  wake_said_.clear();
  rend::log::info("[wake] passive listening is off; the microphone is shut");
}

// A hypothesis, from the passive stream or from the harness. The whole of the
// privacy promise is in the three lines after the match.
void VoiceSession::wake_heard(const std::string& heard) {
  const std::string phrase = wake_phrase_locked_copy();
  if (!wake_match(heard, phrase)) return;
  // Review finding 24. **The phrase and the length, never the sentence.** This
  // line used to print the whole hypothesis the phrase was found in, which is
  // the one thing the passive state exists not to keep: everything decoded
  // while matching is dropped where it stands, including the segment the
  // phrase was in -- and then written to the log anyway, where it survives the
  // session. The phrase is the user's own configured string and the length is
  // what makes a false positive diagnosable, which is the same trade
  // discard_utterance() makes for the one other class of speech this app hears
  // and does not send.
  rend::log::info("[wake] MATCHED \"{}\" in {} characters heard - opening full listening", phrase,
                  heard.size());
  // **What woke the app is never sent.** end_wake() flushes the recogniser and
  // discards the result, and begin_listening() then starts a fresh stream on a
  // freshly discarded capture buffer, so the sentence the phrase was embedded
  // in does not survive into the turn. That is deliberate and it is the cap on
  // what a false positive can cost: the worst case is a microphone the user
  // can see is open, never a sentence they did not mean to send.
  //
  // It also means "Aria, what's the weather" does not carry the question over.
  // The user says the name, the app answers, they ask. That is the trade the
  // privacy decision buys, and it is the honest reading of "nothing reaches
  // Claude until the word matches".
  end_wake();
  listen_restore_.user_opened_the_mic();
  mic_open_ = true;
  // Said, not silent. Nobody pressed anything, so without a word from the app
  // the only evidence the phrase was heard is a button changing colour -- and
  // the user may well not be looking at the window, since not having to is the
  // point of a wake phrase. It goes through announce() rather than being
  // spoken here so that it takes the same gap-and-flush path a worker report
  // takes: the Idle branch speaks it first and opens the microphone after, so
  // the app never says a word into its own open microphone.
  announce(app_text(Msg::WakeHeard));
}

// One frame of passive matching, from the Idle branch.
void VoiceSession::tick_wake() {
  const std::string phrase = wake_phrase_locked_copy();
  if (!wake_phrase_armed(phrase)) {
    end_wake();
    return;
  }
  if (!wake_open_) begin_wake(phrase);
  if (!wake_open_) return;  // the capture device refused; begin_wake said so

  chunk_.clear();
  mic_->drain(chunk_);
  const auto now = std::chrono::steady_clock::now();
  if (!chunk_.empty()) {
    // The same gate, the same constants and the same calibration the Listening
    // branch runs -- deliberately, because two gates would eventually disagree
    // about whether the room is quiet and the wake word would stop working in
    // whichever room they disagreed in.
    const float level = rms(chunk_);
    const float open_for = std::chrono::duration<float>(now - wake_open_at_).count();
    float gate = std::max(wake_floor_ * kGateOverFloor, kGateAbsMin);
    if (open_for < kCalibrateSec) {
      wake_floor_ = std::min(std::max(wake_floor_, level), kFloorMax);
      gate = std::max(wake_floor_ * kGateOverFloor, kGateAbsMin);
    } else if (level < gate) {
      wake_floor_ += (level - wake_floor_) * kFloorRate;
    }
    // The gate, and then the *run* — see kWakeOnsetSec. A single loud frame is
    // not somebody talking, and treating it as if it were is what cost this
    // feature a third of a core in an empty room.
    if (level > gate) {
      if (wake_run_began_.time_since_epoch().count() == 0) wake_run_began_ = now;
      if (std::chrono::duration<float>(now - wake_run_began_).count() >= kWakeOnsetSec)
        wake_last_voice_ = now;
    } else {
      wake_run_began_ = {};
    }
    const bool decoding =
        std::chrono::duration<float>(now - wake_last_voice_).count() < kWakeTailSec;

    if (!decoding) {
      // **The whole of the CPU fix.** A quiet room is not decoded, so an idle
      // machine with a wake phrase set costs what an idle machine costs. The
      // audio is kept for a fraction of a second so that the gate opening does
      // not cost the first syllable; see kWakePrerollSec.
      wake_preroll_.insert(wake_preroll_.end(), chunk_.begin(), chunk_.end());
      const size_t cap = size_t(kWakePrerollSec * kMicRate);
      if (wake_preroll_.size() > cap)
        wake_preroll_.erase(wake_preroll_.begin(), wake_preroll_.end() - cap);
      // The gate has just shut on something that was not the phrase. Drop the
      // segment and start a clean one, so the next thing said is decoded on
      // its own rather than appended to a minute of half-heard room noise.
      if (wake_decoding_) {
        const auto t0 = std::chrono::steady_clock::now();
        (void)eng_.stt->finish();
        eng_.stt->begin();
        wake_decode_ms_ +=
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        wake_segment_began_ = now;
        wake_said_.clear();
        wake_decoding_ = false;
      }
    } else {
      // The measurement `AII_WAKE_DEBUG` prints, and the reason it counts
      // *audio actually decoded* rather than wall-clock seconds: with the gate
      // in front of it the interesting number is no longer "can it keep up"
      // but "how much of the day does it run at all".
      const auto t0 = std::chrono::steady_clock::now();
      if (!wake_preroll_.empty()) {
        eng_.stt->feed(wake_preroll_.data(), (int)wake_preroll_.size(), kMicRate);
        wake_audio_sec_ += float(wake_preroll_.size()) / float(kMicRate);
        wake_preroll_.clear();
      }
      eng_.stt->feed(chunk_.data(), (int)chunk_.size(), kMicRate);
      const std::string p = eng_.stt->partial();
      wake_decode_ms_ +=
          std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
      wake_audio_sec_ += float(chunk_.size()) / float(kMicRate);
      wake_decoding_ = true;

      if (p != wake_said_) {
        wake_said_ = p;
        wake_heard(p);
        if (!wake_open_) return;  // matched; the latch has it now
      }
      // A finished utterance that was not the phrase: drop it and start again.
      // The `finish()` result is discarded, which is the point.
      if (eng_.stt->is_endpoint()) {
        (void)eng_.stt->finish();
        eng_.stt->begin();
        wake_segment_began_ = now;
        wake_said_.clear();
      }
    }
  }
  // A room that never endpoints -- continuous noise, a fan, a television --
  // would otherwise hold one stream open forever. See kWakeSegmentSec.
  if (std::chrono::duration<float>(now - wake_segment_began_).count() >= kWakeSegmentSec) {
    (void)eng_.stt->finish();
    eng_.stt->begin();
    wake_segment_began_ = now;
    wake_said_.clear();
  }
  if (std::getenv("AII_WAKE_DEBUG")) {
    static auto last = std::chrono::steady_clock::time_point{};
    if (std::chrono::duration<float>(now - last).count() >= 1.0f) {
      last = now;
      rend::log::info("[wake-cpu] {:.0f} ms of decoding for {:.1f} s of audio past the gate "
                      "(rtf {:.3f}, open {:.0f} s, floor {:.4f}) heard=\"{}\"",
                      wake_decode_ms_, wake_audio_sec_,
                      wake_audio_sec_ > 0.0f ? wake_decode_ms_ / (wake_audio_sec_ * 1000.0f) : 0.0f,
                      std::chrono::duration<float>(now - wake_open_at_).count(), wake_floor_,
                      wake_said_);
    }
  }
}

// ---------------------------------------------------------------------- M18.2
//
// ## The barge watch
//
// The microphone stays up while the app speaks, and what it hears decides one
// thing only: whether to stop speaking. It is never decoded, never shown and
// never sent. `core/barge_policy.h` owns the arithmetic and
// `docs/bargein-measurements.md` owns the numbers in it; what is here is the
// wiring, and the wiring has three jobs the policy cannot do for itself.
//
//  1. **Be a third owner of one capture device, without breaking the other
//     two.** `begin_listening()` and `begin_wake()` are the other two, and
//     every path that takes the device for one of them calls the other's
//     closer. This one joins that discipline: `end_barge_watch()` is
//     idempotent, `update()` closes the watch on any frame the session is not
//     Thinking or Speaking, and `begin_barge_watch()` closes the wake state
//     first. Nothing else starts or stops `mic_`.
//
//  2. **Not be `Listening`.** The listen timeout, M12.1's restore, M12.2's
//     wake phrase and M3.15's handoff each have "the microphone is shut while
//     the app speaks" written into their comments, and the trace line in
//     `begin_listening()` is the evidence for it. So the watch is a flag and
//     not a state: `state_` stays Thinking or Speaking, none of those four
//     sees a reopened microphone, and the one trace line that says a
//     microphone opened *and what it hears goes to Claude* still means that.
//
//  3. **Not truncate the reply.** On a fire this clears the speech queue and
//     sets `barged_`, which the splitter in run_turn() honours exactly as it
//     honours `muted_`. `cancel_` is untouched, so the turn thread goes on
//     streaming and the whole reply still lands in the panel. That is the
//     difference between this and the SPACE barge-in that predates it, and it
//     is the thing the user asked for.
bool VoiceSession::barge_watch_wanted() const {
  if (!loaded_ || load_failed_) return false;
  if (!mic_ || !speech_) return false;
  // A Talk press is the user's own finger on the microphone; a gesture ends
  // with the device shut and nothing here may reopen it under their hand.
  if (hold_) return false;
  // The conversation is being replaced. `update()` is handed to the reset
  // thread for that second, so a device opened here would never be drained;
  // and during a handover M17.2 keeps the microphone shut on purpose, because
  // anything said into it belongs to a child that does not exist yet.
  if (resetting() || handing_off()) return false;
  // The latch is the ordinary case. A wake phrase is the other one, and it
  // fires differently: see the hand-over at the bottom of tick_barge().
  return mic_open_ || wake_phrase_armed(wake_phrase_locked_copy());
}

void VoiceSession::begin_barge_watch() {
  if (barge_watch_) return;
  // One device, one owner. The wake state has no claim on the microphone
  // while a reply is being spoken, and `update()`'s own guard would end it a
  // frame later anyway; doing it here means the two are never both up even
  // for the frame in between.
  end_wake();
  // A no-op when `finish_utterance()` has just left the device running, which
  // is the ordinary path and is why a barge watch costs no gap in capture at
  // all: `MicIn::start()` returns early when it is already running, so the
  // samples spanning the end of the user's utterance and the start of the
  // reply are one unbroken stream.
  if (!mic_->start()) {
    set_status("mic failed to start");
    return;
  }
  barge_watch_ = true;
  barge_.reset();
  barge_open_at_ = std::chrono::steady_clock::now();
  barge_floor_ = 0.0f;
  barge_preroll_.clear();
  barge_fired_at_ = -1.0f;
  // **`barged_` is deliberately not cleared here.** It belongs to the reply,
  // not to the watch, and run_turn() clears it at the top. Clearing it here
  // would reopen a window of a frame or two in which the turn that was just
  // barged could enqueue another sentence before start_turn()'s join has
  // torn it down -- the reply would go quiet and then say one more thing.
  //
  // Worded for somebody auditing the microphone rather than for somebody
  // debugging this, the same as the wake state's line: it says the device is
  // open, why, and what cannot happen to what it hears.
  rend::log::info("[barge] the microphone stays open while the app speaks - it is watched for "
                  "the sound of you interrupting and nothing heard here is ever decoded or sent");
}

void VoiceSession::end_barge_watch(bool handing_over) {
  if (!barge_watch_) return;
  barge_watch_ = false;
  // `handing_over` is the fire path giving the device straight to
  // `begin_listening()`. Stopping it there and starting it again a line later
  // would drop the audio in between, which is exactly the audio the user is
  // speaking.
  if (!handing_over) mic_->stop();
  // One line per reply, and never a word of what was heard: the leak the reply
  // was measured to produce, the bar that leak set, and whether anybody
  // cleared it. This is the harness for M18.4's tuning and it is the only way
  // to tell "it did not fire" from "it never armed".
  //
  // M18.4 added the second half of the line: the loudest thing the armed rule
  // saw, the longest run over the bar and when it began, and how many runs got
  // past kBargeNoticeSec. "Did not fire" then reads one of three ways -- nothing
  // reached the bar (peak below threshold), something reached it and was
  // refused by the onset (longest run under 0.30 s), or the bar was raised
  // above the person by the leak (threshold well over the gate) -- and each of
  // those is a different knob.
  if (std::getenv("AII_BARGE_DEBUG")) {
    const float gate = std::max(barge_floor_ * kGateOverFloor, kGateAbsMin);
    std::string verdict;
    if (barge_.learning()) {
      verdict = "never armed (less than a second of audible reply)";
    } else {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "%s; armed peak %.5f, longest run %.2f s beginning at %.1f s, %d run%s over "
                    "%.2f s; at the gate alone the longest run would have been %.2f s",
                    barge_fired_at_ >= 0.0f
                        ? ("FIRED at " + std::to_string(barge_fired_at_) + " s").c_str()
                        : "did not fire",
                    barge_.armed_peak(), barge_.longest_run_sec(),
                    barge_.longest_run_at_sec() < 0.0f ? 0.0f : barge_.longest_run_at_sec(),
                    barge_.runs_noticed(), barge_.runs_noticed() == 1 ? "" : "s",
                    kBargeNoticeSec, barge_.gate_longest_run_sec());
      verdict = buf;
    }
    rend::log::info(
        "[barge-debug] reply watched {:.1f} s: learned leak {:.5f}, gate {:.5f}, threshold "
        "{:.5f}, {}",
        barge_.elapsed_sec(), barge_.leak_p99(), gate, barge_.threshold(gate), verdict);
  }
  barge_preroll_.clear();
}

// One frame of the watch, from the Thinking and Speaking branches of update().
void VoiceSession::tick_barge() {
  if (!barge_watch_wanted()) {
    end_barge_watch();
    return;
  }
  if (!barge_watch_) begin_barge_watch();
  if (!barge_watch_) return;  // the capture device refused; begin_barge_watch said so

  chunk_.clear();
  mic_->drain(chunk_);
  if (chunk_.empty()) return;

  // The same gate, the same constants and the same calibration the Listening
  // branch and the wake state run -- for the third time and for the same
  // reason both of those give: a second opinion about whether the room is
  // quiet would eventually disagree with the first, and the day it did, this
  // would silence a reply nobody had interrupted.
  //
  // With one difference, and it is the investigation's: **the floor is not
  // adapted while the speaker is playing.** Everywhere else the floor tracks
  // the quiet frames so the gate follows the room; here the quiet frames are
  // quiet *because the app's own voice is what is in them*, and letting them
  // teach the floor would tune the gate to the reply rather than to the room.
  const auto now = std::chrono::steady_clock::now();
  const float level = rms(chunk_);
  const float spk = speaker_ ? speaker_->level() : 0.0f;
  const float open_for = std::chrono::duration<float>(now - barge_open_at_).count();
  float gate = std::max(barge_floor_ * kGateOverFloor, kGateAbsMin);
  if (open_for < kCalibrateSec) {
    barge_floor_ = std::min(std::max(barge_floor_, level), kFloorMax);
    gate = std::max(barge_floor_ * kGateOverFloor, kGateAbsMin);
  } else if (level < gate && spk <= kBargeSpeakerActive) {
    barge_floor_ += (level - barge_floor_) * kFloorRate;
  }

  // Kept before the verdict, so that a frame which fires is itself in the
  // pre-roll. Bounded at kBargePrerollSec and trimmed again on the way out.
  barge_preroll_.insert(barge_preroll_.end(), chunk_.begin(), chunk_.end());
  const size_t cap = size_t(kBargePrerollSec * kMicRate);
  if (barge_preroll_.size() > cap)
    barge_preroll_.erase(barge_preroll_.begin(), barge_preroll_.end() - cap);

  // `dt` from the samples rather than from the wall clock: the frame loop's
  // period is whatever the renderer gives it, and the only honest duration of
  // a block of audio is how much audio is in it. It also makes the rule's
  // behaviour identical whether the app is running at 60 fps or at 6.
  const float dt = float(chunk_.size()) / float(kMicRate);
  const BargeVerdict verdict = barge_.frame(level, gate, spk, dt);
  // M18.4. Each run that got past kBargeNoticeSec and was refused, as it
  // ends, with the bar it cleared and the peak it reached: a person who was
  // told "no" and a keypress that nearly was not look the same in the summary
  // line, and this is where they are told apart. Debug-only and never a word
  // of what was heard.
  if (verdict != BargeVerdict::Fire && barge_.run_just_ended_sec() > 0.0f &&
      std::getenv("AII_BARGE_DEBUG")) {
    rend::log::info(
        "[barge-debug] {:.2f} s over the bar at {:.1f} s and refused (bar {:.5f}, onset "
        "needs {:.2f} s; speaker {:.4f})",
        barge_.run_just_ended_sec(), barge_.elapsed_sec() - barge_.run_just_ended_sec(),
        barge_.threshold(gate), kBargeOnsetSec, spk);
  }
  if (verdict != BargeVerdict::Fire) return;

  barge_fired_at_ = barge_.elapsed_sec();
  const float run = barge_.voiced_run_sec();
  // The voice stops here and the text does not. `speech_->clear()` drops what
  // is queued and what is playing; `barged_` stops the splitter handing the
  // queue anything more, which is the half the old silence() never had. See
  // the member's comment for why `cancel_` stays where it is.
  barged_ = true;
  speech_->clear();
  rend::log::info("[barge] somebody is talking over the reply {:.2f} s in; the voice stops here "
                  "and the text keeps arriving",
                  barge_fired_at_);

  if (!mic_open_) {
    // A wake phrase is armed and the latch is not. **Opening full listening
    // here would hand the conversation an utterance from somebody who never
    // said the phrase**, which is the one thing M12.2 promises cannot happen,
    // so the reply is silenced and the microphone goes back to passive
    // matching on the next Idle frame. The pre-roll is dropped with it, for
    // the same reason wake_heard() throws away the segment the phrase was in:
    // audio captured before the phrase is not the user's turn.
    end_barge_watch();
    set_status("stopped speaking - say the wake phrase when you want me");
    return;
  }

  // M18.3. The onset run, and only the onset run, goes to the fresh stream:
  // everything before it is the reply's own leakage and the room, and
  // `docs/bargein-measurements.md` has the recogniser decoding "Sorry" out of
  // exactly that. `end_barge_watch(true)` hands the device over without
  // stopping it, so the audio between this frame and begin_listening()'s first
  // one is not lost.
  const size_t want = barge_preroll_samples(barge_preroll_.size(), run, kMicRate);
  std::vector<float> pre(barge_preroll_.end() - want, barge_preroll_.end());
  end_barge_watch(true);
  begin_listening(std::move(pre));
}

// ---------------------------------------------------------------------------
// The two M12 harnesses
// ---------------------------------------------------------------------------
//
// Both are environment variables and both exist for the same reason the
// `--clip`, `AII_MIC_FACE` and `AII_TALK_DEBUG` hatches do: the behaviour being
// built is only reachable by a person at a desk doing something slow, and
// "verified by looking" is the standard this project holds itself to.
//
//   AII_WORKER_SIM="2:out;9:back"      M12.1's two edges, without a real
//                                      `claude` child and the minutes and the
//                                      usage one costs. It drives the *same*
//                                      ListenRestore, the same flag and the
//                                      same restore code as a real worker;
//                                      what it does not exercise is the pool
//                                      itself, which is another agent's file
//                                      and is not what M12.1 changed.
//                                      Events: `out`, `back`, and `shut` /
//                                      `open`, which are the user working the
//                                      microphone button mid-wait.
//
//   AII_WAKE_SAY="4:hey aria"          A hypothesis pushed through the wake
//                                      gate, so the match and the handover to
//                                      full listening can be shown without a
//                                      person speaking into the user's real
//                                      microphone. It does **not** exercise
//                                      the acoustic decode; that is the
//                                      recogniser's job and it is unchanged.
//
// Times are seconds from the first frame after the engines are up.
void VoiceSession::tick_harnesses() {
  if (harness_began_.time_since_epoch().count() == 0) {
    harness_began_ = std::chrono::steady_clock::now();
    const auto parse = [](const char* spec) {
      std::vector<std::pair<float, std::string>> out;
      if (!spec) return out;
      std::string s(spec);
      size_t i = 0;
      while (i < s.size()) {
        const size_t end = std::min(s.find(';', i), s.size());
        const std::string item = s.substr(i, end - i);
        const size_t colon = item.find(':');
        if (colon != std::string::npos)
          out.push_back({(float)std::atof(item.substr(0, colon).c_str()),
                         trim(item.substr(colon + 1))});
        i = end + 1;
      }
      return out;
    };
    worker_script_ = parse(std::getenv("AII_WORKER_SIM"));
    wake_script_ = parse(std::getenv("AII_WAKE_SAY"));
    if (!worker_script_.empty() || !wake_script_.empty())
      rend::log::info("[m12-harness] {} worker event(s), {} scripted utterance(s)",
                      worker_script_.size(), wake_script_.size());
  }
  if (worker_script_.empty() && wake_script_.empty()) return;
  const float t =
      std::chrono::duration<float>(std::chrono::steady_clock::now() - harness_began_).count();
  while (!worker_script_.empty() && worker_script_.front().first <= t) {
    const std::string ev = worker_script_.front().second;
    worker_script_.erase(worker_script_.begin());
    rend::log::info("[m12-harness] t={:.1f}s {}", t, ev);
    if (ev == "out") {
      ++sim_workers_out_;
    } else if (ev == "back") {
      if (sim_workers_out_) --sim_workers_out_;
      worker_reported_.store(true, std::memory_order_release);
    } else if (ev == "shut") {
      set_mic_open(false);
    } else if (ev == "open") {
      set_mic_open(true);
    }
  }
  while (!wake_script_.empty() && wake_script_.front().first <= t) {
    const std::string said = wake_script_.front().second;
    wake_script_.erase(wake_script_.begin());
    rend::log::info("[m12-harness] t={:.1f}s heard \"{}\"", t, said);
    wake_heard(said);
  }
}

void VoiceSession::say(const std::string& text) {
  State s;
  {
    std::lock_guard<std::mutex> l(mutex_);
    s = state_;
  }
  if (s == State::Loading || s == State::Failed) return;
  // Typed while the microphone was open. This used to return here, so a
  // message written during a latched listen could be sent only by closing the
  // mic first -- the keyboard was locked out of the app for as long as the
  // voice channel was up, which is backwards: the two are alternatives, not
  // modes, and the panel already lets speech and typing share the field.
  //
  // Frame-loop only, like everything that touches the recogniser: both callers
  // are on it (the panel directly, the bus through apply_pending()).
  if (s == State::Listening) {
    discard_utterance();
  } else if (s != State::Idle && !handing_off()) {
    // M17.2, the same exception talk_pressed() makes: the handover's summary
    // turn is not a reply being talked over, and cancelling it throws away the
    // note the next session reads. The typed text is held for that session by
    // start_turn() instead.
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
  // Review finding 19. A load that failed is not retried from here, and here
  // is every frame: update() calls this unconditionally, so a failure that
  // cleared `ja_started_` on its own would respawn the loader sixty times a
  // second against a VOICEVOX that is not going to appear. The retry is the
  // *user* asking again -- set_languages() clears this on the edge where the
  // checkbox comes back on -- which is the gesture finding 19 says must work
  // and which, before this, did nothing at all.
  if (ja_failed_) return;
  if (ja_started_.exchange(true)) return;     // built at startup, or already loading, or done
  // The previous attempt's thread, which is finished (it cleared `ja_started_`
  // as its last act) but still joinable. Assigning over a joinable std::thread
  // calls terminate(), so this is not tidiness: it is the line that makes the
  // retry safe.
  if (ja_loader_.joinable()) ja_loader_.join();
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
      // M13.2. The Japanese secondaries could not be validated until now:
      // there was no model to check a style against.
      apply_voice_lists(cfg_, eng_, *speech_, [this](const std::string& s) { log(s); });
      rend::log::info("japanese voice loaded on demand in {:.2f} s",
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
    } else {
      ja_error_ = err;
      // Review finding 19. **Cleared, and in this order.** `ja_started_` means
      // "a load is under way or has succeeded", and a failure is neither; left
      // set, it made the guard above permanent and every later attempt -- the
      // user unticking Japanese and ticking it again, which is the only thing
      // the settings surface offers them to do about a failure -- a silent
      // no-op. `ja_failed_` is raised first so the frame loop's call cannot
      // slip through the cleared flag before the reason for refusing it is
      // visible; the retry lowers `ja_failed_` again on the user's own edge.
      ja_failed_ = true;
      ja_started_.store(false);
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
  // Review finding 19, the other half. This is the edge -- the checkbox has
  // moved and Japanese is wanted -- so a previous failure stops standing in
  // the way. Off-and-on-again is the whole of the user interface a failed
  // voice has (the settings surface shows the error and offers nothing else),
  // and until this it was a gesture that did nothing: `ja_started_` was left
  // set by the failure, so every retry returned at the first line.
  if (sel.japanese && ja_failed_) {
    // The flag and not the message: `ja_error_` is a plain string the loader
    // thread writes and snapshot() reads whenever `ja_failed_` is up, so
    // clearing it from here would be a write racing that read for no gain.
    // It is only ever shown behind the flag, and the next failure overwrites
    // it before raising the flag again.
    ja_failed_ = false;
    log("[lang] the Japanese voice failed to load last time; trying again");
  }
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

size_t VoiceSession::pause_workers() {
  if (!workers_) return 0;
  const size_t running = workers_->running();
  workers_->pause_all();
  return running;
}

void VoiceSession::stop() {
  // The count is taken before the pause, because after it there is nothing
  // running to count. Zero means the user gets the plain sentence: "stopped (0
  // worker(s) paused)" is the app reporting on a thing that did not happen.
  const size_t paused_workers = pause_workers();
  stop_reply_and_mic(paused_workers
                         ? "stopped (" + std::to_string(paused_workers) + " worker(s) paused). ready."
                         : "stopped. ready.");
}

void VoiceSession::stop_reply_and_mic(const std::string& stopped_status) {
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
  // M12.1. Stop is the user saying stop. A worker that reports afterwards must
  // not undo it -- and Stop pauses every running worker anyway, so the reports
  // this is guarding against are the ones already in flight.
  //
  // M17.1: the restarts reach this line too, and they no longer pause
  // anything, so a worker running across a settings restart or a handoff will
  // finish and report into the new session. It must not reopen the microphone
  // there either. The microphone comes back on a restart through
  // `relatch_after_reset_`, which is the user's latch and remembered
  // separately; this forgets only the "a worker is still out, so reopen when
  // it lands" memory, which is exactly the memory that is about to be wrong.
  listen_restore_.user_shut_the_mic();
  end_wake();
  // M18.2. Stop is the user taking the floor back by hand, so the watch that
  // was listening for them doing it by voice goes too. It is the same line
  // end_wake() is, in the same place, for the same reason: this function is
  // reached from Stop, from Reset and from both restarts, and the microphone
  // must be shut on the far side of all four.
  end_barge_watch();
  if (s == State::Thinking || s == State::Speaking) {
    cancel_ = true;
    speech_->clear();
    set_state(State::Idle);
    set_status(stopped_status);
  } else if (s == State::Listening) {
    mic_->stop();
    std::lock_guard<std::mutex> l(mutex_);
    partial_.clear();
    set_state_locked(State::Idle);
    status_ = "cancelled. ready.";
  }
}

bool VoiceSession::quitting_ok() const { return !turn_running_ && !resetting(); }

void VoiceSession::reset() {
  // Nothing to reset before there is a client, and nothing that could be
  // rebuilt after the load has failed: a failed load may never have reached
  // build_llm() at all, and "restart the child" is not an answer to "the
  // recogniser did not load".
  if (!loaded_ || load_failed_) return;
  if (resetting()) return;
  begin_restart(RestartReason::Reset);
}

bool VoiceSession::apply_llm_settings(const ToolPolicy& tools, const std::string& model_arg) {
  // The same two refusals reset() makes, and for the same reasons. A setting
  // saved while the recogniser is still coming up is not lost: it is in the
  // file, and the child this run is about to build reads it.
  if (!loaded_ || load_failed_) return false;
  // Already restarting. This is the whole of the "a hand running down three
  // tick boxes must not start three children" answer on this side — the
  // caller debounces so that the common case never gets here at all, and this
  // is the backstop for the ways in that do not, including a second change
  // made during the second the rebuild takes. That change is not lost either:
  // it is still in the panel, still differs from what comes up in force, and
  // the caller offers it again on the next frame.
  if (resetting()) return false;
  // Nothing to do. The comparison is against the *running* child rather than
  // against the last request, so a box ticked and unticked again before the
  // restart fires resolves to no restart at all, and a picker moved back to
  // where it started costs nothing.
  if (cfg_.tools == tools && cfg_.model_override == model_arg) return false;
  // The first write to `cfg_` after load(), and the one that makes this more
  // than a reset: `build_llm` composes the system prompt from `cfg_.tools` and
  // puts `cfg_.model_override` on the command line, so the new child is the
  // new settings by construction and there is no second place that has to be
  // told. Written here, on the frame loop, before the release store in
  // begin_restart(); the restart thread's acquire is what publishes it.
  cfg_.tools = tools;
  cfg_.model_override = model_arg;
  log("[restart] settings changed: " + model_label(model_arg) + ", tools: " + tool_summary(tools));
  begin_restart(RestartReason::Settings);
  return true;
}

void VoiceSession::begin_restart(RestartReason why) {
  restart_reason_ = why;
  // Reap the previous reset thread. Joinable here means finished, because
  // `resetting_` is false and only the thread itself clears it.
  if (reset_.joinable()) reset_.join();
  // Noted before stop() drops it, and given back when the new child is up.
  // A held Talk press is deliberately *not* remembered: a gesture in flight is
  // in flight, and reproducing one the user is no longer making would be the
  // app pressing its own button. See `relatch_after_reset_`.
  relatch_after_reset_ = mic_open_;
  // Everything Stop does *to this app*, and for the same reasons — a reply
  // half-spoken into a conversation that is about to stop existing, a latch
  // that would reopen the microphone on the next frame, a Talk press whose
  // release would finalise an utterance into a session that never heard its
  // beginning.
  //
  // M17.1, review finding 3. What it deliberately no longer does to anything
  // *else* is pause the workers. A worker is a separate process doing work the
  // user asked for, and none of the three reasons above applies to it: it is
  // not talking, it does not hold the microphone, and its report is a
  // self-contained `<context>` block that the new child can deliver having
  // never heard the conversation that started it. Only Reset pauses, and only
  // because Reset is the user ending this conversation rather than the app
  // replacing its own child — see stop() and the reset() header.
  if (why == RestartReason::Reset) {
    const size_t paused_workers = pause_workers();
    if (paused_workers)
      log("[reset] " + std::to_string(paused_workers) + " running worker(s) paused");
  } else if (const size_t left = workers_ ? workers_->running() : 0) {
    // Said out loud in the log, because "nothing happened" is exactly the kind
    // of fix that cannot be seen afterwards. This line is the evidence that a
    // handoff ran with work still going.
    log(std::string(why == RestartReason::Handoff ? "[handoff] " : "[restart] ") +
        std::to_string(left) + " worker(s) left running across the restart");
  }
  stop_reply_and_mic("stopped. ready.");
  // stop() only raises this for a turn it found Thinking or Speaking. Raised
  // unconditionally here so that a turn in any other shape still unwinds
  // rather than being waited on; run_reset() clears it after the join.
  cancel_ = true;
  switch (why) {
    case RestartReason::Settings: log("[restart] restarting Claude on the new settings"); break;
    case RestartReason::Handoff: log("[handoff] handing the conversation over to a fresh session"); break;
    case RestartReason::Reset: log("[reset] clearing the conversation"); break;
  }
  {
    std::lock_guard<std::mutex> l(mutex_);
    // A status line, not an app_strings entry: that table is for sentences the
    // app *speaks*, and by its own terms excludes the window's chrome. Nothing
    // about a reset is ever spoken — a new session announcing its own amnesia
    // would be the app talking about itself, in a voice that has by definition
    // heard nothing. The same goes for a restart the settings surface asked
    // for: the surface is where the user is looking and where it is said.
    //
    // M3.15 is the one exception in this file to "nothing about a restart is
    // ever spoken", and it does not weaken the rule: the handoff's sentence is
    // said *before* this point and is not about the restart. It is a warning
    // that the app is about to go quiet, in the user's own words, and by the
    // time this status line is written it has already been heard.
    switch (why) {
      case RestartReason::Settings: status_ = "restarting Claude on the new settings..."; break;
      case RestartReason::Handoff: status_ = "housekeeping - handing over to a fresh session..."; break;
      case RestartReason::Reset: status_ = "clearing the conversation..."; break;
    }
  }
  // Set on this thread, before the thread that reads it exists. See the
  // declaration: this store is the fence, not a lock.
  resetting_.store(true, std::memory_order_release);
  reset_ = std::thread([this] { run_reset(); });
}

void VoiceSession::run_reset() {
  // The cancelled turn still owns `eng_.llm` until it unwinds. Joining it here
  // rather than on the frame loop is the whole reason this is a thread:
  // ClaudeCodeClient::turn() returns only once the CLI acknowledges the
  // interrupt, which takes as long as it takes.
  if (turn_.joinable()) turn_.join();
  turn_running_ = false;
  cancel_ = false;

  // One child at a time, by construction. `unique_ptr::reset()` runs
  // ClaudeCodeClient's destructor to completion — it closes the pipe, which is
  // the CLI's EOF, waits three seconds and terminates if it has not gone — so
  // the old conversation is over before build_llm() creates the next process.
  // Doing it in one step (`build_llm` overwriting the pointer) would have had
  // both alive at once, and two `claude` processes on one subscription is
  // exactly the state this must never leave behind.
  eng_.llm.reset();
  std::string err;
  const bool ok = build_llm(cfg_, eng_, [this](const std::string& s) { log(s); }, &err);
  if (ok && eng_.llm) {
    // Per-client, so it does not survive the swap. Same callback as load().
    eng_.llm->set_on_activity([this](const std::string& what) {
      const bool web = what == "WebSearch" || what == "WebFetch";
      rend::log::info("[tool] {}", what);
      set_status(web ? "searching the web..." : "thinking... (" + what + ")");
    });
  }

  // A fresh context window has loaded nothing. Without this the inspector goes
  // on claiming prompts are in Claude's head that the new child has never
  // seen — which is the one thing that window exists not to do. Safe from this
  // thread for the same reason it is safe from the turn thread: both of the
  // objects it reads have exactly one writer at a time, and the turn thread is
  // joined above.
  injector_.clear_session();
  publish_inventory();

  {
    std::lock_guard<std::mutex> l(mutex_);
    // The transcript goes with the context. Keeping it was the alternative and
    // it is the worse one: a transcript is read as "what this conversation has
    // said", and leaving one on screen that the AI can no longer see would
    // manufacture the exact confusion the button exists to remove — the user
    // would refer back to it and be told, correctly and bafflingly, that
    // Claude has no idea. An empty chat cannot be misread.
    //
    // Nothing is written in its place. A marker line would have to be `user`
    // or `assistant`, and putting the app's own words in either mouth is a lie
    // in the one record that is supposed to be verbatim. The status line below
    // is the panel's channel for "what just happened", and it says it.
    // M3.12. The transcript goes for a settings restart too, and the argument
    // above is the whole reason: a transcript the new child cannot see is a
    // trap whatever ended the old one. The one difference is how surprising
    // it is — pressing a button called Reset announces the loss, nudging a
    // tick box does not — and that is answered where the surprise would
    // happen rather than by keeping a transcript that lies: the control says
    // what it costs before it is touched (its tooltip), while it is being
    // applied (the amber line under it) and after (the status line below).
    lines_.clear();
    partial_.clear();
    // `usage_` is deliberately kept: the subscription window is an account
    // fact, not a conversation one, and a reset does not give any of it back.
    if (ok) {
      // What is running, recorded only now. Before this line the surface was
      // still drawing the old child's model and grant, which is what it was
      // still talking to.
      model_in_force_ = cfg_.model_override;
      tools_in_force_ = cfg_.tools;
      // Two sentences for the same event, because the event is not the same.
      // Reset was asked for as an act of forgetting and says so. A settings
      // restart was asked for as "use this model" and the forgetting is its
      // price, so it names the thing that was wanted first and the price
      // second — "Claude remembers nothing of it" as the whole of the news
      // would read as an answer to a question the user did not ask.
      //
      // M3.15 is the third, and it is the only one of the three that is not a
      // loss: the user asked for none of it, was told it was coming, and the
      // session that comes up knows where the conversation had got to. So it
      // says what carried over rather than what did not — and when nothing
      // did, it says that instead, because a fresh session claiming to
      // remember the direction it has not been told is the worst of the three.
      switch (restart_reason_) {
        case RestartReason::Settings:
          restart_note_ = "new settings applied - Claude restarted on " +
                          model_label(cfg_.model_override) +
                          " and holds nothing of the conversation before it.";
          break;
        case RestartReason::Handoff:
          restart_note_ = handoff_summary_.empty()
                              ? "housekeeping done - Claude started fresh, with nothing "
                                "carried over."
                              : "housekeeping done - Claude carried over its own note on "
                                "where we had got to, but none of the wording.";
          break;
        case RestartReason::Reset:
          restart_note_ = "conversation cleared - Claude remembers nothing of it.";
          break;
      }
      status_ = restart_note_ + " ready.";
      set_state_locked(State::Idle);
    } else {
      // There is no AI any more and no way to get one, so this is Failed
      // rather than a status line over a session that cannot answer.
      status_ = "could not start a new Claude session: " + err;
      set_state_locked(State::Failed);
    }
  }
  if (restart_reason_ == RestartReason::Settings)
    log(ok ? "[restart] new session started on " + model_label(cfg_.model_override) +
                 ", tools: " + tool_summary(cfg_.tools) +
                 "; Claude remembers nothing from before"
           : "[restart] could not start a new session: " + err);
  else if (restart_reason_ != RestartReason::Handoff)
    log(ok ? "[reset] new session started; Claude remembers nothing from before"
           : "[reset] could not start a new session: " + err);

  // M3.15. The note crosses here and nowhere else.
  //
  // Only a handoff carries one, and only a handoff that produced a new child.
  // A *reset* must not: it was asked for as an act of forgetting, and handing
  // the new session a summary of the conversation it was told to forget would
  // be the app overruling the button. A settings restart must not either, for
  // the reason M3.12 spent a paragraph on — the user asked for a model, the
  // conversation is the price, and quietly softening the price would make the
  // tooltip that named it a lie.
  //
  // The floor and the one-line warning are cleared whatever ended the old
  // child, because both are facts about a session and there is a new one.
  handoff_floor_ = -1.0;
  handoff_warned_ = false;
  if (restart_reason_ == RestartReason::Handoff) {
    if (ok) {
      carry_over_ = std::move(handoff_summary_);
      log(carry_over_.empty()
              ? "[handoff] new session started with nothing carried over"
              : "[handoff] new session started; the note rides in with its first turn (" +
                    std::to_string(carry_over_.size()) + " bytes)");
    } else {
      carry_over_.clear();
      log("[handoff] could not start a new session: " + err);
    }
  } else {
    carry_over_.clear();
  }
  handoff_summary_.clear();
  // The only way back to `None`, which is what makes the level a single
  // firing: between arming and this line there is no frame on which a second
  // handoff can be armed. It also means a Reset or a settings restart pressed
  // in the middle of a handover cancels it cleanly rather than leaving a stage
  // machine running against a child that has already gone.
  handoff_stage_.store(HandoffStage::None, std::memory_order_release);
  resetting_.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------- M3.15
//
// ## The handover, and the five ways it goes wrong
//
// The user's words: *"when its context starts growing a little bit too big,
// say, for example, around 40%, the AI will auto hand off to the next AI and
// restart itself within the session… before it does this, it will prompt the
// user and say, I just need a moment to do some housekeeping."*
//
// The mechanism is entirely borrowed. `UsageStats::ctx` is the CLI's own
// reported fraction and needs no estimating; `begin_restart()` is M3.6a's
// thread, fence and join; the spoken line goes out through announce() and
// flush_announcements(), which already know how to take the floor without
// speaking into an open microphone. What is new is *when*, and five things
// about "when" are worth more than the code that does them:
//
//  1. **Never mid-turn, never mid-sentence.** Armed from the Idle branch
//     only, after the two flushes and before the microphone reopens — the one
//     point in the frame loop where no turn is running, nothing is queued to
//     say, nothing is half-spoken and the microphone is shut. A handoff that
//     interrupts a reply is worse than one that waits a turn, and a forced
//     handoff (`handoff_now()`) waits at the same gate rather than jumping it.
//
//  2. **Never twice.** See `HandoffStage`: the only way back to `None` is
//     through run_reset(), so the level that is still crossed cannot fire
//     again while the first firing is in flight, and by the time it can, the
//     child it would be reading has been replaced.
//
//  3. **Never against a number that is not one.** A fresh child's `ctx` is
//     -1.0 until it first reports; `handoff_due()` answers `No` to that and
//     the floor is never recorded from it.
//
//  4. **The summary is the one turn that must be allowed to fail.** It is
//     asked for at the exact moment context is scarce, and it costs context
//     itself. So nothing waits on it and nothing is conditional on it: a
//     summary that errors, is cancelled, comes back empty or arrives after
//     tipping the window over produces a handover *with no note*, which is
//     still a handover. The alternative — skip the restart because the
//     summary failed — leaves the session in the state that triggered it,
//     having promised out loud to do something about it.
//
//  5. **The user must not be left talking into a session being torn down.**
//     Arming happens with the microphone shut, and somebody mid-utterance is
//     in Listening, which never reaches the branch that arms.
//
//     That was as far as it went until M17.1/M17.2, and the sentence above
//     used to claim more than the code did. The microphone did *not* stay
//     shut: the latch reopened it on the first Idle frame after the
//     housekeeping line finished, several seconds before the restart, and
//     anything said into it went to start_turn(), to a child about to be
//     replaced, and was cancelled by begin_restart() with the transcript
//     cleared behind it (review finding 4). Two changes, in the two places
//     that lied: the Idle branch does not reopen the microphone while
//     `handing_off()` is true, and a turn that arrives anyway — typed, from
//     the bus, or from a Talk gesture that bypasses the latch — is held in
//     `handoff_user_turns_` and sent to the new session as the user's own
//     turn. The latch is untouched and comes back through
//     `relatch_after_reset_` exactly as it always did.

bool VoiceSession::handing_off() const {
  return handoff_stage_.load(std::memory_order_acquire) != HandoffStage::None;
}

bool VoiceSession::handoff_now() {
  // The same two refusals reset() makes, for the same reasons.
  if (!loaded_ || load_failed_) return false;
  if (resetting()) return false;
  if (handing_off()) return false;
  // Armed, not started. It is consumed by the Idle branch, which is what
  // makes a handoff asked for by name wait for the same settled moment as one
  // the numbers asked for — including waiting for a reply in flight to
  // finish, which is the rule M3.12 settled for the settings restart.
  handoff_forced_.store(true, std::memory_order_release);
  log("[handoff] asked for by name; it will start at the next gap");
  return true;
}

void VoiceSession::tick_handoff(State s) {
  switch (handoff_stage_.load(std::memory_order_acquire)) {
    case HandoffStage::None:
      return;
    case HandoffStage::Speaking:
      // Wait for the housekeeping line to have been *heard*, not merely
      // queued. Three conditions and each one covers a way the other two lie:
      // the state has not been noticed as finished yet (the Speaking branch
      // below is what moves it, one frame after the queue drains), the queue
      // is still playing, or a turn is somehow running. Muted, the queue is
      // idle immediately and this passes on the next frame — which is right:
      // the line was dropped, not delayed.
      if (turn_running_) return;
      if (s == State::Speaking) return;
      if (speech_ && !speech_->idle()) return;
      start_handoff_summary();
      return;
    case HandoffStage::Summarising:
      // The turn thread moves this on, because only it knows when the note is
      // written. Nothing to do here but wait.
      return;
    case HandoffStage::Restarting:
      // Everything M3.6a and M3.12 already do, with a third reason on it.
      begin_restart(RestartReason::Handoff);
      return;
  }
}

bool VoiceSession::begin_handoff_if_due() {
  if (handing_off()) return false;
  if (!eng_.llm) return false;

  const UsageStats u = eng_.llm->usage();
  // What this session costs before anybody has said anything. Recorded from
  // the *first* reading only, and never from an unknown one.
  if (u.ctx >= 0.0 && handoff_floor_ < 0.0) handoff_floor_ = u.ctx;

  const bool forced = handoff_forced_.exchange(false, std::memory_order_acq_rel);
  if (!forced) {
    switch (handoff_due(u.ctx, cfg_.handoff_threshold, handoff_floor_)) {
      case HandoffVerdict::No:
        return false;
      case HandoffVerdict::Unattainable:
        // Said once per session, not once per frame. Deliberately not a
        // permanent switch-off either: the floor is re-measured after every
        // restart, and a first reading inflated by an unusually long opening
        // turn should not cost the feature for the rest of the run.
        if (!handoff_warned_) {
          handoff_warned_ = true;
          log("[handoff] the threshold is at or under what a fresh session already costs (" +
              std::to_string(static_cast<int>(handoff_floor_ * 100.0 + 0.5)) +
              "%), so handing over could not get under it; not handing over");
        }
        return false;
      case HandoffVerdict::Yes:
        break;
    }
  }

  // **40% of what**, stated in the log rather than left to be worked out: the
  // threshold is a fraction of this model's own window, and the same 40% is
  // 80k tokens on a 200k model and 400k on a `[1m]` one. Both numbers, every
  // time, so a reading in the log can never be compared against the wrong one.
  std::string why = forced ? "asked for" : "context";
  why += " at " + std::to_string(static_cast<int>(u.ctx * 100.0 + 0.5)) + "%";
  if (u.ctx_window > 0)
    why += " (~" + std::to_string(static_cast<long long>(u.ctx * (double)u.ctx_window)) + " of " +
           std::to_string(u.ctx_window) + " tokens)";
  if (!forced)
    why += ", threshold " +
           std::to_string(
               static_cast<int>(normalise_handoff_threshold(cfg_.handoff_threshold) * 100.0 + 0.5)) +
           "%";
  log("[handoff] " + why + "; telling the user and starting the handover");

  // **The line comes first.** announce() queues it and flush_announcements()
  // is what actually takes the floor — shutting the microphone, marking a new
  // reply, enqueuing the speech — so calling both here means the sentence is
  // already on its way out before anything else in the sequence happens. Both
  // are called rather than leaving the queue for the next frame, because the
  // next frame is inside the stage machine and the stage machine's first job
  // is to wait for this to finish.
  announce(app_text(Msg::HandoffHousekeeping));
  flush_announcements();
  handoff_stage_.store(HandoffStage::Speaking, std::memory_order_release);
  return true;
}

void VoiceSession::start_handoff_summary() {
  // start_injected_turn()'s setup, with its one deliberate difference kept:
  // no `cancel_` is raised, because there is nothing to cancel — this is only
  // reached from a settled Idle.
  if (turn_.joinable()) turn_.join();
  cancel_ = false;
  {
    std::lock_guard<std::mutex> l(mutex_);
    // No line in the transcript, neither user nor assistant. Nobody said
    // this: it is the app asking the session to write a note about itself,
    // and the transcript is about to be cleared anyway. A `{true, …}` line
    // would put words in the user's mouth and a `{false, …}` one would show
    // the note as something Claude had said out loud.
    set_state_locked(State::Thinking);
    status_ = "housekeeping - writing a note for the next session...";
  }
  handoff_summary_.clear();
  turn_running_ = true;
  handoff_stage_.store(HandoffStage::Summarising, std::memory_order_release);
  turn_ = std::thread([this] {
    run_handoff_summary();
    turn_running_ = false;
    // The one transition this machine makes off the frame loop, and the last
    // thing this thread does. Released after `turn_running_` so that a frame
    // loop that sees `Restarting` cannot reach begin_restart() while the flag
    // still says a turn is live.
    handoff_stage_.store(HandoffStage::Restarting, std::memory_order_release);
  });
}

void VoiceSession::run_handoff_summary() {
  // **Nothing rides on this turn.** Not the pending list, not an injected
  // prompt, not the language instruction, not folder evidence: it is not a
  // conversational turn and the session it is addressed to is over in a
  // second. It goes to the client raw, which is also why it cannot disturb
  // any of the state those mechanisms keep.
  const std::string ask = handoff_request();
  if (ask.empty()) {
    log("[handoff] no handover prompt to send; handing over with nothing carried");
    return;
  }
  ChatResult r = eng_.llm->turn(ask, [](const std::string&) {}, &cancel_);
  // Every one of these is a handover *without* a note rather than a handover
  // that does not happen. See (4) above.
  if (cancel_) {
    log("[handoff] the note was interrupted; handing over with nothing carried");
    return;
  }
  if (!r.ok) {
    log("[handoff] the note failed: " + r.error + "; handing over with nothing carried");
    return;
  }
  std::string note = trim(strip_aii_blocks(r.text));
  if (note.empty()) {
    log("[handoff] the note came back empty; handing over with nothing carried");
    return;
  }
  // A cap, because this text is re-injected and a runaway note would cost the
  // new session the context the handover exists to give it back. **clip_utf8,
  // not a word boundary**: the last time model-written text was cut here it
  // was cut at the nearest space, and Japanese has none, so two spoken
  // summaries in three ended mid-character. This drops the whole code point
  // instead. The limit is generous enough that it should never fire — a note
  // this long is already not the summary that was asked for — and it says so
  // in the log when it does.
  constexpr std::size_t kNoteMax = 4000;
  if (note.size() > kNoteMax) {
    log("[handoff] the note ran to " + std::to_string(note.size()) + " bytes and was cut to " +
        std::to_string(kNoteMax));
    note = clip_utf8(std::move(note), kNoteMax);
  }
  handoff_summary_ = std::move(note);
  log("[handoff] note written, " + std::to_string(handoff_summary_.size()) + " bytes");
  // The note itself, only when asked for. Same switch and the same reason as
  // the reply log in run_turn(): a conversation in a log file is a
  // conversation on disk, and this app is built not to leave one. It is the
  // only way to see what actually carried over.
  if (std::getenv("AII_REPLY_LOG")) {
    std::string one = handoff_summary_;
    std::replace(one.begin(), one.end(), '\n', ' ');
    std::replace(one.begin(), one.end(), '\r', ' ');
    rend::log::info("[handoff-note] {}", one);
  }
}

std::string VoiceSession::handoff_request() {
  // Read at the moment it is used rather than at load, so that editing the
  // file takes effect on the next handover and not on the next launch. It can
  // afford to be: this happens once every several thousand turns, where the
  // system prompt is a launch argument and genuinely cannot hot-reload.
  const std::filesystem::path p = PromptStore::root() / "system" / "handoff.md";
  std::string body;
  {
    std::ifstream f(p, std::ios::binary);
    if (f) body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  body = trim(strip_html_comments(body));
  if (!body.empty()) return body;
  // The seeded file is missing or has been emptied. A built-in copy rather
  // than nothing at all, and a line saying so — silently handing over with no
  // note because a file could not be read is the avatar-seed failure again:
  // wired up correctly, never arrives, nothing says so.
  log("[handoff] cannot read " + p.string() + "; using the built-in wording");
  return
      "You are about to be replaced by a fresh session of yourself, because this "
      "conversation has used up more of your context window than is comfortable. The user "
      "knows; they have just been told you need a moment.\n\n"
      "Write the note that your replacement will read. It gets nothing else - no transcript, "
      "no history, none of this conversation's wording. Only this.\n\n"
      "Cover, in a few short paragraphs and in the language this conversation is being held "
      "in: what we are doing and why; what has been decided; what is outstanding or was about "
      "to happen next; and anything the user has told you about themselves or their setup "
      "that you would be embarrassed to have to ask for twice.\n\n"
      "Write it as notes to yourself. No greeting, no sign-off, no preamble, and nothing "
      "about the handover itself.";
}

std::string VoiceSession::carry_over_block(const std::string& note) {
  // pending_context()'s idiom exactly: a `<context>` block, in English,
  // composed by the app. The instruction after the note is three sentences
  // and every one of them earns its place — what this is, that the wording is
  // gone, and not to talk about it. The last is the one that was learned the
  // hard way elsewhere in this file: a session handed a briefing will open by
  // thanking you for the briefing unless it is told not to.
  return "<context name=\"Handover\" kind=\"state\">\n"
         "You wrote this to yourself a moment ago, just before this session started, because "
         "the session before it had filled too much of its context window. It is all you have "
         "of that conversation: the direction survived, the wording did not. Carry on from it, "
         "and do not mention it, the handover or the restart unless you are asked.\n\n" +
         note + "\n</context>\n\n";
}

void VoiceSession::start_turn(std::string text) {
  if (text.empty()) return;
  // M17.2, review finding 4. A handover is running: this child is about to be
  // replaced, so sending to it would spend the turn on a session that ends
  // before the reply does — and begin_restart() would cancel it a frame or two
  // later and run_reset() would clear the transcript behind it, so the user
  // would be answered by silence with nothing left on screen to show they had
  // spoken. Held instead, and sent to the session that comes up, as their own
  // turn: see `handoff_user_turns_`.
  //
  // Queued and not refused because refusing is the same loss with an apology
  // on it. The alternative considered was a spoken "one moment, handing over";
  // it costs the user their sentence and gains nothing, since the whole of the
  // wait is a few seconds and the machinery to carry a turn across a restart
  // was already here.
  if (handing_off()) {
    handoff_user_turns_.push_back(std::move(text));
    log("[handoff] a turn arrived mid-handover; it is held for the new session (" +
        std::to_string(handoff_user_turns_.size()) + " waiting)");
    set_status("one moment - handing over; what you just said goes to the new session.");
    return;
  }
  // **Ahead of the reset guard below, on purpose.** The last stage of a
  // handover *is* a restart, so `resetting()` is true for the second it takes
  // and this guard would otherwise drop the very turns the one above exists to
  // keep. The two are deliberately different answers to a similar shape: a
  // handover is the app replacing its own child, so the user's words are owed
  // to the session that comes out of it, while a Reset is the user throwing
  // the conversation away and a turn that arrives in the middle of it has no
  // session left to belong to.
  //
  // The reset thread is joining `turn_`; joining it from here as well is
  // undefined behaviour, and the turn would go to a child that is being torn
  // down anyway. This is the single choke point for a user turn — say() and
  // end_listening_and_send() both arrive here — so one guard covers the typed
  // field, the microphone and the script bus alike. It is a fraction of a
  // second and the panel already refuses the gesture; this is the backstop for
  // the ways in that do not go through the panel.
  if (resetting()) {
    log("[reset] ignored a turn that arrived mid-reset");
    return;
  }
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
  // **Only what the user said is evidence that a folder was named** (M15.4,
  // review finding 9). The comment that used to stand here said the opposite
  // and was wrong: it argued that everything going *into* a turn is evidence
  // because `text` is either what the user said or what the app composed on
  // their behalf. The second half of that is the mistake. What the app composes
  // for an injected turn is a worker's report, and a worker's report *contains
  // the worker's own closing sentence* -- prose written by a Claude instance,
  // wrapped by this app and handed back to the conversational instance. Passing
  // it to note_folder_evidence let one model corroborate another's invented
  // folder, and then its own on the next turn, which is precisely the loop
  // `core/cwd_policy.h` says must not exist: model output is never evidence.
  //
  // The cost of getting this right is small and in the right direction. A
  // worker that mentions a folder no longer lends that folder authority, so a
  // spawn the model asks for off the back of a report lands in the app's own
  // directory instead -- which is the safe answer this policy gives whenever it
  // is unsure, and the user can still name the folder out loud.
  if (!is_injected) note_folder_evidence(text);
  // M18.2. A new reply is audible again. This is the **only** place the flag
  // is cleared, and it is cleared here rather than where the watch arms so
  // that a reply which was barged stays silent for the whole of its life --
  // including the frames between the barge and start_turn()'s join, in which
  // the old turn's splitter is still running and would otherwise get one more
  // sentence out after the app had been told to be quiet.
  barged_ = false;
  speech_->mark_new_reply();
  // The one place a reply becomes sound, and therefore the only place mute can
  // honestly be applied. Clearing the queue alone (what silence() used to do)
  // stops the sentence that is playing and nothing else: this callback is on
  // the turn thread and keeps handing the queue the next sentence, so the
  // reply carries on speaking a beat later. The text side of this same loop —
  // `lines_.back().text += delta` above — is untouched, which is what makes
  // mute voice-only.
  // M13.1. The voice a chunk is spoken in, as a slot rather than an engine id.
  //
  // Turn-local, which is what makes "every reply starts on the primary" free:
  // a new reply gets a new level by construction, so there is no session-
  // lifetime state to reset and therefore no reset to forget. It is only ever
  // touched from this thread -- the filter, the splitter and this callback all
  // run inside `feed()` on the turn thread.
  int voice_level = 1;
  SentenceSplitter splitter([this, &voice_level](const std::string& s) {
    if (muted_) {
      // Traced rather than silent: "the app said nothing" and "the app was
      // muted" look identical from outside, and this is the line that tells
      // them apart in a log.
      rend::log::trace("mute: dropped {} chars of speech", s.size());
      return;
    }
    // M18.2. The user is talking over this reply. Exactly the mute path, one
    // reply wide: the sentence is not enqueued, the transcript append above is
    // untouched, and the turn goes on streaming because `cancel_` was never
    // set. The count and not the words, for the reason discard_utterance()
    // gives -- this is text the user decided not to hear.
    if (barged_) {
      rend::log::trace("barge: dropped {} chars of speech; the text is still arriving", s.size());
      return;
    }
    // Same record as flush_announcements()' — a reply's chunks as the splitter
    // hands them over, which for an injected turn is the only place the AI's
    // own words for a scheduled report can be read back.
    rend::log::info("[speak] {}", s);
    speech_->enqueue(s, voice_level);
  }, cfg_.early_words);

  // M13.1. One parser, fed once, in front of both consumers -- the transcript
  // (plain concatenation) and the splitter (synthesis). That is what makes
  // "a marker reaches neither the eyes nor the ears" a property of where this
  // sits, rather than two strippers that have to agree and eventually will not.
  DirectiveFilter filter(
      [this, &splitter](const std::string& t) {
        {
          std::lock_guard<std::mutex> l(mutex_);
          if (!lines_.empty() && !lines_.back().user) lines_.back().text += t;
        }
        splitter.feed(t);
      },
      [this, &splitter, &voice_level](const std::string& token, const std::string& value) {
        // The words before a marker belong to the voice that was speaking when
        // they were written, so they go to the queue before the level moves.
        // `break_now()` and not `flush()`: flush() re-arms the early-chunk rule
        // for the rest of the reply, which made the same text speak differently
        // depending on delta size (docs/design-directives.md, §3.3).
        splitter.break_now();
        // `[v2]` and nothing else, today. A directive that parses but carries a
        // shape this build does not know -- `[v2:extra]`, `[pause]` -- is
        // consumed and logged rather than spoken, which is the whole purpose of
        // having a grammar: the namespace can grow without an older build
        // reading tomorrow's markers aloud.
        if (value.empty() && token.size() >= 2 && token[0] == 'v' &&
            token.find_first_not_of("0123456789", 1) == std::string::npos) {
          const int slot = std::atoi(token.c_str() + 1);
          if (slot >= 1) {
            voice_level = slot;
            rend::log::info("[voice] {} from here", token);
            return;
          }
        }
        rend::log::info("[directive] {} ignored (this build does not know it)",
                        value.empty() ? token : token + ":" + value);
      });
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
  // M3.15. The note the previous session wrote to this one, on the first turn
  // after a handover and on no other. Consumed, so it costs one turn and not
  // every turn: it is context for the session, not a standing instruction,
  // and re-sending it would both cost tokens and invite the model to keep
  // answering it.
  //
  // **On an injected turn too**, unlike the pending list above. The reason
  // the pending list is held back is that a report turn handed the whole
  // queue recites it; this is the opposite — a worker report is the first
  // thing the new session says out loud, and saying it with no idea what the
  // conversation was about is exactly the cold, contextless answer the
  // handover exists to prevent.
  std::string handover;
  if (!carry_over_.empty()) {
    handover = carry_over_block(carry_over_);
    carry_over_.clear();
    log("[handoff] this turn carries the note from the session before it");
  }
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
  // The handover goes in front of everything, including the pending list: it
  // is who this session is, and the rest is what it is being asked.
  const std::string sent = decorate_language(handover + pending + rider_block + injected, eff);
  if (!eff.both()) log("[lang] turn sent with the " + language_spec(eff) + "-only instruction");
  ChatResult r = eng_.llm->turn(sent, [&](const std::string& delta) {
    {
      std::lock_guard<std::mutex> l(mutex_);
      // M13.1. This stays on the *raw* delta, ahead of the filter. A reply that
      // opens with `[v2]` would otherwise hold the UI in "thinking..." for an
      // extra delta, because the filter's first text callback comes after the
      // marker has been consumed.
      if (first) {
        first = false;
        // M18.2. Not when this reply has been barged: the user is talking and
        // the session is in `Listening` for them. A reply that was silenced
        // before its first word arrived (a barge during Thinking) would
        // otherwise announce itself as "speaking..." and take the state --
        // and with it the microphone, on the next Idle frame -- out from
        // under the sentence they are in the middle of.
        if (!barged_) {
          status_ = "speaking...";
          set_state_locked(State::Speaking);
        }
      }
    }
    // The transcript append that used to be here has moved inside the filter's
    // text callback, so markers never reach it.
    filter.feed(delta);
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
  // Order matters: the filter releases any held-back candidate as text first,
  // so a reply ending in a half-written marker still reaches the splitter.
  filter.flush();
  splitter.flush();
  // AII_REPLY_LOG: the reply as one line, off unless asked for. main.cpp logs
  // every turn that is *sent* (`send:`) and nothing logs what came back, so a
  // scripted run could prove what the model was asked and not what it knew —
  // which is the only question a restart-and-forget change can be tested on
  // without a screenshot of a chat window. Off by default because a
  // transcript in a log file is a conversation on disk, and this app is built
  // not to leave one (`--no-session-persistence`).
  if (std::getenv("AII_REPLY_LOG")) {
    std::lock_guard<std::mutex> l(mutex_);
    if (!lines_.empty() && !lines_.back().user) {
      std::string one = lines_.back().text;
      std::replace(one.begin(), one.end(), '\n', ' ');
      std::replace(one.begin(), one.end(), '\r', ' ');
      rend::log::info("[reply] {}", one);
    }
  }
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
      // M18.2. The same exception the two Speaking transitions make, and the
      // one that matters most: a turn that failed *after* the user barged in
      // must not move the session out from under them. Idle here would be
      // read by the next frame as "reply over", and the Idle branch would
      // reopen the microphone on top of the utterance already in progress.
      // The error still reaches the status line and the failure counter, so
      // nothing about it is hidden.
      if (!barged_) set_state_locked(State::Idle);
      ++turn_failed_seq_;
      // M2b.4. A failed *user* turn is visible — they are at the keyboard,
      // they just spoke, the status line says error. A failed **injected**
      // turn is silence where a report was promised ten minutes ago, with
      // nobody at the desk to see the status line, and silence is this app's
      // worst failure. So the promise is kept with a canned line instead.
      injected_turn_failed = is_injected;
    } else if (!barged_) {
      set_state_locked(State::Speaking);  // update() returns to Idle once the audio drains
      status_ = "speaking...";
    } else {
      // M18.2. The reply finished arriving while the user was talking over
      // it. There is nothing left to speak -- the queue was cleared at the
      // fire and the splitter has been dropping sentences ever since -- and
      // the state belongs to their utterance now, so it is left alone. The
      // text is all in the panel, which was the point.
      rend::log::info("[barge] the barged reply finished streaming; all of its text is in the "
                      "panel and none of the rest of it was spoken");
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
  //
  // M18.2. Unless the barge watch wants it, in which case the device stays up
  // and is watched instead of being shut -- a worker report read out over the
  // user is exactly as interruptible as a reply, and for the same reason. The
  // capture buffer is dropped either way: what was said before the app started
  // talking belongs to no turn.
  if (barge_watch_wanted()) {
    begin_barge_watch();
    mic_->discard();
  } else {
    mic_->stop();
  }
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

bool VoiceSession::flush_queued_user_turn() {
  // The same gate flush_injected_turns() stands at, plus the two that say the
  // handover is genuinely over: `handing_off()` is false only once run_reset()
  // has put the stage back to None, which is the line before it clears
  // `resetting_`.
  if (turn_running_ || resetting() || handing_off()) return false;
  if (handoff_user_turns_.empty()) return false;
  std::string text = std::move(handoff_user_turns_.front());
  handoff_user_turns_.erase(handoff_user_turns_.begin());
  // No microphone handling here, unlike flush_injected_turns(). This is only
  // ever reached from the Idle branch before the line that reopens the
  // microphone, and the handover holds it shut for its whole length, so there
  // is no open capture device to take away from anybody. start_turn() puts the
  // user's line in the transcript and the reply is spoken the ordinary way.
  log("[handoff] sending what the user said during the handover to the new session");
  start_turn(std::move(text));
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
// The evidence window behind `resolve_worker_cwd()`. Sixteen turns because a
// folder is often named once, early ("we're in the Renderer checkout today")
// and spawned into several turns later; and capped in bytes as well, because a
// pasted log is a turn too and there is no reason to carry a megabyte of it.
void VoiceSession::note_folder_evidence(const std::string& text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> l(mutex_);
  folder_evidence_.push_back(text.size() > 4096 ? text.substr(0, 4096) : text);
  while (folder_evidence_.size() > 16) folder_evidence_.pop_front();
}

std::string VoiceSession::folder_evidence() const {
  std::lock_guard<std::mutex> l(mutex_);
  std::string out;
  for (const std::string& t : folder_evidence_) {
    out += t;
    out += '\n';
  }
  return out;
}

std::string VoiceSession::pending_context() const {
  const std::vector<Schedule> book = ScheduleBook::instance().list();
  std::vector<ScheduledWorker> running;
  std::size_t ready = 0;
  // M10.5. Drained here, on a *user* turn only — `pending_context()`'s existing
  // rule (`voice_session.cpp:1786`), and a newly written action follows a user
  // turn by construction, so honouring it costs nothing.
  //
  // **This is the half of the prompt cost that is free.** The system prompt's
  // action list is fixed when the `claude` child starts, and since M3.12
  // restarting it discards the conversation — so the only way a script written
  // five minutes ago becomes callable *now* is this block, which is a line on
  // the turns after something happened and nothing on every other turn.
  std::vector<std::string> news;
  std::vector<ActionLevel> actions;
  {
    std::lock_guard<std::mutex> l(mutex_);
    running = scheduled_workers_;
    ready = pending_turns_.size();
    news.swap(action_news_);
    actions = actions_;
  }
  if (book.empty() && running.empty() && ready == 0 && news.empty()) return std::string();

  const auto now = std::chrono::steady_clock::now();
  std::string b;
  b += "<context name=\"Pending\" kind=\"state\">\n";
  // The framing sentence is about promises, and a block that carries only a
  // newly written script has not promised anything — so it says what it is
  // instead. One `if`, rather than one sentence covering two unrelated facts.
  b += (book.empty() && running.empty() && ready == 0)
           ? "What the app itself has changed since your last turn. This is the app's, not "
             "your memory: trust it over anything you recall.\n\n"
           : "Things you promised the user earlier and have not delivered yet. This list is the "
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
  if (!news.empty()) {
    b += "Scripts that have appeared or been allowed since the last turn:\n";
    for (const std::string& n : news) {
      bool armed = false;
      bool temp = false;
      bool known = false;
      for (const ActionLevel& a : actions)
        if (a.name == n) {
          armed = a.armed;
          temp = a.temporary;
          known = true;
        }
      if (!known) continue;
      // M29. A temp row was never "allowed" by anyone — it is armed by sitting
      // in `scripts\tmp\`, not by a decision the user made — so it gets its
      // own sentence rather than borrowing the word "allowed" from the
      // consent it never went through.
      if (temp)
        b += "- " + n + ": temporary script, ready to run now. It lives in scripts\\tmp\\ and "
                        "could be cleared at any point; if the user wants to keep it, rewrite it "
                        "into scripts\\actions\\.\n";
      else
        b += "- " + n + (armed ? ": allowed, you can run it now.\n"
                               : ": waiting to be allowed. Tell the user to open the settings "
                                 "panel, find it under Scripts and press Confirm. Do not try to "
                                 "run it until they have.\n");
    }
    b += "\n";
  }
  if (book.empty() && running.empty() && ready == 0) {
    b += "</context>\n\n";
    return b;
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
    if (workers_ && workers_->spawn(name, a.cwd, a.task, &err, a.model)) {
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
std::string create_schedule(const Command& c, const std::string& evidence, std::string* detail) {
  // M2b.2. The mapping itself now lives in `build_schedule()`, beside the book,
  // because the bus is a second door onto the same policy and the shape-is-the-
  // grade rule is the part that must not be written twice. What stays here is
  // the half that is this door's alone: the *words*. A refusal the model
  // triggered is spoken in the user's register; the bus's is a line in the log.
  ScheduleRequest req;
  req.in = c.in;
  req.say = c.say;
  req.task = c.task;
  // The same gate as `spawn`, and it matters more here, not less: a deferred
  // worker starts minutes later at bypassPermissions, possibly with nobody at
  // the desk to notice which folder it landed in. A `cwd=` the conversation
  // never named is dropped and the app's own folder used instead; the log line
  // below carries the reason. See core/cwd_policy.h.
  const CwdDecision where = resolve_worker_cwd(c.cwd, evidence);
  if (!c.task.empty() && !where.honoured)
    rend::log::info("[schedule] {}", where.why);
  req.cwd = c.task.empty() ? std::string() : where.dir;
  req.name = c.name;
  req.label = c.label;
  req.grade = c.grade;
  // M31. Resolved here, not in `build_schedule()`: `core/schedule.h` does not
  // depend on `core/model_choice.h`, and this is the one door a `model=` on a
  // `schedule` line comes through. Anything the table does not recognise is
  // dropped with one log line rather than reaching a command line -- the same
  // rule `spawn`'s own handler applies below.
  if (!c.model.empty()) {
    const int midx = model_choice_for_key(c.model);
    if (midx >= 0) req.model = model_choice(midx).arg;
    else rend::log::info("[schedule] ignored unknown model=\"{}\"", c.model);
  }
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
  // What this conversation can be said to have named a folder in. Taken once
  // per reply rather than per command, because a block may hold several spawns
  // and they are all answering the same turn.
  const std::string evidence = folder_evidence();
  // M15.1. A block the model did not finish writing is refused rather than run,
  // and the only place that is visible is here: the parser has no logger, so it
  // hands back what it dropped and this is the caller that owes the log a line.
  std::vector<std::string> problems;
  const std::vector<Command> commands = parse_commands(reply_text, &problems);
  for (const std::string& p : problems) log("[aii] " + p);
  for (const Command& c : commands) {
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
    // M3.14. Not a worker verb either, so it is handled before the `workers_`
    // guard: changing a setting has nothing to do with whether a pool exists.
    if (c.verb == "setting") {
      apply_setting(c);
      continue;
    }
    // M10.2/M10.5. Not a worker verb, so it is handled before the `workers_`
    // guard: calling an action has nothing to do with whether a pool exists.
    if (c.verb == "run") {
      apply_run(c);
      continue;
    }
    // M14. Not a worker verb either: a memory is a line in a file, and the
    // session owns the file.
    if (c.verb == "remember" || c.verb == "forget") {
      apply_memory(c);
      continue;
    }
    if (c.verb == "load") {
      if (injector_.request(c.name)) log("[prompts] queued " + c.name + " for the next turn");
      else log("[prompts] refused load name=" + c.name + " (no such prompt in the store)");
      continue;
    }
    // M2b.3. Not a worker verb either, so it needs no pool — a scheduled
    // worker only wants one when it fires, which is M2b.4's problem.
    if (c.verb == "schedule") {
      std::string detail;
      const std::string refusal = create_schedule(c, evidence, &detail);
      log("[schedule] " + std::string(refusal.empty() ? "" : "refused: ") + detail);
      if (!refusal.empty()) announce(refusal);
      continue;
    }
    if (!workers_) continue;
    std::string err;
    if (c.verb == "spawn") {
      // **The folder is the app's decision, not the model's** (core/cwd_policy.h).
      // A worker starts at bypassPermissions, so "which directory" is the one
      // field in this block that can do real damage, and it was also the one
      // field the model was measured inventing -- three spawns in ten landed in
      // a `Documents` folder nobody had mentioned (M3.8). Three prompt wordings
      // failed to move that, so it is settled here instead: an absent or
      // uncorroborated `cwd=` becomes the folder the app itself was launched
      // from, which is the folder the user is looking at and the same one the
      // instance they are talking to is in. Not announced -- like `button` and
      // `load`, this is the model's housekeeping and the user asked for a
      // worker, not a report about where it went. The log has the reason.
      const CwdDecision where = resolve_worker_cwd(c.cwd, evidence);
      if (!where.honoured) log("[worker] " + where.why);
      // M31. `model=` on a spawn line, resolved the same way schedule's is:
      // only a value `model_choice_for_key()` recognises reaches the pool, so
      // the model cannot put an arbitrary string on the child's command line.
      std::string model_arg;
      if (!c.model.empty()) {
        const int midx = model_choice_for_key(c.model);
        if (midx >= 0) model_arg = model_choice(midx).arg;
        else log("[worker] ignored unknown model=\"" + c.model + "\"");
      }
      if (workers_->spawn(c.name, where.dir, c.task, &err, model_arg)) {
        log("[worker] spawned " + c.name + " in " + where.dir);
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
      // M17.3. Timed, and the timing is logged, because this call is the one
      // place in the app that can block the turn thread on another process
      // ending. It is bounded now (WorkerPool::kInterruptGrace), and a bound
      // nobody measures is a bound nobody knows held.
      const auto stop_began = std::chrono::steady_clock::now();
      const bool stopped = workers_->stop(c.name);
      const double took = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - stop_began).count();
      if (!stopped) {
        take_silenced_worker(c.name);
        announce(app_text(Msg::NoWorker, c.name));
      } else {
        log("[worker] stopped " + c.name + " in " + std::to_string(took) + " s");
      }
    }
  }
  request_cancel(std::move(cancels));
}

void VoiceSession::apply_setting(const Command& c) {
  // The key first, because "there is no setting called that" is a different
  // sentence from "that key does not take that value" and the user hears
  // which. A key this format does not define is the model having invented
  // one, and it is **said out loud** rather than logged: the whole hazard of
  // an invented key is that it is inert, and inert failures are the ones that
  // turn up in a file a week later with nobody able to say how.
  const SettingKey* key = setting_key(c.key);
  if (!key) {
    log("[setting] refused key=\"" + c.key + "\" (no such key in settings.json)");
    announce(app_text(Msg::SettingNoSuchKey, c.key));
    return;
  }

  // A real key that nothing running owns. It has its own sentence in the
  // table — the inspector's rect, a value read before there was a window, the
  // format's own version field — so the user is told which of those it is
  // rather than getting one refusal that covers three unrelated facts.
  if (key->cost == SettingCost::NotSettable) {
    log("[setting] refused key=" + c.key + " (not settable while running)");
    announce(app_text(key->say, c.key));
    return;
  }

  std::string err;
  const std::string line = setting_bus_line(*key, c.value, &err);
  if (line.empty()) {
    log("[setting] refused key=" + c.key + " value=\"" + c.value + "\": " + err);
    announce(app_text(Msg::SettingBadValue, c.key));
    return;
  }

  // The confirmation. See `asked_setting_` in the header for why the app holds
  // this and the model does not: the restart discards the conversation the
  // question was asked in, so a "shall I?" that acts in the same reply is a
  // question in grammar only.
  if (key->cost == SettingCost::Restart) {
    const std::string pair = c.key + "=" + c.value;
    const bool confirmed = c.confirm == "yes" || c.confirm == "true" || c.confirm == "on";
    if (!confirmed || asked_setting_ != pair) {
      // Both halves land here, and deliberately answer the same way. An
      // unconfirmed line is the model doing as it was told; a `confirm=yes`
      // nobody was asked about is the model skipping the gesture. The user's
      // experience of the two must be identical — they are asked — or the
      // second one becomes a thing worth trying.
      asked_setting_ = pair;
      log("[setting] asking about " + pair +
          (confirmed ? " (confirm= arrived before the question)" : ""));
      announce(app_text(key->say));
      return;
    }
    asked_setting_.clear();
  }

  // Posted, not applied. `AppBus::post` may be called from any thread and
  // `apply_pending()` runs the handler on the frame loop, which is the rule
  // `announce()` keeps for the same reason. From here on this is the panel's
  // own path: the handler writes `AvatarUiState`, main.cpp mirrors that into
  // `settings.json`, and a model or tool change arms the restart main.cpp
  // already arms when the box is ticked by hand.
  if (!AppBus::instance().post(line, &err)) {
    log("[setting] could not post " + c.key + ": " + err);
    announce(app_text(Msg::SettingBadValue, c.key));
    return;
  }
  log("[setting] " + c.key + " = " + c.value + " (" + key->bus + ")");
  // Nothing is said for a change that costs nothing: the model's own spoken
  // sentence already said what it did, and a second voice repeating it is the
  // app talking over the conversation. The one that costs something says so.
  if (key->cost == SettingCost::NextLaunch) announce(app_text(key->say));
}

void VoiceSession::set_actions(std::vector<ActionFact> list, bool authoring) {
  std::lock_guard<std::mutex> l(mutex_);
  actions_.clear();
  actions_.reserve(list.size());
  for (ActionFact& f : list)
    actions_.push_back(ActionLevel{std::move(f.name), f.armed, f.in_digest, f.temporary});
  actions_authoring_ = authoring;
}

void VoiceSession::note_action_news(const std::vector<std::string>& names) {
  if (names.empty()) return;
  std::lock_guard<std::mutex> l(mutex_);
  for (const std::string& n : names) {
    if (std::find(action_news_.begin(), action_news_.end(), n) == action_news_.end())
      action_news_.push_back(n);
  }
  // Bounded, like everything else that a script or the model can drive. A
  // model that wrote forty files in a turn must cost a line, not a page.
  while (action_news_.size() > 8) action_news_.erase(action_news_.begin());
}

// M10.2/M10.5. Calling an action.
//
// **Every refusal here names the action and is spoken**, and none of them is a
// silent no-op. That is M3.14's finding applied one file along: the hazard of a
// call that quietly did nothing is that the model invents a reason for it, and
// an invented reason is what the user hears. The unarmed case in particular
// does not merely refuse — it is the user's own instruction that the model be
// told to send them to arm it, and the sentence it speaks carries that step.
void VoiceSession::apply_run(const Command& c) {
  Msg say = Msg::Count;
  {
    std::lock_guard<std::mutex> l(mutex_);
    const ActionLevel* found = nullptr;
    for (const ActionLevel& a : actions_)
      if (a.name == c.name) found = &a;
    // A name, never a path and never a body: resolved against the set the app
    // built by looking at a directory, and anything else is the model having
    // invented one.
    if (!found) say = Msg::ScriptNoSuchAction;
    else if (!actions_authoring_) say = Msg::ScriptAuthoringOff;
    else if (!found->in_digest) say = Msg::ScriptPastCap;
    else if (!found->armed) say = Msg::ScriptNotArmed;
  }
  if (say != Msg::Count) {
    log("[action] refused run name=" + c.name);
    announce(app_text(say, c.name));
    return;
  }

  // Posted, not run. `run_commands()` is on the turn thread — the thread that
  // produces the reply — and an action that took a second there would stall
  // speech. This lands in `apply_pending()` on the frame loop, which resolves
  // the name to a path against the authoritative store and publishes the
  // dispatch the Python side is waiting on. Fire and forget by design: the
  // model does not get a return value in the turn that asked.
  std::string err;
  if (!AppBus::instance().post(BusLine("script.run").str("name", c.name).done(), &err)) {
    log("[action] could not post run name=" + c.name + ": " + err);
    return;
  }
  log("[action] run name=" + c.name);
}

void VoiceSession::apply_memory(const Command& c) {
  std::string why;
  if (c.verb == "remember") {
    std::uint64_t id = 0;
    if (memory_.append(c.text, {}, &id, &why)) {
      log("[memory] remembered id=" + std::to_string(id) + " (" + std::to_string(memory_.text_size()) +
          "/" + std::to_string(kMemoryTextCap) + " chars): " + c.text);
    } else {
      // Which refusal is which: the log has the numbers, the user hears the
      // shape. A full store is the one with a remedy, so it gets its own
      // sentence; an empty or oversized line and a write failure are told
      // apart the same way, because "say it shorter" and "check the disk"
      // are different things to do next.
      log("[memory] refused remember: " + why);
      if (why.rfind("full", 0) == 0) announce(app_text(Msg::MemoryFull));
      else if (why.rfind("nothing", 0) == 0 || why.rfind("too long", 0) == 0)
        announce(app_text(Msg::MemoryNothingToSave));
      else announce(app_text(Msg::MemoryNotSaved));
    }
  } else {
    // The id comes from the list this app put in the prompt, so one that
    // does not read as a number is the model inventing one -- and, unlike a
    // `cancel`, it is said aloud: an unspoken failed forget is a memory the
    // user believes is gone.
    char* end = nullptr;
    const unsigned long long n = std::strtoull(c.id.c_str(), &end, 10);
    if (c.id.empty() || (end && *end != '\0') || n == 0) {
      log("[memory] refused forget: could not read id=\"" + c.id + "\"");
      announce(app_text(Msg::MemoryNoSuchId, c.id.empty() ? std::string("?") : c.id));
      return;
    }
    if (memory_.remove(n, &why)) {
      log("[memory] forgot id=" + c.id);
    } else {
      log("[memory] refused forget id=" + c.id + ": " + why);
      announce(app_text(Msg::MemoryNoSuchId, c.id));
    }
  }
  // Whatever happened, the prompt store's copy is the file as it now stands,
  // so a restart -- reset, model change, handoff -- composes the current
  // list. The same rule `set_actions_digest` follows after a script lands.
  set_memory_digest(memory_.digest());
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
  // Skipped while the child is being replaced, for the reason snapshot()
  // gives at the same call: the pointer is being swapped on another thread.
  UsageStats stats;
  if (loaded_ && !resetting() && eng_.llm) stats = eng_.llm->usage();
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
  //
  // `resetting` is tested first and is not an optimisation: the reset thread
  // is replacing the object this pointer names, and `eng_.llm` is a plain
  // unique_ptr, not an atomic one. The flag is what makes the swap safe, and
  // it is safe because it was set on this thread — see the declaration. The
  // usage numbers simply go stale for the second it takes, which is honest:
  // they belong to a child that is on its way out.
  const bool busy = resetting();
  UsageStats stats;
  if (loaded_ && !busy && eng_.llm) stats = eng_.llm->usage();
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
  // M12.2. Read straight off the frame loop's own flag: snapshot() is built on
  // the frame loop, which is the only thing that writes it. The phrase comes
  // out of the member this lock already covers.
  s.wake_listening = wake_open_;
  s.wake_phrase = wake_phrase_;
  // Gated on the state here rather than zeroed wherever the microphone
  // closes: there are five paths out of Listening and only one of them would
  // have remembered, and a stale level would leave the avatar leaning at
  // something that stopped talking.
  s.mic_level = state_ == State::Listening ? mic_level_ : 0.0f;
  s.speak_level = speaking;
  s.load_progress = load_progress_;
  s.load_stage = load_stage_;
  s.lines = lines_;
  s.resetting = busy;
  // "Is there anything to throw away", answered from the one thing the user
  // can see. Reset clears `lines_`, so this is false on a fresh session and
  // false again the moment a reset finishes, with no counter to keep in step.
  s.resettable = !lines_.empty();
  // Not quitting_ok(): `busy` above is this snapshot's reading of resetting(),
  // taken under the same lock, and calling the accessor again here could
  // answer from a later moment than the rest of the fields were filled from.
  // A close button that draws itself from a snapshot deserves a snapshot that
  // agrees with itself.
  s.quit_ok = !turn_running_ && !busy;
  // M3.12. What the child that is running right now was launched with. Not
  // `cfg_`: between apply_llm_settings() and the new child being up, `cfg_`
  // is what has been *asked for*, and the surface's whole job in that second
  // is to say what is still in force.
  s.model_in_force = model_in_force_;
  s.tools_in_force = tools_in_force_;
  if (workers_) s.workers = workers_->snapshot();
  return s;
}

}  // namespace aii
