#pragma once
// The voice loop behind the avatar window: engines load on a background
// thread, the microphone feeds the recogniser from the frame loop, Claude
// turns run on a worker thread, and replies are spoken through the speech
// queue. The window reads an immutable Snapshot each frame.
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_out.h"
#include "audio/mic_in.h"
#include "core/config.h"
#include "core/engines.h"
#include "core/speech_queue.h"
#include "core/worker_pool.h"
#include "llm/llm_client.h"

namespace aii {

class VoiceSession {
 public:
  enum class State { Loading, Idle, Listening, Thinking, Speaking, Failed };

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
  void say(const std::string& text);   // send typed/scripted text as the user turn
  bool quitting_ok() const;            // true once no worker is mid-turn

  Snapshot snapshot() const;
  static const char* state_name(State s);

 private:
  void load();
  // Moves the loading screen on to stage `index` of kLoadStages (loader thread
  // only). Progress becomes the weight of everything before it, so it only
  // ever climbs; an index past the end means loaded, 1.0 and no stage name.
  // Returns the progress it published, for the trace line.
  float begin_load_stage(size_t index);
  void set_mic_open(bool open);
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
  void run_turn(std::string text);
  void run_commands(const std::string& reply_text);
  // Speak a line from the app itself (worker reports) and show it.
  void announce(const std::string& text);
  // Speaks anything announce() left queued, closing the microphone first.
  // True if it took the floor. Frame loop only.
  bool flush_announcements();
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

  std::thread loader_;
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
  float mic_level_ = 0.0f;
  std::vector<Line> lines_;
  // Worker reports waiting for a gap in which to be spoken.
  std::vector<std::string> pending_announce_;
  std::vector<float> chunk_;
};

}  // namespace aii
