// AppBus: the bounds on a channel whose input is hostile by assumption.
//
// Finding 26 of the 21 Sep review: `AppBus::post` bounds had no test. The
// header states the contract this file checks, and the cases are written
// against the header rather than the parser:
//
//   * **inbound is capped** (`kBusInboxMax`) and drops the *newest*, "a flood
//     must not evict a command the frame loop has not applied yet";
//   * a line is length-capped (`kBusLineMax`) before the parser sees a byte,
//     field-capped (`kBusFieldsMax`) and value-capped (`kBusStringMax`,
//     `kBusNumbersMax`) after;
//   * "a line that does not parse, or names a family nobody registered, is
//     counted and logged and is **never fatal**" -- so post() must not throw,
//     whatever it is handed;
//   * outbound drains **in order, oldest first**, a keyed event replaces the
//     queued one with the same key *keeping its position*, and the keyless
//     ones are capped at `kBusEventsMax` dropping oldest with a counter;
//   * nested objects are ignored rather than rejected, so a script written
//     for a later version still works against an earlier one.
//
// The bus is a process-wide singleton with a `reset()` test seam, so every
// group below starts from reset().
//
//   app_bus_test            prints every case and exits non-zero on failure
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "core/app_bus.h"

using aii::AppBus;
using aii::BusLine;
using aii::BusMessage;

namespace {

int g_failures = 0;

void check(const char* name, bool ok, const std::string& detail = {}) {
  if (!ok) ++g_failures;
  std::printf("  %-58s %s%s%s\n", name, ok ? "ok  " : "FAIL",
              detail.empty() ? "" : "  ", detail.c_str());
}

void check_eq(const char* name, std::size_t got, std::size_t want) {
  const bool ok = got == want;
  if (!ok) ++g_failures;
  std::printf("  %-58s %s  got=%zu\n", name, ok ? "ok  " : "FAIL", got);
  if (!ok) std::printf("      wanted %zu\n", want);
}

void check_str(const char* name, const std::string& got, const std::string& want) {
  const bool ok = got == want;
  if (!ok) ++g_failures;
  std::printf("  %-58s %s  %zu bytes\n", name, ok ? "ok  " : "FAIL", got.size());
  if (!ok) {
    std::printf("      wanted [%s]\n", want.c_str());
    std::printf("      got    [%s]\n", got.c_str());
  }
}

AppBus& bus() { return AppBus::instance(); }

// Registers a family that records every message it is handed, so apply order
// and the routing key are both observable.
std::vector<std::string>* g_seen = nullptr;
void record_family(const char* family) {
  bus().add_family(family, [](const BusMessage& m, std::string*) {
    if (g_seen) g_seen->push_back(m.family + "." + m.verb + ":" + m.str("v"));
  });
}

std::string rep(const std::string& unit, std::size_t n) {
  std::string out;
  out.reserve(unit.size() * n);
  for (std::size_t i = 0; i < n; ++i) out += unit;
  return out;
}

// post() is documented as never fatal. A throw out of it would take down the
// script thread, so every hostile case goes through here and the throw itself
// is the failure, not just a wrong return value.
bool post_no_throw(const std::string& line, bool* accepted) {
  try {
    std::string err;
    *accepted = bus().post(line, &err);
    return true;
  } catch (const std::exception&) {
    return false;
  } catch (...) {
    return false;
  }
}

void check_survives(const char* name, const std::string& line, bool want_accepted) {
  bool accepted = false;
  const bool no_throw = post_no_throw(line, &accepted);
  const bool ok = no_throw && accepted == want_accepted;
  if (!ok) ++g_failures;
  std::printf("  %-58s %s  %s\n", name, ok ? "ok  " : "FAIL",
              !no_throw ? "THREW" : (accepted ? "accepted" : "refused"));
  if (no_throw && accepted != want_accepted)
    std::printf("      wanted %s\n", want_accepted ? "accepted" : "refused");
}

}  // namespace

int main() {
  std::printf("app_bus_test\n\n");

  // --- 1. what post() accepts and what it refuses ---------------------------
  std::printf("post: the shape of a line\n");
  bus().reset();
  check_survives("a well-formed line is accepted", "{\"t\":\"avatar.play\"}", true);
  check_survives("not JSON at all", "this is not json", false);
  check_survives("truncated JSON", "{\"t\":\"avatar.play\"", false);
  check_survives("a JSON array, not an object", "[1,2,3]", false);
  check_survives("a bare JSON string", "\"avatar.play\"", false);
  check_survives("a bare number", "42", false);
  check_survives("JSON null", "null", false);
  check_survives("an empty line", "", false);
  check_survives("an object with no \"t\"", "{\"clip\":\"happy\"}", false);
  check_survives("\"t\" that is not a string", "{\"t\":7}", false);
  check_survives("\"t\" with no dot", "{\"t\":\"play\"}", false);
  check_survives("\"t\" with a leading dot", "{\"t\":\".play\"}", false);
  check_survives("\"t\" with a trailing dot", "{\"t\":\"avatar.\"}", false);
  check_survives("\"t\" that is empty", "{\"t\":\"\"}", false);
  // A family nobody registered still parses: it is refused at apply time, in
  // the log, not at the door.
  check_survives("an unregistered family still posts", "{\"t\":\"nosuch.verb\"}", true);

  std::printf("\npost: hostile bytes, none of them fatal\n");
  check_survives("invalid UTF-8 in a value",
                 "{\"t\":\"a.b\",\"v\":\"\xFF\xFE\xFD\"}", false);
  check_survives("a lone continuation byte as the whole line", "\x80\x80", false);
  check_survives("an embedded NUL in the line",
                 std::string("{\"t\":\"a.b\",\"v\":\"x\0y\"}", 21), false);
  check_survives("control bytes", "{\"t\":\"a.b\"}\x01\x02", false);
  // A literal that overflows a double is refused by the parser itself, so the
  // whole line goes rather than the field. post()'s own `isfinite` guard on a
  // number is therefore belt-and-braces rather than the thing that catches
  // this; that is the safe order and it is recorded so a parser swap is
  // noticed. A number within range is accepted however extreme.
  check_survives("a number too large for a double is refused",
                 "{\"t\":\"a.b\",\"n\":1e400}", false);
  check_survives("  ...but a huge in-range number is fine",
                 "{\"t\":\"a.b\",\"n\":1e300}", true);
  check_survives("deeply nested arrays", "{\"t\":\"a.b\",\"v\":" + rep("[", 400) +
                                             rep("]", 400) + "}", true);
  check_survives("duplicate keys", "{\"t\":\"a.b\",\"v\":1,\"v\":2}", true);
  check_survives("a nested object is ignored, not refused",
                 "{\"t\":\"a.b\",\"o\":{\"deep\":{\"deeper\":1}},\"v\":\"kept\"}", true);

  // --- 2. the caps ----------------------------------------------------------
  std::printf("\npost: the caps\n");
  bus().reset();
  {
    // kBusLineMax is checked before the parser sees a byte. A line one byte
    // over is refused even though it is perfectly good JSON.
    const std::string head = "{\"t\":\"a.b\",\"v\":\"";
    const std::string tail = "\"}";
    const std::string pad = rep("x", aii::kBusLineMax - head.size() - tail.size());
    check_survives("a line exactly at kBusLineMax", head + pad + tail, true);
    check_survives("a line one byte over kBusLineMax", head + pad + "x" + tail, false);
  }
  bus().reset();
  {
    // A string value is clipped to kBusStringMax, not refused: the message
    // still arrives, shorter.
    bus().post("{\"t\":\"a.b\",\"v\":\"" + rep("y", 2000) + "\"}");
    std::string got;
    bus().add_family("a", [&got](const BusMessage& m, std::string*) { got = m.str("v"); });
    bus().apply_pending();
    check_eq("a long string value is clipped to kBusStringMax", got.size(),
             aii::kBusStringMax);
  }
  bus().reset();
  {
    // Fields past kBusFieldsMax are ignored, not refused -- an unknown field
    // from a later version must never take the message down.
    std::string line = "{\"t\":\"a.b\"";
    for (std::size_t i = 0; i < aii::kBusFieldsMax + 20; ++i)
      line += ",\"f" + std::to_string(i) + "\":" + std::to_string(i);
    line += "}";
    std::size_t fields = 0;
    bool accepted = false;
    post_no_throw(line, &accepted);
    bus().add_family("a", [&fields](const BusMessage& m, std::string*) {
      fields = m.fields.size();
    });
    bus().apply_pending();
    check("a message with too many fields is still accepted", accepted);
    check_eq("fields past kBusFieldsMax are dropped", fields, aii::kBusFieldsMax);
  }
  bus().reset();
  {
    // kBusNumbersMax is 4096 entries, and it turns out to be unreachable
    // through post(): the shortest an entry can be written is two bytes
    // ("1,"), so 4096 of them are already past the 8192-byte kBusLineMax and
    // the line is refused at the door before the parser sees it. That is the
    // right order for the two caps to fire in -- the cheaper one first -- and
    // it is recorded here so that a later change to either cap is noticed.
    std::string line = "{\"t\":\"a.b\",\"cells\":[";
    for (std::size_t i = 0; i < aii::kBusNumbersMax + 100; ++i)
      line += (i ? ",1" : "1");
    line += "]}";
    std::size_t count = 0;
    bus().add_family("a", [&count](const BusMessage& m, std::string*) {
      const std::vector<double>* v = m.nums("cells");
      count = v ? v->size() : 0;
    });
    bool accepted = false;
    post_no_throw(line, &accepted);
    check("a kBusNumbersMax-long list breaks kBusLineMax first", !accepted);
    bus().apply_pending();

    // Now one that fits the line cap: 1500 single-digit entries.
    std::string ok_line = "{\"t\":\"a.b\",\"cells\":[";
    for (std::size_t i = 0; i < 1500; ++i) ok_line += (i ? ",1" : "1");
    ok_line += "]}";
    check("  ...and a list inside the line cap is accepted",
          ok_line.size() <= aii::kBusLineMax && bus().post(ok_line));
    bus().apply_pending();
    check_eq("  ...with every entry kept", count, 1500);
  }
  bus().reset();
  {
    // Nested triples flatten into the same list as flat numbers, and anything
    // in the array that is not a number is skipped rather than refused.
    std::size_t count = 0;
    double first = -1.0;
    bus().add_family("avatar", [&](const BusMessage& m, std::string*) {
      const std::vector<double>* v = m.nums("cells");
      count = v ? v->size() : 0;
      if (v && !v->empty()) first = (*v)[0];
    });
    bus().post("{\"t\":\"avatar.cells\",\"cells\":[[3,4,7],\"skip\",[5,6,8],null]}");
    bus().apply_pending();
    check_eq("nested triples flatten into one list", count, 6);
    check("  ...in order", first == 3.0);
  }

  // --- 3. the inbox cap: newest dropped -------------------------------------
  std::printf("\npost: the inbox cap drops the newest\n");
  bus().reset();
  {
    std::vector<std::string> seen;
    g_seen = &seen;
    record_family("a");
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < aii::kBusInboxMax + 25; ++i) {
      std::string err;
      if (bus().post("{\"t\":\"a.b\",\"v\":\"" + std::to_string(i) + "\"}", &err)) ++accepted;
      else if (i < aii::kBusInboxMax) check("early post refused", false, err);
    }
    check_eq("exactly kBusInboxMax posts are accepted", accepted, aii::kBusInboxMax);
    const std::size_t applied = bus().apply_pending();
    check_eq("  ...and all of them are applied", applied, aii::kBusInboxMax);
    // Newest dropped, not oldest: the first command is still there and the
    // order is arrival order.
    check("the OLDEST command survived the flood",
          !seen.empty() && seen.front() == "a.b:0");
    check("the last surviving command is the kBusInboxMax-th",
          !seen.empty() && seen.back() == "a.b:" + std::to_string(aii::kBusInboxMax - 1));
    // The refusal is logged rather than silent.
    const std::vector<std::string> status = bus().take_status();
    check("the refusals are in the status log", status.size() == 25,
          std::to_string(status.size()) + " lines");
    check("  ...and take_status() empties it", bus().take_status().empty());
    // Draining the inbox makes room again.
    std::string err;
    check("the inbox accepts again once applied", bus().post("{\"t\":\"a.b\"}", &err), err);
    g_seen = nullptr;
  }

  // --- 4. apply_pending: order and unknown families -------------------------
  std::printf("\napply_pending: arrival order, unknown families logged\n");
  bus().reset();
  {
    std::vector<std::string> seen;
    g_seen = &seen;
    record_family("avatar");
    record_family("theme");
    bus().post("{\"t\":\"avatar.play\",\"v\":\"1\"}");
    bus().post("{\"t\":\"nosuch.verb\",\"v\":\"2\"}");
    bus().post("{\"t\":\"theme.set\",\"v\":\"3\"}");
    bus().post("{\"t\":\"avatar.load\",\"v\":\"4\"}");
    const std::size_t applied = bus().apply_pending();
    check_eq("only the registered families count as applied", applied, 3);
    const bool order = seen.size() == 3 && seen[0] == "avatar.play:1" &&
                       seen[1] == "theme.set:3" && seen[2] == "avatar.load:4";
    check("handlers run in arrival order", order);
    const std::vector<std::string> status = bus().take_status();
    check("the unknown family is logged", status.size() == 1 &&
                                              status[0] == "no such family: nosuch");
    check_eq("a second apply_pending has nothing to do", bus().apply_pending(), 0);
    g_seen = nullptr;
  }
  bus().reset();
  {
    // A handler that fills `error` is logged and is not counted as applied.
    bus().add_family("avatar", [](const BusMessage&, std::string* err) {
      *err = "refused by the handler";
    });
    bus().post("{\"t\":\"avatar.play\"}");
    check_eq("a handler that refuses is not counted", bus().apply_pending(), 0);
    check("  ...and its reason is logged", bus().take_status().size() == 1);
  }

  // --- 5. post_lines --------------------------------------------------------
  std::printf("\npost_lines: blanks and # comments skipped\n");
  bus().reset();
  {
    record_family("a");
    std::vector<std::string> seen;
    g_seen = &seen;
    const std::size_t n = bus().post_lines(
        "# a comment\n"
        "\n"
        "   \n"
        "{\"t\":\"a.b\",\"v\":\"1\"}\n"
        "not json\n"
        "  {\"t\":\"a.b\",\"v\":\"2\"}  \n");
    check_eq("two of six lines are accepted", n, 2);
    bus().apply_pending();
    check("both arrived in order",
          seen.size() == 2 && seen[0] == "a.b:1" && seen[1] == "a.b:2");
    g_seen = nullptr;
  }

  // --- 6. outbound: order, coalescing, the hard cap -------------------------
  std::printf("\npublish and drain_events\n");
  bus().reset();
  {
    bus().publish("one");
    bus().publish("two");
    bus().publish("three");
    const std::vector<std::string> out = bus().drain_events();
    check("drain returns everything, oldest first",
          out.size() == 3 && out[0] == "one" && out[2] == "three");
    check("  ...and empties the queue", bus().drain_events().empty());
  }
  bus().reset();
  {
    // A keyed event replaces the queued one with the same key *in place*, so
    // the continuous signals keep the order they first arrived in.
    bus().publish("state-1", "session.state");
    bus().publish("level-1", "session.level");
    bus().publish("state-2", "session.state");
    bus().publish("state-3", "session.state");
    const std::vector<std::string> out = bus().drain_events();
    check("a keyed event coalesces in place",
          out.size() == 2 && out[0] == "state-3" && out[1] == "level-1");
  }
  bus().reset();
  {
    // The hard cap is for the keyless ones. Oldest dropped first, with a
    // counter so a reader can see that it missed some.
    for (std::size_t i = 0; i < aii::kBusEventsMax + 10; ++i)
      bus().publish("turn-" + std::to_string(i));
    const std::vector<std::string> out = bus().drain_events();
    check_eq("the queue stops at kBusEventsMax", out.size(), aii::kBusEventsMax);
    check("the newest lines are the ones kept",
          out.back() == "turn-" + std::to_string(aii::kBusEventsMax + 9) &&
              out.front() == "turn-10");
    check_eq("and the drops are counted", bus().take_events_dropped(), 10);
    check_eq("  ...and the counter is cleared", bus().take_events_dropped(), 0);
  }
  bus().reset();
  {
    check("has_listener() is false until something asks", !bus().has_listener());
    bus().set_listener(true);
    check("  ...and true once it does", bus().has_listener());
    bus().publish("x");
    check("publishing does not depend on it", bus().drain_events().size() == 1);
  }

  // --- 7. BusLine and the accessors -----------------------------------------
  std::printf("\nBusLine and the never-surprising accessors\n");
  bus().reset();
  {
    const std::string line = BusLine("session.level")
                                 .num("mic", 0.31)
                                 .num("speaker", 0.0)
                                 .flag("muted", true)
                                 .str("note", "a \"quoted\" line\nbroken")
                                 .done();
    check("BusLine escapes quotes and newlines",
          line.find("a \\\"quoted\\\" line\\nbroken") != std::string::npos, line);
    // A line the bus builds must be a line the bus accepts.
    std::string err;
    check("a line BusLine built round-trips through post", bus().post(line, &err), err);
  }
  bus().reset();
  {
    // The header: "a missing field, or one of the wrong kind, is the default".
    std::string s = "unset";
    double n = -1.0;
    bool f = true;
    bool has = true;
    const std::vector<double>* nums = nullptr;
    bus().add_family("a", [&](const BusMessage& m, std::string*) {
      s = m.str("nope", "fallback");
      n = m.num("text", -5.0);
      f = m.flag("text", false);
      has = m.has("nope");
      nums = m.nums("text");
    });
    bus().post("{\"t\":\"a.b\",\"text\":\"hello\"}");
    bus().apply_pending();
    check_str("a missing string is the fallback", s, "fallback");
    check("a string read as a number is the fallback", n == -5.0);
    check("a string read as a flag is the fallback", !f);
    check("has() is false for a missing field", !has);
    check("nums() is null for a non-list field", nums == nullptr);
  }
  bus().reset();
  {
    // A number reads as a flag and a bool reads as a number, which is what
    // "never surprise a handler" means for the two that do overlap.
    double n = -1.0;
    bool f = false;
    bus().add_family("a", [&](const BusMessage& m, std::string*) {
      n = m.num("on", -1.0);
      f = m.flag("count", false);
    });
    bus().post("{\"t\":\"a.b\",\"on\":true,\"count\":3}");
    bus().apply_pending();
    check("a bool read as a number is 1", n == 1.0);
    check("a non-zero number read as a flag is true", f);
  }

  // --- 8. reset -------------------------------------------------------------
  std::printf("\nreset: the test seam forgets everything\n");
  bus().reset();
  {
    record_family("a");
    bus().publish("x");
    bus().post("{\"t\":\"a.b\"}");
    bus().set_listener(true);
    bus().reset();
    check("events are gone", bus().drain_events().empty());
    check("the inbox is gone", bus().apply_pending() == 0);
    check("the status log is gone", bus().take_status().empty());
    check("the listener flag is cleared", !bus().has_listener());
    // Families too, so the next post to "a" is an unknown family.
    bus().post("{\"t\":\"a.b\"}");
    bus().apply_pending();
    check("the families are gone", bus().take_status().size() == 1);
  }

  std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
