// The `aii` module reference, checked against the module.
//
// `assets/scripts/AII-MODULE.md` is what the assistant reads before writing a
// script, and it is hand-written prose about a C++ file. That is a drift risk
// with a specific, quiet failure: a verb renamed in `aii_pyhost.cpp` leaves the
// sheet naming a function that no longer exists, the model writes a script
// calling it, and the failure surfaces as one line in a settings panel long
// after the change that caused it.
//
// So this asserts both directions:
//
//   * every call the sheet names is really defined by the module, and
//   * every verb the module defines is named somewhere in the sheet.
//
// The second is the one that earns its keep. The first fails loudly the moment
// a script runs; the second fails as a capability nobody is told about, which
// is exactly the gap this reference was written to close.
//
//   module_reference_test   prints every case and exits non-zero on failure
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void ok(const char* what, bool cond) {
  if (!cond) ++g_failures;
  std::printf("  %-58s %s\n", what, cond ? "ok  " : "FAIL");
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The names the module actually defines: `m.def("<name>",` and the handful
// written in the Python shim as `def <name>(`.
std::set<std::string> module_verbs(const std::string& src) {
  std::set<std::string> out;
  const auto collect = [&](const std::string& opener, bool quoted) {
    size_t at = 0;
    while ((at = src.find(opener, at)) != std::string::npos) {
      size_t p = at + opener.size();
      while (p < src.size() && (src[p] == ' ' || src[p] == '\n' || src[p] == '\r' || src[p] == '\t'))
        ++p;
      if (quoted) {
        if (p >= src.size() || src[p] != '"') { at = p; continue; }
        ++p;
      }
      const size_t start = p;
      while (p < src.size() && (islower(static_cast<unsigned char>(src[p])) || src[p] == '_' ||
                                isdigit(static_cast<unsigned char>(src[p]))))
        ++p;
      if (p > start) out.insert(src.substr(start, p - start));
      at = p;
    }
  };
  collect("m.def(", true);
  collect("\ndef ", false);
  return out;
}

}  // namespace

int main() {
  const std::string sheet = read_file(std::string(AII_ASSETS_DIR) + "/scripts/AII-MODULE.md");
  const std::string src = std::string(AII_PYHOST_SOURCE);
  const std::string module = read_file(src);

  std::printf("\n-- the two files are where they are supposed to be --\n");
  ok("the reference sheet was found", !sheet.empty());
  ok("the module source was found", !module.empty());
  if (sheet.empty() || module.empty()) {
    std::printf("\nFAIL (could not read one of the files)\n");
    return 1;
  }

  const std::set<std::string> verbs = module_verbs(module);
  ok("the module defines a plausible number of verbs", verbs.size() >= 25);

  // `post`, `poll_lines` and `wait_lines` are the transport underneath `poll`,
  // `wait` and `send`, and the sheet documents the wrappers on purpose -- a
  // script that uses the raw ones is working below the interface. `run` is the
  // name an *action* defines, not one the module provides. Anything beginning
  // with `_` is a private helper of the generated bootstrap and is skipped by
  // the leading-underscore rule below rather than by being listed here.
  const std::set<std::string> not_in_sheet = {"post", "poll_lines", "wait_lines", "run"};

  std::printf("\n-- every verb the module has is in the sheet --\n");
  int missing = 0;
  for (const std::string& v : verbs) {
    if (!v.empty() && v[0] == '_') continue;  // private
    if (not_in_sheet.count(v)) continue;
    if (sheet.find("aii." + v) == std::string::npos) {
      std::printf("      not documented: aii.%s\n", v.c_str());
      ++missing;
    }
  }
  ok("no verb is undocumented", missing == 0);

  std::printf("\n-- every call the sheet names really exists --\n");
  int invented = 0;
  size_t at = 0;
  while ((at = sheet.find("aii.", at)) != std::string::npos) {
    size_t p = at + 4;
    const size_t start = p;
    while (p < sheet.size() && (islower(static_cast<unsigned char>(sheet[p])) || sheet[p] == '_' ||
                                isdigit(static_cast<unsigned char>(sheet[p]))))
      ++p;
    const std::string name = sheet.substr(start, p - start);
    at = p;
    // `aii.scripts` and `aii.lib_dir` are attributes; `aii.ui` is the window
    // submodule (M28), documented in its own sheet, AII-UI.md.
    if (name.empty() || name == "scripts" || name == "lib_dir" || name == "ui") continue;
    if (!verbs.count(name)) {
      std::printf("      the sheet names a call the module does not define: aii.%s\n",
                  name.c_str());
      ++invented;
    }
  }
  ok("the sheet invents nothing", invented == 0);

  // The one the user actually asked about: the assistant could not find how to
  // mute itself. If this name ever moves, the sheet must move with it.
  std::printf("\n-- the reason this file exists --\n");
  ok("muting is documented", sheet.find("aii.mute") != std::string::npos);
  ok("and the module really has it", verbs.count("mute") == 1);

  std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
