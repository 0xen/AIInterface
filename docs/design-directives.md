# M13.1 / M13.3 — the inline directive channel

A design note, not an implementation. Written 20 Sep 2026 against the tree at `5210de5`, while M11
and M12 are in flight. Every claim about existing code carries a file and a line; where this note
is inferring rather than reading, it says so. A throwaway probe was built to settle the streaming
question and it found a defect in the obvious design — see §3.

---

## 0. What was measured, and what it changed

A standalone probe was compiled in the scratchpad against the **real** `core/text_util.cpp` and a
copy of `core/sentence_splitter.cpp`. It asserts, for twenty-one grammar cases, that feeding a
reply one byte at a time gives byte-identical output to feeding it in one delta, and it runs the
proposed filter in front of the actual `SentenceSplitter`.

Results that matter:

- **Every grammar case in §5 below is the probe's observed output**, not a prediction.
- **Holdback is 0 bytes for text with no `[` in it** — English and Japanese alike — and **3 bytes**
  at worst for a reply containing `[v2]`.
- **A marker at the head of a reply delays the first spoken chunk by exactly its own byte length
  and not one byte more.** Probe: with `[v2] ` prefixed, the first chunk emerged after 68 bytes of
  stream instead of 63 — the 5 bytes of the marker itself.
- **The obvious implementation is wrong**, and the probe caught it. Using
  `SentenceSplitter::flush()` as the mid-reply boundary made the same reply speak differently
  depending on delta size. See §3.3.

---

## 1. The path a reply actually takes

Read, not assumed:

| Step | File:line | What happens |
|---|---|---|
| CLI JSON → delta | `src/llm/claude_code_client.cpp:225-228` | `text_delta` events; `on_delta_(t)` with whatever chunk the CLI sent |
| whole-reply fallback | `src/llm/claude_code_client.cpp:260-264` | if **no** partial deltas arrived, the entire reply is delivered as **one** `on_delta_` call |
| the turn's delta lambda | `src/avatar/voice_session.cpp:1844-1855` | appends to the transcript (1847), flips state to Speaking on the first delta (1848-1852), then `splitter.feed(delta)` (1854) |
| the splitter | `src/core/sentence_splitter.cpp:64-116` | fences, sentence ends, the early first chunk |
| emit | `src/core/sentence_splitter.cpp:118-123` | `strip_markdown`, drop if nothing speakable, call `emit_` |
| the emit lambda | `src/avatar/voice_session.cpp:1747-1760` | **mute is applied here** (1748), `[speak]` log (1758), `speech_->enqueue(s)` (1759) |
| synthesis | `src/core/speech_queue.cpp:53-100` | worker thread; `split_by_script` per sentence; engine by script |

Two consumers, and **they are genuinely separate code paths**: the transcript is built by string
concatenation at `voice_session.cpp:1847` and never passes through the splitter at all. Nothing
that the splitter drops (fenced code, unspeakable chunks) is missing from the transcript, and
nothing the transcript shows is guaranteed to be spoken. Any design that strips in one place only
will leak into the other.

### 1.1 The recommended insertion point

> **In the turn's delta lambda, `src/avatar/voice_session.cpp:1844-1855`, in front of both
> consumers.** Construct a `DirectiveFilter` beside the splitter at `voice_session.cpp:1747`; in
> the lambda, replace the raw `lines_.back().text += delta` at line 1847 and the
> `splitter.feed(delta)` at line 1854 with a single `filter.feed(delta)`, and let the filter's two
> callbacks drive the two consumers.

```
filter.OnText(x)      -> { lock mutex_; lines_.back().text += x; }   // was voice_session.cpp:1847
                         splitter.feed(x);                            // was voice_session.cpp:1854
filter.OnDirective(t,v) -> splitter.break_now();                      // §3.3
                           apply(t, v) or log an unknown one
```

One parser, fed once, serving both consumers — which is what makes "markers reach neither the eyes
nor the ears" a property of the design rather than two strippers that have to agree.

**One detail that must not move.** The `first` flag at `voice_session.cpp:1848-1852` (status →
`speaking...`, state → Speaking) must stay on the **raw** delta, before `filter.feed()`. A reply
that opens with `[v2]` would otherwise hold the UI in `thinking...` for an extra delta.

**Why not elsewhere:**

- *Inside `SentenceSplitter`.* Only serves the ears. The transcript at `voice_session.cpp:1847`
  would still show every marker. It also grows a voice concept into a clean core primitive with
  three call sites — `voice_session.cpp:1747`, `src/main.cpp:202`, `src/main.cpp:282` — two of
  which have nothing to do with voices.
- *Inside `ClaudeCodeClient` at `claude_code_client.cpp:228`.* M11 owns that file. It also makes an
  LLM transport know about speech, and `worker_pool.cpp:105` builds a second client with its own
  prompt whose replies would silently inherit directive parsing.
- *Post-hoc on the finished text, like `strip_aii_blocks`* (`src/core/worker_pool.cpp:359`, applied
  at `voice_session.cpp:1889`). **Cannot work for speech at all** — by the time the turn ends the
  reply has been spoken. It is viable for the eyes alone, and is the fallback if live transcript
  stripping is judged too risky; it costs markers being visible for the whole streaming duration,
  which is exactly what the ` ```aii ` block already does today.

---

## 2. How it composes with the existing ` ```aii ` stripping

There are **two** mechanisms today and they are not the same one:

1. **Ears.** `SentenceSplitter::scan` (`sentence_splitter.cpp:88-107`) tracks ` ``` ` and discards
   everything between fences. This is generic: it silences *all* fenced code, not just `aii`.
2. **Eyes.** `strip_aii_blocks` (`worker_pool.cpp:359-374`) removes only ` ```aii … ``` ` spans,
   applied once at end of turn at `voice_session.cpp:1889` (and at `voice_session.cpp:1580`).

**They compose, they do not fight — provided the filter is fence-aware.** This is not optional. A
reply that shows a code sample containing `[v2]` must print it. The splitter would have silenced it
anyway; the transcript would not. So the filter tracks ` ``` ` itself and passes fenced text through
verbatim.

Two `` ` ``-tracking state machines now exist in series, which sounds like a divergence risk and is
not: both key off the identical three-backtick token in the identical byte stream, and the filter
emits the fence markers verbatim, so the splitter downstream sees exactly what it sees today. The
probe covers this (`code ```x [v2] y``` end` → unchanged, no directive).

**A directive is never valid inside a fence.** A model that puts `[v2]` inside a code block has
written code, not a directive.

**Nothing about `strip_aii_blocks` needs to change.** An `aii` block is fenced, so the filter never
looks inside it, and line 1889 still removes it from the transcript afterwards.

---

## 3. Surviving streaming

### 3.1 The rule

**Hold back only a live candidate at the tail of the buffer, and nothing else.**

A directive is `[`, a lowercase token, an optional `:value`, `]`, with no spaces and a hard cap of
`kMaxDirective = 32` bytes including both brackets. Scanning is a single left-to-right pass:

- No `[` in the buffer → everything is emitted immediately. **Zero holdback.** Probe-confirmed for
  both English and Japanese.
- A `[` whose following bytes already **violate** the grammar → prose, emitted immediately, scanning
  resumes at the character *after* that `[`. No waiting: the verdict is reached on bytes already in
  hand. `[see figure 1]` dies at the space after `see`, in the same `feed()` call.
- A `[` whose following bytes are still **grammar-legal but unterminated**, and which runs to the
  end of the buffer → this and only this is held back, until the next delta.
- The candidate dies on length: past 32 bytes it is declared prose and released.

So the worst case is 32 bytes of delay, only for a tail that literally begins `[` + lowercase, and
the measured worst case on a real dialogue reply was **3 bytes**. A trailing partial ` `` ` is held
back on the same principle so a fence is never half-seen.

At `flush()` (end of reply) an unterminated candidate is released verbatim: `trailing [v2` speaks
and displays as `trailing [v2`. **Nothing is ever lost.**

### 3.2 Why this does not delay speech

The splitter emits when it finds a sentence end or, for the first chunk only, an early break
(`sentence_splitter.cpp:99-110`). Held-back bytes are at the tail, so they can only postpone a break
that would have fallen *inside the marker itself* — and a marker contains no comma, no terminator
and no whitespace, so no break can fall inside one. The marker is invisible to the word count
because it is removed before the splitter ever sees it. Probe: first-chunk latency moved by exactly
the marker's own 5 bytes.

Both extremes are covered: one byte per delta (probe) and the entire reply in one delta
(`claude_code_client.cpp:264`) produce identical output.

### 3.3 The defect the probe found — do not use `flush()`

The obvious way to make a directive a sentence boundary is to call `SentenceSplitter::flush()` when
one arrives. **That is wrong**, and the probe caught it on the first run.

`flush()` (`sentence_splitter.cpp:69-78`) clears `in_code_` **and** sets `emitted_any_ = false`
(line 77). Resetting `emitted_any_` re-arms the early-chunk rule
(`sentence_splitter.cpp:108`) for the rest of the reply, so every subsequent utterance gets cut at
its first comma. Observed:

```
"Sure. [v2] Hi, I am Bob. [v1] And that was Bob."
  one delta   : v1: Sure. | v2: Hi, I am Bob. | v1: And that was Bob.
  byte-wise   : v1: Sure. | v2: Hi,  | v2: I am Bob. | v1: And that was Bob.   <-- wrong
```

Clearing `in_code_` is the second, quieter half of the same bug: it would desynchronise the
splitter's fence state from the filter's.

**Fix, verified:** a separate `break_now()` that emits the pending buffer as an utterance and keeps
`in_code_` and `emitted_any_` intact.

```cpp
// core/sentence_splitter.h, beside flush()
void break_now();  // emit what is pending as one utterance; keep stream state

// core/sentence_splitter.cpp
void SentenceSplitter::break_now() {
  scan(false);
  if (!in_code_) {
    std::string rest = trim(buf_);
    if (!rest.empty()) { emit_sentence(rest); buf_.clear(); }
  }
}
```

With that substituted, the probe's two runs match and all checks pass.

### 3.4 A pre-existing non-determinism, for the record

While proving the above: **the shipped splitter already speaks a reply differently depending on
delta size**, independent of M13. `sentence_splitter.cpp:108` consults `find_early_break` only when
no full sentence end is present, so a first sentence that arrives whole is never cut early, while
the same sentence arriving byte-wise is cut at its first comma. This is not caused by directives and
is not proposed to be fixed here — it is recorded so nobody later blames M13 for it.

---

## 4. What `SpeechQueue` needs

### 4.1 The signature

**An overload, not a changed signature.**

```cpp
// core/speech_queue.h
void enqueue(const std::string& sentence);                 // unchanged; == enqueue(s, 1)
void enqueue(const std::string& sentence, int voice_slot); // 1-based; 1 is the primary

// The per-language lists. Slot 1 is never in them: it is the engine's own
// configured voice (cfg.kokoro_sid / cfg.vv_style), one owner of record.
// Slot N>1 indexes en[N-2] / ja[N-2]. Empty lists == today's behaviour exactly.
void set_voices(std::vector<int> en, std::vector<int> ja);
```

Reasons an overload wins over changing `enqueue`:

- The other caller, `voice_session.cpp:1999` (the announcement/scheduled-report path), keeps working
  untouched, and so do `src/main.cpp:202` and `src/main.cpp:282`.
- "A missing setting means exactly today's behaviour" falls out of `voice_slot = 1` by default,
  which is the rule this file keeps everywhere.

`queue_` (`speech_queue.h:58`) becomes `std::deque<Utterance>` with
`struct Utterance { std::string text; int voice = 1; };`.

**The slot, not the engine id, travels.** Resolution needs the language, and the language is only
known inside `run()` after `split_by_script` (`speech_queue.cpp:69`) — a single sentence can carry
both. So `SpeechQueue` holds the lists and resolves per run.

### 4.2 What it does to the rest of the class

- **`clear()`** (`speech_queue.cpp:35-42`): unchanged. It clears the deque and bumps `generation_`;
  neither cares what is in the elements. **Mute and stop therefore kill a multi-voice passage
  exactly as they kill a single-voice one**, which is what M13 asks for, at no cost.
- **`idle()`** (`speech_queue.cpp:44-47`): unchanged.
- **Mute at the splitter** (`voice_session.cpp:1748`): unchanged in substance. The emit callback
  gains a voice parameter and still returns early when `muted_`. Mute stays voice-only because the
  transcript path is fed separately — the argument at `voice_session.cpp:1740-1746` survives intact.
- **Sentence-level chunking**: unchanged, because a directive is a hard boundary (§3.3), so a voice
  never changes *within* an enqueued utterance. This is the property that keeps the change small.
- **The voice-switch beat needs widening.** `speech_queue.cpp:68,86` gaps 120 ms when
  `spoke_last != engine`. Two Kokoro voices are the *same* engine pointer, so a change from v1 to v2
  in English would get no beat — and the beat is arguably more needed there, since the listener has
  no language change to cue them. `spoke_last` must become the pair `{engine, native_voice}`.

### 4.3 What `TtsEngine` needs

`TtsEngine::synthesize` (`tts/tts_engine.h:20`) has no voice parameter; the id is baked in at
construction (`kokoro_tts.cpp:11`, `voicevox_tts.cpp:13`). Add a non-pure overload so nothing else
has to change:

```cpp
// tts/tts_engine.h
virtual bool synthesize(const std::string& text, AudioChunk& out) = 0;   // the configured voice
virtual bool synthesize_as(const std::string& text, int native_voice, AudioChunk& out) {
  return synthesize(text, out);                                          // engines that cannot: primary
}
```

Both implementations are one substituted argument and **no engine state is mutated**, which matters:
a `set_voice()` API would be a trap the moment anything but the worker thread calls it.

- Kokoro: `SherpaOnnxOfflineTtsGenerate(tts_, text, native_voice < 0 ? sid_ : native_voice, speed_)`
  — `kokoro_tts.cpp:42`. Free.
- VOICEVOX: the same substitution for `style_` at `voicevox_tts.cpp:60`.

### 4.4 A constraint on M13.2 found while reading

**VOICEVOX loads exactly one model file**: `models/vvms/0.vvm`, hard-coded at
`voicevox_tts.cpp:41-46`. So a `"ja"` voice list may only contain **style ids present in `0.vvm`**.
Anything else will fail inside `voicevox_synthesizer_tts` at run time — after the utterance has been
dequeued, i.e. as silence. The `ja` list needs validation at load, or every miss must fall back to
`style_` and log, which is what M13's "a slot that does not exist falls back to that language's
primary, logged" already requires. Inferred from reading the loader, not tested against a bad id.

### 4.5 Where the level lives, and why "every reply starts on the primary" is free

The `SentenceSplitter` is constructed **inside `run_turn`**, at `voice_session.cpp:1747`, so it is
already per-turn. Put the `DirectiveFilter` and the `int voice_level_ = 1` beside it as turn-local
locals and the rule costs nothing: a new reply gets a new filter and a fresh level by construction.
There is no session-lifetime state to reset and therefore no reset to forget.

---

## 5. The grammar

```
directive := '[' token ( ':' value )? ']'
token     := [a-z] [a-z0-9_]{0,15}
value     := [A-Za-z0-9._-]{1,15}
```

- No whitespace anywhere inside. No nesting. Case-sensitive: the token is lowercase-only.
- Hard cap **32 bytes** including both brackets; over-long is prose. The cap is what bounds the
  streaming holdback, so it is load-bearing, not cosmetic.
- The value is **not** lowercase-only, so `[speed:0.8]` and a future `[emote:Happy]` both parse.
- **A `[` that fails the grammar is prose**, emitted verbatim, and scanning resumes at the character
  *after* that `[` — not after the failure point, so a stray bracket cannot swallow a real directive
  later in the sentence.
- **A directive that parses but the build does not recognise is consumed, logged, and never
  spoken.** This is the whole point of the namespace and it includes a *known token in an unknown
  shape*: see `[v2:extra]` below.

### 5.1 The decided cases — all twenty-one probe-confirmed

| Input | Displayed & spoken | Directives | Why |
|---|---|---|---|
| `[v2] hello` | `hello` | `v2` | the base case |
| `[v2] [v3] text` | `text` | `v2`, `v3` | both consumed in order; net level 3. The buffer between them is whitespace and is dropped by `has_speakable_content` (`sentence_splitter.cpp:120`), so no empty utterance is enqueued |
| `[V2] hello` | `[V2] hello` | — | uppercase fails at the first token character. Same rule that keeps `[WARN]` prose |
| `[v2 ] hello` | `[v2 ] hello` | — | a space ends the token and the next char is not `]` or `:` |
| `[]` | `[]` | — | empty token. Spoken path drops it anyway as unspeakable; the transcript keeps it |
| `[v2:extra]` | *nothing* | `v2:extra` | **parses**, so it is machine traffic and must not be read aloud. `v2` takes no value, so this is an **unknown shape**: consumed, logged, **and the voice level is not changed.** Treating it as prose would read "v2 extra" out loud, which is the exact failure the design forbids |
| `[v2:] x` | `[v2:] x` | — | a `:` with no value fails the grammar |
| `[averyverylong…] x` | verbatim | — | over the 32-byte cap |
| `[see figure 1]` | verbatim | — | space after the token |
| `[WARN]` | verbatim | — | uppercase |
| `done [v2]` | `done` | `v2` | marker at end of reply: consumed at `flush()`, logged, nothing emitted, level irrelevant |
| `line\n[v2]\nnext` | `line` / `next` | `v2` | marker alone on a line. The leading `\n` already ended a sentence; the trailing `\n` leaves an empty buffer that is dropped |
| `trailing [` | `trailing [` | — | unterminated: released verbatim at `flush()` |
| `trailing [v`, `trailing [v2` | verbatim | — | same; **nothing is lost** |
| `` code ```x [v2] y``` end `` | verbatim | — | fenced; scanning is off |
| `nested [see [v2] figure]` | `nested [see  figure]` | `v2` | the outer `[` fails, scanning resumes one character later and finds a real `[v2]`. A consequence of the resume rule, decided and documented |
| `[pause]`, `[emote:happy]`, `[speed:0.8]` | *nothing* | as written | future citizens; today all three are unknown, so all three are consumed and logged |

### 5.2 The literal-bracket residual, stated honestly

If the user asks the model to write the literal text `[v2]` **outside a code fence**, it will be
eaten. Two protections cover the realistic cases — the grammar (which rejects everything a human
would naturally write in brackets) and fences (which cover every request to *show* markup) — and
they are, in this note's judgement, enough for M13.1.

**There is no escape sequence, and that is a recommendation, not an oversight.** `[[v2]]` → `[v2]`
is one branch in `match()` if it is ever wanted, but adding it now buys a rule nobody has asked for
and a second thing to explain to the model in a prompt that §6 argues must stay one line. Record the
limitation; add the escape when a real reply hits it.

---

## 6. M13.3 — the prompt half

### 6.1 What the machinery actually is

Verified: the system prompt is **not** a C++ literal. It is Markdown assets composed at launch.

- The block template lives at `assets/prompts/system/workers.md:8-18` — nine verb lines inside a
  fenced ` ```aii ` block. Smaller ones at `assets/prompts/system/scripts.md:7-10` and
  `assets/prompts/system/settings.md:7-10`.
- Composed by `aii::system_prompt()` at `src/core/prompt_store.cpp:661-725`, graph at
  `assets/prompts/graph.json:6-45`, seeded at `prompt_store.cpp:108`.
- Delivered as **`--system-prompt` on the command line**, `src/llm/claude_code_client.cpp:113`, set
  at `src/core/engines.cpp:56`.
- Conditionals are `{{#key}}…{{/key}}`, expanded at `prompt_store.cpp:582-644`, and **the truth
  function only knows ToolPolicy group keys** — `prompt_store.cpp:547-561`. Examples:
  `workers.md:1,3,5,7`.
- Substituted digests are the other route: `{{scripts}}` and `{{settings}}`, `prompt_store.cpp:699-709`.

**This is the finding that shapes M13.3.** `{{#voices}}` will *not* work — voices are not a tool
group. Adding one to `core/tool_policy.h` to carry a speech feature would be a category error.
**Use the digest route**: a `{{voices}}` substitution the app fills, exactly as
`set_actions_digest` fills `{{settings}}` (`src/avatar/action_store.cpp:326-354`, called from
`src/avatar/main.cpp:880`), and which expands to **the empty string when no secondary voices are
configured**. That is the mechanism for "only paid when it matters", and the blank-run collapse at
`prompt_store.cpp:565-578` already removes the gap a dropped block leaves.

### 6.2 What the measurements forbid

- `personal/MANAGER-HANDOFF.md:263-265`: *"Prompt length is not free. Every attempt to add prose for a
  residual case bought hallucinated readings or spurious queries."*
- `docs/MILESTONES.md:1936-1937`: three prompt wordings written and measured against the `cwd`
  hallucination, *"none of them moved it"*.
- `src/core/prompt_store.h:263-268` and `src/avatar/action_store.cpp:336-340`: **the row and the
  syntax line are what move this model; surrounding prose bought hallucinated readings.**

So: **the lever is the syntax line. Write the syntax line and almost nothing else.**

### 6.3 Recommended wording

A new node beside the others, `assets/prompts/system/voices.md`, whose entire body is a
`{{voices}}` substitution — so the file is one token of machinery and the app decides whether any
of it is sent. When two English voices and one Japanese are configured, the app emits:

```
You have a second voice. Put [v2] before a line to speak it in that voice and [v1] to
return to your own; every reply starts on [v1].

[v1] Voices available: English 2, Japanese 1.

Use this only when you have been asked to perform a dialogue or read parts. Never in an
ordinary reply.
```

Three elements, each earning its place:

1. **The syntax line**, which the measurements say is the lever, and which shows both markers —
   because `[v1]` is the return and a model given only `[v2]` has no way back.
2. **A count, not a list.** "English 2, Japanese 1" is generated and bounds the model to slots that
   exist, without naming voices it cannot hear and would then describe.
3. **One sentence of restriction**, which is unavoidable: the user's constraint is *"only called
   upon specifically when the AI requires it"*, and there is no syntax that expresses "rarely". Two
   short sentences, negative-form, no examples, no rationale — the shape `prompt_store.h:263-268`
   says works.

**Cost.** By `estimate_tokens` (`text_util.cpp:152-167`, English at 3.6 chars/token), the block is
~300 characters ≈ **83 tokens per turn**, and **0 tokens when the `voices` setting is absent or
empty**, which is the default and therefore what every existing user pays. For scale, `settings.md`
is budgeted at 1,201 bytes of prose (`docs/design-scripts.md:494-497`); this is a quarter of that.
It is also inside the cached prefix — `--system-prompt` is constant for a session
(`prompt_store.h:27-34`: not hot-reloadable by design) — so after the first turn it is a cache read,
not a fresh charge. **Inferred from the caching model, not measured here**; `cache_read_tokens` is
already tracked at `claude_code_client.cpp:243` and would confirm it in one run.

**What to measure before believing this.** M3.13's lesson is that wording is not argued, it is
tested. Run ten ordinary turns with the block present and count how many volunteer a `[v2]` nobody
asked for. If any do, the fix is to cut sentence 3 to its first clause — not to add a paragraph
explaining when not to.

---

## 7. Where this contradicts, or strains, the committed design

Nothing in the committed design turned out to be impossible. Four things need recording:

1. **`flush()` cannot be the boundary** (§3.3). Measured, not argued. `break_now()` is the
   one addition the committed design did not anticipate, and it is in `core/`, owned by nobody
   currently executing.
2. **`[v2:extra]` had to be decided against the obvious reading.** The design says a directive is
   "a lowercase token, an optional `:value`", which makes `[v2:extra]` a *valid* directive of an
   unknown shape. It is therefore consumed and logged, **not** treated as prose. Anything else reads
   "v2 extra" aloud. This is a strengthening of the unknown-directive rule, not a departure from it.
3. **The 120 ms voice-switch beat is keyed to the engine, not the voice**
   (`speech_queue.cpp:68,86`), so it silently does nothing between two Kokoro voices — the case M13
   exists to create. Small fix, easy to miss, and its absence would be heard as the splice the
   comment at `speech_queue.cpp:80-85` was written to prevent.
4. **VOICEVOX loads one `.vvm`** (`voicevox_tts.cpp:41`), which bounds M13.2's `ja` list to styles in
   `0.vvm` more tightly than "a different character is not free" suggests — an unloaded style is not
   expensive, it is a runtime failure that reaches the user as silence.

---

## 8. What M11 and M12 will move under this

Written against `5210de5`. Expect to re-check:

- **M12 owns `src/avatar/voice_session.*`.** The insertion point in §1.1 is inside `run_turn`, which
  is in that file. Line numbers **will** move; the anchors that matter are the delta lambda passed to
  `eng_.llm->turn(...)` and the emit lambda passed to the `SentenceSplitter` constructor, both of
  which are structural and unlikely to disappear.
- **M12 owns `src/core/config.*` and `src/core/engines.*`**, where the `voices` key and the call to
  `set_voices()` belong. M13.2 should land after M12.
- **M11 owns `src/llm/claude_code_client.*`.** This note deliberately changes nothing there, and
  §1.1 rejects that location on its own merits, so M11 landing should not disturb anything here.
- **`src/core/sentence_splitter.*`, `src/core/speech_queue.*`, `src/tts/*` and
  `assets/prompts/**` are owned by neither**, and that is where every change this note proposes
  actually lives except the insertion point itself.

## 9. How to test it without the GUI

**`src/main.cpp:282-296` is already the harness.** The `--speak` path builds a `SentenceSplitter`
and, at `main.cpp:286`, deliberately *"feed[s] one UTF-8 code point at a time so the splitter
behaves as it does with streamed deltas."* That is the streaming question's test rig, already in the
repo, already byte-wise, and it is where M13.1's regression tests belong. Add a `tests/` case in the
shape of `tests/clip_utf8_test.cpp` asserting the §5.1 table, plus the one-delta / one-byte
equivalence assertion the probe used, which is the check that catches the §3.3 class of bug.
