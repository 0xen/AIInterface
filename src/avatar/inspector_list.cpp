#include "inspector_list.h"

#include "imgui.h"

#include <cstdio>
#include <string>
#include <vector>

#include "imgui_layer.h"

namespace aii {
namespace {

// The palette is the window's, one shade apart per role: a heading, a row's
// name, and the quieter text of everything that describes the row rather than
// being it.
ImVec4 heading() { return ui_color(0.91f, 0.92f, 0.94f); }
ImVec4 body() { return ui_color(0.80f, 0.83f, 0.88f); }
ImVec4 quiet() { return ui_color(0.58f, 0.61f, 0.68f); }
ImVec4 dim() { return ui_color(0.46f, 0.49f, 0.56f); }
// The one colour that is not a shade of the others, for the one thing on this
// window that is not a statement about Claude's head but about this app
// failing to fill it.
ImVec4 warn() { return ui_color(0.88f, 0.62f, 0.38f); }

// How long ago, in the coarsest unit that still says something. Seconds below
// a minute, then minutes, then hours and minutes — a prompt injected 73
// minutes ago is "1 h 13 m ago" and not "4380 s ago", which is a number nobody
// reads as a duration.
//
// **It is deliberately a relative time and not a clock time.** "12:04:31"
// answers a question nobody asked (the session is the unit here, not the day)
// and it is also static: a relative age visibly ticks, which is the cheapest
// possible evidence that this window is showing a live list rather than one
// built at startup and never touched again.
std::string ago(double seconds) {
  if (seconds < 1.0) return "just now";
  char buf[64];
  const long long s = static_cast<long long>(seconds);
  if (s < 60) std::snprintf(buf, sizeof buf, "%llds ago", s);
  else if (s < 3600) std::snprintf(buf, sizeof buf, "%lldm %llds ago", s / 60, s % 60);
  else std::snprintf(buf, sizeof buf, "%lldh %lldm ago", s / 3600, (s % 3600) / 60);
  return buf;
}

// The "when it was injected" cell.
//
// Two shapes, because there are two ways a prompt gets into this session and
// the difference matters to the reader. A global prompt *is* the launch
// argument: it was there before the first token and there is no moment to
// name, so it says "session start" and ages from the start of the session. A
// project or skill prompt arrived on one particular turn, which is a moment,
// so it says when.
std::string when_text(const PromptRow& r, double uptime) {
  if (!r.injected) return "not injected";
  const std::string age = ago(uptime - r.at);
  return r.at_session_start ? "session start, " + age : "mid-session, " + age;
}

const char* section_title(PromptSection s) {
  switch (s) {
    case PromptSection::Cli: return "From the Claude Code CLI";
    case PromptSection::Global: return "Global";
    case PromptSection::Project: return "Project (loaded)";
    case PromptSection::Skill: return "Skills";
  }
  return "";
}

// One line under each heading saying what the section *is*, because three bare
// nouns do not tell a first-time reader which of them they can change.
const char* section_note(PromptSection s) {
  switch (s) {
    case PromptSection::Cli:
      return "Context the CLI supplies by itself. No flag of ours removes it and nothing "
             "here edits it - it is listed so this window is not a list of only the half "
             "we control.";
    case PromptSection::Global:
      return "Composed into --system-prompt when the session started, in this order. "
             "Changing one takes effect on the next launch.";
    case PromptSection::Project:
      return "Injected into a single turn, the first turn that mentions them, and never "
             "sent again this session.";
    case PromptSection::Skill:
      return "The same, for skill prompts.";
  }
  return "";
}

// What an empty section says, and it matters that this is three sentences and
// not one.
//
// Project and Skills are empty on a normal run: the shipped `graph.json` has
// no project or skill nodes, and the editor that would have authored one was
// removed, so "nothing here" is this app's ordinary state rather than a
// failure. An empty section that said only "none" would read as breakage, and
// the user would go looking for the bug. So the text names the reason and the
// one way to change it.
void empty_section(PromptSection s, bool ready, int defined) {
  ImGui::PushStyleColor(ImGuiCol_Text, dim());
  if (!ready) {
    // Distinct from "nothing here" on purpose: the store is read on the load
    // thread, several seconds after the window can first be opened, and
    // "none" during that window would be a lie with a short shelf life.
    ImGui::TextWrapped("Not read yet - the prompt store is loaded with the engines.");
  } else if (defined > 0) {
    ImGui::TextWrapped(
        "%d defined, none mentioned yet this session. They are injected on the first turn "
        "that mentions them.",
        defined);
  } else if (s == PromptSection::Project || s == PromptSection::Skill) {
    ImGui::TextWrapped(
        "None defined. This is the normal state: nothing in the app authors these yet, so "
        "the store ships without any. They are added by hand, in graph.json under "
        "%%APPDATA%%\\AIInterface\\prompts.");
  } else {
    ImGui::TextWrapped("None.");
  }
  ImGui::PopStyleColor();
}

// M5.3 --- how an estimate is allowed to look.
//
// **It must not look like a count.** The number behind it came out of a ratio
// (~3.6 characters per token in English, ~1.6 in Japanese; see
// `estimate_tokens`), and there is no tokenizer in this process to check it
// against. "1,183" would be a claim this app cannot support, and the user
// would then reasonably read the footer's comparison as an exact shortfall.
// So every figure carries a `~`, and it is rounded hard enough that nobody
// could mistake it for a measurement: to the nearest ten below a thousand, and
// to one decimal place in thousands above it.
std::string tokens_text(int n) {
  char buf[32];
  if (n >= 1000) std::snprintf(buf, sizeof buf, "~%.1fk", n / 1000.0);
  else std::snprintf(buf, sizeof buf, "~%d", ((n + 5) / 10) * 10);
  return buf;
}

// The same, for a number that came from the CLI rather than from us. No `~`
// below a thousand -- these are counted, not estimated -- but still rounded in
// thousands, because a context window is not interesting to the last token.
std::string real_tokens_text(long long n) {
  char buf[32];
  if (n >= 1000000) std::snprintf(buf, sizeof buf, "%gM", static_cast<double>(n) / 1000000.0);
  else if (n >= 10000) std::snprintf(buf, sizeof buf, "%.0fk", n / 1000.0);
  else if (n >= 1000) std::snprintf(buf, sizeof buf, "%.1fk", n / 1000.0);
  else std::snprintf(buf, sizeof buf, "%lld", n);
  return buf;
}

// A fullness percentage. One decimal below ten per cent, because a fresh
// session sits at a fraction of one and "0%" would read as "nothing is in
// there", which is the opposite of what the next sentence goes on to say.
std::string percent_text(double fraction) {
  char buf[32];
  const double pct = fraction * 100.0;
  std::snprintf(buf, sizeof buf, pct < 10.0 ? "%.1f%%" : "%.0f%%", pct);
  return buf;
}

// The trigger words, as the reason a row has not fired.
std::string triggers_text(const std::vector<std::string>& t) {
  if (t.empty())
    // Not a misconfiguration: a node with no triggers is still reachable
    // through M3.4's `load name=`, which resolves against ids and titles too.
    // Saying so is the difference between "broken" and "loaded another way".
    return "no trigger words - only by name";
  std::string s;
  for (const std::string& w : t) {
    if (!s.empty()) s += ", ";
    s += '"' + w + '"';
  }
  return s;
}

const ImGuiTableFlags kTableFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingStretchProp |
                                    ImGuiTableFlags_PadOuterX;

// M5.4: the prompts that exist and have not fired.
//
// **A separate table, under its own heading, entirely in the dimmest colour
// the window has.** The one thing this must never do is let a glance read an
// available prompt as a loaded one -- that would make the window lie about the
// single thing it exists to be trusted on. So the separation is structural and
// not only a shade: a different table, a column no loaded row has ("Trigger
// words"), and a token cell that says "if loaded" in words. Three independent
// signals, because colour alone fails the glance test, and fails it silently
// for anyone who cannot see the difference.
//
// On a normal run this draws nothing: the shipped `graph.json` has no project
// or skill nodes, so there is nothing available either, and the empty states
// above are the whole of what that run shows.
void available_rows(PromptSection s, const PromptInventory& inv) {
  int n = 0;
  for (const PromptRow& r : inv.rows)
    if (r.section == s && !r.injected && !r.failed) ++n;
  if (n == 0) return;

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, dim());
  ImGui::TextWrapped("Available, not loaded - not in Claude's head. Listed so the words that "
                     "would load them are visible before they are needed.");
  ImGui::PopStyleColor();

  // Every cell in this table is dim(), its headers included, so the block
  // reads as one greyed-out thing rather than as live rows with a grey tint.
  ImGui::PushStyleColor(ImGuiCol_Text, dim());
  const std::string id = std::string("##available_") + section_title(s);
  if (ImGui::BeginTable(id.c_str(), 4, kTableFlags)) {
    ImGui::TableSetupColumn("Prompt", ImGuiTableColumnFlags_WidthStretch, 0.26f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.28f);
    ImGui::TableSetupColumn("Trigger words", ImGuiTableColumnFlags_WidthStretch, 0.30f);
    ImGui::TableSetupColumn("Tokens", ImGuiTableColumnFlags_WidthStretch, 0.16f);
    ImGui::TableHeadersRow();
    for (const PromptRow& r : inv.rows) {
      if (r.section != s || r.injected || r.failed) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", r.title.c_str());
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", r.source.c_str());
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", triggers_text(r.triggers).c_str());
      ImGui::TableNextColumn();
      // "if loaded", in words, so this number cannot be swept into the total
      // by a reader skimming the column.
      if (r.est_tokens >= 0) ImGui::TextWrapped("%s if loaded", tokens_text(r.est_tokens).c_str());
      else ImGui::TextUnformatted("-");
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleColor();
}

// Prompts that are declared and could not be read. Its own block, above both
// tables, because it belongs in neither: it is not loaded, and it is not
// "available" either -- no trigger word will ever load it. Listing one of
// these as an ordinary row would be a worse lie than the omission this
// replaces, which is why it is three lines of its own rather than a flag on a
// row somewhere in the table.
void failed_rows(PromptSection s, const PromptInventory& inv) {
  int n = 0;
  for (const PromptRow& r : inv.rows)
    if (r.section == s && r.failed) ++n;
  if (n == 0) return;

  ImGui::PushStyleColor(ImGuiCol_Text, warn());
  ImGui::TextWrapped("Declared, but could not be read - none of this reached Claude:");
  for (const PromptRow& r : inv.rows) {
    if (r.section != s || !r.failed) continue;
    ImGui::Bullet();
    ImGui::TextWrapped("%s (%s)", r.title.c_str(), r.source.c_str());
  }
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

// One section: heading, note, the table of what is loaded, and beneath it
// (M5.4) what is available and is not.
void section(PromptSection s, const PromptInventory& inv) {
  int drawn = 0, defined = 0;
  for (const PromptRow& r : inv.rows) {
    // A failed row is neither defined-and-waiting nor drawn: it has its own
    // block, and counting it here would make the empty state say "1 defined,
    // none mentioned yet", which is a promise it will arrive later.
    if (r.section != s || r.failed) continue;
    ++defined;
    if (r.injected) ++drawn;
  }

  ImGui::PushStyleColor(ImGuiCol_Text, heading());
  ImGui::SeparatorText(section_title(s));
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Text, quiet());
  ImGui::TextWrapped("%s", section_note(s));
  ImGui::PopStyleColor();
  ImGui::Spacing();

  // Above everything, including the empty states: if a section is empty
  // *because* its prompts could not be read, "None." on its own would be the
  // lie this block exists to prevent.
  failed_rows(s, inv);

  if (drawn == 0) {
    // M5.2's three empty states, unchanged and still first. M5.4's rows go
    // *under* this text rather than in place of it: "3 defined, none mentioned
    // yet this session" is the sentence that explains the greyed-out table,
    // and a list of available prompts with no such sentence above it would
    // leave the reader to work out for themselves why none of them are in
    // Claude.
    //
    // `defined - drawn` and not `defined`: a section whose prompts all fired
    // is never empty, so the count reaching empty_section is always the number
    // still waiting.
    empty_section(s, inv.ready, defined - drawn);
    available_rows(s, inv);
    ImGui::Spacing();
    ImGui::Spacing();
    return;
  }

  if (ImGui::BeginTable(section_title(s), 4, kTableFlags)) {
    ImGui::TableSetupColumn("Prompt", ImGuiTableColumnFlags_WidthStretch, 0.30f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.36f);
    ImGui::TableSetupColumn("Injected", ImGuiTableColumnFlags_WidthStretch, 0.22f);
    // The header says "est." and every cell carries a "~", because a column
    // headed "Tokens" is read as a count whatever a footnote says elsewhere.
    ImGui::TableSetupColumn("Tokens (est.)", ImGuiTableColumnFlags_WidthStretch, 0.12f);
    ImGui::PushStyleColor(ImGuiCol_Text, quiet());
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();

    for (const PromptRow& r : inv.rows) {
      if (r.section != s || !r.injected) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextColored(body(), "%s", r.title.c_str());
      ImGui::TableNextColumn();
      // A CLI row's "source" is not a file and must not look like one: saying
      // "not managed here" in the column the user reads for "where do I edit
      // this" is the whole point of listing these rows at all.
      if (r.section == PromptSection::Cli)
        ImGui::TextColored(dim(), "%s - %s", r.source.c_str(), kNotManagedHere);
      else
        ImGui::TextColored(quiet(), "%s", r.source.c_str());
      ImGui::TableNextColumn();
      ImGui::TextColored(quiet(), "%s", when_text(r, inv.uptime).c_str());
      ImGui::TableNextColumn();
      // M5.3. `-1` is a CLI row: that text exists, reaches the model and costs
      // real tokens, and this app has never seen a byte of it. "unknown" is
      // the only honest cell, and it is also the first hint of what the footer
      // goes on to say outright.
      if (r.est_tokens >= 0) ImGui::TextColored(quiet(), "%s", tokens_text(r.est_tokens).c_str());
      else ImGui::TextColored(dim(), "unknown");
    }
    ImGui::EndTable();
  }
  available_rows(s, inv);
  ImGui::Spacing();
  ImGui::Spacing();
}

// The footer: our estimate, the real fullness, and the distance between them.
//
// **That distance is the point of this window and the hardest thing on it to
// word.** Every number above is a guess about text this app wrote. `ctx` is
// measured: the CLI reports what it actually handed the model on the last
// turn. The two are not competing attempts at one quantity, so the difference
// between them is not an error bar -- it is the size of everything in Claude's
// head that never passed through here: the CLI's own context (the first
// section, listed and unmeasurable), the conversation so far, and every file,
// command and search result the session has pulled in.
//
// So the footer never says "off by", "missing" or "discrepancy". It names the
// two numbers as the different things they are, and then says what the gap
// between them consists of, in that order. A reader who finishes this and goes
// looking for a bug has been told the wrong thing.
void footer(const PromptInventory& inv) {
  int total = 0, unknown = 0;
  for (const PromptRow& r : inv.rows) {
    if (!r.injected) continue;  // an available prompt has cost nothing yet
    if (r.est_tokens >= 0) total += r.est_tokens;
    else ++unknown;
  }

  ImGui::PushStyleColor(ImGuiCol_Text, heading());
  ImGui::SeparatorText("Totals");
  ImGui::PopStyleColor();

  ImGui::PushStyleColor(ImGuiCol_Text, body());
  ImGui::TextWrapped("%s tokens, estimated, for the prompts listed above.",
                     tokens_text(total).c_str());
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Text, quiet());
  ImGui::TextWrapped(
      "Estimated from character counts - about 3.6 characters per token in English, 1.6 in "
      "Japanese - because there is no tokenizer in this app. It is the right order of "
      "magnitude, not a figure to reconcile.");
  if (unknown > 0)
    ImGui::TextWrapped(
        "%d row%s above cannot be estimated at all: the CLI's own context is text this app "
        "never sees.",
        unknown, unknown == 1 ? "" : "s");
  ImGui::PopStyleColor();
  ImGui::Spacing();

  if (inv.ctx < 0.0) {
    ImGui::PushStyleColor(ImGuiCol_Text, dim());
    ImGui::TextWrapped(
        "How full Claude's context actually is will appear here once a turn has been "
        "answered - the CLI reports it, we do not measure it.");
    ImGui::PopStyleColor();
    return;
  }

  const long long window = inv.ctx_window;
  const long long used = window > 0 ? static_cast<long long>(inv.ctx * window + 0.5) : -1;

  ImGui::PushStyleColor(ImGuiCol_Text, body());
  if (used >= 0)
    ImGui::TextWrapped("Claude's context is %s full: about %s tokens of a %s window.",
                       percent_text(inv.ctx).c_str(), real_tokens_text(used).c_str(),
                       real_tokens_text(window).c_str());
  else
    ImGui::TextWrapped("Claude's context is %s full.", percent_text(inv.ctx).c_str());
  ImGui::PopStyleColor();

  ImGui::PushStyleColor(ImGuiCol_Text, quiet());
  if (used > total) {
    ImGui::TextWrapped(
        "The prompts on this page are roughly %s of that. The other %s or so is everything "
        "this window cannot show you: the CLI's own context above, the conversation so far, "
        "and every file, command and search result this session has read. That gap is the "
        "measure of how much of Claude's head we do not put there, not a shortfall in the "
        "list.",
        tokens_text(total).c_str(), real_tokens_text(used - total).c_str());
  } else {
    // Reachable early, when the estimate's own slack is wider than the little
    // that has actually been sent. Saying so beats drawing a negative gap and
    // inviting the reader to explain it.
    ImGui::TextWrapped(
        "That is close to the estimate above, which is as near as these two numbers come: "
        "almost nothing has reached the session yet beyond the prompts themselves.");
  }
  ImGui::PopStyleColor();
}

}  // namespace

void draw_prompt_list(const PromptInventory& inv) {
  ImGui::Spacing();
  // The CLI's own context comes first, ahead of the prompts this app wrote.
  //
  // **Where the "not managed here" rows live, and why they are a section of
  // their own.** Folding them into Global would put them in the same list as
  // `system/voice.md` and imply they came from the store and can be turned off
  // by editing it. They cannot: M3.5 measured a live model and found a harness
  // preamble, cwd, git status, platform, model id, token budget, the date and
  // the user's email address still arriving after every flag we have was
  // applied. A fourth section says what Global cannot - that this text has a
  // different owner.
  //
  // It is first rather than last because it reaches Claude first, and because
  // the one thing this window must not do is let the user believe our three
  // sections are the whole of what Claude knows. A list of things you cannot
  // change, parked at the bottom of a scrolling page, is a footnote; at the
  // top it is part of the answer.
  section(PromptSection::Cli, inv);
  section(PromptSection::Global, inv);
  section(PromptSection::Project, inv);
  section(PromptSection::Skill, inv);
  // The footer is last because it is a conclusion: it only means anything
  // after the reader has seen what is being totalled.
  footer(inv);
}

}  // namespace aii
