#pragma once
// Background worker: takes sentences, picks an engine by script, synthesises,
// and pushes samples straight to the speaker ring buffer.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "audio/audio_out.h"
#include "tts/tts_engine.h"

namespace aii {

class SpeechQueue {
 public:
  SpeechQueue(TtsEngine* english, TtsEngine* japanese, AudioOut* out);
  ~SpeechQueue();

  void enqueue(const std::string& sentence);
  void clear();                 // drop queued sentences and any audio not yet played
  bool idle() const;            // nothing queued, nothing synthesising, nothing playing
  void wait_idle();

  // Called on the worker thread the first time audio is pushed after a clear/start.
  void set_on_first_audio(std::function<void()> cb) { on_first_audio_ = std::move(cb); }
  void mark_new_reply() { first_audio_pending_ = true; }
  void set_on_status(std::function<void(const std::string&)> cb) { on_status_ = std::move(cb); }

 private:
  void run();

  TtsEngine* en_;
  TtsEngine* ja_;
  AudioOut* out_;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  std::atomic<bool> busy_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> first_audio_pending_{false};
  std::atomic<unsigned> generation_{0};
  std::function<void()> on_first_audio_;
  std::function<void(const std::string&)> on_status_;
};

}  // namespace aii
