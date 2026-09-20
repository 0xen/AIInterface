#pragma once
// Background worker: takes sentences, picks an engine by script, synthesises,
// and pushes samples straight to the speaker ring buffer.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <vector>
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

  // The Japanese voice arrives late, or not at all (M8.3). With Japanese
  // switched off in the settings, VOICEVOX is never built and this is
  // constructed with `japanese == nullptr`; switching Japanese on mid-session
  // loads it on a background thread and hands it over here when it is ready.
  //
  // Atomic rather than mutex-guarded because the worker thread reads it once
  // per script run and the frame loop writes it at most once a session: a lock
  // on the read path would cost more than the hand-over it is protecting. A
  // run that reads the old null simply takes the English fallback below, which
  // is what it would have done a microsecond earlier anyway.
  void set_japanese(TtsEngine* japanese) { ja_.store(japanese, std::memory_order_release); }
  bool has_japanese() const { return ja_.load(std::memory_order_acquire) != nullptr; }

  void enqueue(const std::string& sentence);
  // M13.1/M13.2. The same, in a numbered voice. Slots are 1-based and **slot 1
  // is always the engine's own configured voice** (`cfg.kokoro_sid`,
  // `cfg.vv_style`) -- one owner of record for the primary, so it is never in
  // a list and cannot drift from the setting the user chose. Slot N>1 indexes
  // `en[N-2]` / `ja[N-2]`.
  //
  // The *slot* travels, not the engine id, because resolving one needs the
  // language and the language is not known until `split_by_script` runs inside
  // the worker: a single sentence can carry both.
  void enqueue(const std::string& sentence, int voice_slot);

  // The per-language secondary voices, in engine-native ids. Empty lists --
  // the default -- mean exactly today's behaviour, because every slot then
  // resolves to the primary.
  //
  // **Both lists must already be valid for their engine.** A bad id fails in
  // opposite directions and neither is recoverable here: Kokoro silently speaks
  // in `af_alloy` instead of refusing, and VOICEVOX returns an error that
  // reaches the user as silence. Validation therefore belongs where the lists
  // are read, before anything is spoken.
  void set_voices(std::vector<int> en, std::vector<int> ja);

  void clear();                 // drop queued sentences and any audio not yet played
  bool idle() const;            // nothing queued, nothing synthesising, nothing playing
  void wait_idle();

  // Called on the worker thread the first time audio is pushed after a clear/start.
  void set_on_first_audio(std::function<void()> cb) { on_first_audio_ = std::move(cb); }
  void mark_new_reply() { first_audio_pending_ = true; }
  void set_on_status(std::function<void(const std::string&)> cb) { on_status_ = std::move(cb); }

 private:
  void run();

  struct Utterance {
    std::string text;
    int voice = 1;  // slot, not an engine id
  };

  // English is never swapped and never absent: it is also the fallback the
  // script splitter routes every Latin run to (names, numbers, code words),
  // which appear inside Japanese replies too. So Kokoro loads whatever the
  // language setting says, and VOICEVOX is the only conditional one.
  TtsEngine* en_;
  std::atomic<TtsEngine*> ja_;
  AudioOut* out_;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Utterance> queue_;
  std::vector<int> en_voices_;  // slot 2 onward; slot 1 is the engine's own
  std::vector<int> ja_voices_;
  std::atomic<bool> busy_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> first_audio_pending_{false};
  std::atomic<unsigned> generation_{0};
  std::function<void()> on_first_audio_;
  std::function<void(const std::string&)> on_status_;
};

}  // namespace aii
