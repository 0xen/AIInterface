# AIInterface

Voice interface to Claude for Windows 11, in C++. Current milestone: `voiceloop`, a console
push-to-talk loop. Speech recognition and synthesis run locally on the CPU; Claude runs
through the locally installed Claude Code CLI on your subscription (no API key needed).
No audio is ever written to disk.

Plan: `PROJECT_OUTLINE.md`. Research: `docs/`. Engine experiments and evidence: `spikes/`.

## Build

Requires Visual Studio 2022, CMake 3.24+, and the model/engine folders produced by the spikes
(`models/`, `spikes/stt/bin/`, `spikes/tts_cpu/voicevox/`).

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target voiceloop
```

## Run

```
build\Release\voiceloop.exe
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
