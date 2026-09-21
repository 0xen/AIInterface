#pragma once
// C ABI of `aii_pyhost.dll` — the `aii` Python module (M2.6).
//
// **Why this is a DLL of its own and not a file inside avatar.exe.** The
// module is pybind11, and pybind11 is CPython, and CPython is imported partly
// as *data* (`Py_None`, `PyExc_ImportError`, every type object). The Windows
// delay-load helper only covers function imports, so an executable that links
// `python313.lib` at all maps `python313.dll` at process start — for every
// user, scripting or not. A DLL sidesteps that entirely: `avatar.exe` links
// neither Python nor pybind11, and this file is `LoadLibrary`'d only on the
// runs that have a script to run. That is the same shape, and the same
// reasoning, as the engine's own `rend_pyhost.dll`, which the viewer loads
// only when a scene declares `<Script>` nodes.
//
// Keep this header free of Python, pybind11 and `AppBus`'s definition — it is
// included path-only, with no link, exactly as `rend/pyhost/pyhost.h` is.
//
// **The bus is passed in, never looked up.** `aii_core` is a static library,
// so a copy linked into this DLL would carry its *own* `AppBus::instance()`
// and the two halves of the app would talk past each other in total silence.
// `aiiPyHostRegister` therefore takes the one the frame loop uses, and every
// binding in here goes through that pointer.

namespace aii {
class AppBus;
}

extern "C" {

// Points the `aii` module at this process's bus and tells it which scripts
// are about to run (they arrive in Python as `aii.scripts`).
//
// **Call before the interpreter is initialised.** The module registers itself
// into CPython's inittab at DLL load; this call only fills in what it talks
// to. `false` means the arguments were bad or a registration is already live.
bool aiiPyHostRegister(aii::AppBus* bus, const char* const* scriptPaths, int scriptCount);

// Gives the registration back (M19.3). Only for the case where the caller
// registered and then could not start the interpreter: with no interpreter
// there is no script thread to be mid-call, and the module is left exactly as
// it was before `aiiPyHostRegister`, so a retry can report its own failure
// instead of "a registration is already live". **Never call this while a host
// is running** — the scripts' bus would go out from under them.
void aiiPyHostUnregister(void);

// Flips `aii.should_quit()` and wakes anything blocked in `aii.wait()`. Called
// before the interpreter is stopped, so a well-behaved script leaves its loop
// on its own rather than on a KeyboardInterrupt.
void aiiPyHostQuit(void);

// Path of the generated bootstrap, written by the caller, that the host
// actually runs. Here only so the two halves cannot disagree about the name.
const char* aiiPyHostBootName(void);

// The bootstrap's source. It runs each user script on its own thread and
// reports a raising one through `aii.status()`, which is what makes a script
// with a syntax error a line in the settings surface instead of silence.
const char* aiiPyHostBootSource(void);
}

namespace aii {
// Function-pointer types for GetProcAddress consumers.
using AiiPyRegisterFn = bool (*)(AppBus*, const char* const*, int);
using AiiPyQuitFn = void (*)(void);
using AiiPyUnregisterFn = void (*)(void);
using AiiPyTextFn = const char* (*)(void);
}  // namespace aii
