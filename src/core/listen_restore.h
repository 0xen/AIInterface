#pragma once
// M12.1. "When the sub-agent comes back, it starts listening again — **but
// only if it was listening in the first place**."
//
// The last clause is the whole of this file. What is being built is not "open
// the microphone when a worker finishes"; it is "remember the state the app
// was in before it went quiet, and restore *that*". If the user had shut the
// microphone on purpose, a worker coming back must leave it shut.
//
// Header-only and pure, for `core/handoff_policy.h`'s reason: the interesting
// cases are the ones nobody can be bothered to reproduce by hand — three
// workers, a user who changed their mind halfway, a latch that timed out —
// and a rule that can be exercised in a unit test is a rule that gets its
// negative case checked. `VoiceSession` holds one of these and does nothing
// else about any of it.
//
// ## The four questions, answered
//
// **When is the state taken?** At the start of the wait: the frame on which
// the number of running workers goes from none to some. Not at each spawn —
// a second worker started while the first is still out is the *same* wait, and
// re-reading the latch then would record a microphone that has already gone
// quiet because of the first one. `wait_began()` is a no-op while a wait is
// already in progress, which is what makes that true.
//
// **Several workers: first back, last back, or each?** *Each.* Every report
// restores, and restoring is idempotent — the second one finds the microphone
// already open and changes nothing. First-back-only would leave a user who
// deliberately shut the mic after report one, and then changed nothing,
// looking at exactly the same screen as a user whose second report was
// swallowed; last-back-only would make the app sit silent through a five
// minute worker because a thirty second one was also out. The memory is
// cleared when the *last* worker is back (`wait_ended()`), so the next wait
// takes a fresh reading.
//
// **The user changed the state themselves while waiting: who wins?** *The
// user, always, and permanently for that wait.* `user_shut_the_mic()` does not
// flip the remembered value, it **forgets** it — so no later worker in the
// same wait can restore anything either. That is the version you can see from
// the outside: once you have touched the microphone yourself, nothing the app
// is waiting on will touch it again until the next wait begins. A rule that
// merely overwrote the memory with the new state would leave "I shut it, then
// a worker reopened it, so I shut it again, and another worker reopened it"
// reachable, which is the app arguing with the person using it.
//
// **What does `timing.listen_timeout` do to the memory?** *Nothing at all*,
// and this is the point of the whole feature rather than an exception to it.
// The latch closing itself on a silent room is the "when the AI stops
// listening to me" the request opens with — it is the app going quiet, not the
// user going quiet, so it must not count as the user taking the microphone.
// `VoiceSession::close_latch_after_silence()` therefore does not call
// `user_shut_the_mic()`, and `Stop`, the Talk gesture and the microphone
// button all do.
#include <cstddef>

namespace aii {

class ListenRestore {
 public:
  // The frame on which workers went from none running to some. `listening` is
  // the latch as it stands right now. A second call while a wait is already
  // open is ignored — see the header comment.
  void wait_began(bool listening) {
    if (waiting_) return;
    waiting_ = true;
    listen_before_wait_ = listening;
  }

  // A worker reported. True means: put the latch back on. False covers three
  // different situations that all deserve the same silence — no wait is open,
  // the microphone was already shut when the wait began, or the user has since
  // taken the microphone for themselves.
  bool worker_reported() const { return waiting_ && listen_before_wait_; }

  // The last worker is back. The memory goes with it, so the next wait reads
  // the latch again rather than inheriting a minutes-old answer.
  void wait_ended() {
    waiting_ = false;
    listen_before_wait_ = false;
  }

  // The user shut the microphone themselves — the button, SPACE, the Talk
  // gesture, or Stop. The memory is dropped for the rest of this wait.
  // **Not** the listen timeout: that is the app, not the user.
  void user_shut_the_mic() { listen_before_wait_ = false; }

  // The user opened it themselves, mid-wait — the same button, or the wake
  // phrase. This *re-arms* rather than forgetting, which is the one asymmetry
  // in this class and is worth the sentence: "listening" is now the state they
  // chose, so if the latch times out five minutes later with a worker still
  // out, that timeout is exactly the event the restore exists to undo. Dropping
  // the memory here would punish a user for having answered the app.
  void user_opened_the_mic() {
    if (waiting_) listen_before_wait_ = true;
  }

  // For the log line and the test only.
  bool waiting() const { return waiting_; }
  bool armed() const { return waiting_ && listen_before_wait_; }

 private:
  bool waiting_ = false;
  bool listen_before_wait_ = false;
};

}  // namespace aii
