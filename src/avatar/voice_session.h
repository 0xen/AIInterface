#pragma once
// The voice loop behind the avatar window: engines load on a background
// thread, the microphone feeds the recogniser from the frame loop, Claude
// turns run on a worker thread, and replies are spoken through the speech
// queue. The window reads an immutable Snapshot each frame.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
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
    // Reset (user, 19 Sep 2026). Two facts the transport row's fourth slot
    // needs and cannot work out for itself.
    //
    // `resettable` is "is there anything to throw away", and it is defined as
    // "the transcript is not empty" rather than as a turn counter, because
    // reset clears the transcript: the two are then the same fact, and the
    // rule the user can see on screen ("the button is live exactly when there
    // is something in the chat") is the rule the code enforces.
    //
    // `resetting` is the second or so in which the old `claude` child is being
    // torn down and a new one started. Deliberately not a sixth `State`: the
    // session is genuinely idle throughout — no turn, no microphone, no
    // speech — and a state nothing else in this app knows about would have to
    // be taught to the avatar, to `anything_to_stop()` and to every switch on
    // `State` in the panel, all to say something only the reset button draws.
    bool resettable = false;
    bool resetting = false;
    // M3.12. What the `claude` child that is running *now* was launched with —
    // the `--model` argument ("" = no flag) and the tool policy. The settings
    // surface draws its picker and its tick boxes against these, and since a
    // change to either restarts the child (apply_llm_settings), they are the
    // only honest source: they are written by the restart thread when the new
    // child is up, so a restart that failed to start one does not claim the
    // new values are in force.
    std::string model_in_force;
    ToolPolicy tools_in_force;
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
  // M2c.2. A finished report is waiting for a voice. Any thread. It exists for
  // the harness (`--say-on-report`): the moment a report lands is the moment
  // the riding path has to be entered to be observed at all, and from outside
  // the process that moment is otherwise invisible until it has already been
  // spoken. Nothing in the app's own behaviour reads it.
  bool reports_waiting() const;
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
  // Reset: throw the conversation away and start a fresh one (user, 19 Sep
  // 2026). **Frame loop only**, and a no-op until the engines are up.
  //
  // The conversation is not a variable in this process. It lives inside the
  // `claude -p --input-format stream-json` child, which is long-lived and runs
  // with `--no-session-persistence`, so there is nothing on disk to delete and
  // no message that means "forget". The only honest reset is to end that child
  // and start another with the same options — same system prompt, same tool
  // list, same model — which is what this does. There is exactly one child at
  // any moment, by construction: the old one's destructor runs to completion
  // before `build_llm()` is called, and it is the destructor that closes the
  // pipe and waits.
  //
  // What it does, in order, and what it deliberately leaves alone:
  //
  //   * Everything stop() does first — the reply in flight is interrupted, the
  //     speech queue is emptied, the microphone latch and any held Talk
  //     gesture are dropped, running workers are paused. A turn must not be
  //     writing into a session that no longer exists, so the turn thread is
  //     joined before the child is replaced.
  //   * **The latch comes back.** It is closed for the second the rebuild
  //     takes and reopened afterwards if it was open, because closing it is
  //     how the mechanism works rather than what the button means — see
  //     `relatch_after_reset_`. A held Talk gesture is not reproduced.
  //   * The transcript is cleared, and so is the injected-prompt set
  //     (`PromptInjector::clear_session`, which has been waiting for exactly
  //     this caller). A fresh context window has loaded nothing.
  //   * Mute is untouched: it is a preference about this window, not a fact
  //     about the conversation. Language selection, the listen timeout, the
  //     avatar and the settings file likewise.
  //   * **Workers are not killed.** A worker is a separate process doing work
  //     the user asked for; clearing the chat is not a reason to throw away
  //     ten minutes of a build. They are paused by stop(), exactly as Stop
  //     pauses them, and reports they have already queued stay queued — every
  //     one of them is a self-contained `<context …>` block naming the work,
  //     so the fresh session can deliver it without ever having heard the
  //     conversation that started it.
  //   * **Schedules are not cancelled.** A schedule is a promise made to the
  //     user in words. Reset clears what the AI remembers, not what the app
  //     owes, and the new session is told what is outstanding on its first
  //     turn anyway: pending_context() is built from the schedule book, which
  //     this does not touch.
  //
  // It returns immediately; the teardown and relaunch run on `reset_`, because
  // the child's destructor waits up to three seconds for it to exit and
  // start() waits half a second for the new one, and a frame loop that stalls
  // for that long is a window that stops compositing. While it runs,
  // `resetting()` is true and update() hands the session over: nothing on the
  // frame loop touches `turn_` or `eng_.llm` until it clears.
  void reset();
  // M3.12: a changed model or tool grant, taken up **now** (user, 19 Sep 2026:
  // "restart immediately, lose context"). **Frame loop only.**
  //
  // `--model` and `--allowedTools` are fixed when the child starts, so there
  // is no level to push down and the three honest answers were: wait for the
  // next launch (what M3.8 and M3.11 shipped, and what the amber lines said),
  // restart and replay the conversation (M3.6, unbuilt), or restart and lose
  // it. This is the third. It is the same machinery reset() uses, down to the
  // thread, because it is the same act: one child ends and another starts.
  //
  // What differs from reset() is only what it *means*, and that is two things:
  //
  //   * The new child is built from a Config this has just written, so it is
  //     the first thing in this app that changes `cfg_` after load(). Safe
  //     because the write happens on the frame loop before `resetting_` is
  //     released, and the only reader of these two fields is `build_llm` on
  //     the restart thread, after it acquires.
  //   * The status line says a setting was applied rather than that the
  //     conversation was cleared. The user did not ask to forget; they asked
  //     for a different model, and losing the conversation is the price of it,
  //     which is a sentence about the price rather than about the act.
  //
  // Returns false and does nothing when the values already match what is in
  // force, when the engines are not up, and while a restart is already
  // running — the last of which is what keeps a hand running down the tick
  // boxes to one restart rather than three. The caller is expected to let a
  // change settle and to wait for a reply in flight; see main.cpp.
  bool apply_llm_settings(const ToolPolicy& tools, const std::string& model_arg);
  // M3.15. Hand over to a fresh session **now**, whatever the context reading
  // says (user, 19 Sep 2026: *"when its context starts growing a little bit
  // too big, say, for example, around 40%, the AI will auto hand off to the
  // next AI and restart itself within the session… before it does this, it
  // will prompt the user and say, I just need a moment to do some
  // housekeeping"*). **Frame loop only**, and it returns straight away: this
  // arms a sequence that takes several seconds and several frames.
  //
  // The name is `handoff_now` and not `restart` on purpose — it is not a
  // fourth way to replace the child, it is the one thing that *ends* in a
  // replacement, and it is the whole sequence rather than the last step of it:
  //
  //   1. the housekeeping line is spoken, **first**, because the point of it
  //      is that the several seconds of silence are explained while they are
  //      happening rather than apologised for once they are over;
  //   2. the outgoing session is asked, in a turn that is never spoken and
  //      never reaches the transcript, to write a note to its replacement;
  //   3. the child is replaced through exactly the machinery reset() and
  //      apply_llm_settings() use, down to the thread;
  //   4. that note rides in with the new session's first turn.
  //
  // What it will not do: interrupt anything. It arms, and the sequence starts
  // at the next settled moment — no turn running, nothing queued to say,
  // nothing half-spoken, the microphone not in the middle of an utterance.
  // A handoff that cuts a reply in half is worse than one that waits, and
  // waiting costs a turn's worth of context at most.
  //
  // Returns false and arms nothing when the engines are not up, while a
  // restart is already running, and while a handoff is already under way —
  // the last of which is what a second press, or a second script, gets.
  bool handoff_now();
  // True from the moment a handoff is armed until the new child is up. Frame
  // loop only; it exists for the callers that have to refuse a second one.
  bool handing_off() const;
  // True from the moment reset() is called until the new child is up. Frame
  // loop and snapshot only.
  bool resetting() const { return resetting_.load(std::memory_order_acquire); }
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
  // M2b.2. Cancel one id and say nothing about it. **Frame loop only.** This
  // is apply_cancels()'s body, one id at a time, and it is public because the
  // bus needs the *act* without the sentence: apply_cancels() speaks when a
  // cancel misses because by then the model has already told the user out loud
  // that it is cancelled, and a script has told the user nothing at all. A
  // miss is data the script asked for and gets back on the bus; speaking it
  // would be the app announcing another program's bookkeeping.
  //
  // Safe from the bus handler because AppBus::apply_pending() runs earlier in
  // the same frame, on the same thread, as the schedule tick: a schedule is
  // either still in the book here, or it fired on an earlier frame and is
  // already in `scheduled_workers_`. There is no frame on which it is neither,
  // which is the whole of M2b.5's argument and this call inherits it.
  bool cancel_schedule(std::uint64_t id);
  // M2b.2. The pending list as data rather than as the sentence
  // pending_context() writes for the model. Same three-part definition of
  // "pending" — schedules not yet due, and workers a schedule started that are
  // still running — because a script asking "what is pending?" is asking the
  // same question the user is, and answering it from the book alone would tell
  // a script that a build it is waiting for does not exist. Any thread.
  struct PendingItem {
    std::uint64_t id = 0;
    std::string kind;   // "timer" / "worker", or "running" once it has started
    std::string label;
    bool phrased = false;
    double seconds = 0.0;  // until due; for a running one, how long it has run
  };
  std::vector<PendingItem> pending_items() const;
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
  // M2c.1/M2c.2. One report waiting for a voice. Defined here rather than
  // beside `pending_turns_` because a batch of these now travels *into*
  // run_turn() as well as sitting in the queue — see rider_reports().
  //
  // Two fields exist for the two failure modes this queue has: `fallback` is
  // what is said if the turn does not come back, so a model call can never
  // swallow a report; `report` (with `live_worker`) holds the raw sentence
  // instead of a composed turn, so a batch that arrived together can be merged
  // into a single turn when the floor finally comes free.
  struct PendingTurn {
    std::string sent;      // the composed turn; empty for a live worker report
    std::string report;    // the raw `shown` line, for a live worker report
    std::string fallback;  // spoken verbatim if the turn fails
    bool live_worker = false;
  };

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
  // Why the child is being replaced. The mechanism is identical either way —
  // this decides one status line and one log line, and nothing else. It is not
  // a mode: a restart for a setting still clears the transcript, still keeps
  // the workers and the schedules, still gives the latch back.
  enum class RestartReason { Reset, Settings, Handoff };
  // The shared body of reset() and apply_llm_settings(), on the frame loop:
  // everything stop() does, the latch noted, the fence released and the thread
  // started. See both callers for what each of them means by it.
  void begin_restart(RestartReason why);
  // begin_restart()'s tail, on `reset_`. Joins the turn thread, replaces the
  // child, clears the transcript and the injected-prompt set, and publishes
  // the new status. Nothing else may touch `turn_` or `eng_.llm` while it
  // runs; see the guard at the top of update() and `resetting_`.
  void run_reset();

  // ------------------------------------------------------------- M3.15
  //
  // ## The handover, as four frame-loop states and one thread
  //
  // The sequence is spread over several seconds and cannot be a function, so
  // it is a small machine that `update()` advances. Every transition but one
  // happens on the frame loop; the exception is the summary turn finishing,
  // which is the thread that was running it.
  //
  //   None        nothing happening. The only state in which the threshold is
  //               looked at, and it is looked at from the Idle branch only —
  //               which is what "never mid-turn, never mid-sentence, never
  //               with the microphone open" reduces to, since Idle already
  //               means all three.
  //   Speaking    the housekeeping line has been handed to the speech queue
  //               (or dropped, if muted). Waits for it to actually finish.
  //   Summarising the silent turn is running on `turn_`.
  //   Restarting  a summary is written, or was not, and the child can go.
  //
  // The re-arming question — `ctx` crossing the threshold is a **level**, not
  // an edge, so it stays crossed — is answered by `None` being reachable only
  // from `run_reset()`. Between arming and the new child being up there is no
  // frame on which a second handoff can start, and once the new child is up
  // its `ctx` is the cost of existing rather than the cost of the
  // conversation. A `ctx` of -1.0 (a child that has not reported yet) is
  // never read as a low number; see `core/handoff_policy.h`.
  enum class HandoffStage { None, Speaking, Summarising, Restarting };
  // Called from update() before the state branches, because the stage that
  // matters most (`Restarting`) is reached while the session is Thinking, and
  // Thinking has no branch of its own.
  void tick_handoff(State s);
  // The Idle branch's tail: read the context, decide, and — if it is time —
  // say the housekeeping line and enter the machine. True when it took the
  // frame, in which case the microphone is deliberately *not* reopened.
  bool begin_handoff_if_due();
  // The silent turn. Sets up `turn_` exactly as start_injected_turn() does and
  // differs from it in what it does with the reply: nothing is spoken, nothing
  // is shown, nothing is stripped into the transcript. The reply is the note,
  // and it goes into `handoff_summary_`.
  void start_handoff_summary();
  void run_handoff_summary();
  // The prose sent by that turn: `prompts/system/handoff.md`, which is seeded
  // and editable, with a built-in copy for when it cannot be read. **Not** a
  // system prompt and deliberately not a node in `graph.json` — it is one
  // turn's words, not every turn's, and composing it would put a paragraph
  // about a rare event in front of every question the user ever asks.
  std::string handoff_request();
  // The note, wrapped for the new session's first turn. The same idiom as
  // pending_context(): a `<context>` block, in English, composed by the app,
  // because everything the *model* reads is machine traffic (app_strings.h).
  static std::string carry_over_block(const std::string& note);

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
  // M2c.2. `rider` is a batch of finished live-worker reports the user's own
  // turn is carrying, so the answer and the aside are one reply instead of two
  // utterances. Empty on every other path, including every injected turn.
  void run_turn(std::string text, bool is_injected, std::string fallback = std::string(),
                std::vector<PendingTurn> rider = std::vector<PendingTurn>());
  // M2c.2. Take the live-worker reports that are ready *right now*, for the
  // user turn that is about to start. Exactly the batch flush_injected_turns()
  // would have taken — the leading run of live reports — so a scheduled report
  // sitting in front of them keeps its place and nothing is reordered. Empty
  // is the ordinary answer and costs the turn nothing.
  std::vector<PendingTurn> take_riding_reports();
  // M2c.2. Put a rider back at the head of the queue when the turn carrying it
  // did not speak: cancelled, or failed. This is the whole of "nothing is
  // lost" on the rider path — the report goes back to being an ordinary queued
  // report and the next gap delivers it on its own, exactly as before.
  void requeue_riding_reports(std::vector<PendingTurn> rider);
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
  // Records one turn's *input* text as evidence that a folder was named, and
  // returns the whole window joined up. See `folder_evidence_`.
  void note_folder_evidence(const std::string& text);
  std::string folder_evidence() const;
  // M2b.4. The text handed to Claude when a scheduled worker finishes.
  static std::string scheduled_report_prompt(WorkerPool::State state, const std::string& shown);
  // M2c.1. The text handed to Claude when one or more *live* workers finish.
  // Takes the whole batch, because two workers finishing in the same gap are
  // one thing to say, not two turns talking over each other.
  static std::string live_report_prompt(const std::vector<std::string>& shown);
  // M2c.2. The same facts as live_report_prompt(), framed as an aside to be
  // added to the end of an answer the user is waiting for rather than as a
  // reply of its own.
  static std::string rider_report_prompt(const std::vector<std::string>& shown);
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
  // The reset thread, and the flag that fences the frame loop off from `turn_`
  // and `eng_.llm` while it owns them.
  //
  // The flag is the whole of the synchronisation and it is enough because of
  // who writes it. It is set to true **on the frame loop**, before `reset_` is
  // started; snapshot() and update() are frame-loop-only, so from that store
  // onwards no frame can reach the client pointer the reset thread is about to
  // swap. It is cleared by the reset thread with a release store once the new
  // client is in place, and the frame loop's acquire load is what publishes
  // it. No lock, and none that would not have to be held across a whole turn.
  std::thread reset_;
  std::atomic<bool> resetting_{false};
  // Which of the two callers started the restart that is running. Written on
  // the frame loop before `resetting_` is released and read only by the
  // restart thread, which is the same fence the `cfg_` write uses.
  RestartReason restart_reason_ = RestartReason::Reset;
  // What the running child was launched with, mirrored into the snapshot.
  // Written by the restart thread under `mutex_` **only when the new child
  // came up**, and by load() for the first one, so the settings surface can
  // never be told that a model is in force that nothing is running on.
  std::string model_in_force_;
  ToolPolicy tools_in_force_;
  // What the last restart *was*, as the one clause that explains it — "the
  // conversation was cleared", "the new settings are in force", "the note
  // carried over". Written by the restart thread under `mutex_` and read by
  // the frame loop afterwards, which is the same fence as the two above.
  //
  // It exists because there are two places that sentence is said and they had
  // drifted: run_reset() writes it with "ready." on the end, and the relatch
  // branch of update() writes it again with "listening..." on the end. That
  // second copy was a literal about *reset* and was already being shown after
  // a settings restart.
  std::string restart_note_;
  // The microphone latch was on when reset() was called, and is owed back.
  //
  // Reset has to close the latch on the way in — update() is handed to the
  // reset thread for that second, so an open microphone would go undrained —
  // but closing it is a side effect of the mechanism, not the point of the
  // button. Stop drops the latch because stopping is what Stop *means*; reset
  // means forget, and a user in conversation mode who cleared the context did
  // not ask to be dropped out of conversation mode as well. With
  // `startup.auto_listen` on (19 Sep 2026) the latch is the state the app
  // chooses for itself at launch, so dropping it here would be reset quietly
  // undoing a setting.
  //
  // Frame loop only, both ends: written by reset(), read and cleared by
  // update() on the first frame after `resetting_` falls.
  bool relatch_after_reset_ = false;

  // M3.15. Where the handover is up to. Atomic for one transition only —
  // `Summarising` -> `Restarting`, made by the summary thread as the last
  // thing it does — and read on the frame loop; every other write is the
  // frame loop's own.
  std::atomic<HandoffStage> handoff_stage_{HandoffStage::None};
  // A handoff asked for by name rather than by the numbers (`handoff_now()`).
  // A flag rather than a stage because a forced handoff still waits for the
  // same settled moment as an automatic one: it is consumed by the Idle
  // branch, not acted on where it is set.
  std::atomic<bool> handoff_forced_{false};
  // **What this session cost before anybody said anything** — its first
  // reported `ctx`, or negative until it has one. It is the guard against the
  // single most expensive failure this feature has: a threshold at or under
  // the cost of merely existing would have every fresh session already over
  // the line, handing over again, one summary turn per turn, forever. See
  // `handoff_due()`, which refuses rather than firing. Frame loop only; reset
  // by run_reset() because a new child is a new floor.
  double handoff_floor_ = -1.0;
  // That refusal, said once per session rather than once per frame.
  bool handoff_warned_ = false;
  // The note, written by the summary turn on `turn_` and read by the restart
  // thread after it has joined it. No lock, for the same reason `injector_`
  // needs none: the two threads never overlap, by construction.
  std::string handoff_summary_;
  // The same note after the restart, waiting for the first turn of the new
  // session to carry it. Written by the restart thread, consumed by the turn
  // thread, and those two are ordered by `resetting_` — no turn starts while
  // it is set, and the write happens before it falls.
  std::string carry_over_;

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
  // The record itself is declared at the top of this section, because a batch
  // of them now rides into run_turn() as well as waiting here.
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
  // The folders this conversation can be said to have named, as raw text: what
  // the user typed or dictated, and what the *app* composed and handed to the
  // model (a worker's report, a pending list). It is the evidence
  // `resolve_worker_cwd()` weighs a `cwd=` against before a worker is started
  // at bypassPermissions in it -- see core/cwd_policy.h.
  //
  // Only the **input** side of a turn is recorded, never Claude's reply. That
  // is the whole guarantee: a folder the model invented and then said out loud
  // must not become the corroboration for the same folder next turn. Written
  // on the turn thread, read on the turn thread, and guarded anyway because
  // the panel may come for it later.
  std::deque<std::string> folder_evidence_;
  std::vector<float> chunk_;
};

}  // namespace aii
