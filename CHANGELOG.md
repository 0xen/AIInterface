# Changelog

What changed in this app, newest first, written for the person using it and for the
assistant inside it. The assistant reads this when asked what is new, so entries are short,
plain and free of file paths; the technical record with measurements is `docs/MILESTONES.md`.
Every milestone that merges adds an entry here in the same commit.

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
- **Starting the app by double-clicking it works again.** Since the previous day's changes it
  died silently within a blink of being opened from Explorer, though it ran fine from a
  terminal. The cause was the log file being opened twice with sharing denied; the second
  open failed and the first line written to it ended the process. If a similar fault ever
  happens again the log will now say where, instead of just stopping.

### Changed
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
- **Talking over the assistant on this machine:** the app's own voice never reaches the webcam
  microphone through the headset, so echo cancellation is probably unnecessary here.
  Loudspeakers were not measured.

### Known
- Language detection between English and Japanese still has no confidence gate. The decoder's
  confidence is now available and measured; wiring it into a "say that again" is the next step.
- The spoken memory flow has been used for real ("remember" and recall), but "forget that" and
  the full-file case have not been tried by voice.
- Stopping a worker that has already finished can still freeze the window for up to three
  seconds.
- Talking over the assistant has not yet been tuned with a person in the room.
