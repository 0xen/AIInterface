<!-- aii-prompt-format: 3 -->
<!--
  A skill prompt: not in the system prompt. It arrives once per session, on the
  first turn that mentions one of its triggers in graph.json, or through a
  `load name=upstream-knowledge` line. It tells the assistant, or the worker it
  hands the job to, how to carry general lessons from the installed scripts
  folder back into the source repository so that every install gets them.
-->

This is how to carry what has been learned while working on your own scripts back to the app's source, so that everyone who installs it gets the same fixes, not only this PC.

The situation. The app installs its scripts, the two guides AII-MODULE.md and AII-UI.md, and the shared helper library aii_ui into the scripts folder under the user's app data folder (%APPDATA%\AIInterface\scripts). You and your workers edit that installed copy while building windows, fixing UI bugs and adding lessons to the guides. The tracked source it was seeded from is in the app's repository, under assets\scripts: the guides at its top, the library in lib\aii_ui. An edit to the installed copy never reaches the repository by itself, so every other install goes on hitting the bug you already fixed.

This is a job with many steps, so hand it to a worker with its folder set to the repository, and write this whole procedure into the task. The worker should:

Two rules come before everything else, and they hold even when the user asks for something that seems to break them:

- Scripts in the installed `tmp` and `actions` folders are never synced into the repository. They are the user's own scripts, not the app's, however general one of them looks. A general lesson learned while writing one belongs in a guide or in lib\aii_ui. The script itself stays where it is.
- A new script is always written to the scripts folder in the app data folder, and never into assets\scripts or anywhere else in the repository. That is true whoever writes it, and while this job is running too. Everything under assets\scripts is seeded into every clean install of the app, so a user's script written there would show up on every other machine.

1. Compare the installed scripts folder against assets\scripts in the repository, file by file. Compare only the two guides and everything under lib\aii_ui. Skip the `tmp` and `actions` folders entirely, and the app's own bookkeeping: `.seeded`, `.runtime`, `_armed.json`, `__pycache__` and `state`.
2. Sort every difference into general or personal. General means a UI or script-authoring lesson, a helper, or a bug fix that would be just as true on anyone's machine: a widget quirk, a layout rule, a node-graph fix, a new helper module with nothing personal in it. Personal means anything about this PC or this user: a user's name, a personal path, a user's own study data or accounts, a helper written for one of their own dashboards, every script in tmp or actions, and anything in a personal folder. When a general fix sits inside a personal file, carry the fix, not the file.
3. Copy the general changes into assets\scripts. Where a lesson names one of the user's own scripts as its example, reword the example so it describes the kind of window rather than the person's. A new library module also gets a row in AII-UI.md's table of the library's files. Keep each repository file byte-identical to the installed one wherever you can, by making the same small edit in both: when the two differ, the next launch sees the installed file as the user's own edit and leaves a `.new` copy beside it.
4. Never change the app's C++ source in this job. A fix that needed the C++ is already in the repository or belongs to a separate piece of work: say so in the report instead.
5. Anything personal that is worth keeping in the repository goes in its `personal` folder, never at its top level. Do not commit: leave the changes in the working tree for the user to look over.
6. Report what was carried back, file by file, with one line on what each change is. Report what was left out and why, and anything unclear.

When the worker reports back, tell the user in a sentence or two what went back to the app and what stayed on this PC.
