#pragma once
// The ```aii block: the assistant's one command channel back into this app.
//
// Lifted out of `core/worker_pool.cpp` in M15.1. It was sitting in the same
// translation unit as the worker pool, which owns a `ClaudeCodeClient` and
// therefore the whole process-spawning, Windows-linking half of the app -- so
// the one piece of code in this program that reads untrusted model output and
// decides what to *do* about it was the one piece that could not be put in a
// test. Here it depends on the standard library and `core/text_util.h` only,
// and `tests/aii_block_test.cpp` is the consequence.
//
// The file deliberately knows nothing about workers, schedules, settings,
// memories or buttons. It turns text into `Command` values and says what it
// refused; every verb's meaning belongs to its handler.
#include <functional>
#include <string>
#include <vector>

namespace aii {

// One command parsed out of a fenced ```aii block in a reply.
//
// The block is this app's only agent->app command channel, and stays that way:
// M1c.2's toolbar buttons are a verb here rather than a second mechanism, and
// M2.5's bus is specced to reuse the same registry behind it.
struct Command {
  std::string verb;  // spawn | pause | stop | schedule (for the session)
  std::string name;
  std::string cwd;
  std::string task;
  // The `schedule` verb's fields (M2b.3). `in` is a spoken-shaped delay
  // ("10m", "90s") parsed by `aii::parse_delay`; `say` is the exact sentence a
  // bare timer speaks when it fires. `grade` is normally left empty and
  // derived from the shape -- a line with `say=` is a fixed report, a line with
  // `task=` is a phrased one -- and exists as a key so the bus (M2b.2) can be
  // explicit where a script has no shape to signal with.
  std::string in;
  std::string say;
  std::string grade;
  // M31. `spawn`'s and `schedule ... task=`'s optional `model=<default|opus|
  // sonnet|haiku>` -- the settings.json key spelling, not a `--model` string,
  // exactly as `setting value=` already reads keys rather than CLI args. A
  // single token, not end-of-line: every legal value is one word. The
  // handler passes it through only when `model_choice_for_key()` recognises
  // it; anything else is ignored with one log line, because the model does
  // not get to name arbitrary strings on a command line.
  std::string model;
  // The `button` verb's fields (M1c.2). Kept in the same struct rather than a
  // variant because the block is line-oriented key=value either way and one
  // parser is what makes a new verb a few lines instead of a format.
  std::string id;
  std::string label;
  std::string tip;
  std::string path;
  // The `setting` verb's fields (M3.14). `key` is a dotted `section.key` of
  // `settings.json` -- the file's own vocabulary rather than a second one, so
  // that the words the model uses are the words the user reads.
  //
  // **`confirm` is a field and not prose, which is the whole of how the
  // question gets asked.** A change that restarts the `claude` child throws
  // the conversation away (M3.12), so warning about it has to survive being
  // forgotten -- and a warning the model merely *says* can be skipped in the
  // same reply that acts, which is a question in grammar only. Making the
  // consent a token the model has to write turns an omission into a
  // commission, and `VoiceSession::apply_setting` then refuses it outright
  // unless the app asked on an earlier turn. See M3.13: the lever on this
  // model's behaviour was the syntax line, not the paragraph beside it.
  std::string key;
  std::string value;
  std::string confirm;
  // The `remember` verb's field (M14). Prose, so like `task=` it runs to the
  // end of the line when unquoted. `forget` addresses a memory by `id`, the
  // same field `cancel` and `button` already use.
  std::string text;
};

// Applies the app-owned `button` verb. Supplied by the caller rather than
// called directly, because `core/button_registry.cpp` includes <windows.h> and
// <shellapi.h> -- it opens folders with ShellExecute -- and dragging it in here
// would cost this file exactly the property it was split out to gain. The
// binding lives in `worker_pool.cpp`, one function away.
using ButtonApplyFn = std::function<void(const Command&)>;

// Finds ```aii fenced blocks in `text` and parses their command lines.
//
// Values are unquoted single tokens, or `"quoted like this"` when they contain
// spaces; `task=`, `path=`, `say=` and `text=` run to the end of the line when
// unquoted, since all four are routinely the last thing on it and all four
// routinely contain spaces.
//
// **An unterminated block is not parsed** (M15.1, review finding 8). A reply cut
// off by a token limit mid-block used to have its half-written lines parsed and
// dispatched, which could spawn a bypassPermissions worker on half a sentence,
// while `strip_aii_blocks` dropped the very same text from the transcript -- so
// the user was shown nothing and the app acted anyway. A block with no closing
// fence is now dropped by both, and the reason is appended to `problems` for
// the caller to log. The parser has no logger of its own; this is the same
// out-parameter `expand_tool_sections` reports its notes through.
//
// **App-owned verbs are applied here and are not returned.** `button` has no
// worker to dispatch to -- it registers with ButtonRegistry through `on_button`
// -- and this is the one point every ```aii``` block in the process already
// flows through, exactly once per completed reply, which is precisely the
// cardinality registration wants. A `button` line is dropped either way: with
// no handler it is applied by nobody rather than handed to a caller that has no
// idea what to do with it.
//
// `problems` may be null; `on_button` may be empty.
std::vector<Command> parse_commands(const std::string& text,
                                    std::vector<std::string>* problems,
                                    const ButtonApplyFn& on_button);

// The same text with every ```aii block removed, for display. An unterminated
// block takes the rest of the text with it, which is the display half of the
// rule above: what is not executed is not shown either.
std::string strip_aii_blocks(const std::string& text);

}  // namespace aii
