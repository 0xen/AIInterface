// When the session hands over to itself, checked as arithmetic rather than by
// sitting through 80,000 tokens of conversation.
//
// M3.15's firing rule is three numbers: the CLI's reported context fraction,
// the threshold, and what this session already cost before the user said
// anything. Everything expensive about the feature -- the spoken line, the
// summary turn, the restart -- hangs off a `Yes` from `handoff_due()`, and
// every one of the named failure modes is a wrong answer from this function:
//
//   * a fresh child's `ctx` of -1.0 read as a number rather than as "unknown";
//   * a threshold under what a fresh session costs, which hands over forever;
//   * `40` in the settings file meaning 40 times the window.
//
// The oscillation failure is not testable here on purpose: this function is
// memoryless and says `Yes` for as long as the numbers do. What stops it
// firing twice is the caller's state machine, and the evidence for that is a
// live run.
#include <cstdio>
#include <string>

#include "core/handoff_policy.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

}  // namespace

int main() {
  using namespace aii;
  const auto due = [](double ctx, double at, double floor) { return handoff_due(ctx, at, floor); };

  std::printf("the threshold, as the file may spell it\n");
  check(normalise_handoff_threshold(0.40) == 0.40, "a fraction is a fraction");
  check(normalise_handoff_threshold(40.0) == 0.40,
        "**40 is read as 40 per cent**, which is what a person writes after saying it out loud");
  check(normalise_handoff_threshold(0.0) == 0.0, "0 is off, the same spelling listen_timeout uses");
  check(normalise_handoff_threshold(-1.0) == 0.0, "so is a negative");
  check(normalise_handoff_threshold(99.0) == 0.95, "a threshold above 95% is clamped, not obeyed");

  std::printf("switched off\n");
  check(due(0.99, 0.0, 0.03) == HandoffVerdict::No, "a threshold of 0 never fires, at any context");

  std::printf("unknown is not a number\n");
  check(due(-1.0, 0.40, -1.0) == HandoffVerdict::No,
        "**a fresh child reporting -1.0 does not fire**");
  check(due(-1.0, 0.40, 0.03) == HandoffVerdict::No,
        "and still does not once a floor is known");

  std::printf("the line itself\n");
  check(due(0.39, 0.40, 0.03) == HandoffVerdict::No, "just under is under");
  check(due(0.40, 0.40, 0.03) == HandoffVerdict::Yes, "exactly on the line fires");
  check(due(0.87, 0.40, 0.03) == HandoffVerdict::Yes, "well over fires");
  check(due(0.40, 40.0, 0.03) == HandoffVerdict::Yes,
        "and the percentage spelling means the same thing here");

  std::printf("a threshold no fresh session could sit under\n");
  check(due(0.06, 0.02, 0.05) == HandoffVerdict::Unattainable,
        "**2% against a session that costs 5% to exist is a loop, not a handover**");
  check(due(0.06, 0.05, 0.05) == HandoffVerdict::Unattainable,
        "equal counts: a new session would be on the line the moment it came up");
  check(due(0.90, 0.02, 0.05) == HandoffVerdict::Unattainable,
        "and it is reported as broken even when the context is genuinely full -- "
        "firing once first would spend a summary turn proving what the numbers said");
  check(due(0.06, 0.02, -1.0) == HandoffVerdict::Yes,
        "with no floor reported yet there is nothing to compare against, so it fires");

  std::printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
