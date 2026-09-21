// A line of JSON from the CLI that is not the shape this app expected.
//
// Review finding 1, M15.2. `ClaudeCodeClient::handle_line` had only its
// `json::parse` inside a try; every read after it was a `value()` or an
// `operator[]`, both of which throw `nlohmann::type_error` when the parent is
// not an object or the field is not the type asked for. It runs on the reader
// thread, where nothing catches, so the whole app went down through
// `std::terminate` -- no status line, no log, the window simply gone.
//
// The four lines below are the ones that were named when the finding was
// written. They are fed to a client that was constructed and never started, so
// there is no `claude` process anywhere near this test: `handle_line` is the
// one part of that class that can be exercised on its own, which is why it is
// public. If any of them threw, this test would not print a failure -- it would
// die, exactly as the app did.
#include <cstdio>
#include <string>

#include "llm/claude_code_client.h"
#include "llm/json_shape.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

aii::ClaudeCodeClient::Options quiet_options() {
  aii::ClaudeCodeClient::Options o;
  o.exe = "claude-that-is-never-run";
  return o;
}

}  // namespace

int main() {
  using nlohmann::json;

  // ---- 1. the helpers, on their own -------------------------------------
  std::printf("member() and field() on the wrong shape\n");
  {
    const json arr = json::array({1, 2, 3});
    check(aii::member(arr, "type").is_null(), "a field of an array is null, not a throw");
    check(aii::field<std::string>(arr, "type", "?") == "?", "and reads as the fallback");
    const json obj = json::parse(R"({"a":1,"b":null,"c":"text"})");
    check(aii::member(obj, "missing").is_null(), "a key that is not there is null");
    check(aii::field<std::string>(obj, "a", "?") == "?",
          "**a number read as a string falls back** rather than throwing type_error");
    check(aii::field<int>(obj, "c", -1) == -1, "and a string read as a number does the same");
    check(aii::field<int>(obj, "b", -1) == -1, "an explicit null falls back");
    check(aii::field<int>(obj, "a", -1) == 1, "and a field of the right type reads normally");
    check(aii::member(aii::member(obj, "missing"), "deeper").is_null(),
          "nesting one inside the other is safe all the way down");
  }

  // ---- 2. the four recorded lines ---------------------------------------
  //
  // Reaching the line after each call is the assertion. `check` is here so the
  // output names what survived rather than printing nothing.
  std::printf("lines that used to end the app\n");
  {
    aii::ClaudeCodeClient c(quiet_options());
    c.handle_line("[1, 2, 3]");
    check(true, "**a top-level array**: type is read off a non-object and the line is dropped");

    c.handle_line(R"({"type":"rate_limit_event","rate_limit_info":null})");
    c.handle_line(R"({"type":"rate_limit_event","rate_limit_info":{"unifiedWindows":{"five_hour":null}}})");
    c.handle_line(R"({"type":"rate_limit_event","rate_limit_info":{"unifiedWindows":{"five_hour":"soon"}}})");
    check(true, "a rate_limit_event with a null window, in all three places it can be null");

    c.handle_line(R"({"type":"stream_event"})");
    c.handle_line(R"({"type":"stream_event","event":"content_block_delta"})");
    check(true, "a stream_event with no event object, and with a string where the object goes");

    c.handle_line(R"({"type":"result","result":42,"is_error":true})");
    check(true, "a result whose result field is a number");

    c.handle_line("not json at all");
    c.handle_line("");
    c.handle_line("{}");
    c.handle_line(R"({"type":42})");
    check(true, "and the odds and ends: unparseable, empty, bare object, numeric type");
  }

  // ---- 3. a good line still does its job --------------------------------
  //
  // The risk in wrapping a handler in a try is that it starts swallowing the
  // cases it was meant to handle. One assertion that the normal path still
  // works, read back through the accessors the app itself uses.
  std::printf("a well-formed line is still read\n");
  {
    aii::ClaudeCodeClient c(quiet_options());
    c.handle_line(
        R"({"type":"system","subtype":"init","session_id":"abc-123","model":"claude-opus-5[1m]"})");
    check(c.session_id() == "abc-123", "the session id from a system/init event");
    check(c.model() == "claude-opus-5[1m]", "and the model it reported");
    check(c.usage().ctx_window == 1000000,
          "and the \"[1m]\" suffix is still read as the long context window");
  }
  {
    // The one bad line before it must not stop the good one after it: the
    // reader keeps going, which is the whole point of dropping a line rather
    // than dying on it.
    aii::ClaudeCodeClient c(quiet_options());
    c.handle_line("[\"nonsense\"]");
    c.handle_line(R"({"type":"system","subtype":"init","session_id":"after","model":"claude-haiku-5"})");
    check(c.session_id() == "after", "**a bad line does not cost the app the line after it**");
    check(c.usage().ctx_window == 200000, "and a model with no suffix gets the documented default");
  }

  std::printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
