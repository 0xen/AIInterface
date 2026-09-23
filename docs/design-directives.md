# The inline directive channel

This describes the inline directive channel: a way for the model to put short machine
instructions inside its reply text, so they steer the app (for example, switching voice)
without ever being shown or spoken. Every claim about existing code carries a file and a
line.

---

## 0. What the design guarantees

- **Zero holdback for text with no `[` in it**, in English and Japanese alike. Nothing is
  delayed unless the buffer's tail could still be the start of a directive.
- **Holdback is bounded by `kMaxDirective` (32 bytes)**, and in practice far smaller: a
  reply containing `[v2]` holds back only the few bytes of the marker itself while its
  brackets are still open.
- **A marker at the head of a reply delays the first spoken chunk by exactly its own byte
  length and not one byte more.**
- **The splitter's own `flush()` cannot be used as the mid-reply boundary.** Using it makes
  the same reply speak differently depending on how the delta stream is chunked. See §3.3
  for why, and for the `break_now()` that replaces it.

---

## 1. The path a reply actually takes

| Step | File:line | What happens |
|---|---|---|
| CLI JSON → delta | `src/llm/claude_code_client.cpp:225-228` | `text_delta` events; `on_delta_(t)` with whatever chunk the CLI sent |
| whole-reply fallback | `src/llm/claude_code_client.cpp:260-264` | if **no** partial deltas arrived, the entire reply is delivered as **one** `on_delta_` call |
| the turn's delta lambda | `src/avatar/voice_session.cpp:1844-1855` | appends to the transcript (1847), flips state to Speaking on the first delta (1848-1852), then `splitter.feed(delta)` (1854) |
| the splitter | `src/core/sentence_splitter.cpp:64-116` | fences, sentence ends, the early first chunk |
| emit | `src/core/sentence_splitter.cpp:118-123` | `strip_markdown`, drop if nothing speakable, call `emit_` |
| the emit lambda | `src/avatar/voice_session.cpp:1747-1760` | **mute is applied here** (1748), `[speak]` log (1758), `speech_->enqueue(s)` (1759) |
| synthesis | `src/core/speech_queue.cpp:53-100` | worker thread; `split_by_script` per sentence; engine by script |

Two consumers, and **they are genuinely separate code paths**: the transcript is built by
string concatenation at `voice_session.cpp:1847` and never passes through the splitter at
all. Nothing that the splitter drops (fenced code, unspeakable chunks) is missing from the
transcript, and nothing the transcript shows is guaranteed to be spoken. Any design that
strips in one place only will leak into the other.

### 1.1 The insertion point

**In the turn's delta lambda, `src/avatar/voice_session.cpp:1844-1855`, in front of both
consumers.** A `DirectiveFilter` sits beside the splitter at `voice_session.cpp:1747`; the
lambda feeds it once, and the filter's two callbacks drive the two consumers:

```
filter.OnText(x)      -> { lock mutex_; lines_.back().text += x; }   // was voice_session.cpp:1847
                         splitter.feed(x);                            // was voice_session.cpp:1854
filter.OnDirective(t,v) -> splitter.break_now();                      // §3.3
                           apply(t, v) or log an unknown one
```

One parser, fed once, serving both consumers — which is what makes "markers reach neither
the eyes nor the ears" a property of the design rather than two strippers that have to
agree.

**One detail that must not move.** The `first` flag at `voice_session.cpp:1848-1852`
(status → `speaking...`, state → Speaking) stays on the **raw** delta, before
`filter.feed()`. A reply that opens with `[v2]` would otherwise hold the UI in
`thinking...` for an extra delta.

**Why not elsewhere:**

- *Inside `SentenceSplitter`.* Only serves the ears. The transcript at
  `voice_session.cpp:1847` would still show every marker. It also grows a voice concept
  into a clean core primitive with three call sites — `voice_session.cpp:1747`,
  `src/main.cpp:202`, `src/main.cpp:282` — two of which have nothing to do with voices.
- *Inside `ClaudeCodeClient` at `claude_code_client.cpp:228`.* That file is the LLM
  transport and should not know about speech. `worker_pool.cpp:105` builds a second client
  with its own prompt whose replies would silently inherit directive parsing if the filter
  lived there.
- *Post-hoc on the finished text, like `strip_aii_blocks`* (`src/core/worker_pool.cpp:359`,
  applied at `voice_session.cpp:1889`). **Cannot work for speech at all** — by the time the
  turn ends the reply has already been spoken. It is viable for the eyes alone, and would
  be a fallback if live transcript stripping were ever judged too risky; it costs markers
  being visible for the whole streaming duration, which is exactly what the ` ```aii `
  block already does today.

---

## 2. How it composes with the existing ` ```aii ` stripping

There are **two** mechanisms and they are not the same one:

1. **Ears.** `SentenceSplitter::scan` (`sentence_splitter.cpp:88-107`) tracks ` ``` ` and
   discards everything between fences. This is generic: it silences *all* fenced code, not
   just `aii`.
2. **Eyes.** `strip_aii_blocks` (`worker_pool.cpp:359-374`) removes only
   ` ```aii … ``` ` spans, applied once at end of turn at `voice_session.cpp:1889` (and at
   `voice_session.cpp:1580`).

**They compose, they do not fight — provided the filter is fence-aware.** This is not
optional. A reply that shows a code sample containing `[v2]` must print it. The splitter
would have silenced it anyway; the transcript would not. So the filter tracks ` ``` ` itself
and passes fenced text through verbatim.

Two `` ` ``-tracking state machines exist in series, which sounds like a divergence risk and
is not: both key off the identical three-backtick token in the identical byte stream, and
the filter emits the fence markers verbatim, so the splitter downstream sees exactly what it
sees today (`code ```x [v2] y``` end` comes through unchanged, with no directive taken).

**A directive is never valid inside a fence.** A model that puts `[v2]` inside a code block
has written code, not a directive.

**Nothing about `strip_aii_blocks` needs to change.** An `aii` block is fenced, so the
filter never looks inside it, and line 1889 still removes it from the transcript afterwards.

---

## 3. Surviving streaming

### 3.1 The rule

**Hold back only a live candidate at the tail of the buffer, and nothing else.**

A directive is `[`, a lowercase token, an optional `:value`, `]`, with no spaces and a hard
cap of `kMaxDirective = 32` bytes including both brackets. Scanning is a single
left-to-right pass:

- No `[` in the buffer → everything is emitted immediately. **Zero holdback**, in English
  and Japanese alike.
- A `[` whose following bytes already **violate** the grammar → prose, emitted immediately,
  scanning resumes at the character *after* that `[`. No waiting: the verdict is reached on
  bytes already in hand. `[see figure 1]` dies at the space after `see`, in the same
  `feed()` call.
- A `[` whose following bytes are still **grammar-legal but unterminated**, and which runs
  to the end of the buffer → this and only this is held back, until the next delta.
- The candidate dies on length: past 32 bytes it is declared prose and released.

So the worst case is 32 bytes of delay, only for a tail that literally begins `[` +
lowercase; an ordinary marker such as `[v2]` costs only its own few bytes. A trailing
partial ` `` ` is held back on the same principle so a fence is never half-seen.

At `flush()` (end of reply) an unterminated candidate is released verbatim: `trailing [v2`
speaks and displays as `trailing [v2`. **Nothing is ever lost.**

### 3.2 Why this does not delay speech

The splitter emits when it finds a sentence end or, for the first chunk only, an early break
(`sentence_splitter.cpp:99-110`). Held-back bytes are at the tail, so they can only postpone
a break that would have fallen *inside the marker itself* — and a marker contains no comma,
no terminator and no whitespace, so no break can fall inside one. The marker is invisible to
the word count because it is removed before the splitter ever sees it.

Both extremes are covered: one byte per delta and the entire reply in one delta
(`claude_code_client.cpp:264`) produce identical output.

### 3.3 Why `flush()` cannot be the boundary

The obvious way to make a directive a sentence boundary is to call
`SentenceSplitter::flush()` when one arrives. **That is wrong.**

`flush()` (`sentence_splitter.cpp:69-78`) clears `in_code_` **and** sets
`emitted_any_ = false` (line 77). Resetting `emitted_any_` re-arms the early-chunk rule
(`sentence_splitter.cpp:108`) for the rest of the reply, so every subsequent utterance gets
cut at its first comma:

```
"Sure. [v2] Hi, I am Bob. [v1] And that was Bob."
  one delta   : v1: Sure. | v2: Hi, I am Bob. | v1: And that was Bob.
  byte-wise   : v1: Sure. | v2: Hi,  | v2: I am Bob. | v1: And that was Bob.   <-- wrong
```

Clearing `in_code_` is the second, quieter half of the same problem: it would desynchronise
the splitter's fence state from the filter's.

**The fix** is a separate `break_now()` that emits the pending buffer as an utterance and
keeps `in_code_` and `emitted_any_` intact:

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

With that substituted, the two runs above match and all checks pass.

### 3.4 A pre-existing non-determinism, for the record

Independent of the directive channel: **the splitter already speaks a reply differently
depending on delta size.** `sentence_splitter.cpp:108` consults `find_early_break` only when
no full sentence end is present, so a first sentence that arrives whole is never cut early,
while the same sentence arriving byte-wise is cut at its first comma. The directive channel
does not cause this and does not fix it; it is recorded here so it is not mistaken for a
side effect of this design.

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

- The other caller, `voice_session.cpp:1999` (the announcement/scheduled-report path),
  keeps working untouched, and so do `src/main.cpp:202` and `src/main.cpp:282`.
- "A missing setting means exactly today's behaviour" falls out of `voice_slot = 1` by
  default, which is the rule this design keeps everywhere.

`queue_` (`speech_queue.h:58`) is a `std::deque<Utterance>` with
`struct Utterance { std::string text; int voice = 1; };`.

**The slot, not the engine id, travels.** Resolution needs the language, and the language is
only known inside `run()` after `split_by_script` (`speech_queue.cpp:69`) — a single
sentence can carry both. So `SpeechQueue` holds the lists and resolves per run.

### 4.2 What it does to the rest of the class

- **`clear()`** (`speech_queue.cpp:35-42`): unchanged. It clears the deque and bumps
  `generation_`; neither cares what is in the elements. **Mute and stop therefore kill a
  multi-voice passage exactly as they kill a single-voice one**, at no extra cost.
- **`idle()`** (`speech_queue.cpp:44-47`): unchanged.
- **Mute at the splitter** (`voice_session.cpp:1748`): unchanged in substance. The emit
  callback gains a voice parameter and still returns early when `muted_`. Mute stays
  voice-only because the transcript path is fed separately — the argument at
  `voice_session.cpp:1740-1746` survives intact.
- **Sentence-level chunking**: unchanged, because a directive is a hard boundary (§3.3), so
  a voice never changes *within* an enqueued utterance. This is the property that keeps the
  change small.
- **The voice-switch beat needs widening.** `speech_queue.cpp:68,86` gaps 120 ms when
  `spoke_last != engine`. Two Kokoro voices are the *same* engine pointer, so a change from
  v1 to v2 in English gets no beat — and the beat is arguably more needed there, since the
  listener has no language change to cue them. `spoke_last` must become the pair
  `{engine, native_voice}`.

### 4.3 What `TtsEngine` needs

`TtsEngine::synthesize` (`tts/tts_engine.h:20`) has no voice parameter; the id is baked in at
construction (`kokoro_tts.cpp:11`, `voicevox_tts.cpp:13`). A non-pure overload covers it
without changing anything else:

```cpp
// tts/tts_engine.h
virtual bool synthesize(const std::string& text, AudioChunk& out) = 0;   // the configured voice
virtual bool synthesize_as(const std::string& text, int native_voice, AudioChunk& out) {
  return synthesize(text, out);                                          // engines that cannot: primary
}
```

Both implementations are one substituted argument and **no engine state is mutated**, which
matters: a `set_voice()` API would be a trap the moment anything but the worker thread calls
it.

- Kokoro: `SherpaOnnxOfflineTtsGenerate(tts_, text, native_voice < 0 ? sid_ : native_voice, speed_)`
  — `kokoro_tts.cpp:42`. Free.
- VOICEVOX: the same substitution for `style_` at `voicevox_tts.cpp:60`.

### 4.4 A constraint this puts on the voice list

**VOICEVOX loads exactly one model file**: `models/vvms/0.vvm`, hard-coded at
`voicevox_tts.cpp:41-46`. So a `"ja"` voice list may only contain **style ids present in
`0.vvm`**. Anything else fails inside `voicevox_synthesizer_tts` at run time — after the
utterance has been dequeued, i.e. as silence. The `ja` list needs validation at load, or
every miss must fall back to `style_` and log: a slot that does not exist falls back to that
language's primary, logged.

### 4.5 Where the level lives, and why "every reply starts on the primary" is free

The `SentenceSplitter` is constructed **inside `run_turn`**, at `voice_session.cpp:1747`, so
it is already per-turn. The `DirectiveFilter` and the `int voice_level_ = 1` sit beside it as
turn-local locals, so the rule costs nothing: a new reply gets a new filter and a fresh level
by construction. There is no session-lifetime state to reset and therefore no reset to
forget.

---

## 5. The grammar

```
directive := '[' token ( ':' value )? ']'
token     := [a-z] [a-z0-9_]{0,15}
value     := [A-Za-z0-9._-]{1,15}
```

- No whitespace anywhere inside. No nesting. Case-sensitive: the token is lowercase-only.
- Hard cap **32 bytes** including both brackets; over-long is prose. The cap is what bounds
  the streaming holdback, so it is load-bearing, not cosmetic.
- The value is **not** lowercase-only, so `[speed:0.8]` and a future `[emote:Happy]` both
  parse.
- **A `[` that fails the grammar is prose**, emitted verbatim, and scanning resumes at the
  character *after* that `[` — not after the failure point, so a stray bracket cannot
  swallow a real directive later in the sentence.
- **A directive that parses but the build does not recognise is consumed, logged, and never
  spoken.** This is the whole point of the namespace and it includes a *known token in an
  unknown shape*: see `[v2:extra]` below.

### 5.1 The decided cases

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
| `nested [see [v2] figure]` | `nested [see  figure]` | `v2` | the outer `[` fails, scanning resumes one character later and finds a real `[v2]`. A consequence of the resume rule |
| `[pause]`, `[emote:happy]`, `[speed:0.8]` | *nothing* | as written | reserved for future use; today all three are unknown, so all three are consumed and logged |

### 5.2 The literal-bracket residual, stated honestly

If the user asks the model to write the literal text `[v2]` **outside a code fence**, it
will be eaten. Two protections cover the realistic cases: the grammar (which rejects
everything a human would naturally write in brackets) and fences (which cover every request
to *show* markup). Together they are judged enough.

**There is no escape sequence, and that is deliberate.** `[[v2]]` → `[v2]` would be one
branch in `match()` if it were ever wanted, but adding it now buys a rule nobody has asked
for and a second thing to explain to the model in a prompt that §6 keeps to one line. The
limitation is recorded here rather than pre-empted.

---

## 6. The prompt half

### 6.1 What the machinery actually is

The system prompt is **not** a C++ literal. It is Markdown assets composed at launch.

- The block template lives at `assets/prompts/system/workers.md:8-18` — nine verb lines
  inside a fenced ` ```aii ` block. Smaller ones at `assets/prompts/system/scripts.md:7-10`
  and `assets/prompts/system/settings.md:7-10`.
- Composed by `aii::system_prompt()` at `src/core/prompt_store.cpp:661-725`, graph at
  `assets/prompts/graph.json:6-45`, seeded at `prompt_store.cpp:108`.
- Delivered as **`--system-prompt` on the command line**, `src/llm/claude_code_client.cpp:113`,
  set at `src/core/engines.cpp:56`.
- Conditionals are `{{#key}}…{{/key}}`, expanded at `prompt_store.cpp:582-644`, and **the
  truth function only knows ToolPolicy group keys** — `prompt_store.cpp:547-561`. Examples:
  `workers.md:1,3,5,7`.
- Substituted digests are the other route: `{{scripts}}` and `{{settings}}`,
  `prompt_store.cpp:699-709`.

`{{#voices}}` does **not** work for this feature — voices are not a tool group, and adding
one to `core/tool_policy.h` to carry a speech feature would be a category error. **The digest
route is used instead**: a `{{voices}}` substitution the app fills, exactly as
`set_actions_digest` fills `{{settings}}` (`src/avatar/action_store.cpp:326-354`, called from
`src/avatar/main.cpp:880`), and which expands to **the empty string when no secondary voices
are configured**. That is the mechanism for "only paid when it matters", and the blank-run
collapse at `prompt_store.cpp:565-578` already removes the gap a dropped block leaves.

### 6.2 What experience with this model forbids

Prompt length is not free for this model: prose added to cover a residual case has reliably
bought hallucinated readings or spurious queries rather than fixing the case. The syntax
line and the row it lives in are what move the model (`src/core/prompt_store.h:263-268` and
`src/avatar/action_store.cpp:336-340`); surrounding explanation does not.

So: **the lever is the syntax line. Write the syntax line and almost nothing else.**

### 6.3 Wording

A node beside the others, `assets/prompts/system/voices.md`, whose entire body is a
`{{voices}}` substitution — so the file is one token of machinery and the app decides
whether any of it is sent. When two English voices and one Japanese are configured, the app
emits:

```
You have a second voice. Put [v2] before a line to speak it in that voice and [v1] to
return to your own; every reply starts on [v1].

[v1] Voices available: English 2, Japanese 1.

Use this only when you have been asked to perform a dialogue or read parts. Never in an
ordinary reply.
```

Three elements, each earning its place:

1. **The syntax line**, which shows both markers — because `[v1]` is the return and a model
   given only `[v2]` has no way back.
2. **A count, not a list.** "English 2, Japanese 1" is generated and bounds the model to
   slots that exist, without naming voices it cannot hear and would then describe.
3. **One sentence of restriction**: the feature should be used only when the assistant is
   specifically asked to perform a dialogue or read parts, and there is no syntax that
   expresses "rarely" more compactly than saying so directly. Two short sentences,
   negative-form, no examples, no rationale — the shape `prompt_store.h:263-268` says works.

**Cost.** By `estimate_tokens` (`text_util.cpp:152-167`, English at 3.6 chars/token), the
block is ~300 characters ≈ **83 tokens per turn**, and **0 tokens when the `voices` setting
is absent or empty**, which is the default and therefore what every existing user pays. For
scale, `settings.md` is budgeted at 1,201 bytes of prose (`docs/design-scripts.md:494-497`);
this is a quarter of that. It is also inside the cached prefix — `--system-prompt` is
constant for a session (`prompt_store.h:27-34`: not hot-reloadable by design) — so after the
first turn it is a cache read, not a fresh charge. `cache_read_tokens` is tracked at
`claude_code_client.cpp:243` and can confirm this in one run.

**What to check before trusting the wording.** Wording for this model is not argued, it is
tested: run ordinary turns with the block present and count how many volunteer a `[v2]`
nobody asked for. If any do, the fix is to cut sentence 3 to its first clause — not to add a
paragraph explaining when not to.

---

## 7. Points worth flagging explicitly

1. **`flush()` cannot be the boundary** (§3.3). `break_now()` is the addition that a naive
   reading of the sentence-splitter design would miss, and it lives in `core/`.
2. **`[v2:extra]` is decided against the obvious reading.** A directive is "a lowercase
   token, an optional `:value`", which makes `[v2:extra]` a *valid* directive of an unknown
   shape. It is therefore consumed and logged, **not** treated as prose. Anything else reads
   "v2 extra" aloud. This is a strengthening of the unknown-directive rule, not a departure
   from it.
3. **The 120 ms voice-switch beat is keyed to the engine, not the voice**
   (`speech_queue.cpp:68,86`), so it silently does nothing between two Kokoro voices — the
   case multi-voice output exists to create. Its absence would be heard as the splice the
   comment at `speech_queue.cpp:80-85` was written to prevent.
4. **VOICEVOX loads one `.vvm`** (`voicevox_tts.cpp:41`), which bounds the `ja` list to
   styles in `0.vvm` more tightly than "a different character is not free" suggests — an
   unloaded style is not expensive, it is a runtime failure that reaches the user as
   silence.

---

## 8. What this touches, and what it deliberately leaves alone

- **The insertion point (§1.1) is inside `run_turn`**, in `src/avatar/voice_session.cpp`.
  The anchors that matter are the delta lambda passed to `eng_.llm->turn(...)` and the emit
  lambda passed to the `SentenceSplitter` constructor; both are structural.
- **`src/core/config.*` and `src/core/engines.*`** hold the `voices` key and the call to
  `set_voices()`.
- **`src/llm/claude_code_client.*` is deliberately untouched.** §1.1 rejects putting the
  filter there on its own merits, independent of who else is working in that file.
- **`src/core/sentence_splitter.*`, `src/core/speech_queue.*`, `src/tts/*` and
  `assets/prompts/**`** are where the rest of the change actually lives, outside the
  insertion point itself.

## 9. How to test it without the GUI

**`src/main.cpp:282-296` is a harness for exactly this.** The `--speak` path builds a
`SentenceSplitter` and, at `main.cpp:286`, deliberately feeds one UTF-8 code point at a time
so the splitter behaves as it does with streamed deltas. `tests/directive_filter_test.cpp`
is the regression test built on this: it asserts the §5.1 table and the one-delta /
one-byte-at-a-time equivalence that §3.3's bug class would break, both against the real
`DirectiveFilter` and a copy of `SentenceSplitter`. `tests/sentence_splitter_test.cpp` covers
the splitter on its own.
