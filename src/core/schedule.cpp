#include "core/schedule.h"

#include <algorithm>
#include <cstdlib>

namespace aii {
namespace {

// The bus's rule, applied here: bound an untrusted string rather than reject
// the message carrying it. A script or a model that sends a 4 KB label gets a
// truncated label, not a refused timer.
void clamp(std::string& s) {
  if (s.size() > kScheduleStringMax) s.resize(kScheduleStringMax);
}

}  // namespace

bool parse_delay(const std::string& text, double* seconds) {
  // Strip spaces and lowercase, so "10 Min" and "10min" are one case.
  std::string t;
  t.reserve(text.size());
  for (char ch : text) {
    if (ch == ' ' || ch == '\t') continue;
    t.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
  }
  if (t.empty()) return false;

  double total = 0.0;
  bool any = false;
  size_t i = 0;
  while (i < t.size()) {
    // number
    const size_t num_start = i;
    while (i < t.size() && ((t[i] >= '0' && t[i] <= '9') || t[i] == '.')) ++i;
    if (i == num_start) return false;  // a unit with no number, or junk
    const double value = atof(t.substr(num_start, i - num_start).c_str());
    // unit: the first letter is enough ("m", "min", "minutes" all agree), and
    // a bare number is seconds because that is what every other duration in
    // this codebase is.
    double mult = 1.0;
    const size_t unit_start = i;
    while (i < t.size() && t[i] >= 'a' && t[i] <= 'z') ++i;
    if (i > unit_start) {
      switch (t[unit_start]) {
        case 's': mult = 1.0; break;
        case 'm': mult = 60.0; break;
        case 'h': mult = 3600.0; break;
        case 'd': mult = 86400.0; break;
        default: return false;
      }
    }
    total += value * mult;
    any = true;
  }
  if (!any) return false;
  if (!(total > 0.0)) return false;  // also catches NaN
  if (total > kScheduleMaxDelaySeconds) return false;
  if (seconds) *seconds = total;
  return true;
}

// M2b.5. The units a person asks in. Rounded on purpose: see the header.
//
// There is no "in 3 minutes and 42 seconds" band and there never will be. The
// bands below are the ones a person uses out loud, and the hedge ("about")
// carries the rounding honestly instead of hiding it — the one place it is
// dropped is under a minute, where "in less than a minute" is already exact
// enough to act on and "about a minute" would be vaguer than the truth.
std::string describe_delay(double seconds) {
  if (seconds <= 0.5) return "due now";
  if (seconds < 45.0) return "in less than a minute";
  if (seconds < 90.0) return "in about a minute";
  const long total_min = static_cast<long>(seconds / 60.0 + 0.5);
  if (total_min < 60) return "in about " + std::to_string(total_min) + " minutes";
  const long h = total_min / 60;
  const long m = total_min % 60;
  std::string out = "in about " + std::to_string(h) + (h == 1 ? " hour" : " hours");
  if (m > 0) out += " " + std::to_string(m) + (m == 1 ? " minute" : " minutes");
  return out;
}

const char* to_string(ReportGrade grade) {
  return grade == ReportGrade::Phrased ? "phrased" : "fixed";
}

ReportGrade grade_from_string(const std::string& s) {
  return s == "phrased" ? ReportGrade::Phrased : ReportGrade::Fixed;
}

double Schedule::seconds_until(std::chrono::steady_clock::time_point now) const {
  return std::chrono::duration<double>(due - now).count();
}

ScheduleBook& ScheduleBook::instance() {
  static ScheduleBook book;
  return book;
}

void ScheduleBook::note_locked(std::string line) {
  if (status_.size() >= 64) status_.erase(status_.begin());
  status_.push_back(std::move(line));
}

std::uint64_t ScheduleBook::create(std::chrono::duration<double> delay, ScheduleAction action,
                                   ReportGrade grade, std::string* error) {
  if (action.kind.empty()) {
    if (error) *error = "a schedule needs a kind";
    std::lock_guard<std::mutex> l(mutex_);
    note_locked("schedule refused: no kind");
    return 0;
  }
  clamp(action.kind);
  clamp(action.label);
  clamp(action.report);
  clamp(action.task);
  clamp(action.name);
  clamp(action.cwd);

  // Clamped at zero rather than refused. "Do it now" is a legitimate thing to
  // ask for, and a negative delay from a clock skew must fire rather than sit
  // in the book for the rest of the session.
  const double secs = std::max(0.0, delay.count());
  const auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> l(mutex_);
  if (pending_.size() >= kSchedulesMax) {
    if (error) *error = "too many schedules pending";
    note_locked("schedule refused: " + std::to_string(kSchedulesMax) + " already pending");
    return 0;
  }
  Schedule s;
  s.id = next_id_++;
  s.created = now;
  s.due = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(secs));
  s.due_wall = std::chrono::system_clock::now() +
               std::chrono::duration_cast<std::chrono::system_clock::duration>(
                   std::chrono::duration<double>(secs));
  s.action = std::move(action);
  s.grade = grade;
  // Kept sorted, so tick() looks at the front and stops. Insertion is O(n) on
  // a list bounded at 64; a heap would be a worse trade because `list()` and
  // `cancel()` both want the ordered view.
  const auto at = std::upper_bound(
      pending_.begin(), pending_.end(), s.due,
      [](std::chrono::steady_clock::time_point d, const Schedule& e) { return d < e.due; });
  pending_.insert(at, std::move(s));
  return next_id_ - 1;
}

bool ScheduleBook::cancel(std::uint64_t id) {
  std::lock_guard<std::mutex> l(mutex_);
  const auto it = std::find_if(pending_.begin(), pending_.end(),
                               [id](const Schedule& s) { return s.id == id; });
  if (it == pending_.end()) return false;
  pending_.erase(it);
  return true;
}

std::vector<Schedule> ScheduleBook::list() const {
  std::lock_guard<std::mutex> l(mutex_);
  return pending_;
}

std::size_t ScheduleBook::size() const {
  std::lock_guard<std::mutex> l(mutex_);
  return pending_.size();
}

std::vector<Schedule> ScheduleBook::tick(std::chrono::steady_clock::time_point now) {
  std::vector<Schedule> fired;
  std::lock_guard<std::mutex> l(mutex_);
  // The owner claim. The first tick names the delivery thread; a later tick
  // from anywhere else delivers nothing and says so. A consumer added in
  // M2b.2-M2b.5 that ticks from a script thread or a worker poll therefore
  // finds out in the log, on the first frame, instead of as the rare crash
  // this design exists to avoid.
  const auto self = std::this_thread::get_id();
  if (!owner_claimed_) {
    owner_ = self;
    owner_claimed_ = true;
  } else if (owner_ != self) {
    note_locked("schedule tick() called off the frame loop; ignored");
    return fired;
  }
  // Sorted, so the first not-yet-due entry ends it.
  std::size_t n = 0;
  while (n < pending_.size() && pending_[n].due <= now) ++n;
  if (n == 0) return fired;
  fired.assign(std::make_move_iterator(pending_.begin()),
               std::make_move_iterator(pending_.begin() + static_cast<std::ptrdiff_t>(n)));
  // Erased before returning: fired exactly once, whatever the consumer does
  // with it.
  pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
  return fired;
}

std::vector<Schedule> ScheduleBook::tick() { return tick(std::chrono::steady_clock::now()); }

std::vector<Schedule> ScheduleBook::take_pending() {
  std::lock_guard<std::mutex> l(mutex_);
  std::vector<Schedule> out;
  out.swap(pending_);
  return out;
}

std::vector<std::string> ScheduleBook::take_status() {
  std::lock_guard<std::mutex> l(mutex_);
  std::vector<std::string> out;
  out.swap(status_);
  return out;
}

std::thread::id ScheduleBook::owner_thread() const {
  std::lock_guard<std::mutex> l(mutex_);
  return owner_;
}

void ScheduleBook::reset() {
  std::lock_guard<std::mutex> l(mutex_);
  pending_.clear();
  status_.clear();
  next_id_ = 1;
  owner_ = std::thread::id{};
  owner_claimed_ = false;
}

}  // namespace aii
