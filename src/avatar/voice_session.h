#pragma once
// The voice loop behind the avatar window: engines load on a background
// thread, the microphone feeds the recogniser from the frame loop, Claude
// turns run on a worker thread, and replies are spoken through the speech
// queue. The window reads an immutable Snapshot each frame.
#include <atomic>
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
    std::string partial;  // live transcript while listening
    std::vector<Line> lines;
  };

  explicit VoiceSession(Config cfg);
  ~VoiceSession();

  void update();                       // once per frame, main thread
  void toggle_talk();                  // start listening / stop and send (barge-in while speaking)
  void silence();                      // stop the audio, keep the text coming
  void pause();                        // cancel the current reply and stop the audio
  void say(const std::string& text);   // send typed/scripted text as the user turn
  bool quitting_ok() const;            // true once no worker is mid-turn

  Snapshot snapshot() const;
  static const char* state_name(State s);

 private:
  void load();
  void begin_listening();
  void start_turn(std::string text);
  void run_turn(std::string text);
  void set_state(State s);
  void set_status(const std::string& s);
  void log(const std::string& s);

  Config cfg_;
  Engines eng_;
  std::unique_ptr<AudioOut> speaker_;
  std::unique_ptr<MicIn> mic_;
  std::unique_ptr<SpeechQueue> speech_;

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
  std::string usage_;
  std::string partial_;
  std::vector<Line> lines_;
  std::vector<float> chunk_;
};

}  // namespace aii
