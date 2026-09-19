# AIInterface

Voice interface to Claude for Windows 11, in C++. The program is `avatar`:

- `avatar`: a small transparent always-on-top window in the bottom-right corner (a 16x16
  black-and-white pixel slime that reacts to the session) with the transcript, a usage
  readout and Talk / Silence / Pause buttons. Built on the sibling Renderer engine.
  **A default build produces this and nothing else.**

`voiceloop` — the same loop as a console push-to-talk program (milestone 1, confirmed
working) — and the unit tests are development targets, off by default; see
"Building the development targets".

Speech recognition and synthesis run locally on the CPU; Claude runs through the locally
installed Claude Code CLI on your subscription (no API key needed). No audio is ever written
to disk.

Plan: `PROJECT_OUTLINE.md`. Research: `docs/`. Engine experiments and evidence: `spikes/`.

## Build

Requires Visual Studio 2022, CMake 3.24+, the Claude Code CLI signed in to your subscription,
the prebuilt engines and models described in the next section (about 1.3 GB, none of it in git),
and for `avatar` the Renderer engine checked out beside this repo (`..\Renderer`, or set
`AII_RENDERER_DIR`) plus the Vulkan SDK (its `dxc` compiles the shaders).

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`avatar.exe` lands in `build\bin\Release\` next to `rend.dll` and the engine DLLs, and it is
the only executable there: the tests and `voiceloop` are behind the options below.

**Run `build\bin\Release\avatar.exe`.** It is standalone where it stands: every DLL it loads,
`data\` and `pre-prompt.md` are already beside it.

The build can also assemble the app into a single folder — `avatar.exe`, the DLLs it loads,
`data\shaders\`, a `BUILD-INFO.txt` naming the configuration and commit, and nothing else.
That is release packaging, so **it is off by default**: `-DAII_STAGE_LATEST=ON` turns it on and
it stages into `latest\` at the root of the checkout (`AII_LATEST_DIR` moves it). `latest\` is
gitignored. Staging happens when `avatar.exe` relinks, so a build with nothing to do does not
touch it. Note that changing the option's default does not change an existing build directory:
a `build\` configured before 19 Sep 2026 has `AII_STAGE_LATEST=ON` in its cache and will keep
staging until you pass `-DAII_STAGE_LATEST=OFF` once or delete the cache.

**What it stages is not a package you can copy elsewhere.** The models, the VOICEVOX core and
`assets\` are compiled in as absolute paths into this checkout (`docs\RELEASE-HANDOVER.md`, B1),
so the staged `avatar.exe` runs on the machine that built it and nowhere else.

### Building the development targets

| Option | Default | Builds |
|---|---|---|
| `AII_BUILD_TESTS` | OFF | the seven unit tests in `tests\` (`schedule_test`, `clip_utf8_test`, `app_strings_test`, `failure_reason_test`, `prompt_body_test`, `worker_report_test`, `avatar_children_test`) |
| `AII_BUILD_VOICELOOP` | OFF | `voiceloop`, the milestone-1 console loop |
| `AII_BUILD_AVATAR` | ON | the app itself |
| `AII_BUILD_SCRIPTS` | ON | the in-process Python script host (M2.6) |

```
cmake -S . -B build -DAII_BUILD_TESTS=ON
cmake --build build --config Release
build\bin\Release\schedule_test.exe && build\bin\Release\clip_utf8_test.exe && ^
build\bin\Release\app_strings_test.exe && build\bin\Release\failure_reason_test.exe && ^
build\bin\Release\prompt_body_test.exe && build\bin\Release\avatar_children_test.exe
```

Each of those six prints its cases and exits non-zero on failure; run them after touching
anything they cover. (`worker_report_test` is a manual harness — it spawns a real Claude
child, so it takes arguments: `worker_report_test <name> <cwd> <task...>`.) Turn the option
back off with `cmake -S . -B build -DAII_BUILD_TESTS=OFF` — it is cached, so it stays on
until you do. Executables built while an option was on are **not** removed when it goes off;
delete `build\bin\Release\` to clear them.

The spikes in `spikes\` are separate CMake projects with their own build directories
(`stt_test`, `mic_stt`, `codeswitch_bench`); nothing in the main build refers to them, so they
are never built from here.

## Avatar window

```
build\bin\Release\avatar.exe
```

| Control | Action |
|---|---|
| SPACE or Talk | start listening; again to stop and send (also barges in while Claude speaks) |
| S or Silence | stop the audio, keep the text coming |
| E or Pause | cancel the reply in flight |
| Esc or Q | quit |

Flags: `--say "text"` sends one turn as soon as the engines are up, `--seconds N` quits after N
seconds, `--no-voice` shows the window without loading any engine, `--opaque` gives a normal
window. Engines load on a background thread, so the window appears at once and the status
line reports progress.

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
```

Workers run with `--permission-mode bypassPermissions`, because nothing in this app can answer a
permission prompt and a worker that asked would hang forever. That means a worker can read, write
and run commands in the directory it is given without asking. Give the assistant directories you
are willing to hand over. Set `AII_WORKER_BYPASS=0` to make workers ask instead, accepting that
they will stall when they do.

The window uses the engine's D3D12 backend: on the AMD driver here only a DirectComposition
swapchain gives per-pixel alpha (the engine's Vulkan path composites opaque). The text panel
is drawn with GDI into a bitmap each time something changes and blitted by a shader, which is
what makes Japanese text work through the system fonts. Shaders live in
`src/avatar/shaders/` and are compiled to DXIL and SPIR-V at build time.

## Getting the engines and models

Models, prebuilt SDKs, DLLs and executables are gitignored. `CMakeLists.txt` and `src/main.cpp`
expect them at the exact paths below (the spike folders are reused on purpose until the project
gets a proper dependency layout). Run these from the repository root in PowerShell.
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

`models\zonos2\` (13 GB) and the `zonos2.cpp` source trees under `spikes\tts_src\` belong to
the abandoned ZONOS2 experiment; see `spikes\README.md`. `kokoro-multi-lang-v1_1` and the
SenseVoice model were only used as cross-checks during the spikes.

## Run the console loop (`voiceloop`)

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
| `AII_KOKORO_SID` | `3` | English voice (3 = af_heart, 2 = af_bella) |
| `AII_VOICEVOX_STYLE` | `2` | Japanese voice style (2 = 四国めたん ノーマル; 3 = ずんだもん, 8 = 春日部つむぎ, 10 = 雨晴はう) |
| `AII_EARLY_WORDS` | `12` | the first chunk of a reply is spoken at a comma or after this many words; `0` waits for full sentences |
| `AII_WORKER_BYPASS` | `1` | background workers skip permission prompts; `0` makes them ask, which stalls them |

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
