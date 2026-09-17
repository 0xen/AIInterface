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

void SpeechQueue::enqueue(const std::string& sentence) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(sentence);
  }
  cv_.notify_one();
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
    std::string sentence;
    unsigned gen;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      sentence = std::move(queue_.front());
      queue_.pop_front();
      gen = generation_;
      busy_ = true;
    }
    // A sentence can mix the two languages ("その file は ready です"), so it
    // is spoken run by run, each in the voice that fits, in order.
    const TtsEngine* spoke_last = nullptr;
    for (const auto& run : split_by_script(sentence)) {
      if (gen != generation_) break;  // a clear() landed mid-sentence
      TtsEngine* ja = ja_.load(std::memory_order_acquire);
      TtsEngine* engine = run.japanese ? ja : en_;
      if (!engine || !engine->ok()) engine = (en_ && en_->ok()) ? en_ : ja;
      AudioChunk chunk;
      bool okay = engine && engine->synthesize(run.text, chunk);
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
        if (spoke_last && spoke_last != engine) {
          const size_t n = static_cast<size_t>(out_->sample_rate()) * kVoiceSwitchPauseMs / 1000;
          const std::vector<float> gap(n, 0.0f);
          out_->push(gap.data(), gap.size());
        }
        out_->push(chunk.samples.data(), chunk.samples.size());
        spoke_last = engine;
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
