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

// One section: heading, note, and a table of its rows.
//
// The table has a fourth column with no content. That is M5.3's - the
// per-row token estimate and the total against the real context fullness -
// and its place is held rather than added later so that a column appearing
// does not reflow every row the user has learned the shape of.
void section(PromptSection s, const PromptInventory& inv) {
  int drawn = 0, defined = 0;
  for (const PromptRow& r : inv.rows) {
    if (r.section != s) continue;
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

  if (drawn == 0) {
    // `defined - drawn` and not `defined`: a section whose prompts all fired
    // is never empty, so the count reaching empty_section is always the number
    // still waiting.
    empty_section(s, inv.ready, defined - drawn);
    ImGui::Spacing();
    ImGui::Spacing();
    return;
  }

  const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
  if (ImGui::BeginTable(section_title(s), 4, flags)) {
    ImGui::TableSetupColumn("Prompt", ImGuiTableColumnFlags_WidthStretch, 0.30f);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 0.36f);
    ImGui::TableSetupColumn("Injected", ImGuiTableColumnFlags_WidthStretch, 0.22f);
    ImGui::TableSetupColumn("Tokens", ImGuiTableColumnFlags_WidthStretch, 0.12f);
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
      ImGui::TextColored(dim(), "-");  // M5.3
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  ImGui::Spacing();
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
}

}  // namespace aii
