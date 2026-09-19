<!--
M3.15. The note the outgoing session writes to the one that replaces it.

**This file is not a system prompt, and it is deliberately not in graph.json.**
It sits here because the prompt store seeds this whole tree into
`%APPDATA%\AIInterface\prompts\`, which is what makes it editable by the
person whose conversation it summarises — but it is never composed into
`--system-prompt` and costs nothing on any turn but the one it is used on.
It is sent, on its own, as the last thing the outgoing session is asked.

Edit the prose below freely. Two things about it are load-bearing rather than
stylistic, and the app cannot enforce either:

  * It must ask for **direction, not wording**. Replaying the conversation is
    M3.6 and was declined as too costly; what carries over here is what we are
    doing, what is settled and what is outstanding.
  * It must not ask for anything spoken. This reply is never read out and
    never reaches the transcript — the user has already been told, in the
    app's own voice, that this is housekeeping.

If this file is missing or empty the app uses a built-in copy of the same
request and says so in the log. It does not skip the handover.
-->

You are about to be replaced by a fresh session of yourself, because this
conversation has used up more of your context window than is comfortable. The
user knows; they have just been told you need a moment.

Write the note that your replacement will read. It gets nothing else — no
transcript, no history, none of this conversation's wording. Only this.

Cover, in a few short paragraphs and in the language this conversation is
being held in:

- what we are doing, and why
- what has been decided, including anything you were asked not to do again
- what is outstanding, or was about to happen next
- anything the user has told you about themselves or their setup that you
  would be embarrassed to have to ask for twice

Write it as notes to yourself, not as a message to the user. No greeting, no
sign-off, no preamble about writing a summary, and nothing about the handover
itself. Leave out anything you would not miss.
