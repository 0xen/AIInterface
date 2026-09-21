# Changelog

What changed in this app, newest first, written for the person using it and for the
assistant inside it. The assistant reads this when asked what is new, so entries are short,
plain and free of file paths; the technical record with measurements is `docs/MILESTONES.md`.
Every milestone that merges adds an entry here in the same commit.

## 2026-09-21

### Added
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

- **The recogniser now reports how sure it was** of each utterance, from the decoder's own
  scores. Not yet used to decide anything; the measurements say it catches two thirds of the
  cases where a Japanese word came out as invented English, and never fires on ordinary English.
- **A pinned-language mode** for the recogniser that notices when you switched to the other
  language and goes back for it. Built and measured, not switched on: for a Japanese word
  inside an English sentence it is slightly worse than what ships today, and a pin without the
  recovery is the worst setting measured, so the app keeps automatic detection for now.

### Investigated
- **The recogniser's own language tag** was recovered by patching sherpa-onnx and measured. It
  is useless: it never appears on the utterances where it would matter. The pinned build stays.
- **Talking over the assistant on this machine:** the app's own voice never reaches the webcam
  microphone through the headset, so echo cancellation is probably unnecessary here.
  Loudspeakers were not measured.

### Known
- Language detection between English and Japanese still has no confidence gate. Work on that
  is in progress: reading the decoder's confidence and pinning the recogniser to the
  conversation's language.
- The spoken end-to-end test of memory ("remember X", restart, ask, "forget that") has not
  been run with a real model yet.
- Stopping a worker that has already finished can still freeze the window for up to three
  seconds.
