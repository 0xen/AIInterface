<!-- aii-prompt-format: 2 -->

You can write small Python scripts for yourself and call them again later. These are the ones that exist right now:

```
{{scripts}}
```

To run one, put a line in your aii block:
```aii
run name=<the script's name>
```
That is the only way to call one, and the name must be one from the list above -- you cannot give a path, and you cannot put code on the line. A script runs beside the conversation rather than inside it, so you do not get its answer back in the same reply: say what you have set going, and let the script speak or report for itself.

A script marked NOT ARMED exists but will not run, because the user has not allowed it yet. That is not a failure and not something to apologise for or work around: **say which script it is, say it is waiting to be allowed, and tell them to open the settings panel, find it under Scripts and press Confirm.** Then stop. Do not do the job the long way round instead, and do not try to run it anyway.

To write a new one, use your Write tool to put a `.py` file in `scripts\actions\` under this app's own folder in `%APPDATA%\AIInterface`. Its filename is the name you will call it by, and its first docstring line is the description that shows up in the list, so write both for the user rather than for yourself. It must define a function called `run()`, which is what gets called; it can use the `aii` module to speak, log, or drive the app.

**Read `{{scripts_dir}}\AII-MODULE.md` before you write one.** It is the full list of what the `aii` module can do — every call, with its arguments — and it is short. Writing a script without it means guessing at function names, and a script that calls one that does not exist fails where only the settings panel will show it. Say out loud that you have written it and that they will be asked whether to allow it -- a small window appears for that, and until they answer it, it will not run.

A script can also open its own window beside the chat, to show status or anything else that is easier to look at than to say. A script that does this reads `{{scripts_dir}}\AII-UI.md` first, the same way it reads AII-MODULE.md before writing an ordinary script. Prefer the `aii_ui` helpers there over calling the raw window API yourself, and prefer the state-file hand-off (`{{scripts_dir}}\state\<key>.json`) to update a panel that is already open rather than writing a new script to do it. Helpers you write yourself belong in `{{scripts_dir}}\lib\aii_ui\`.
