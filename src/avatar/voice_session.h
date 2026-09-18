#pragma once
// The voice loop behind the avatar window: engines load on a background
// thread, the microphone feeds the recogniser from the frame loop, Claude
// turns run on a worker thread, and replies are spoken through the speech
// queue. The window reads an immutable Snapshot each frame.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_out.h"
#include "audio/mic_in.h"
#include "core/config.h"
#include "core/engines.h"
#include "core/language.h"
#include "core/prompt_store.h"
#include "core/schedule.h"
#include "core/speech_queue.h"
#include "core/worker_pool.h"
#include "llm/llm_client.h"

namespace aii {

class VoiceSession {
 public:
  enum class State { Loading, Idle, Listening, Thinking, Speaking, Failed };

  // M8.3. Where the Japanese voice is. `Absent` is the ordinary state when
  // Japanese has never been switched on this session — VOICEVOX was skipped at
  // startup on purpose, which is the second of startup the user asked to save,
  // not a failure. The settings surface reads this to say what is happening,
  // which is the whole of the on-demand load's user interface: nothing is ever
  // *spoken* about it, so a voice that arrives while the app is muted cannot
  // announce itself.
  enum class VoiceLoad { Absent, Loading, Ready, Failed };

  struct Line {
    bool user = false;
    std::string text;
  };
  struct Snapshot {
    State state = State::Loading;
    std::string status;   // one line: what is happening / last error
    std::string usage;    // subscription window readout, empty until known
    UsageStats usage_stats;  // the same numbers unformatted (negative = unknown)
    std::string partial;  // live transcript while listening
    // Bumped once each time an utterance is finalised *without* being sent —
    // a hold-to-dictate release. `partial` then holds the finished decode
    // rather than the last half-word, and the panel writes it into the message
    // field one last time. A counter rather than a flag because the panel reads
    // snapshots at frame rate and must apply that write exactly once; every
    // other way of leaving Listening leaves this alone, so the Pause and
    // Silence behaviour M1b.4 settled is untouched.
    unsigned dictated_seq = 0;
    // Bumped once each time a turn comes back with an error. The status line
    // carries the text, but a front-end that wants to *react* to the failure
    // needs an edge, and "status happens to start with error:" is not one.
    // AvatarController is the first reader (M2.4: a failed turn is what puts
    // the `?` over the avatar's head).
    unsigned turn_failed_seq = 0;
    // M1f.1. Bumped once each time the latched microphone closed itself
    // because it had heard no voice for the configured timeout. An edge, for
    // the same reason turn_failed_seq is one: the status line carries words,
    // and a front-end that wants to *react* cannot react to words.
    //
    // **This is the seam M1f.3 hangs the silent reaction off.** Nothing is
    // spoken when it fires — the user has walked away — so the whole of the
    // user-visible answer is what a reader does with this counter: the status
    // line, the microphone icon's face, and the slime dozing off. M1f.1
    // deliberately does none of that; it only makes the moment visible.
    unsigned listen_timeout_seq = 0;
    // The two continuous signals the avatar's motion is driven by (M2.4).
    // Both are already computed inside the loop; neither had a way out of it.
    //
    // `mic_level` is the RMS of the microphone measured against the noise
    // gate this session is already tracking, not a raw amplitude: the gate is
    // what "the user is talking" means here, and a raw level would read as
    // loud in a noisy room and silent in a quiet one. 0 unless listening.
    // `speak_level` is the raw RMS of what the speaker is playing, 0 when it
    // is not playing anything.
    float mic_level = 0.0f;
    float speak_level = 0.0f;
    // Engine bring-up, for the loading screen. The progress is weighted by
    // measured load times, so it tracks the wait rather than the stage count,
    // and it never goes backwards: it stops where it is if a stage fails.
    float load_progress = 0.0f;  // 0..1, 1.0 once loading has finished
    std::string load_stage;      // display name of the running stage, empty once loaded
    std::vector<Line> lines;
    std::vector<WorkerPool::Snapshot> workers;
    // M8.3, all three for the settings surface.
    VoiceLoad japanese_voice = VoiceLoad::Absent;
    std::string japanese_voice_error;   // empty unless japanese_voice == Failed
    // What the app is *actually* doing, which is not always what the
    // checkboxes say: see effective_langs() for the one case where they differ.
    LanguageSelection effective_langs;
  };

  explicit VoiceSession(Config cfg);
  ~VoiceSession();

  void update();                       // once per frame, main thread
  // The microphone latch. Click on to listen, click again to mute. The
  // session owns the latch rather than the UI, because stop() drops it too
  // and a UI-owned copy would just set it again on the next frame.
  // While it is on, a pause long enough to look like the end of an
  // utterance sends that utterance on its own, and the mic reopens once the
  // reply finishes — so one click carries a whole conversation, and the
  // speakers are never transcribed back in as the user. Turning it off sends
  // whatever was captured but not yet sent.
  void toggle_mic();
  // The two halves of the Talk gesture (M1b.3). The microphone opens on the
  // press, before it is known whether this is a click or a hold, so that a
  // hold loses none of its first words and a click that took 300 ms is not
  // punished for it; the release is what gives the press its meaning.
  //
  // `over_button` is false when the pointer left the button before it came up
  // — the usual escape hatch out of a press you did not mean. `held` is the
  // caller's verdict on the duration, made against kTalkHoldSeconds in
  // avatar_ui.h, because both the button and SPACE have to agree on it.
  // A release with `held` set finalises the utterance into the message field
  // without sending it; a short one latches the microphone on, which is the
  // conversation mode a click has always meant.
  void talk_pressed();
  void talk_released(bool over_button, bool held);
  bool mic_open() const { return mic_open_; }
  // A Talk press is down and this session is recording for it (M1b.3). Frame
  // loop only, like mic_open(). It is what the microphone button draws its
  // "dictating" face from, and the panel has no other way to tell a hold from
  // a latch — a SPACE hold never touches the button at all.
  bool mic_hold() const { return hold_; }
  // The persistent mute of Claude's *voice* (16 Sep 2026). Not the old
  // one-shot silence(), which only emptied the queue and was overtaken by the
  // next sentence of the same reply a moment later: this suppresses at the
  // source as well, so nothing synthesised after it is ever queued. The reply
  // still arrives as text — mute is voice-only, and the transcript is
  // untouched. Cuts mid-sentence when it goes on, because a mute that waits
  // for the current sentence is not a mute.
  void set_muted(bool muted);
  bool muted() const { return muted_; }
  // Stop: cancel the reply in flight, drop the mic latch and any held Talk
  // gesture, and pause every running worker. It was called pause() until
  // 16 Sep 2026; nothing here can resume a paused worker turn, so the name was
  // the only pause-like thing about it. The behaviour is unchanged.
  void stop();
  // M8.3. Which languages are on, pushed down from the panel every frame the
  // same way mute is: a level, not an edge, so there is one owner of record
  // (the settings file) and this is a no-op unless it changed.
  //
  // A change does three things immediately, none of which waits for the next
  // utterance: the recogniser's language option is rewritten (it is re-read
  // per 560 ms chunk, so this reaches an utterance already in flight), the
  // per-turn instruction to Claude changes from the next turn, and switching
  // Japanese on for the first time starts the VOICEVOX load on its own thread.
  //
  // The caller guarantees at least one language is on; a selection with none
  // is repaired to both rather than obeyed.
  void set_languages(LanguageSelection sel);
  // M1f.1. How long the *latched* microphone may hear no voice before it
  // closes itself, in seconds; <= 0 means never. Pushed down as a level every
  // frame, exactly as mute and the language selection are, so that the
  // settings file stays the one owner of record and this is a no-op unless it
  // changed. **This is the call M1f.2 makes.** Any thread.
  void set_listen_timeout(float seconds);
  float listen_timeout() const;
  void say(const std::string& text);   // send typed/scripted text as the user turn
  bool quitting_ok() const;            // true once no worker is mid-turn

  // M2b.4. A schedule that has come due. Called from the frame loop's tick
  // point in main.cpp, once per fired schedule, and it **never speaks from
  // here** — see the comment on the definition. `Fixed` queues the sentence
  // the schedule was created with; `Phrased` starts the work and arranges for
  // the conversational instance to describe the outcome in its own words.
  void deliver_schedule(const Schedule& s);
  // M2b.5. Apply every cancel the AI asked for. **Frame loop only, and called
  // from the same block in main.cpp as the schedule tick, immediately after
  // it** — that placement is the whole of the cancel-versus-fire argument and
  // is not an accident of ordering. A cancel that arrives on the turn thread
  // is queued by request_cancel() and applied here, so by the time it is
  // looked up, any schedule that came due on this frame has already been
  // delivered and, if it started a worker, registered. "Cancelled" and "fired"
  // were already exclusive inside the book (M2b.1); this is what keeps them
  // exclusive across the gap between the book and the worker the fire started.
  void apply_cancels();
  // M2b.5. Any thread. The ```aii``` block runs on the turn thread, so this
  // only queues; see apply_cancels(). Every id from one block arrives in one
  // call, so a "cancel everything" that partly misses says one sentence rather
  // than one per item.
  void request_cancel(std::vector<std::uint64_t> ids);
  // M2b.4. What the app says about schedules it is dropping at shutdown.
  // **One line for all of them**, not one per schedule: this runs during
  // teardown, where the frame loop has already stopped and nothing can be
  // spoken at all. See the definition for why that is a fact rather than a
  // policy, and where the honest place for this promise actually is.
  void drop_schedules(const std::vector<Schedule>& dropped);

  Snapshot snapshot() const;

  // M5.2. A copy of what is in Claude's head, for the prompt inspector. Any
  // thread; the frame loop is the caller.
  //
  // **A copy of a published value, not a read of the live objects.** The store
  // is filled on the load thread and the injector is written on the turn
  // thread; this returns a `PromptInventory` those threads built and left
  // behind under `mutex_`, with the age stamped on the way out. So the window
  // is live — a prompt injected by the turn running right now is in the next
  // frame's copy — without the frame loop ever touching `prompts_` or
  // `injector_`.
  PromptInventory prompt_inventory() const;

  static const char* state_name(State s);

 private:
  void load();
  // What the user asked for, and what can actually be delivered right now.
  //
  // They differ for about a second, once: Japanese switched on mid-session is
  // requested immediately but VOICEVOX takes ~1.1 s to load, and in that gap
  // routing a Japanese reply anywhere would mean routing it to the English
  // voice, which reads it as garbage. So Japanese counts as on only once its
  // voice is actually ready, and for that second the recogniser stays pinned
  // and Claude is still told to stay in English. The load then becomes
  // invisible rather than a window in which the app half-works.
  //
  // It is also what a permanently failed VOICEVOX collapses to, correctly: the
  // checkbox stays on, the settings surface says the voice failed, and nothing
  // downstream pretends Japanese is available.
  LanguageSelection requested_langs() const;
  LanguageSelection effective_langs() const;
  // Push the effective selection into the recogniser. Cheap and idempotent.
  void apply_stt_language();
  // Start the on-demand VOICEVOX load if it is wanted and not already here.
  void ensure_japanese_voice();
  // Moves the loading screen on to stage `index` of kLoadStages (loader thread
  // only). Progress becomes the weight of everything before it, so it only
  // ever climbs; an index past the end means loaded, 1.0 and no stage name.
  // Returns the progress it published, for the trace line.
  float begin_load_stage(size_t index);
  void set_mic_open(bool open);
  // M1f.1. The latch has heard no voice for long enough: close it, silently.
  // Frame loop only. Deliberately *not* set_mic_open(false), which sends what
  // was captured — see the definition.
  void close_latch_after_silence(float quiet_for);
  void begin_listening();
  // Closes the mic and decodes what is left, returning the final text. Both
  // ends of an utterance go through here so the decode is written once.
  std::string finish_utterance();
  // Closes the mic, decodes what is left and starts the turn. Leaves the
  // state Idle instead when nothing intelligible was said.
  void end_listening_and_send();
  // The same close and decode, but the text becomes a dictation for the
  // message field instead of a turn (M1b.3).
  void end_listening_unsent();
  void start_turn(std::string text);
  // M2b.4. A turn nobody typed: the app telling Claude that something it
  // deferred has finished, so the report comes back in the AI's own words and
  // in the language this conversation is being held in. Same client, same
  // session id, so it is the same conversation rather than a fresh one.
  // Frame loop only, and only with the floor already taken.
  // M2c.1. `fallback` is what is spoken if the turn itself fails — the raw
  // report, already written and already in the user's language, so a model
  // call that does not come back still cannot lose the news. Empty means "no
  // raw copy exists", and the canned Msg::ScheduledReportLost is used.
  void start_injected_turn(std::string sent, std::string fallback);
  void run_turn(std::string text, bool is_injected, std::string fallback = std::string());
  void run_commands(const std::string& reply_text);
  // Speak a line from the app itself (worker reports) and show it.
  void announce(const std::string& text);
  // Same, where what is shown and what is spoken differ: a worker report is
  // named in the transcript and unnamed in the voice.
  void announce(const std::string& shown, const std::string& spoken);
  // Speaks anything announce() left queued, closing the microphone first.
  // True if it took the floor. Frame loop only.
  bool flush_announcements();
  // M2b.4. The same, for a report that wants Claude's own words: starts one
  // queued injected turn if the floor is genuinely free. True if it took it.
  // Frame loop only, and deliberately tried *after* flush_announcements(), so
  // a canned line already waiting is heard before a turn is spent.
  bool flush_injected_turns();
  // M2b.4. Queue one report for the conversational instance. Any thread — a
  // worker thread is where a scheduled worker's report arrives.
  // M2c.1: `fallback` rides with it; see start_injected_turn().
  void queue_injected_turn(std::string sent, std::string fallback = std::string());
  // M2c.1. Queue a *live* worker's finished report for the conversational
  // instance. Unlike queue_injected_turn() this holds the raw report rather
  // than a composed turn, because several of them that arrive together are
  // merged into one turn at flush time rather than becoming one model call
  // each. Any thread — the worker pool's report thread is where these arrive.
  void queue_worker_report(std::string shown, std::string spoken);
  // M2b.4. True if `name` was a worker a schedule started, and forgets it.
  // M2b.5: `phrased` comes back with it, because the list now holds every
  // schedule-started worker — visibility and cancellation want all of them —
  // where before it held only the ones that report through a turn.
  bool take_scheduled_worker(const std::string& name, bool* phrased);
  // M2b.5. Mark `name` as stopped on purpose, so its report says nothing.
  void silence_worker(const std::string& name);
  // M2b.5. True if `name` was silenced, and forgets it.
  bool take_silenced_worker(const std::string& name);
  // M2b.5. The block handed to Claude with the user's turn, listing what the
  // user is still waiting for: schedules not yet due, workers a schedule
  // started that are still running, and reports that are ready but not yet
  // spoken. Empty when there is nothing pending, so a session that never
  // schedules anything sends byte-for-byte what it sent before this existed.
  // Any thread; called from the turn thread.
  std::string pending_context() const;
  // M2b.4. The text handed to Claude when a scheduled worker finishes.
  static std::string scheduled_report_prompt(WorkerPool::State state, const std::string& shown);
  // M2c.1. The text handed to Claude when one or more *live* workers finish.
  // Takes the whole batch, because two workers finishing in the same gap are
  // one thing to say, not two turns talking over each other.
  static std::string live_report_prompt(const std::vector<std::string>& shown);
  void set_state(State s);
  // The same, for callers that already hold mutex_ because they are publishing
  // a state change together with the text that goes with it. Both overloads
  // exist so that no path assigns state_ directly: the trace line lives here,
  // and a transition that skipped it would be a hole in the record.
  void set_state_locked(State s);
  void set_status(const std::string& s);
  void log(const std::string& s);

  Config cfg_;
  Engines eng_;
  std::unique_ptr<AudioOut> speaker_;
  std::unique_ptr<MicIn> mic_;
  std::unique_ptr<SpeechQueue> speech_;
  std::unique_ptr<WorkerPool> workers_;

  // M3.3 / M3.4. The store is kept because the injector holds only what it
  // needs; `injector_` is touched from the turn thread (decorate, and the
  // `load` verb that run_commands applies at the end of the same turn) and
  // from nowhere else, which is why neither is behind `mutex_`.
  PromptStore prompts_;
  PromptInjector injector_;
  // M5.2. The inspector's view of both, republished by whichever of those two
  // threads last changed them, and read by the frame loop under `mutex_`. It
  // exists because the alternative — the window reaching into `prompts_` and
  // `injector_` to build its own list — is a read of the turn thread's data
  // from the frame loop, i.e. the race the two comments above spent their
  // whole length ruling out.
  PromptInventory inventory_;                                 // mutex_
  std::vector<std::pair<std::string, double>> prompt_times_;  // turn thread; id -> uptime
  // When this session began, for the inspector's "how long ago". Steady, so a
  // clock change mid-session cannot make a prompt look like it was injected in
  // the future. Set once, in the constructor, so it covers the load as well.
  std::chrono::steady_clock::time_point session_began_ = std::chrono::steady_clock::now();
  // Republish `inventory_` from the current store and injector. Turn thread or
  // load thread only — it reads both — and it takes `mutex_` itself.
  void publish_inventory();

  // Frame-loop state: touched only from update()/set_mic_open().
  bool mic_open_ = false;
  // Claude's voice is muted (voice-only; the text still arrives). Atomic
  // because the turn thread reads it for every sentence the splitter emits,
  // while the frame loop is what sets it.
  std::atomic<bool> muted_{false};
  // A Talk press is down and this session opened the microphone for it. It is
  // deliberately not the same thing as mic_open_: a hold must not auto-send on
  // a pause and must not reopen after a reply, which is exactly what the latch
  // means. Anything that takes the microphone away (the latch, Stop) clears
  // it, so a release that arrives afterwards is a no-op rather than a second
  // close.
  bool hold_ = false;
  // The noise gate behind end-of-utterance detection: when the microphone
  // last carried something louder than the room, and the room level it is
  // being judged against.
  std::chrono::steady_clock::time_point last_voice_{};
  std::chrono::steady_clock::time_point listen_began_{};
  float noise_floor_ = 0.0f;

  // M1f.1. The auto-listen timeout.
  //
  // `listen_timeout_` is the configured value in seconds, <= 0 meaning never.
  // Atomic only because M1f.2 may push it from a settings surface that is not
  // guaranteed to be this frame loop; the read below is the frame loop's.
  //
  // `timeout_blocked_at_` is the whole of the "do not count the app's own
  // busy time as silence" rule, and it is a **reset**, not a pause: every
  // frame on which the timeout is not eligible to run — not listening, not
  // latched, a Talk press held, a turn in flight, or the speaker playing —
  // this is stamped with now. The elapsed silence is then measured from
  // max(last_voice_, timeout_blocked_at_), so the instant the app stops being
  // busy the user gets a fresh, whole window rather than a stale one that
  // expires a heartbeat later. That is the honest behaviour as well as the
  // safe one: a user who has just been answered is being invited to reply,
  // and the clock on that invitation should start when the invitation ends.
  //
  // Both are frame-loop-only in every path that exists today (the microphone
  // is drained from update(), not from an audio callback), so there is no
  // lock here and none is needed; see the note above the check in update().
  std::atomic<float> listen_timeout_{0.0f};
  std::chrono::steady_clock::time_point timeout_blocked_at_{};
  // The same noise gate as last_voice_, read over a longer window: when the
  // gate was last open *continuously* for kVoiceRunSec, and when the run
  // currently open began (zero when the gate is shut). This is what the
  // timeout measures from, and it is not a second detector — endpointing and
  // the timeout are decided against the same threshold on the same samples,
  // which is the property that keeps them from disagreeing about whether the
  // user is talking. See kVoiceRunSec for why the duration is there at all,
  // and for the measurement that put it there.
  std::chrono::steady_clock::time_point last_sustained_voice_{};
  std::chrono::steady_clock::time_point voice_run_began_{};
  // The decoder's last hypothesis, as the timeout saw it. A change in it is
  // the second thing that restarts the clock; see the note at the assignment.
  // Frame loop only, and deliberately separate from `partial_` under mutex_,
  // which the panel reads and which is cleared on paths this must not follow.
  std::string timeout_partial_;

  // M8.3. Which stages of the bring-up table this run actually performs, by
  // index. Built in the constructor from the language selection, because a
  // skipped Japanese voice must not leave a weighted slice of the progress bar
  // that nothing ever fills.
  std::vector<size_t> stage_ids_;

  // The language selection, as two bits in one atomic so that it can never be
  // read half-updated — two separate atomics have a window in which both look
  // false, and "no language at all" is the one state nothing here handles.
  std::atomic<unsigned> langs_bits_{0};

  std::thread loader_;
  // The on-demand Japanese voice (M8.3). Its own thread, for the same reason
  // the startup load has one: the frame loop must keep running. It is started
  // at most once per session and joined in the destructor.
  std::thread ja_loader_;
  std::atomic<bool> ja_loading_{false};
  std::atomic<bool> ja_failed_{false};
  std::atomic<bool> ja_started_{false};
  std::string ja_error_;  // written by the loader before ja_failed_ is set
  std::thread turn_;
  std::atomic<bool> loaded_{false};
  std::atomic<bool> load_failed_{false};
  std::atomic<bool> turn_running_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<unsigned> turn_generation_{0};

  mutable std::mutex mutex_;
  State state_ = State::Loading;
  std::string status_;
  float load_progress_ = 0.0f;
  std::string load_stage_;
  std::string usage_;
  std::string partial_;
  unsigned dictated_seq_ = 0;
  unsigned turn_failed_seq_ = 0;
  unsigned listen_timeout_seq_ = 0;  // M1f.1; see Snapshot::listen_timeout_seq
  float mic_level_ = 0.0f;
  std::vector<Line> lines_;
  // Worker reports waiting for a gap in which to be spoken.
  std::vector<std::string> pending_announce_;
  // M2b.4. Reports waiting for a gap in which a *turn* can be run. One string
  // each: the text handed to the conversational instance. Kept separate from
  // pending_announce_ because the two cost different things and take the floor
  // differently — an announcement is instant, a turn spends usage and seconds.
  //
  // M2c.1 turned the string into a record. Two fields were added and each is
  // there for one of the two failure modes this queue has: `fallback` is what
  // is said if the turn does not come back, so a model call can never swallow
  // a report; `report` (with `live_worker`) holds the raw sentence instead of
  // a composed turn, so a batch of live reports that arrived together can be
  // merged into a single turn when the floor finally comes free.
  struct PendingTurn {
    std::string sent;      // the composed turn; empty for a live worker report
    std::string report;    // the raw `shown` line, for a live worker report
    std::string fallback;  // spoken verbatim if the turn fails
    bool live_worker = false;
  };
  std::vector<PendingTurn> pending_turns_;
  // M2b.4. Workers this session started *from a schedule*, by name. Their
  // completion is reported by an injected turn instead of the canned sentence
  // a live worker gets, which is what makes a Japanese conversation hear
  // Japanese even though the worker's own task and reply were English. Erased
  // when it reports, so a later worker reusing the name is a live one again.
  //
  // M2b.5 turned this from a list of names into a small record, for two
  // reasons that are really one. First, a fired `Phrased` schedule leaves the
  // book the instant it fires while the user is still waiting for its report,
  // so "what is pending?" answered from `ScheduleBook::list()` alone would say
  // nothing is pending; this list is the other half of that answer. Second, to
  // cancel one of these the user has to be able to name it, and the only
  // handle ever spoken about is the schedule's own id — so the id rides along
  // and the id space stays single, monotonic and never reused.
  //
  // The entry is pushed *before* `spawn()` is called and erased if the spawn
  // fails, which is what makes the fire-then-register handoff atomic from the
  // cancel's point of view: there is no frame on which a schedule is neither
  // in the book nor in this list. See apply_cancels().
  struct ScheduledWorker {
    std::uint64_t id = 0;   // the schedule that started it; still the handle
    std::string name;       // the worker's name, for WorkerPool::stop()
    std::string label;      // what the user called it
    std::chrono::steady_clock::time_point started{};
    bool phrased = true;    // report through an injected turn (M2b.4)
    bool running = false;   // spawn() has returned successfully
    bool cancelled = false; // a cancel landed while it was still starting
  };
  std::vector<ScheduledWorker> scheduled_workers_;
  // M2b.5. Cancel requests from the turn thread, applied on the frame loop.
  std::vector<std::uint64_t> pending_cancels_;
  // M2b.5. Workers stopped on purpose, by name, whose completion report is
  // therefore not news. `WorkerPool::stop()` cancels the turn and the worker
  // reports itself as Paused on the way out — which is right for the pool and
  // wrong for the user here: they have just asked for this to stop and been
  // told it is stopping, so a "Paused." read out afterwards, or worse a whole
  // injected turn describing it, is the app talking about itself. Checked and
  // erased by the report callback.
  std::vector<std::string> silenced_workers_;
  std::vector<float> chunk_;
};

}  // namespace aii
