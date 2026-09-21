// A `claude` that refuses to start, for `client_kill_test`.
//
// M26.1, finding 17. The case being reproduced is the CLI failing before it
// ever speaks stream-json: not signed in, an unknown `--model`, a node that
// will not load. All of those print a line on stderr and exit non-zero, and
// nothing about them reaches stdout, which is the only stream the client used
// to read.
//
// `cmd.exe` is the stand-in everywhere else in that test, but it cannot be
// this one: the client composes its own command line as the exe followed by
// `-p --input-format stream-json ...`, so there is no way to hand cmd a `/c`
// and therefore no way to make it print a chosen line and exit with a chosen
// code. Seven lines of C++ can. It writes what a signed-out CLI writes and
// exits 3, and the test asserts on both halves of the sentence the client
// builds out of that.
#include <cstdio>

int main() {
  // A blank line and a banner first, on purpose: `last_words()` is supposed to
  // return the reason and not the decoration around it.
  std::fprintf(stderr, "\nClaude Code v2.1.275\n");
  std::fprintf(stderr, "not signed in: run `claude /login`\n");
  std::fflush(stderr);
  return 3;
}
