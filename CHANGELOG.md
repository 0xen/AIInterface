# Changelog

What changed in this app, newest first, written for the person using it and for the
assistant inside it. The assistant reads this when asked what is new, so entries are short,
plain and free of file paths.
Every milestone that merges adds an entry here in the same commit.

## 2026-09-23

### Added
- **The assistant can clear the chat.** Ask it to clear the chat or the log and the panel
  empties, while the conversation itself carries on and nothing it knows is forgotten.
- **A toolbar button can run one of the assistant's scripts.** A window or a check it
  built for you is now one click away, instead of asking for it again each time.
- **Ring gauges in its windows.** The small windows the assistant opens can now draw real
  progress rings — smooth circles filled clockwise, in one colour or several, with a label
  in the middle and a caption underneath — instead of rings pieced together from text.
- **Notes to a worker already on a job.** The assistant can now leave a running worker a
  note — new information it needs, or a change of plan — and the worker reads it the
  moment its current step finishes. It can also ask a worker that has already reported
  back a follow-up question, and get an answer that remembers everything that came before.

- **Its windows say when they were opened.** A dim line at the bottom of each of the
  assistant's windows gives the date and time it was opened, so after a fix you can tell
  at a glance whether you are looking at the new copy or an old one.
- **Fixes to its own windows can go back into the app.** Ask the assistant to sync its
  scripts upstream and it hands a worker the job of carrying general lessons and fixes
  from its installed window toolkit back to the app's source, leaving anything personal
  to this PC behind, so every install gets them.

### Changed
- **The workers strip opens by itself when a worker starts.** You no longer have to open it
  yourself to see one appear; pressing the Workers button still closes it.

### Fixed
- **The app no longer closes by itself a few seconds after starting on NVIDIA graphics.**
  Its panels were drawn without telling the graphics card where their text lived; some
  drivers let that pass, and NVIDIA's crashed on it. If the app ever does close on its
  own, its log now ends with where it crashed rather than simply stopping.
- **Setup fetches the Japanese voice again.** On some PCs the setup script's answer to the
  voice's licence question arrived garbled, so the voice was never downloaded and the build
  stopped on a missing file.
- **A window with tabs no longer trips an error message from the UI toolkit.** A tab that was
  not selected made the window skip the rest of what the script had drawn, which left the
  tab bar and the footer region unclosed; the skip now stops at the next tab.
- **A node map keeps its layout when you reopen its window.** Closing a window and opening
  it again could pile every box of its map on top of each other until you changed the
  view, because the new window was handed the old one's positions. A reopened window
  now starts clean, and the boxes appear where the map puts them.
- **Right-drag moves every node map, not just the first one opened.** With two of the
  assistant's diagram windows open, the second only moved with the middle button, and
  Alt-drag did nothing there. Now each window gets its own settings.
- **Tooltips in its windows only appear when you point at something.** Before, a tooltip
  the assistant added to a window showed all the time, whatever the pointer was over.
- **A worker coming home no longer loops for minutes.** Reusing a worker's name for a new
  job could leave the little animation for "a worker just finished" playing over and over;
  it now plays once, as it should.
- **The assistant's windows can be resized now.** Before, they snapped back to the size the
  script asked for every time it redrew, so a resize never stuck.

## 2026-09-22

### Added
- **Small windows of its own.** The assistant can now open small windows beside the chat to
  show you things — a project's build status that goes orange, red or green, a progress bar,
  a log — and it has a toolkit of ready-made panels it can extend itself.
- **A scratch folder for throwaway scripts.** For something written for one occasion — a
  quick check, a window for the build that's running right now — the assistant can now write
  and run a script straight away, with no approval to wait on. It's not kept between
  sessions and may be cleared at any time. Ask it to keep one and it writes a proper copy
  that asks to be allowed, once, the normal way.
- **Node graphs in its windows.** The assistant can now draw boxes with pins in the small
  windows it opens, wire them together, and let you drag them around too — and it can read
  back whatever you connected.

### Fixed
- **Nodes in a graph no longer sit on top of each other.** Once each box has been drawn
  and its real size is known, any box overlapping another is nudged right or down until
  they all have room, once, and then left alone so your own dragging is respected.
- **A rewritten window no longer flickers against its old self.** Asking the assistant to
  change a window it already had open made two copies of the script draw into the same
  window, alternating every frame. Opening a window that is already open now hands it to
  the new script, and the old one stops by itself.
- **Hint text in node graphs is readable.** The dim style the assistant used for the
  example sentences inside nodes was nearly invisible on the grey node background, which
  looked like the Japanese font was missing. The font was always there; the colour is
  brighter now.
- **Node graphs can be moved around now.** The first one opened could not be panned at all:
  the button that pans was never passed on to the window. Right-drag pans, Alt-drag pans,
  the middle button pans, and a selected link goes with the Delete key. The assistant's
  windows also take the keyboard now, so typing into one of their text fields works.
- **Starting the app by double-clicking it works again.** Since the previous day's changes it
  died silently within a blink of being opened from Explorer, though it ran fine from a
  terminal. The cause was the log file being opened twice with sharing denied; the second
  open failed and the first line written to it ended the process. If a similar fault ever
  happens again the log will now say where, instead of just stopping.

### Changed
- **Workers now run on a lighter model by default.** Cheaper and quicker, and good enough
  for most of what gets handed off. Change it in settings, or per task if you want a
  particular one done on something else.
- **The assistant, and its workers, can now drive your Chrome browser.** Turn on Browser
  under Tools and it can open pages, read them, click and fill things in for you, without
  asking each time. Off until you turn it on, because it acts in your browser without
  asking, and it needs the Claude in Chrome extension.
- **Choosing a microphone and a speaker.** The app can now be told which microphone and
  which output to use by name, with two environment variables, instead of always taking the
  Windows defaults. If the name matches nothing it says so and lists what it found. Useful when
  the default microphone is a headset that hears nothing.
- **Talking over the assistant, measured on loudspeakers.** With the reply coming out of
  loudspeakers near the microphone, the assistant's own voice is loud enough at the microphone
  that the safety margin which stops it interrupting itself also stops a person at ordinary
  volume from interrupting it, at least for the English voice. It will not cut itself off, but
  you may have to speak up. The debug line now says how close you came. Proper echo
  cancellation is the fix and is not done.

## 2026-09-21

### Added
- **Ask what is new.** Say "what's new", "what changed", "新機能" or anything like them and
  the assistant reads this file and tells you, then offers to go through any of it. It only
  reads it on a turn that asks, and only once in a conversation, so it costs nothing the rest
  of the time.
- **Memory.** You can say "remember that ..." and the assistant keeps it between conversations,
  including after the reset button and after restarts. "Forget that" removes one. The list is a
  plain text file in the app's data folder, editable by hand, and it holds a few dozen lines;
  the app says aloud if it is full.
- **Talking over the assistant.** While it speaks, the microphone now stays open and watches for
  your voice. Talk over it for about a third of a second and it stops speaking at once, the rest
  of its reply still appears in the panel, and what you said becomes the next turn. This needs
  a microphone that can hear you: the app uses the Windows default input device, and on the
  development machine that is a headset microphone that hears nothing, so the webcam microphone
  must be made the default first. Not yet tuned with a person in the room.
- **Prompt files say which format they are.** If a prompt file in your data folder was written
  for a different version of the app, the app uses its own copy, leaves yours untouched, and
  says so in the log.
- A test for every rule the assistant's command block follows, and tests for the sentence
  splitter, the script splitter that chooses the voice, the event bus, audio buffers, the
  resampler, and the process-kill path. Twenty-seven tests in all, up from thirteen.
- **The recogniser now reports how sure it was** of each utterance, from the decoder's own
  scores. Not yet used to decide anything; the measurements say it catches two thirds of the
  cases where a Japanese word came out as invented English, and never fires on ordinary English.
- **A pinned-language mode** for the recogniser that notices when you switched to the other
  language and goes back for it. Built and measured, not switched on: for a Japanese word
  inside an English sentence it is slightly worse than what ships today, and a pin without the
  recovery is the worst setting measured, so the app keeps automatic detection for now.


### Fixed
- **Your edits to prompts and avatar frames are no longer silently overwritten** after an
  update. The app now compares file contents, not timestamps. If both you and the update
  changed the same file, yours stays and the new one lands beside it with `.new` on the end.
  On the first run after this update every file you have hand-edited gets one of those.
- **Settings changes no longer pause running workers.** Only the Stop button and Reset do.
  An automatic hand-over to a fresh session, which happens when a conversation gets long,
  leaves your workers running.
- **Talking during a hand-over is no longer lost.** What you say while the app is handing over
  is held and sent to the new session as your own words.
- **A worker that will not stop can no longer freeze the conversation.** After three seconds
  the app ends it, and it reports as stopped rather than failed.
- **A malformed message from Claude Code no longer closes the app.** It is logged and skipped.
- **When Claude Code fails to start, the app now says why.** A sign-in problem or a bad model
  name used to surface as an empty reason; the child's own last words and exit code are now
  in the message. A launch failure no longer dumps the whole prompt into the status line, and
  the app checks the Windows command-line limit before launching (today's prompt uses about
  sixty percent of it).
- **A half-finished command block is no longer executed.** If a reply is cut off in the middle
  of its command block, nothing in it runs.
- **Workers can no longer be sent to a folder only a worker mentioned.** Only folders you name
  count as evidence, and a near-miss spelling of the app's own folder now resolves to the app's
  folder rather than the spelling.
- **A script you allowed is tied to its contents.** If the file changes after you allowed it,
  the app asks again before running it. Scripts in the scripts folder are now held until you
  allow them too. Turning auto-allow on no longer silently allows everything already waiting.
- **A failed settings save now shows in amber** in the settings panel and is retried, instead of
  saying "Saved". Running the app with a model override no longer rewrites your saved model.
  After a settings change, a restarted assistant is told the new values, not the old.
- **Wrong-reason messages.** "The user cancelled the subscription" in a reply is no longer read
  as the worker having been stopped. A failed Japanese voice load can now be retried by
  unticking and re-ticking Japanese.
- **The audio path no longer stalls.** The playback and capture callbacks take no lock and
  allocate nothing; a chunk at the wrong sample rate is converted instead of played at the
  wrong pitch.
- **Setup checks what it downloaded.** Checksums for the archives that were still on this
  machine, extraction that cannot leave half a tree behind, and two files the check had
  forgotten.
- **The voices prompt's explanatory comment was being sent to Claude on every launch.**
  Comments at the top of prompt files now stay out of the prompt.
- **Garbage bytes from the recogniser no longer reach the wake-phrase matcher** as characters.
- The wake phrase match no longer logs the surrounding speech, only its length.
- Duplicate code: one colour parser instead of three, one text decoder instead of three, one
  shared base under the six extra windows.

### Investigated
- **The recogniser's own language tag** was recovered by patching sherpa-onnx and measured. It
  is useless: it never appears on the utterances where it would matter. The pinned build stays.
- **Talking over the assistant through a headset:** the app's own voice never reaches the webcam
  microphone through the headset, so echo cancellation is probably unnecessary there.
  Loudspeakers were not measured.

### Known
- Language detection between English and Japanese still has no confidence gate. The decoder's
  confidence is now available and measured; wiring it into a "say that again" is the next step.
- The spoken memory flow has been used for real ("remember" and recall), but "forget that" and
  the full-file case have not been tried by voice.
- Stopping a worker that has already finished can still freeze the window for up to three
  seconds.
- Talking over the assistant has not yet been tuned with a person in the room.
