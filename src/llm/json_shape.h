#pragma once
// Reading a field out of JSON that arrived from somewhere else.
//
// M15.2, review finding 1. Both clients in this directory read a stream of
// events written by something the app does not control -- the Claude Code CLI's
// stream-json on one side, the API's SSE on the other -- and both did it with
// `j["key"]` and `j.value("key", default)` straight off the parse. Neither is
// safe on JSON of an unexpected shape: `value()` throws `type_error` when the
// parent is not an object, `get<T>()` throws when the field is there but of
// another type, and this all happens on the reader thread, where an escaped
// exception is `std::terminate` and the app is gone with no message.
//
// The two helpers here make the shape a value rather than a promise: a field
// that is missing, or whose parent is not an object, reads as null and falls
// back; a field of the wrong type falls back too. That is the right answer for
// a telemetry stream, where a version of the CLI that renames a key should cost
// this app a wrong number in a status line and nothing else.
//
// They are not a substitute for a try/catch around the whole handler. A future
// edit that reaches for `j["x"]` again would be back where this started, so the
// backstop stays; these are what stop the backstop from having to fire.
#include <string>

#include "json.hpp"

namespace aii {

// The named field of `j`, or a null json when `j` is not an object or has no
// such key. Never throws, never inserts (nlohmann's own `operator[]` on a
// non-const object does insert, which is how a missing `event` became a null
// that the next `value()` call threw on).
inline const nlohmann::json& member(const nlohmann::json& j, const char* key) {
  static const nlohmann::json kNull;
  if (!j.is_object()) return kNull;
  const auto it = j.find(key);
  return it == j.end() ? kNull : *it;
}

// The named field of `j` read as a `T`, or `fallback` when it is missing, null,
// or of a type that is not a `T`. A `result` whose `result` field is a number
// rather than the expected string is the real case this was written for.
template <class T>
T field(const nlohmann::json& j, const char* key, T fallback) {
  const nlohmann::json& v = member(j, key);
  if (v.is_null()) return fallback;
  try {
    return v.get<T>();
  } catch (...) {
    return fallback;
  }
}

}  // namespace aii
