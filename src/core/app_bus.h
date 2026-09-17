#pragma once
// The app bus (M2.5): one JSON-line channel between this app and whatever is
// scripting it.
//
// **It is an app bus, not an avatar bus.** The milestone plan sketched it as
// an avatar channel, and then the user asked for a script to register toolbar
// buttons and set avatar themes, neither of which is avatar animation. So
// avatar control is one *family* of messages among several — `avatar`, `theme`,
// `toolbar` today, `task` (M2b) next — and the bus itself knows about none of
// them. It carries lines, bounds them, parses them and routes them by family
// name; what a family means lives in the handler the app registers for it.
//
// Adding a family later is therefore a data change: one `add_family()` call and
// one handler function. Nothing in this file, in the queue, in the bounds or in
// the parser learns that the family exists.
//
// ## The message shape
//
//   app -> script   {"t":"session.state","value":"listening"}
//                   {"t":"session.level","mic":0.31,"speaker":0.0}
//                   {"t":"session.usage","ctx":0.82,"session":0.44,"week":0.1}
//                   {"t":"worker.state","name":"counter","state":"done"}
//                   {"t":"turn.text","role":"assistant","text":"..."}
//
//   script -> app   {"t":"avatar.play","clip":"happy","hold":3.0}
//                   {"t":"avatar.sprite","name":"steam","on":true}
//                   {"t":"avatar.cells","cells":[[3,4,4294967295]]}
//                   {"t":"avatar.load","name":"slime"}
//                   {"t":"theme.set","name":"ember"}
//                   {"t":"toolbar.button","id":"proj","label":"Project","path":"C:\\x"}
//
// `t` is `family.verb`. The plan's sketch used a bare verb (`{"t":"play"}`),
// which was unambiguous while the channel was avatar-only and stops being so
// the moment `theme` and `toolbar` are on it — `load` already means two
// different things (load an avatar, load a prompt). The dot is the routing key
// and it is the only structure the transport imposes.
//
// ## The envelope is flat, on purpose
//
// A message is a JSON object of *scalar* fields (string, number, bool) plus
// flat numeric lists. Nested objects are ignored rather than rejected, which is
// what "unknown fields are ignored" has to mean if a script written for a later
// version is to keep working against an earlier one. There is no reachable
// recursion in what a handler sees, so no handler can be handed a structure it
// did not expect.
//
// ## Untrusted input
//
// Every inbound line is treated as hostile. It is length-capped before it is
// parsed, field-capped and value-capped after; a line that does not parse, or
// names a family nobody registered, is counted and logged and is never fatal.
// No handler here invents a privilege: `toolbar` reaches
// `ButtonRegistry::add_path_button`, the same validated door the assistant's
// ```aii``` block already uses, and `ButtonActionKind::Invoke` stays
// unreachable because the untrusted entry point cannot produce one.
//
// ## Bounds
//
// Outbound events are published whether or not anything is listening, so the
// queue has to be bounded twice over:
//
//   * **Coalescing.** An event published with a `key` *replaces* any queued
//     event with the same key. The continuous signals — state, levels, usage,
//     one row per worker — use their topic as the key, so at 60 Hz with nothing
//     draining they occupy a fixed handful of slots rather than 3600 a minute.
//     This is the bound that matters: it is structural, not a cap.
//   * **A hard cap** (`kBusEventsMax`) for the keyless ones (turn text), oldest
//     dropped first, with a counter so a reader can see that it missed some.
//
// Inbound is capped too (`kBusInboxMax`), dropping the *newest*: a flood must
// not evict a command the frame loop has not applied yet, and the sender gets a
// refusal line in the log rather than silence.
//
// ## Threads
//
// `publish()`, `post()` and `drain_events()` may be called from any thread and
// never block on a consumer — there is no consumer to block on, only a short
// mutex around a bounded container.
//
// **`apply_pending()` is frame-loop only, and that is the rule that matters.**
// It is the one call that runs handlers, and a handler touches the avatar, the
// registry and the settings. That is `VoiceSession::announce()`'s rule and the
// reason it keeps it: work that arrives off the frame loop is queued, and the
// frame loop is the only thing that acts on it. Getting this wrong does not
// fail loudly — it fails as a rare crash or a torn frame.
//
// This paragraph used to call `drain_events()` frame-loop-only too. That was
// never a safety claim (it takes the same mutex as everything else) and M2.6's
// script thread drains from Python. What is true, and is the real hazard, is
// that **draining empties the queue for everybody**: there is one queue and
// therefore one consumer's worth of events. Two readers — a script and
// `--bus-out`, or two scripts — split the stream between them unless something
// above fans it out, which is exactly what `aii_pyhost` does for the script
// threads.
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aii {

// Caps. In one place because they are the whole of the memory story.
constexpr std::size_t kBusLineMax = 8192;     // bytes of one inbound line
constexpr std::size_t kBusFieldsMax = 32;     // fields of one inbound message
constexpr std::size_t kBusStringMax = 512;    // bytes of one string value
constexpr std::size_t kBusNumbersMax = 4096;  // entries of one numeric list
constexpr std::size_t kBusEventsMax = 256;    // queued outbound events
constexpr std::size_t kBusInboxMax = 64;      // queued inbound messages
constexpr std::size_t kBusStatusMax = 64;     // queued log lines

// One field of an inbound message. Scalars and flat numeric lists; there is
// deliberately nothing recursive here.
struct BusValue {
  enum class Kind { Str, Num, Bool, Nums } kind = Kind::Str;
  std::string s;
  double n = 0.0;
  bool b = false;
  std::vector<double> nums;
};

// A parsed inbound message: the routing key split, and the flat fields.
struct BusMessage {
  std::string family;  // before the first '.' in "t"
  std::string verb;    // after it
  std::map<std::string, BusValue> fields;

  // Accessors that never throw and never surprise: a missing field, or one of
  // the wrong kind, is the default. A handler therefore has no error path for
  // "the script sent a number where I wanted a string" — it gets the default
  // and carries on, which is what ignoring unknown input has to mean.
  std::string str(const char* key, const std::string& fallback = {}) const;
  double num(const char* key, double fallback = 0.0) const;
  bool flag(const char* key, bool fallback = false) const;
  const std::vector<double>* nums(const char* key) const;
  bool has(const char* key) const;
};

// What a family does. `error` may be filled with one short line; it goes to the
// log, never to the user and never to speech, exactly as a refused toolbar
// button does.
using BusHandler = std::function<void(const BusMessage& msg, std::string* error)>;

// Builds one outbound line. A builder rather than a JSON dependency in this
// header: the outbound vocabulary is a dozen flat objects and this keeps
// json.hpp out of everything that publishes.
class BusLine {
 public:
  explicit BusLine(const std::string& topic);
  BusLine& str(const char* key, const std::string& value);
  BusLine& num(const char* key, double value, int decimals = 3);
  BusLine& flag(const char* key, bool value);
  std::string done() const;

 private:
  std::string out_;
};

class AppBus {
 public:
  // Process-wide, like ButtonRegistry and for the same reason: its producers
  // are the frame loop, the turn thread and (M2.6) a script thread, and one
  // store with one mutex is the whole of the concurrency design.
  static AppBus& instance();

  // ---- outbound: app -> script ----------------------------------------
  // Publishes whether or not anything is listening. `key` non-empty replaces
  // any queued event carrying the same key; empty appends. Any thread.
  void publish(std::string line, const std::string& key = {});
  // Everything queued, in order, oldest first. Frame loop.
  std::vector<std::string> drain_events();
  // Events dropped at the cap since the last call, and clears the counter.
  std::size_t take_events_dropped();
  // True when something has asked for the outbound stream. Publishers may use
  // it to skip building a line nobody will read; they are not required to, and
  // the bus behaves identically either way.
  bool has_listener() const;
  void set_listener(bool listening);

  // ---- inbound: script -> app -----------------------------------------
  // Parses, bounds and queues one line. Any thread. Returns false when the
  // line was refused (too long, malformed, no `t`, inbox full); `error` is
  // then one short line. A refusal is never fatal and never reaches the user.
  bool post(const std::string& line, std::string* error = nullptr);
  // The same for a blob of text: splits on newlines, skips blanks and `#`
  // comments, posts each line. Returns how many were accepted.
  std::size_t post_lines(const std::string& text);

  // Registers the handler for one family. Called once per family at startup,
  // before the first frame. Registering a family twice replaces the handler.
  void add_family(std::string family, BusHandler handler);

  // Applies every queued inbound message, in arrival order, through the family
  // handlers. **Frame loop only, at one defined point**: this is what makes a
  // command that lands mid-frame land at a frame boundary instead. Returns the
  // number applied.
  std::size_t apply_pending();

  // Refusals and drops since the last call, for the log, and empties them.
  // Same contract as ButtonRegistry::take_status(): a producer with no console
  // of its own, and a message silently dropped is the thing that cannot be
  // debugged later.
  std::vector<std::string> take_status();

  // Test seam: forgets every family, queue and counter. Not used by the app.
  void reset();

 private:
  AppBus() = default;
  void note_locked(std::string line);

  struct Queued {
    std::string key;
    std::string line;
  };

  mutable std::mutex mutex_;
  std::deque<Queued> events_;
  std::deque<BusMessage> inbox_;
  std::map<std::string, BusHandler> families_;
  std::vector<std::string> status_;
  std::size_t events_dropped_ = 0;
  bool listening_ = false;
};

}  // namespace aii
