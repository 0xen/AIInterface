// M13.1: the inline directive channel, checked without an engine or a window.
//
// Two properties are being defended here and they fail in different ways.
//
// **The grammar** decides what is machine traffic and what is English. Get it
// too loose and `[see figure 1]` disappears from a reply; too strict and a real
// `[v2]` is read aloud as "v two". Every case in the table below is from
// `docs/design-directives.md` §5.1, and the negative cases are the point.
//
// **Streaming equivalence** is the property a probe found the obvious design
// violating: a reply must produce byte-identical output whether the CLI sends
// it in one delta or one byte at a time. That is not a theoretical concern --
// `claude_code_client.cpp` really does deliver a whole reply in one call when
// no partial deltas arrived, and really does stream it otherwise, so both paths
// are live in production.
//
//   directive_filter_test   prints every case and exits non-zero on failure
#include <cstdio>
#include <string>
#include <vector>

#include "core/directive_filter.h"

using aii::DirectiveFilter;

namespace {

int g_failures = 0;

void ok(const char* what, bool cond) {
  if (!cond) ++g_failures;
  std::printf("  %-58s %s\n", what, cond ? "ok  " : "FAIL");
}

struct Run {
  std::string text;                 // everything the consumers would see
  std::vector<std::string> marks;   // directives, in order, as "token" or "token:value"
};

// `chunk` of 0 means one delta; anything else feeds that many bytes at a time.
Run drive(const std::string& reply, size_t chunk) {
  Run r;
  DirectiveFilter f([&](const std::string& t) { r.text += t; },
                    [&](const std::string& tok, const std::string& val) {
                      r.marks.push_back(val.empty() ? tok : tok + ":" + val);
                    });
  if (chunk == 0) {
    f.feed(reply);
  } else {
    for (size_t i = 0; i < reply.size(); i += chunk) f.feed(reply.substr(i, chunk));
  }
  f.flush();
  return r;
}

std::string joined(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) {
    if (!s.empty()) s += ",";
    s += x;
  }
  return s;
}

// One row of the §5.1 table. Checked in one delta *and* byte-wise, because a
// row that passes one way and not the other is exactly the defect this file
// exists to catch.
void row(const char* what, const std::string& in, const std::string& want_text,
         const std::string& want_marks) {
  const Run one = drive(in, 0);
  const Run bytes = drive(in, 1);
  const bool text_ok = one.text == want_text;
  const bool marks_ok = joined(one.marks) == want_marks;
  const bool same = one.text == bytes.text && joined(one.marks) == joined(bytes.marks);
  ok(what, text_ok && marks_ok && same);
  if (!text_ok) std::printf("      text  want [%s] got [%s]\n", want_text.c_str(), one.text.c_str());
  if (!marks_ok)
    std::printf("      marks want [%s] got [%s]\n", want_marks.c_str(), joined(one.marks).c_str());
  if (!same)
    std::printf("      byte-wise differs: [%s] marks [%s]\n", bytes.text.c_str(),
                joined(bytes.marks).c_str());
}

void grammar() {
  std::printf("\n-- what is a directive, and what is English --\n");
  row("the base case", "[v2] hello", " hello", "v2");
  row("two in a row: both consumed, in order", "[v2] [v3] text", "  text", "v2,v3");
  row("uppercase is prose", "[V2] hello", "[V2] hello", "");
  row("a space ends the token", "[v2 ] hello", "[v2 ] hello", "");
  row("an empty token is prose", "[]", "[]", "");
  row("a known token in an unknown shape still parses", "[v2:extra]", "", "v2:extra");
  row("a colon with no value is prose", "[v2:] x", "[v2:] x", "");
  row("over the byte cap is prose", "[abcdefghijklmnopqrstuvwxyzabcdefghij] x",
      "[abcdefghijklmnopqrstuvwxyzabcdefghij] x", "");
  row("ordinary bracketed prose survives", "[see figure 1]", "[see figure 1]", "");
  row("a log level survives", "[WARN] disk full", "[WARN] disk full", "");
  row("a marker at the end of a reply", "done [v2]", "done ", "v2");
  row("a marker alone on its line", "line\n[v2]\nnext", "line\n\nnext", "v2");
  row("a value that is not lowercase parses", "[speed:0.8] x", " x", "speed:0.8");

  std::printf("\n-- nothing is ever lost --\n");
  row("an unterminated bracket", "trailing [", "trailing [", "");
  row("an unterminated token", "trailing [v", "trailing [v", "");
  row("an unterminated marker", "trailing [v2", "trailing [v2", "");

  std::printf("\n-- fences: a marker in a code block is code --\n");
  row("fenced text passes through", "code ```x [v2] y``` end", "code ```x [v2] y``` end", "");
  row("an unclosed fence still passes through", "a ```x [v2]", "a ```x [v2]", "");

  std::printf("\n-- the resume rule --\n");
  // The outer '[' fails the grammar, scanning resumes one character later, and
  // the real directive inside it is still found. Decided, not accidental.
  row("a failed bracket does not swallow a real one", "nested [see [v2] figure]",
      "nested [see  figure]", "v2");
}

void streaming() {
  std::printf("\n-- one delta or one byte at a time, the same reply --\n");
  // The dialogue shape this feature exists for, checked at every chunk size
  // that could split a marker.
  const std::string dialogue = "Sure. [v2] Hi, I am Bob. [v1] And that was Bob.";
  const Run one = drive(dialogue, 0);
  bool all_same = true;
  for (size_t chunk = 1; chunk <= 8; ++chunk) {
    const Run r = drive(dialogue, chunk);
    if (r.text != one.text || joined(r.marks) != joined(one.marks)) {
      all_same = false;
      std::printf("      chunk %zu differs: [%s]\n", chunk, r.text.c_str());
    }
  }
  ok("a dialogue survives every chunk size from 1 to 8", all_same);
  ok("and its markers are stripped from the text",
     one.text == "Sure.  Hi, I am Bob.  And that was Bob.");
  ok("and arrive in order", joined(one.marks) == "v2,v1");

  // Japanese has no spaces, and this project has been bitten once already by a
  // word-boundary assumption. A multi-byte character must never be split by the
  // holdback rule.
  const std::string ja = "はい。[v2] 私はボブです。[v1] 以上です。";
  const Run ja_one = drive(ja, 0);
  bool ja_same = true;
  for (size_t chunk = 1; chunk <= 4; ++chunk)
    if (drive(ja, chunk).text != ja_one.text) ja_same = false;
  ok("Japanese survives every chunk size", ja_same);
  ok("and its markers are gone", ja_one.text == "はい。 私はボブです。 以上です。");

  // The holdback claim from the design note, checked rather than trusted: text
  // with no '[' in it costs nothing at all.
  size_t held = 0;
  {
    std::string seen;
    DirectiveFilter f([&](const std::string& t) { seen += t; }, [](const std::string&, const std::string&) {});
    const std::string plain = "No brackets here at all, just ordinary words.";
    f.feed(plain);
    held = plain.size() - seen.size();
  }
  ok("text with no bracket is held back by 0 bytes", held == 0);
}

}  // namespace

int main() {
  grammar();
  streaming();
  std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
