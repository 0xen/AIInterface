#pragma once
// M5.2: the prompt inspector's body — the live, read-only list of what is in
// Claude's head right now.
//
// **Why this is its own file and not more of `inspector_window.cpp`.** That
// file's comment above `BeginChild("##inspector_body")` says the child is the
// seam M5.2-M5.4 fill and is the whole of what they touch there. Drawing this
// needs file-scope helpers — a duration formatter, a section table — and every
// one of them would have had to live outside that child. So the window keeps
// its one line and the list lives here, which also means M5.3's token column
// and M5.4's greyed-out rows never reopen the window file at all.
//
// It draws and returns. It owns no state: everything it shows arrives in the
// inventory, which the session published and the frame loop copied. That is
// not a style preference — `PromptStore` is filled on the load thread and
// `PromptInjector` is written on the turn thread, so a list that fetched its
// own data here would be reading them from the frame loop.
#include "core/prompt_store.h"

namespace aii {

// Draw the three sections plus the CLI's own irreducible context, inside
// whatever ImGui container is current. Call between BeginChild and EndChild.
void draw_prompt_list(const PromptInventory& inv);

}  // namespace aii
