#include "core/speech_queue.h"

#include <chrono>
#include <vector>

#include "core/text_util.h"

namespace aii {
namespace {
// Silence inserted where a sentence hands over from one voice to the other.
// Long enough to hear as a deliberate beat, short enough not to read as the
// end of a clause.
constexpr size_t kVoiceSwitchPauseMs = 120;
}  // namespace

SpeechQueue::SpeechQueue(TtsEngine* english, TtsEngine* japanese, AudioOut* out)
    : en_(english), ja_(japanese), out_(out) {
  thread_ = std::thread([this] { run(); });
}

SpeechQueue::~SpeechQueue() {
  stop_ = true;
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void SpeechQueue::enqueue(const std::string& sentence) { enqueue(sentence, 1); }

void SpeechQueue::enqueue(const std::string& sentence, int voice_slot) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(Utterance{sentence, voice_slot});
  }
  cv_.notify_one();
}

void SpeechQueue::set_voices(std::vector<int> en, std::vector<int> ja) {
  std::lock_guard<std::mutex> lock(mutex_);
  en_voices_ = std::move(en);
  ja_voices_ = std::move(ja);
}


void SpeechQueue::clear() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    ++generation_;
  }
  out_->clear();
}

bool SpeechQueue::idle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.empty() && !busy_ && out_->idle();
}

void SpeechQueue::wait_idle() {
  while (!idle()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void SpeechQueue::run() {
  for (;;) {
    Utterance utterance;
    unsigned gen;
    std::vector<int> en_voices, ja_voices;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      utterance = std::move(queue_.front());
      queue_.pop_front();
      gen = generation_;
      busy_ = true;
      // Copied under the lock and read unlocked for the rest of the run: the
      // lists are written once at startup and are two short vectors, so this
      // costs less than holding the queue's mutex across synthesis would.
      en_voices = en_voices_;
      ja_voices = ja_voices_;
    }
    // A sentence can mix the two languages ("その file は ready です"), so it
    // is spoken run by run, each in the voice that fits, in order.
    //
    // The voice slot is resolved per run rather than per utterance for the same
    // reason the engine is: the slot means "the Nth voice *of the language this
    // run is in*", so one sentence carrying both languages resolves twice.
    const TtsEngine* spoke_last = nullptr;
    int spoke_last_voice = -2;  // -1 is a real value (the primary), so not that
    for (const auto& run : split_by_script(utterance.text)) {
      if (gen != generation_) break;  // a clear() landed mid-sentence
      TtsEngine* ja = ja_.load(std::memory_order_acquire);
      TtsEngine* engine = run.japanese ? ja : en_;
      if (!engine || !engine->ok()) engine = (en_ && en_->ok()) ? en_ : ja;
      // Which list to read is decided by the engine that will actually speak,
      // not by the script of the run -- the two differ whenever the fallback
      // above has fired, and a VOICEVOX style handed to Kokoro is a speaker id
      // that engine will silently accept as somebody else entirely.
      const bool ja_engine = ja != nullptr && engine == ja;
      int voice = -1;
      if (utterance.voice > 1) {
        const std::vector<int>& list = ja_engine ? ja_voices : en_voices;
        const size_t index = static_cast<size_t>(utterance.voice - 2);
        if (index < list.size()) voice = list[index];
      }
      AudioChunk chunk;
      bool okay = engine && engine->synthesize_as(run.text, voice, chunk);
      if (okay && gen == generation_) {  // discard if a clear() happened meanwhile
        if (chunk.sample_rate != out_->sample_rate() && on_status_)
          on_status_("warning: engine rate " + std::to_string(chunk.sample_rate) + " != output rate " +
                     std::to_string(out_->sample_rate()));
        // Handing straight from one voice to the other inside a sentence is
        // jarring: the two engines have different timbre and neither leaves
        // any room at its edges, so the switch lands as a splice. A beat of
        // silence reads as the speaker changing language rather than as a
        // glitch. Only between different voices, never before the first run
        // or between two runs that ended up on the same engine anyway.
        //
        // M13.2 widened this from the engine to the engine *and* the voice. Two
        // Kokoro voices are the same engine pointer, so a change from v1 to v2
        // within English used to get no beat at all -- and it is needed more
        // there, not less, because the listener has no language change to cue
        // them that somebody else is speaking.
        if (spoke_last && (spoke_last != engine || spoke_last_voice != voice)) {
          const size_t n = static_cast<size_t>(out_->sample_rate()) * kVoiceSwitchPauseMs / 1000;
          const std::vector<float> gap(n, 0.0f);
          out_->push(gap.data(), gap.size());
        }
        out_->push(chunk.samples.data(), chunk.samples.size());
        spoke_last = engine;
        spoke_last_voice = voice;
        if (first_audio_pending_.exchange(false) && on_first_audio_) on_first_audio_();
      } else if (!okay && on_status_) {
        on_status_(std::string("synthesis failed (") + (engine ? engine->name() : "none") + "): " +
                   run.text);
      }
    }
    busy_ = false;
  }
}

}  // namespace aii
