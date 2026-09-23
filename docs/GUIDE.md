# AIInterface: the full guide

Everything the README leaves out. The README gets you from a clone to a running window;
this is the reference for what the window does, how it is configured, what it downloads,
and how it is built for development.

Contents: [Building](#building) · [The window](#the-window) · [Interrupting](#interrupting-it) ·
[Voices](#more-than-one-voice) · [Workers](#background-workers) · [Browser](#browser-control) ·
[Engines by hand](#getting-the-engines-and-models-by-hand) · [Console loop](#the-console-loop-voiceloop) ·
[Environment variables](#configuration-environment-variables) · [How a turn flows](#how-a-turn-flows) ·
[Components](#components) · [Development builds and tests](#development-builds-and-tests)

## Building

Requires Visual Studio 2022 with the C++ workload, CMake 3.24+, the Vulkan SDK (its `dxc`
compiles the shaders), and the Claude Code CLI signed in to your subscription. The engine
comes with the repository as a submodule; the engines and models — about 1.27 GB, none of
it in git — are fetched by the setup script.

```
git clone --recurse-submodules https://github.com/0xen/AIInterface.git
cd AIInterface
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

If you already cloned without `--recurse-submodules`, run `git submodule update --init
--recursive` first — `third_party\Renderer` is otherwise empty and the configure step
stops with that instruction.

`scripts\setup.ps1` checks the build tools, downloads each component from its own
publisher, puts it where the build expects it, and verifies every path the app will look
for at runtime. It is safe to re-run: each step skips what is already there.
`-CheckOnly` verifies without downloading anything. It reports missing build tools with
their download URLs rather than installing them for you. The next section documents what
it fetches, for anyone who would rather do it by hand or needs to understand a failure.

### The engine

`third_party\Renderer` is a submodule pinned to the commit this app is known to build
against, so a clone gets that engine rather than whatever its master happens to be. To
build against a sibling checkout instead — which is how the engine itself is developed —
name it explicitly:

```
cmake -S . -B build-engine -DAII_RENDERER_DIR=C:/github/Renderer
```

`AII_RENDERER_DIR` is cached, so a build directory configured before the submodule existed
keeps the sibling path it was configured with until you pass the variable again or delete
the cache.

`avatar.exe` lands in `build\bin\Release\` next to `rend.dll` and the engine DLLs, and it is
the only executable there: the tests and `voiceloop` are behind the options below.

**Run `build\bin\Release\avatar.exe`.** It is standalone where it stands: every DLL it loads,
`data\` and `pre-prompt.md` are already beside it.

The build can also assemble the app into a single folder — `avatar.exe`, the DLLs it loads,
`data\shaders\`, a `BUILD-INFO.txt` naming the configuration and commit, and nothing else.
That is release packaging, so **it is off by default**: `-DAII_STAGE_LATEST=ON` turns it on and
it stages into `latest\` at the root of the checkout (`AII_LATEST_DIR` moves it). `latest\` is
gitignored. Staging happens when `avatar.exe` relinks, so a build with nothing to do does not
touch it. The option is cached, so a build directory keeps whatever value it was configured
with until you pass it again or delete the cache.

**What it stages is not a package you can copy elsewhere.** The models, the VOICEVOX core and
`assets\` are compiled in as absolute paths into this checkout, so the staged `avatar.exe` runs
on the machine that built it and nowhere else.


## The window

```
build\bin\Release\avatar.exe
```

| Control | Action |
|---|---|
| SPACE or the microphone button | start listening; again to stop and send. Pressed while Claude is speaking it **cancels** the reply — the voice stops and so does the text, at whatever word had arrived |
| S or the speaker button | mute Claude's voice. A level, remembered across runs: audio already playing is cut and nothing further is spoken, while replies still arrive as text |
| E or the stop button | cancel the reply in flight, close the mic, pause every worker |
| Type, then Enter | send a typed message. This works while the microphone is open: what was being heard is discarded, the typed text is the turn, and listening resumes after the reply |
| the reset button | throw the conversation away and start a new one |
| Esc or Q | quit |

The assistant can mute itself, too: ask it to be quiet and it sets `panel.muted`
and carries on replying in text. The speaker button or S brings it back.

The window uses the engine's D3D12 backend: on AMD drivers only a DirectComposition
swapchain gives per-pixel alpha (the engine's Vulkan path composites opaque). The text panel
is drawn with GDI into a bitmap each time something changes and blitted by a shader, which is
what makes Japanese text work through the system fonts. Shaders live in
`src/avatar/shaders/` and are compiled to DXIL and SPIR-V at build time.

### Interrupting it

**Talking over it works too, and it is not the same thing as SPACE.** While the microphone
latch is on (or a wake phrase is set), the microphone stays open while Claude speaks, and a
loud, deliberate interruption — about a third of a second of continuous speech, which a cough
or a keypress does not reach — stops the voice mid-sentence and opens listening for what you
are saying. The reply is *silenced*, not cancelled: its text goes on arriving in the panel in
full, so nothing is lost by interrupting. SPACE is the other choice and is still there for
when you want the reply to stop altogether. Nothing heard while Claude is speaking is ever
decoded or sent — it is watched for loudness and for nothing else — and if you say something
and finish it, that utterance supersedes the reply you talked over. `AII_BARGE_DEBUG=1` logs
one line per reply saying how much of Claude's own voice reached the microphone, what bar
that set, whether anything cleared it, and how close the loudest thing came; each run over the
bar that was refused for being too short gets a line of its own. One caveat: with the
reply coming out of loudspeakers near the microphone, Claude's own voice raises that bar above
a person's ordinary speaking level, so it will not interrupt itself but you will have to speak
up to interrupt it. Headphones do not have the problem.

Flags: `--say "text"` sends one turn as soon as the engines are up, `--seconds N` quits after N
seconds, `--no-voice` shows the window without loading any engine, `--opaque` gives a normal
window. Engines load on a background thread, so the window appears at once and the status
line reports progress.

### More than one voice

Ask for a dialogue and the assistant can perform it in different voices. It marks them inline:
`[v2]` switches to the second voice of whatever language the line is in, `[v1]` returns to its
own, and every reply starts on `[v1]`. The markers are machine traffic — stripped from the
transcript and never spoken — and a marker inside a ``` fence is code, not a directive.

The voices are per language and independent, so two English and two Japanese is as expressible as
three of one. `[v1]` is always the primary you configured (`AII_KOKORO_SID`, `AII_VOICEVOX_STYLE`)
and is deliberately not in either list. The defaults ship non-empty, because a feature that does
nothing until you guess four integers is a feature nobody finds:

| Slot | English | Japanese |
|---|---|---|
| `[v1]` | `af_heart` (sid 3) | 四国めたん ノーマル (style 2) |
| `[v2]` | `bm_george` (26) — British man, 142 Hz | 雨晴はう (10) |
| `[v3]` | `am_onyx` (17) — deep American man, 90 Hz | ずんだもん (3) |

The English trio is separated by 50–114 Hz of pitch
and three different accent/sex pairings. **The Japanese pair is the less certain one** — every
Japanese voice installed sits within 30 Hz of every other, so its separation rests on timbre.

A voice id the engine does not have is dropped when the engines load, with a line in the log, and
that slot falls back to the primary. This matters because neither engine fails safely on its own:
Kokoro silently substitutes a third voice nobody chose, and VOICEVOX returns an error that would
reach you as silence. Only the model knows about voices that exist — the prompt is told a count,
and is told nothing at all when no secondary voices are configured.

### Background workers

Ask for work to be done ("start a worker called build in C:\myrepo that runs the tests") and the
assistant spawns a separate Claude Code instance with file and command tools in that directory.
Each worker appears as a line in the panel with its state and what it is doing; when one finishes,
its one-sentence summary goes to the panel and the transcript verbatim, and the assistant itself
says out loud what it amounts to — in the language the conversation is being held in, rather than
reading a sentence that was written to be read. A worker that failed or was stopped keeps its
plain canned line. Say "pause the build worker" to interrupt one; the Pause button
interrupts the reply in flight and every running worker.

The assistant drives this by ending a reply with a fenced block that is displayed but never spoken
and is removed from the transcript once it has run:

```
spawn name=build cwd=C:\myrepo task=Run the test suite and report what failed.
pause name=build
stop name=build
clearchat
```

`clearchat` empties the chat panel's transcript without touching the conversation, workers or
schedules; scripts have the same thing as `aii.clear_chat()`.

Workers run with `--permission-mode bypassPermissions`, because nothing in this app can answer a
permission prompt and a worker that asked would hang forever. That means a worker can read, write
and run commands in the directory it is given without asking. Give the assistant directories you
are willing to hand over. Set `AII_WORKER_BYPASS=0` to make workers ask instead, accepting that
they will stall when they do.

Workers run on the `opus` alias by default — cheaper and quicker than the conversational model,
and good enough for most of what gets spawned. Change it in the settings panel (`model.worker`,
under the Model picker: `default`, `opus`, `sonnet` or `haiku`) or per task, with `model=` on a
`spawn` or `schedule ... task=` line.


### Browser control

Turn on the **Browser** tool group (off by default) and both the assistant and its workers can
drive your Chrome — open tabs, read pages, click, type, screenshot — without asking, through the
Claude in Chrome extension. It needs that extension installed and Chrome open; without it the
switch does nothing. `AII_WORKER_CHROME=0` keeps it from workers specifically while leaving the
conversational instance's own access alone.

## Getting the engines and models by hand

`scripts\setup.ps1` does all of this for you, and verifies the result. What follows is the
same work by hand, for anyone who would rather see each step or is debugging one the
script reported.

Models, prebuilt SDKs, DLLs and executables are gitignored. `CMakeLists.txt` and `src/main.cpp`
expect them at the exact paths below. The `spikes\` folders are where the engines were first
tried and are still their home, until the project gets a proper dependency layout. Run these
from the repository root in PowerShell.
Everything is a public download; nothing needs an account or token.

### 1. sherpa-onnx prebuilt (recognition + Kokoro runtime), 20 MB

```
mkdir spikes\stt\bin
curl.exe -L --retry 5 -o $env:TEMP\sherpa.tar.bz2 https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.8/sherpa-onnx-v1.13.8-win-x64-shared-MD-Release.tar.bz2
tar xjf $env:TEMP\sherpa.tar.bz2 -C spikes\stt\bin
```

Expected: `spikes\stt\bin\sherpa-onnx-v1.13.8-win-x64-shared-MD-Release\{include,lib}` with
`sherpa-onnx-c-api.lib`, `sherpa-onnx-c-api.dll`, `onnxruntime.dll`. A newer release will
work if you update `SHERPA_DIR` in `CMakeLists.txt`.

### 2. Recognition model: Nemotron-3.5 streaming, 475 MB

```
mkdir models
curl.exe -L --retry 5 -o models\nemotron.tar.bz2 https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-560ms-int8-2026-06-11.tar.bz2
tar xjf models\nemotron.tar.bz2 -C models
```

Expected: `models\sherpa-onnx-nemotron-3.5-asr-streaming-0.6b-560ms-int8-2026-06-11\{encoder,decoder,joiner}.int8.onnx` and `tokens.txt`.

### 3. English voice: Kokoro-82M multi-lang v1.0, 350 MB

```
curl.exe -L --retry 5 -o models\kokoro.tar.bz2 https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-multi-lang-v1_0.tar.bz2
tar xjf models\kokoro.tar.bz2 -C models
```

Expected: `models\kokoro-multi-lang-v1_0\{model.onnx,voices.bin,tokens.txt,espeak-ng-data,lexicon-*.txt}`.
Use v1.0, not v1.1: v1.1 lacks the af_heart and af_bella voices the app defaults to.

### 4. Japanese voice: VOICEVOX Core 0.17.0, about 250 MB

The bare release zip holds only the C API. Its downloader also fetches the matching ONNX Runtime,
the Open JTalk dictionary and the voice model, and shows the licence terms (answer `y`):

```
mkdir spikes\tts_cpu\voicevox
curl.exe -L -o spikes\tts_cpu\voicevox\download-windows-x64.exe https://github.com/VOICEVOX/voicevox_core/releases/download/0.17.0/download-windows-x64.exe
cd spikes\tts_cpu\voicevox
echo y | .\download-windows-x64.exe -o .\voicevox_core --models-pattern 0.vvm --exclude additional-libraries
cd ..\..\..
mkdir models\voicevox
move spikes\tts_cpu\voicevox\voicevox_core\dict models\voicevox\dict
move spikes\tts_cpu\voicevox\voicevox_core\models models\voicevox\models
```

Expected after the move:

- `spikes\tts_cpu\voicevox\voicevox_core\c_api\{include\voicevox_core.h, lib\voicevox_core.lib, lib\voicevox_core.dll}`
- `spikes\tts_cpu\voicevox\voicevox_core\onnxruntime\lib\voicevox_onnxruntime.dll`
- `models\voicevox\dict\open_jtalk_dic_utf_8-1.11\`
- `models\voicevox\models\vvms\0.vvm` (四国めたん, ずんだもん, 春日部つむぎ, 雨晴はう)

`--exclude additional-libraries` skips the DirectML and CUDA builds; the app runs VOICEVOX on
CPU. The downloader's pager may print a panic when stdout is not a terminal; the download still
completes. More voices: drop further `.vvm` files from
https://github.com/VOICEVOX/voicevox_vvm/releases into `models\voicevox\models\vvms\`
(the app currently loads only `0.vvm`).

### 5. Claude Code CLI

Install Claude Code and sign in once so `%USERPROFILE%\.local\bin\claude.exe` exists
(override the path with `AII_CLAUDE_EXE`). No API key is used.

### Not needed for the app

`models\zonos2\` (13 GB), if you have it, belongs to an abandoned GPU synthesis experiment.
`kokoro-multi-lang-v1_1` and the SenseVoice model were only used as cross-checks.

## The console loop (`voiceloop`)

Not built by default. Configure with `-DAII_BUILD_VOICELOOP=ON` first (see
"Building the development targets"), then:

```
build\bin\Release\voiceloop.exe
```

| Key | Action |
|---|---|
| SPACE | start listening; press again to stop and send |
| T | type a message instead of speaking |
| S | stop the assistant mid-reply |
| Q | quit |

Pressing SPACE while the assistant is speaking cuts it off and starts listening (barge-in).
After each reply a timing line and a usage line (five-hour and seven-day subscription windows) are printed.

Scripted modes, useful for testing without a microphone:

```
build\Release\voiceloop.exe --say "Hello, what can you do?"      # one turn through Claude, spoken
build\Release\voiceloop.exe --speak "Text to speak. 日本語も。"    # synthesis only, no Claude
```

## Configuration (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `AII_BACKEND` | `code` | `code` = Claude Code CLI on your subscription; `api` = Messages API (needs `ANTHROPIC_API_KEY`) |
| `AII_CLAUDE_EXE` | `%USERPROFILE%\.local\bin\claude.exe` | path to the Claude Code executable |
| `AII_MODEL` | backend default | model override (e.g. `opus`, `sonnet`, or a full model id) |
| `AII_EFFORT` | `low` | reasoning effort; `low` keeps replies snappy |
| `AII_STT_LANG` | `auto` | `auto`, `en` or `ja` for the recogniser |
| `AII_MIC` | Windows default | capture device, by any part of its name (e.g. `Webcam`); a name that matches nothing fails the start and lists the devices |
| `AII_SPEAKER` | Windows default | playback device, the same way (e.g. `Digital Output`) |
| `AII_KOKORO_SID` | `3` | English voice (3 = af_heart, 2 = af_bella) |
| `AII_VOICEVOX_STYLE` | `2` | Japanese voice style (2 = 四国めたん ノーマル; 3 = ずんだもん, 8 = 春日部つむぎ, 10 = 雨晴はう) |
| `AII_VOICES_EN` | `26,17` | the English voices `[v2]`, `[v3]`… select, as Kokoro speaker ids; empty switches them off |
| `AII_VOICES_JA` | `10,3` | the Japanese ones, as VOICEVOX styles from the loaded `0.vvm`; empty switches them off |
| `AII_EARLY_WORDS` | `12` | the first chunk of a reply is spoken at a comma or after this many words; `0` waits for full sentences |
| `AII_WORKER_BYPASS` | `1` | background workers skip permission prompts; `0` makes them ask, which stalls them |
| `AII_WORKER_MODEL` | `model.worker` setting | overrides which model workers run on (`opus`, `sonnet`, `haiku`, or a full model id); the setting alone already covers this for everyone but a harness |
| `AII_WORKER_CHROME` | `1` | background workers get Chrome tools when the Browser group is on; `0` withholds it from workers while leaving the conversational instance's own access alone |

## How a turn flows

1. Microphone samples go straight from the capture callback into the sherpa-onnx streaming recogniser (Nemotron-3.5, auto language detection). Partial text is shown while you talk.
2. On stop, the final transcript is sent as one user message to the long-lived Claude Code process over stdin (`--input-format stream-json`), which keeps the conversation for the session.
3. Text deltas stream back, are printed, and are cut into sentences. Text inside ``` fences is shown but not spoken.
4. Each sentence is routed by script: Japanese to VOICEVOX Core, everything else to Kokoro (sherpa-onnx). Samples go into a ring buffer that miniaudio plays through the default output device.

## Components

| Component | Library | Licence |
|---|---|---|
| Audio I/O | miniaudio | public domain / MIT-0 |
| Recognition | sherpa-onnx + NVIDIA Nemotron-3.5-ASR-Streaming-0.6B | Apache-2.0 / OpenMDW-1.1 |
| English voice | Kokoro-82M via sherpa-onnx | Apache-2.0 |
| Japanese voice | VOICEVOX Core 0.17 | MIT (voices: VOICEVOX terms, credit "VOICEVOX:四国めたん") |
| JSON | nlohmann/json | MIT |
| HTTP (api backend) | WinHTTP | Windows |

## Development builds and tests

### Building the development targets

| Option | Default | Builds |
|---|---|---|
| `AII_BUILD_TESTS` | OFF | the unit tests in `tests\`, one executable per `*_test.cpp` |
| `AII_BUILD_VOICELOOP` | OFF | `voiceloop`, the console loop |
| `AII_BUILD_AVATAR` | ON | the app itself |
| `AII_BUILD_SCRIPTS` | ON | the in-process Python script host |

```
cmake -S . -B build -DAII_BUILD_TESTS=ON
cmake --build build --config Release
for %t in (build\bin\Release\*_test.exe) do @(%t >nul && echo ok %t || echo FAILED %t)
```

Each test prints its cases and exits non-zero on failure; run them after touching
anything they cover. (`worker_report_test` is a manual harness — it spawns a real Claude
child, so it takes arguments: `worker_report_test <name> <cwd> <task...>`.) Turn the option
back off with `cmake -S . -B build -DAII_BUILD_TESTS=OFF` — it is cached, so it stays on
until you do. Executables built while an option was on are **not** removed when it goes off;
delete `build\bin\Release\` to clear them.

