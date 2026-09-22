// M28. UiBridge is the mutex-guarded store script windows live in; this
// checks its bounds, its latch and its id composition with no ImGui and no
// Python anywhere near it. See docs/design-script-ui.md, section 7, for the
// case list this follows.
#include "core/ui_bridge.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

aii::UiWindowSpec spec(const std::string& key, const std::string& title = "t") {
  aii::UiWindowSpec s;
  s.key = key;
  s.title = title;
  s.w = 360;
  s.h = 240;
  return s;
}

void test_key_validation() {
  check(aii::ui_valid_key("a"), "single character key is valid");
  check(aii::ui_valid_key("build.status-1_2"), "letters, digits, dot, dash, underscore are valid");
  check(!aii::ui_valid_key(""), "empty key is invalid");
  check(!aii::ui_valid_key("has space"), "a space is invalid");
  check(!aii::ui_valid_key("has/slash"), "a slash is invalid");
  const std::string long_key(aii::kUiKeyMax, 'a');
  check(aii::ui_valid_key(long_key), "a key exactly at the cap is valid");
  const std::string too_long(aii::kUiKeyMax + 1, 'a');
  check(!aii::ui_valid_key(too_long), "a key one byte over the cap is invalid");
}

void test_open_refuses_malformed_key() {
  aii::UiBridge bridge;
  std::string error;
  const bool ok = bridge.open(spec("bad key"), &error);
  check(!ok, "open() refuses a malformed key");
  check(!error.empty(), "and gives a one-line reason");
}

void test_open_refuses_a_seventh_window() {
  aii::UiBridge bridge;
  for (std::size_t i = 0; i < aii::kUiWindowsMax; ++i) {
    std::string error;
    const bool ok = bridge.open(spec("w" + std::to_string(i)), &error);
    check(ok, "opening up to the cap succeeds");
    check(error.empty(), "and reports nothing");
  }
  std::string error;
  const bool ok = bridge.open(spec("one-too-many"), &error);
  check(!ok, "a seventh window is refused");
  check(!error.empty(), "with a one-line reason");
  check(bridge.keys().size() == aii::kUiWindowsMax, "the refused window was not added");
}

void test_reopen_updates_spec_and_clears_flags_but_keeps_results() {
  aii::UiBridge bridge;
  check(bridge.open(spec("k", "first title")), "first open succeeds");

  // The script closed it, the app closed it on the user's behalf too, and
  // there is a result sitting from before either happened.
  aii::UiResult r;
  r.id = "btn";
  r.clicked = true;
  bridge.set_results("k", {r});
  bridge.mark_closed("k");
  bridge.close("k");
  check(!bridge.is_open("k"), "is_open is false once both flags are set");

  check(bridge.open(spec("k", "second title")), "reopening the same key succeeds");
  check(bridge.is_open("k"), "reopening clears both closed flags");

  std::vector<aii::UiWindowSpec> open;
  std::vector<std::string> closing;
  bridge.snapshot(&open, &closing);
  bool found_new_title = false;
  for (const auto& s : open) {
    if (s.key == "k") found_new_title = (s.title == "second title");
  }
  check(found_new_title, "reopening updates the spec (title)");

  const auto results = bridge.take_results("k");
  check(results.size() == 1 && results[0].id == "btn" && results[0].clicked,
        "reopening keeps results that were there before");
}

void test_submit_unknown_key_fails() {
  aii::UiBridge bridge;
  aii::UiFrame frame;
  const bool ok = bridge.submit("nope", frame);
  check(!ok, "submit() to an unknown key returns false");
}

void test_submit_drops_commands_past_the_cap() {
  aii::UiBridge bridge;
  bridge.open(spec("k"));
  aii::UiFrame frame;
  frame.cmds.resize(aii::kUiCommandsMax + 10);
  std::string error;
  const bool ok = bridge.submit("k", frame, &error);
  check(ok, "submit() still succeeds when commands are dropped");
  check(!error.empty(), "and reports that something was cut");
  aii::UiFrame out;
  bridge.frame_for("k", &out);
  check(out.cmds.size() == aii::kUiCommandsMax, "commands past the cap are dropped");
}

void test_submit_truncates_strings_at_a_utf8_boundary() {
  aii::UiBridge bridge;
  bridge.open(spec("k"));
  aii::UiCommand cmd;
  // Three-byte UTF-8 characters (the yen sign area) repeated past the cap, so
  // a naive byte cut would land mid-character.
  std::string long_label;
  while (long_label.size() < aii::kUiTextMax + 30) long_label += "\xE3\x81\x82";  // U+3042
  cmd.label = long_label;
  cmd.text = long_label;
  aii::UiFrame frame;
  frame.cmds.push_back(cmd);
  std::string error;
  const bool ok = bridge.submit("k", frame, &error);
  check(ok, "submit() succeeds while truncating");
  check(!error.empty(), "and reports the truncation");
  aii::UiFrame out;
  bridge.frame_for("k", &out);
  check(out.cmds[0].label.size() <= aii::kUiTextMax, "label is at or under the byte cap");
  check(out.cmds[0].text.size() <= aii::kUiTextMax, "text is at or under the byte cap");
  // Every remaining byte must belong to a whole 3-byte sequence: size % 3 == 0
  // for a string made entirely of the same 3-byte character.
  check(out.cmds[0].label.size() % 3 == 0, "the label was not cut inside a UTF-8 sequence");
}

void test_submit_caps_items_and_values() {
  aii::UiBridge bridge;
  bridge.open(spec("k"));
  aii::UiCommand cmd;
  cmd.op = aii::UiOp::Combo;
  cmd.items.assign(aii::kUiItemsMax + 5, "x");
  cmd.values.assign(aii::kUiValuesMax + 5, 1.0f);
  aii::UiFrame frame;
  frame.cmds.push_back(cmd);
  std::string error;
  const bool ok = bridge.submit("k", frame, &error);
  check(ok, "submit() succeeds while capping lists");
  check(!error.empty(), "and reports it");
  aii::UiFrame out;
  bridge.frame_for("k", &out);
  check(out.cmds[0].items.size() == aii::kUiItemsMax, "items are capped");
  check(out.cmds[0].values.size() == aii::kUiValuesMax, "values are capped");
}

void test_submit_reports_nothing_when_nothing_cut() {
  aii::UiBridge bridge;
  bridge.open(spec("k"));
  aii::UiCommand cmd;
  cmd.label = "fine";
  aii::UiFrame frame;
  frame.cmds.push_back(cmd);
  std::string error;
  const bool ok = bridge.submit("k", frame, &error);
  check(ok, "submit() of a small frame succeeds");
  check(error.empty(), "and reports nothing when nothing was cut");
}

void test_generation_increments() {
  aii::UiBridge bridge;
  bridge.open(spec("k"));
  aii::UiFrame frame;
  bridge.submit("k", frame);
  const auto g1 = bridge.generation_of("k");
  bridge.submit("k", frame);
  const auto g2 = bridge.generation_of("k");
  check(g2 > g1, "each submit() gets a later generation");
}

void test_results_latch_and_clear_but_values_stay() {
  aii::UiBridge bridge;
  bridge.open(spec("k"));

  aii::UiResult r;
  r.id = "btn";
  r.clicked = true;
  r.f[0] = 1.5f;
  bridge.set_results("k", {r});

  // A second render frame where the button was not clicked again: the latch
  // must survive the OR-merge.
  aii::UiResult r2;
  r2.id = "btn";
  r2.clicked = false;
  r2.f[0] = 1.5f;
  bridge.set_results("k", {r2});

  auto results = bridge.take_results("k");
  check(results.size() == 1, "one result stored for one id");
  check(results[0].clicked, "clicked survived two set_results calls (OR-latch)");
  check(results[0].f[0] == 1.5f, "the value came through");

  auto after = bridge.take_results("k");
  check(after.size() == 1, "the result entry itself is not removed by take_results");
  check(!after[0].clicked, "clicked is cleared once taken");
  check(after[0].f[0] == 1.5f, "but the value stays");
}

void test_mark_closed_flips_is_open_without_removing() {
  aii::UiBridge bridge;
  bridge.open(spec("k"));
  check(bridge.is_open("k"), "freshly opened window is open");
  bridge.mark_closed("k");
  check(!bridge.is_open("k"), "mark_closed flips is_open to false");
  bool found = false;
  for (const auto& key : bridge.keys()) found = found || key == "k";
  check(found, "but the entry is still there");
}

void test_close_and_remove() {
  aii::UiBridge bridge;
  bridge.open(spec("k"));
  bridge.close("k");
  std::vector<aii::UiWindowSpec> open;
  std::vector<std::string> closing;
  bridge.snapshot(&open, &closing);
  bool in_closing = false;
  for (const auto& key : closing) in_closing = in_closing || key == "k";
  check(in_closing, "close() puts the key in snapshot()'s closing list");
  bool in_open = false;
  for (const auto& s : open) in_open = in_open || s.key == "k";
  check(!in_open, "and it is not in the open list");

  bridge.remove("k");
  bool still_there = false;
  for (const auto& key : bridge.keys()) still_there = still_there || key == "k";
  check(!still_there, "remove() forgets the entry");
}

void test_ui_compose_id() {
  check(aii::ui_compose_id({}, "label") == "label", "an empty stack is just the label");
  check(aii::ui_compose_id({"a"}, "label") == "a/label", "one pushed id joins with '/'");
  check(aii::ui_compose_id({"a", "b"}, "label") == "a/b/label", "several pushed ids all join");
}

void test_snapshot_excludes_user_closed_from_open() {
  aii::UiBridge bridge;
  bridge.open(spec("k"));
  bridge.mark_closed("k");
  std::vector<aii::UiWindowSpec> open;
  std::vector<std::string> closing;
  bridge.snapshot(&open, &closing);
  bool in_open = false;
  for (const auto& s : open) in_open = in_open || s.key == "k";
  check(!in_open, "a user-closed window is not in snapshot()'s open list");
  bool in_closing = false;
  for (const auto& key : closing) in_closing = in_closing || key == "k";
  check(!in_closing, "and it is not in closing either -- the script never asked to close it");
}

void test_epoch_moves_on_every_open() {
  std::printf("\n-- takeover: the epoch moves on every open ---------------------\n");
  aii::UiBridge b;
  aii::UiWindowSpec spec;
  spec.key = "k";
  spec.title = "one";
  check(b.epoch_of("k") == 0, "unknown key has epoch 0");
  b.open(spec);
  check(b.epoch_of("k") == 1, "first open gives epoch 1");
  spec.title = "two";
  b.open(spec);
  check(b.epoch_of("k") == 2, "a second open of the same key moves the epoch on");
  aii::UiFrame f;
  b.submit("k", f);
  check(b.epoch_of("k") == 2, "submitting does not move the epoch");
  b.mark_closed("k");
  b.open(spec);
  check(b.epoch_of("k") == 3, "reopening after the user closed it moves it on too");
}

}  // namespace

int main() {
  std::printf("ui_bridge_test\n");
  test_key_validation();
  test_open_refuses_malformed_key();
  test_open_refuses_a_seventh_window();
  test_reopen_updates_spec_and_clears_flags_but_keeps_results();
  test_submit_unknown_key_fails();
  test_submit_drops_commands_past_the_cap();
  test_submit_truncates_strings_at_a_utf8_boundary();
  test_submit_caps_items_and_values();
  test_submit_reports_nothing_when_nothing_cut();
  test_generation_increments();
  test_results_latch_and_clear_but_values_stay();
  test_mark_closed_flips_is_open_without_removing();
  test_close_and_remove();
  test_ui_compose_id();
  test_snapshot_excludes_user_closed_from_open();
  test_epoch_moves_on_every_open();
  std::printf(failures ? "\n%d failed\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
