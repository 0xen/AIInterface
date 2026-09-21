// The ```aii block, checked as a format rather than as a habit.
//
// Review finding 25: this parser reads untrusted model output and decides what
// the app does about it -- spawn a worker at bypassPermissions, change a
// setting, cancel a schedule, write a memory -- and until M15.1 it had no case
// anywhere. It had none because it lived in `worker_pool.cpp` beside a class
// that spawns processes, so a test of the format could not be linked without
// the whole app behind it. Moving it to `core/aii_block.cpp` is what makes this
// file possible; the cases below are the ones the prompts actually teach the
// model to write, plus the one the prompts cannot prevent.
//
// That last one is finding 8, and it is the reason this file exists at all: a
// reply cut off by a token limit used to have its half-written block parsed and
// dispatched, while `strip_aii_blocks` dropped the same text from the
// transcript. The app acted on a sentence the model never finished and the user
// was shown nothing. Both halves now refuse it, and the two must agree -- so
// every case here is checked through the parser *and* through the stripper.
#include <cstdio>
#include <string>
#include <vector>

#include "core/aii_block.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

// The parser as the app calls it, minus the button binding: problems collected,
// buttons counted rather than registered.
std::vector<aii::Command> parse(const std::string& text,
                                std::vector<std::string>* problems = nullptr,
                                int* buttons = nullptr) {
  return aii::parse_commands(text, problems, [buttons](const aii::Command&) {
    if (buttons) ++*buttons;
  });
}

std::string block(const std::string& lines) { return "```aii\n" + lines + "\n```"; }

}  // namespace

int main() {
  using aii::Command;
  using aii::strip_aii_blocks;

  // ---- 1. every verb the dispatcher knows ------------------------------
  //
  // The list is `VoiceSession::run_commands` plus the two verbs applied before
  // it: `button` inside the parser, and `cancel`, `setting`, `run`, `load`,
  // `remember` and `forget` in the session. If a verb is added to the dispatcher
  // and not to this list, nothing here fails -- which is exactly why the list is
  // written out rather than derived.
  std::printf("the verbs the dispatcher knows\n");
  {
    const std::vector<Command> c = parse(block(
        "spawn name=counter cwd=C:\\github task=count the files\n"
        "pause name=counter\n"
        "stop name=counter\n"
        "cancel id=7\n"
        "schedule in=10m say=the pasta is done\n"
        "setting key=llm.model value=haiku confirm=yes\n"
        "run name=lights\n"
        "load name=reviewer\n"
        "remember text=the user prefers short answers\n"
        "forget id=3"));
    check(c.size() == 10, "ten verbs in one block come back as ten commands");
    if (c.size() == 10) {
      check(c[0].verb == "spawn" && c[0].name == "counter" && c[0].cwd == "C:\\github" &&
                c[0].task == "count the files",
            "spawn keeps its name, its folder and its task");
      check(c[1].verb == "pause" && c[1].name == "counter", "pause addresses a worker by name");
      check(c[2].verb == "stop" && c[2].name == "counter", "stop addresses a worker by name");
      check(c[3].verb == "cancel" && c[3].id == "7", "cancel addresses a schedule by id");
      check(c[4].verb == "schedule" && c[4].in == "10m" && c[4].say == "the pasta is done",
            "schedule keeps its delay and the exact sentence it will speak");
      check(c[5].verb == "setting" && c[5].key == "llm.model" && c[5].value == "haiku" &&
                c[5].confirm == "yes",
            "**setting carries its confirm token**, which is the whole of how consent is asked");
      check(c[6].verb == "run" && c[6].name == "lights", "run names an armed action");
      check(c[7].verb == "load" && c[7].name == "reviewer", "load names a prompt in the store");
      check(c[8].verb == "remember" && c[8].text == "the user prefers short answers",
            "remember carries prose");
      check(c[9].verb == "forget" && c[9].id == "3", "forget addresses a memory by id");
    }
  }
  {
    // `button` is the one verb applied inside the parser and never returned:
    // it has no worker to dispatch to, and the caller supplies the handler
    // because ButtonRegistry opens folders through <shellapi.h> and cannot be
    // linked into a standard-library-only file.
    int buttons = 0;
    const std::vector<Command> c =
        parse(block("button id=repo label=\"Open the repo\" tip=\"where the code is\" "
                    "path=C:\\github\\AIInterface"),
              nullptr, &buttons);
    check(c.empty(), "button is applied, not returned: the caller gets nothing to dispatch");
    check(buttons == 1, "and the handler it was given saw it exactly once");
  }
  {
    // With no handler the line is still dropped rather than handed back. A
    // caller that did not ask to apply buttons has nothing it could do with one.
    const std::vector<Command> c =
        aii::parse_commands(block("button id=repo label=Repo path=C:\\github"), nullptr, {});
    check(c.empty(), "with no handler at all, a button line is dropped and not returned");
  }

  // ---- 2. quoting, and the fields that run to the end of the line -------
  std::printf("quoting and end-of-line values\n");
  {
    const std::vector<Command> c =
        parse(block("button id=docs label=\"Open the docs\" tip=\"two words here\" path=C:\\a b"),
              nullptr, nullptr);
    check(c.empty(), "(the button above is applied, so the assertions are on the next one)");
  }
  {
    const std::vector<Command> c = parse(block("spawn name=\"the counter\" task=count things"));
    check(c.size() == 1 && c[0].name == "the counter",
          "a quoted value keeps the spaces inside it and loses the quotes");
  }
  {
    const std::vector<Command> c = parse(block("spawn name=a task=look at the log and say what broke"));
    check(c.size() == 1 && c[0].task == "look at the log and say what broke",
          "**task= runs to the end of the line unquoted**: the prompts teach it last and prose");
  }
  {
    const std::vector<Command> c = parse(block("schedule in=5m say=the pasta is done, go and drain it"));
    check(c.size() == 1 && c[0].say == "the pasta is done, go and drain it",
          "say= runs to the end of the line too, commas and all");
  }
  {
    const std::vector<Command> c = parse(block("remember text=likes tea, not coffee, in the morning"));
    check(c.size() == 1 && c[0].text == "likes tea, not coffee, in the morning",
          "text= runs to the end of the line");
  }
  {
    int buttons = 0;
    std::vector<Command> got;
    aii::parse_commands(block("button id=pf path=C:\\Program Files\\Thing"), nullptr,
                        [&](const Command& c) { ++buttons; got.push_back(c); });
    check(buttons == 1 && got[0].path == "C:\\Program Files\\Thing",
          "path= runs to the end of the line, which is what makes Program Files work unquoted");
  }
  {
    // A single token stops at the space. This is why `value=` is not an
    // end-of-line field: the token after it has to be readable as a key.
    const std::vector<Command> c = parse(block("setting key=llm.model value=haiku confirm=yes"));
    check(c.size() == 1 && c[0].value == "haiku" && c[0].confirm == "yes",
          "an unquoted single token stops at the space, so the key after it is still read");
  }
  {
    const std::vector<Command> c = parse(block("setting key=avatar.name value=\"pixel cat\""));
    check(c.size() == 1 && c[0].value == "pixel cat",
          "a value with a space still works when it is quoted");
  }

  // ---- 3. the name guard ------------------------------------------------
  //
  // `spawn`, `pause` and `stop` address a worker by name and a nameless one is
  // unrunnable, so the parser drops them. Everything else names something the
  // parser does not know about -- a schedule id, a settings key, a memory -- and
  // is let through to a handler that can say *why* it was refused, out loud.
  std::printf("the name guard\n");
  {
    const std::vector<Command> c = parse(block("spawn task=do something\npause\nstop"));
    check(c.empty(), "**a worker verb with no name never reaches the dispatcher**");
  }
  {
    const std::vector<Command> c = parse(block(
        "schedule in=10m say=hello\ncancel id=4\nsetting key=a.b value=c\n"
        "remember text=hello\nforget id=1"));
    check(c.size() == 5,
          "schedule, cancel, setting, remember and forget pass without a name: their own "
          "handler is what can tell the user why");
  }
  {
    const std::vector<Command> c = parse(block("run\nload"));
    check(c.empty(),
          "run and load are *not* on that list, so a nameless one is dropped here (they name "
          "an action and a prompt, and both are addressed by name)");
  }
  {
    // An unknown verb is dropped when it has no name, and passed through when it
    // does -- the parser has no list of verbs, by design, and the dispatcher
    // ignores what it does not recognise. Recorded as it is, not as one might
    // wish it were.
    const std::vector<Command> c = parse(block("frobnicate wildly=yes\nfrobnicate name=x"));
    check(c.size() == 1 && c[0].verb == "frobnicate",
          "an unknown verb is dropped without a name; with one it is passed on and the "
          "dispatcher is what ignores it");
  }
  {
    const std::vector<Command> c = parse(block("\n   \n\nspawn name=a task=b\n   \n"));
    check(c.size() == 1, "blank and whitespace-only lines are skipped");
  }

  // ---- 4. a block inside prose, and two blocks in one reply -------------
  std::printf("blocks inside a reply\n");
  {
    const std::string reply =
        "I will start that now.\n\n" + block("spawn name=counter task=count the files") +
        "\n\nIt should take a minute.";
    const std::vector<Command> c = parse(reply);
    check(c.size() == 1 && c[0].name == "counter", "a block between two paragraphs is found");
    check(strip_aii_blocks(reply) == "I will start that now.\n\n\n\nIt should take a minute.",
          "and the transcript keeps the prose either side of it, and only that");
  }
  {
    const std::string reply = "First.\n" + block("spawn name=a task=one") + "\nThen.\n" +
                              block("spawn name=b task=two") + "\nDone.";
    const std::vector<Command> c = parse(reply);
    check(c.size() == 2 && c[0].name == "a" && c[1].name == "b",
          "two blocks in one reply both run, in the order they were written");
    const std::string shown = strip_aii_blocks(reply);
    check(shown.find("spawn") == std::string::npos && shown.find("First.") != std::string::npos &&
              shown.find("Then.") != std::string::npos && shown.find("Done.") != std::string::npos,
          "and neither of them reaches the transcript, while all three sentences do");
  }
  {
    const std::string reply = "Nothing to do here at all.";
    check(parse(reply).empty(), "a reply with no block parses to nothing");
    check(strip_aii_blocks(reply) == reply, "and is shown exactly as it was written");
  }

  // ---- 5. the unterminated block (review finding 8) ---------------------
  //
  // The failure this guards against is not hypothetical prose: a `max_tokens`
  // stop lands mid-block, and the verb that suffers most is `spawn`, which
  // starts a child at bypassPermissions in a folder the half-line may not have
  // finished naming.
  std::printf("an unterminated block is refused\n");
  {
    const std::string reply =
        "Starting that now.\n\n```aii\nspawn name=counter cwd=C:\\github task=count the fi";
    std::vector<std::string> problems;
    const std::vector<Command> c = parse(reply, &problems);
    check(c.empty(), "**a block with no closing fence is not parsed and not dispatched**");
    check(problems.size() == 1, "and it is reported, once, for the caller to log");
    check(!problems.empty() && problems[0].find("unterminated") != std::string::npos,
          "the report says what was wrong in a word the log can carry");
    check(strip_aii_blocks(reply) == "Starting that now.",
          "the stripper agrees: what is not executed is not shown either");
  }
  {
    // The opening fence as the very last thing in the reply: no body, no close.
    const std::string reply = "Here goes.\n```aii";
    std::vector<std::string> problems;
    check(parse(reply, &problems).empty(), "an opening fence with nothing after it runs nothing");
    check(problems.size() == 1, "and is reported rather than passed over in silence");
    check(strip_aii_blocks(reply) == "Here goes.", "and is not shown");
  }
  {
    // A finished block followed by a truncated one: the first still runs. The
    // refusal is per block, not per reply -- a reply that did one thing
    // correctly before being cut off did that one thing.
    const std::string reply = block("spawn name=a task=one") + "\nAnd also:\n```aii\nspawn name=b ta";
    std::vector<std::string> problems;
    const std::vector<Command> c = parse(reply, &problems);
    check(c.size() == 1 && c[0].name == "a",
          "a complete block before a truncated one still runs; the truncated one does not");
    check(problems.size() == 1, "one problem, for the one block that was cut off");
  }
  {
    // No out-parameter is a supported call: the one-argument wrapper in
    // `worker_pool.cpp` uses it. It must refuse just the same, silently.
    const std::string reply = "```aii\nspawn name=counter task=go";
    check(aii::parse_commands(reply, nullptr, {}).empty(),
          "with nowhere to report it, an unterminated block is still refused");
  }
  {
    // A well-formed block is not reported as a problem. Worth an assertion
    // because a reporting channel that cries wolf gets ignored.
    std::vector<std::string> problems;
    parse(block("spawn name=a task=b"), &problems);
    check(problems.empty(), "a well-formed block reports no problems at all");
  }

  std::printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
  return failures == 0 ? 0 : 1;
}
