#pragma once
// M2.6: Python, in process, on the engine's own host.
//
// The user's decision, and its whole shape: *"just like the viewer
// application python is ran in our code. Not as a separate process."* So this
// reuses `rend_pyhost.dll` — the engine's embedded CPython 3.13.15 — rather
// than embedding a second interpreter, and adds one DLL of its own,
// `aii_pyhost.dll`, carrying the `aii` module over M2.5's bus.
//
// ## When Python is loaded, and what it costs when it is not
//
// The viewer's answer is the precedent: it `LoadLibrary`s its host **only
// when a scene declares `<Script>` nodes**, so a scene without scripts
// involves no Python at all. The equivalent trigger here is *a script to
// run*, and there is exactly one way to have one:
//
//   * a `.py` file sitting directly in `%APPDATA%\AIInterface\scripts\`, or
//   * `--script <path>`, repeatable, for harnesses — the viewer's own flag.
//
// **A fresh install has neither**, because the shipped example is seeded one
// level down, into `scripts\examples\`, and the scan is deliberately not
// recursive. So scripting is opt-in by the act of copying a file up one
// directory, and a user who never does that pays a `directory_iterator` over
// an empty folder — no `LoadLibrary`, no `python313.dll`, no interpreter, and
// nothing added to the loading screen's stages.
//
// That is also why `avatar.exe` does not link pybind11 or Python itself. The
// Windows delay-load helper covers function imports only, and CPython is
// imported partly as data (`Py_None`, every exception object), so an exe that
// links `python313.lib` maps `python313.dll` at process start whether or not
// it ever starts an interpreter. Both halves live behind `LoadLibrary`
// instead; see `src/pyhost/aii_pyhost.h`.
//
// ## What a misbehaving script can and cannot do
//
// **It cannot stall the frame loop.** Scripts run on the host's own thread
// and the frame loop never touches the interpreter (the engine measured this:
// 60 fps held through a 5 s sleep and a 3 s GIL-holding busy spin). The app's
// side of the bus is a short mutex around bounded deques — a script that
// stops draining coalesces to a fixed handful of queued events, and a script
// that floods `post()` is dropped at `kBusInboxMax` with a counted refusal.
//
// **A script that fails does not take the app down; a script that means to,
// can.** Those are two different claims and only the first one is enforced.
// Each user script runs inside the generated bootstrap's `try`, on its own
// thread; a syntax error, a missing import or a throw on the first event is
// caught, its last line goes to the Scripts section of the settings surface
// through `script.status`, and the other scripts carry on. If the whole host
// refuses to start, that is a line there too, and the app runs exactly as it
// does today — the built-in policy is what runs when nothing is scripting, and
// the app is fully expressive with scripting switched off.
//
// What is *not* true, and was written here until M19.2, is that a script
// cannot end the process. This is a full CPython in this process's address
// space: `os._exit()` ends it immediately, `ctypes` reaches any address the
// app can, and no interpreter flag in reach would change that. **The boundary
// is the bus vocabulary, not the interpreter** — what `aii` exposes is a fixed
// set of verbs with no `run a command` and no `register a command button`, and
// that is what bounds what a script is *offered*. It bounds nothing about what
// a script that goes outside the module can reach.
//
// Which is why M19.2 put a consent gate in front of the directory
// (`action_store.h`, finding 12): the honest containment for "this file can do
// anything this app can do" is the user having said yes to that file, not a
// promise about what Python will refuse.
//
// **It cannot hold the avatar.** Every script request is a lease with a
// ceiling (`AvatarController::kScriptLeaseMax`), and the user taking the
// microphone outranks it.
//
// The one thing it *can* still do is delay shutdown by a moment: stopping the
// host joins the script thread. The bootstrap runs user scripts as daemon
// threads and waits on `should_quit()`, so the join returns as soon as the
// bootstrap's own loop notices — but a script that catches `KeyboardInterrupt`
// inside a tight loop will cost the exit a beat.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aii {

class AppBus;

class ScriptHost {
 public:
  // Both out of line: `Impl` is incomplete here on purpose (it holds the
  // engine's MessageQueue and two HMODULEs, neither of which main.cpp has any
  // business seeing), and an inline constructor would need its destructor.
  ScriptHost();
  ~ScriptHost();

  // Seeds `assets/scripts` into `%APPDATA%\AIInterface\scripts` (the shared
  // `recursive | update_existing` rule, so a better example reaches a machine
  // that has already run the app — 636f24e) and returns the scripts that will
  // actually run: `extra` first, exactly as the viewer runs `--script` ahead
  // of a scene's own, then the user's `scripts\*.py` in sorted order.
  static std::vector<std::string> discover(const std::vector<std::string>& extra);

  // Loads the two DLLs and starts the interpreter on its own thread. Only
  // called when `discover()` returned something. False with `status()` set
  // when a DLL is missing or the host refused; the app then runs unscripted.
  bool start(AppBus& bus, const std::vector<std::string>& scripts);

  // Frame loop. The engine's host is wired to a `rend::renderer::MessageQueue`
  // that nothing here consumes — this app has no scene — so it is drained and
  // dropped once a frame. Without that, a script that called into the `rend`
  // module would grow that queue for the rest of the run.
  void tick();

  // Asks scripts to stop, then joins the host thread. Idempotent; the
  // destructor calls it. The DLLs are never freed (CPython dislikes unload),
  // which is the engine's rule too.
  void stop();

  bool running() const;
  std::size_t script_count() const { return scripts_.size(); }

  // One line for the settings surface: why the host did not start, or how
  // many scripts are running. `script.status` messages from the scripts
  // themselves land in `BusBindings`, not here.
  const std::string& status() const { return status_; }
  bool status_ok() const { return status_ok_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<std::string> scripts_;
  std::string status_;
  bool status_ok_ = true;
};

}  // namespace aii
