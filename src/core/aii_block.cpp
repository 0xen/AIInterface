#include "core/aii_block.h"

#include "core/text_util.h"

namespace aii {
namespace {

void note(std::vector<std::string>* problems, const std::string& what) {
  if (problems) problems->push_back(what);
}

}  // namespace

std::vector<Command> parse_commands(const std::string& text,
                                    std::vector<std::string>* problems,
                                    const ButtonApplyFn& on_button) {
  std::vector<Command> out;
  size_t pos = 0;
  for (;;) {
    const size_t open = text.find("```aii", pos);
    if (open == std::string::npos) break;
    size_t body = text.find('\n', open);
    // The opening fence is the last thing in the text: there is no body and no
    // closing fence, so there is nothing to run. Reported rather than ignored,
    // because it is the same truncation as the case below and the log should
    // say so.
    if (body == std::string::npos) {
      note(problems, "unterminated ```aii block: the opening fence is the end of the reply");
      break;
    }
    ++body;
    const size_t close = text.find("```", body);
    // **No closing fence: refuse the whole block** (review finding 8). The
    // reply stopped in the middle of writing it -- a token limit, a dropped
    // connection -- and the lines that did arrive are as likely to be half a
    // line as a whole one. Executing them is acting on a sentence the model did
    // not finish saying, at bypassPermissions in the case of `spawn`. Dropped
    // here, dropped by `strip_aii_blocks`, and named in `problems` so that the
    // one place this is visible -- the log -- says what happened.
    if (close == std::string::npos) {
      const std::string tail = trim(text.substr(body));
      note(problems, "unterminated ```aii block, not executed: " +
                         std::to_string(tail.size()) + " bytes after the opening fence");
      break;
    }
    const std::string block = text.substr(body, close - body);
    pos = close + 3;

    size_t line_start = 0;
    while (line_start < block.size()) {
      size_t nl = block.find('\n', line_start);
      if (nl == std::string::npos) nl = block.size();
      const std::string line = trim(block.substr(line_start, nl - line_start));
      line_start = nl + 1;
      if (line.empty()) continue;

      Command c;
      const size_t sp = line.find(' ');
      c.verb = sp == std::string::npos ? line : line.substr(0, sp);
      std::string rest = sp == std::string::npos ? "" : trim(line.substr(sp + 1));
      // key=value pairs. A value may be "quoted" when it contains spaces --
      // needed once `button` arrived, whose label and tooltip are prose and
      // whose path may sit under Program Files. Unquoted, `task=` and `path=`
      // still run to the end of the line: both are usually last, both usually
      // contain spaces, and requiring quotes there would break every reply the
      // assistant has been taught to write.
      while (!rest.empty()) {
        const size_t eq = rest.find('=');
        if (eq == std::string::npos) break;
        const std::string key = trim(rest.substr(0, eq));
        std::string value;
        if (eq + 1 < rest.size() && rest[eq + 1] == '"') {
          const size_t quote = rest.find('"', eq + 2);
          value = rest.substr(eq + 2, quote == std::string::npos ? std::string::npos : quote - eq - 2);
          rest = quote == std::string::npos ? "" : trim(rest.substr(quote + 1));
        } else if (key == "task" || key == "path" || key == "say" || key == "text") {
          value = trim(rest.substr(eq + 1));
          rest.clear();
        } else {
          const size_t end = rest.find(' ', eq + 1);
          value = rest.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1);
          rest = end == std::string::npos ? "" : trim(rest.substr(end + 1));
        }
        if (key == "name") c.name = value;
        else if (key == "cwd") c.cwd = value;
        else if (key == "task") c.task = value;
        else if (key == "id") c.id = value;
        else if (key == "label") c.label = value;
        else if (key == "tip") c.tip = value;
        else if (key == "path") c.path = value;
        else if (key == "run") c.run = value;
        else if (key == "in") c.in = value;
        else if (key == "say") c.say = value;
        else if (key == "grade") c.grade = value;
        // M3.14. `value=` stays a single token rather than joining `task=`
        // and `path=` in running to the end of the line: every value this
        // format takes is one word ("haiku", "off", "45", "en,ja"), and the
        // end-of-line rule exists for fields that are prose. A value with a
        // space in it -- a themed avatar name -- still works quoted.
        else if (key == "key") c.key = value;
        else if (key == "value") c.value = value;
        else if (key == "confirm") c.confirm = value;
        else if (key == "text") c.text = value;
        else if (key == "model") c.model = value;
      }
      if (c.verb.empty()) continue;
      // App-owned verbs are applied here and dropped: see the header. A
      // refusal is not spoken and does not reach the transcript -- the button
      // is the agent's own housekeeping, and reading "I could not add that
      // button" aloud would spend a spoken sentence on something the user
      // never asked for. It is recorded for the log instead.
      if (c.verb == "button") {
        // M33. Exactly one of `path=`/`run=`: a button that does not say what
        // it does is not one to add, and one that says both is ambiguous in
        // a way that is cheaper to refuse here than to guess about at the
        // registry. Like every other refusal in this block, it is not spoken
        // -- see the paragraph above.
        if (c.path.empty() == c.run.empty()) {
          note(problems, "button '" + c.id + "' needs exactly one of path= or run=");
          continue;
        }
        if (on_button) on_button(c);
        continue;
      }
      // The `name` guard is a worker-verb guard: `spawn`, `pause` and `stop`
      // all address a worker by name and a nameless one is unrunnable. A bare
      // timer has nothing to name, so `schedule` is let through and validated
      // by its own handler, which can tell the user *why* it was refused.
      // M2b.5 adds `cancel`, which addresses a schedule by id and has no name
      // either. Both are validated by their own handler, which is what can tell
      // the user why it was refused; `spawn`, `pause` and `stop` are still
      // dropped without one, because a nameless worker verb is unrunnable.
      // M3.14 adds `setting`, which addresses a key rather than a worker and
      // so has no name either. Like the other two it is validated by its own
      // handler, because only that handler can say *why* -- "there is no
      // setting called that" and "that key does not take that value" are
      // different sentences and the user hears both.
      // M14 adds `remember` and `forget`, which address a memory by text or by
      // id and have no name. Same rule: their handler is the one that can say
      // "that is full" or "there is no memory numbered that".
      // `clearchat` takes no fields at all; it is applied by the session.
      if (c.verb == "schedule" || c.verb == "cancel" || c.verb == "setting" ||
          c.verb == "remember" || c.verb == "forget" || c.verb == "clearchat" ||
          !c.name.empty())
        out.push_back(std::move(c));
    }
  }
  return out;
}

std::string strip_aii_blocks(const std::string& text) {
  std::string out;
  size_t pos = 0;
  for (;;) {
    const size_t open = text.find("```aii", pos);
    if (open == std::string::npos) {
      out += text.substr(pos);
      break;
    }
    out += text.substr(pos, open - pos);
    const size_t close = text.find("```", open + 6);
    if (close == std::string::npos) break;  // unterminated: drop the rest
    pos = close + 3;
  }
  return trim(out);
}

}  // namespace aii
