#include "script_host.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "action_store.h"
#include "core/app_bus.h"
#include "core/user_paths.h"
#include "pyhost/aii_pyhost.h"
#include "rend/core/paths.h"
#include "rend/pyhost/pyhost.h"
#include "rend/renderer/message_queue.h"

namespace fs = std::filesystem;

namespace aii {
namespace {

// Where the user's scripts live, beside the prompts and the avatars that are
// already there. One directory, flat: a `.py` in it runs, and everything
// below it — `examples\`, and the generated bootstrap in `.runtime\` — does
// not. That subdirectory rule is the whole of the opt-in, so it is stated in
// one place and read in one place.
fs::path scripts_root() { return user_data_root() / "scripts"; }

bool is_python_file(const fs::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".py";
}

}  // namespace

struct ScriptHost::Impl {
  // Nothing here consumes it. The engine's host insists on one because its
  // `rend` module is built around it; this app has no scene, so the queue is
  // a bin that `tick()` empties.
  rend::renderer::MessageQueue queue;
  HMODULE aii_dll = nullptr;
  HMODULE rend_dll = nullptr;
  AiiPyQuitFn quit = nullptr;
  rend::pyhost::StopFn stop = nullptr;
  rend::pyhost::RunningFn running = nullptr;
  bool started = false;
};

ScriptHost::ScriptHost() = default;
ScriptHost::~ScriptHost() { stop(); }

std::vector<std::string> ScriptHost::discover(const std::vector<std::string>& extra) {
  // The seed runs on every start, scripting or not, and refreshes a file only
  // when the shipped bytes changed and the installed ones are still the bytes
  // this app put there. That is the installer rule this project already got
  // wrong twice: an "only if the directory is missing" seed is a one-way door
  // and a better example authored later would never reach a machine that had
  // run the app before (636f24e), while the mtime rule that replaced it would
  // overwrite an example the user had edited (M16). The example lands in
  // `scripts\examples\`, which is *not* scanned, so seeding it does not turn
  // Python on for anybody.
  const fs::path root = scripts_root();
  std::error_code ec;
  fs::create_directories(root, ec);
  std::string seed_err;
  std::vector<std::string> seed_notes;
  if (!seed_tree(fs::path(AII_ASSETS_DIR) / "scripts", root, &seed_err, &seed_notes))
    std::fprintf(stderr, "[scripts] %s\n", seed_err.c_str());
  for (const std::string& note : seed_notes) std::fprintf(stderr, "[scripts] %s\n", note.c_str());

  std::vector<std::string> out;
  // `--script` is not gated. The gate exists because a file can appear in a
  // directory without the user having asked for it; a path they typed on their
  // own command line is the ask, and a consent window in front of it would be
  // a dialog confirming what the user just said.
  for (const std::string& p : extra) {
    if (fs::exists(p, ec) && !ec) out.push_back(fs::absolute(p, ec).string());
  }
  std::vector<std::string> mine;
  for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
    if (ec) break;
    if (!e.is_regular_file(ec) || ec) continue;
    if (!is_python_file(e.path())) continue;
    const std::string name = e.path().filename().string();
    if (!name.empty() && (name[0] == '_' || name[0] == '.')) continue;
    // M19.2, finding 12. **A policy runs only if the user has allowed these
    // bytes.** Before this, dropping a `.py` here was the whole of the
    // decision: the file ran on a full CPython at the next launch, for the
    // life of the app, with nothing asked and nothing said — a weaker gate
    // than the one an *action* has to pass, though a policy can do strictly
    // more. The record is `ActionStore`'s, because the queue and the window
    // that ask the question are already there and a second consent store
    // would be a second thing to get wrong.
    //
    // The announcement is the store's job, not this one's: `ActionStore` scans
    // this same directory every second, so a file held here appears in the
    // approval queue whether it arrived before this launch or during it.
    if (!ActionStore::policy_allowed(e.path())) {
      std::fprintf(stderr, "[scripts] holding '%s': not allowed yet\n", name.c_str());
      continue;
    }
    mine.push_back(e.path().string());
  }
  // Sorted, so "which script ran first" is a property of the name rather than
  // of the order the filesystem happened to hand them back.
  std::sort(mine.begin(), mine.end());
  out.insert(out.end(), mine.begin(), mine.end());
  return out;
}

bool ScriptHost::start(AppBus& bus, const std::vector<std::string>& scripts, UiBridge* ui) {
  // M10.2. An empty list is no longer a refusal: the host also carries the
  // action dispatcher, so an install with no policy scripts and one action
  // still wants an interpreter. "Should there be a host at all" is the
  // caller's question now, and main.cpp answers it with "a policy, or an
  // action, or --script".
  scripts_ = scripts;
  impl_ = std::make_unique<Impl>();

  const fs::path dir = rend::executableDirectory();

  // Our DLL is loaded first, and before the interpreter exists: `aii`
  // registers itself into CPython's inittab as this DLL loads, which only
  // works while Py_Initialize has not run. The engine's host is what runs it,
  // on its own thread, in the call below.
  //
  // M19.3, finding 32. **Loading the DLL is not registering the bus, and the
  // second of those is done last.** `aiiPyHostRegister` refuses a second
  // registration, so when it ran here — ahead of the bootstrap write and the
  // engine DLL — any later failure left the module pointing at a bus while
  // `start()` returned false, and the *next* attempt reported "the aii module
  // refused to register": a true sentence about the retry and a useless one
  // about the fault, which was a missing DLL or an unwritable file. It now
  // runs after everything that can fail without it, and the one failure that
  // can still happen afterwards unregisters on its way out.
  impl_->aii_dll = LoadLibraryW((dir / L"aii_pyhost.dll").wstring().c_str());
  const auto reg = impl_->aii_dll ? reinterpret_cast<AiiPyRegisterFn>(
                                        GetProcAddress(impl_->aii_dll, "aiiPyHostRegister"))
                                  : nullptr;
  const auto boot_name = impl_->aii_dll ? reinterpret_cast<AiiPyTextFn>(
                                              GetProcAddress(impl_->aii_dll, "aiiPyHostBootName"))
                                        : nullptr;
  const auto boot_src = impl_->aii_dll ? reinterpret_cast<AiiPyTextFn>(
                                             GetProcAddress(impl_->aii_dll, "aiiPyHostBootSource"))
                                       : nullptr;
  impl_->quit = impl_->aii_dll
                    ? reinterpret_cast<AiiPyQuitFn>(GetProcAddress(impl_->aii_dll, "aiiPyHostQuit"))
                    : nullptr;
  if (!reg || !boot_name || !boot_src || !impl_->quit) {
    status_ = "aii_pyhost.dll is missing or out of date; scripts skipped.";
    status_ok_ = false;
    return false;
  }

  // The host is handed one file: the generated bootstrap, which runs the
  // user's scripts inside a `try` on threads of its own. It is rewritten every
  // run from the string baked into the DLL, so it can never go stale, and it
  // lives under `.runtime\` where the scan does not look.
  std::error_code ec;
  const fs::path runtime = scripts_root() / ".runtime";
  fs::create_directories(runtime, ec);
  const fs::path boot = runtime / boot_name();
  {
    std::ofstream f(boot, std::ios::binary | std::ios::trunc);
    if (!f) {
      status_ = "cannot write " + boot.string() + "; scripts skipped.";
      status_ok_ = false;
      return false;
    }
    f << boot_src();
  }

  impl_->rend_dll = LoadLibraryW((dir / L"rend_pyhost.dll").wstring().c_str());
  const auto start_fn = impl_->rend_dll ? reinterpret_cast<rend::pyhost::StartFn>(
                                              GetProcAddress(impl_->rend_dll, "rendPyHostStart"))
                                        : nullptr;
  impl_->stop = impl_->rend_dll ? reinterpret_cast<rend::pyhost::StopFn>(
                                      GetProcAddress(impl_->rend_dll, "rendPyHostStop"))
                                : nullptr;
  impl_->running = impl_->rend_dll ? reinterpret_cast<rend::pyhost::RunningFn>(
                                         GetProcAddress(impl_->rend_dll, "rendPyHostRunning"))
                                   : nullptr;
  if (!start_fn || !impl_->stop) {
    status_ = "rend_pyhost.dll is unavailable; scripts skipped.";
    status_ok_ = false;
    return false;
  }

  // M28. Both optional: a missing symbol here means a stale aii_pyhost.dll
  // beside a newer exe, and that must still start scripts that never touch
  // `aii.ui` rather than refuse the whole host over a window feature they
  // do not use.
  if (const auto set_ui = reinterpret_cast<AiiPySetUiBridgeFn>(
          GetProcAddress(impl_->aii_dll, "aiiPyHostSetUiBridge"))) {
    set_ui(ui);
  } else {
    std::fprintf(stderr, "[scripts] aii_pyhost.dll has no aiiPyHostSetUiBridge; aii.ui "
                          "will report no window host.\n");
  }
  if (const auto set_lib_dir = reinterpret_cast<AiiPySetLibDirFn>(
          GetProcAddress(impl_->aii_dll, "aiiPyHostSetLibDir"))) {
    set_lib_dir((scripts_root() / "lib").string().c_str());
  } else {
    std::fprintf(stderr, "[scripts] aii_pyhost.dll has no aiiPyHostSetLibDir; scripts\\lib "
                          "will not be on sys.path.\n");
  }

  // Everything that can fail has been done. Now the bus goes in, immediately
  // before the interpreter that will read it.
  std::vector<const char*> ptrs;
  for (const std::string& s : scripts_) ptrs.push_back(s.c_str());
  // `ptrs.data()` on an empty vector may be null, which the registrar now
  // accepts for a count of zero.
  if (!reg(&bus, ptrs.empty() ? nullptr : ptrs.data(), static_cast<int>(ptrs.size()))) {
    status_ = "the aii module refused to register; scripts skipped.";
    status_ok_ = false;
    return false;
  }

  const std::string bootPath = boot.string();
  const char* one = bootPath.c_str();
  if (!start_fn(&impl_->queue, &one, 1)) {
    status_ = "the Python host refused to start; scripts skipped.";
    status_ok_ = false;
    impl_->stop = nullptr;
    // The registration is given back, so a second `start()` reports whatever
    // stopped the host rather than the leftovers of the first attempt.
    if (const auto unreg = reinterpret_cast<AiiPyUnregisterFn>(
            GetProcAddress(impl_->aii_dll, "aiiPyHostUnregister")))
      unreg();
    return false;
  }
  impl_->started = true;
  status_ = scripts_.empty()
                ? std::string("Ready for actions.")
                : std::to_string(scripts_.size()) +
                      (scripts_.size() == 1 ? " script" : " scripts") + " running.";
  status_ok_ = true;
  return true;
}

void ScriptHost::tick() {
  if (!impl_ || !impl_->started) return;
  impl_->queue.drain();
}

void ScriptHost::stop() {
  if (!impl_ || !impl_->started) return;
  impl_->started = false;
  // The flag first, so a loop polling `aii.should_quit()` leaves on its own
  // terms; the host's own Stop then raises KeyboardInterrupt for anything
  // asleep and joins the thread.
  if (impl_->quit) impl_->quit();
  // Then a short, bounded wait for the interpreter to come down on its own.
  // The host's Stop raises KeyboardInterrupt to break a sleeping script, and
  // on Windows that arrives often enough as "OSError: Signal 2 ignored due to
  // race condition" — a traceback on stderr at every exit, for a shutdown that
  // was perfectly orderly. Asking first and interrupting second turns the
  // usual case quiet without giving up the hard stop for a script that will
  // not leave.
  if (impl_->running) {
    for (int i = 0; i < 40 && impl_->running(); ++i) {
      ::Sleep(10);
    }
  }
  if (impl_->stop) impl_->stop();
  // Deliberately no FreeLibrary, on either DLL: CPython does not survive
  // being unloaded, and this is the engine's rule for the same reason.
}

bool ScriptHost::running() const {
  return impl_ && impl_->started && impl_->running && impl_->running();
}

}  // namespace aii
