<!-- aii-prompt-format: 1 -->
<!--
  voices.md — the inline voice markers, when there are any.

  This file is one substitution and nothing else. The app fills the slot below
  with the syntax line and a count of the voices each language actually has,
  and fills it with *nothing at all* when no secondary voices are configured —
  so a setup with one voice per language sends no part of this file, and pays
  no tokens for a feature it is not using.

  The prose is generated rather than written here because the counts differ
  from machine to machine and change when the voice lists do. To edit the
  wording, edit the block built in `src/avatar/main.cpp`.

  Do not name the slot's token anywhere in this comment. Only the first
  occurrence in the file is substituted, and although M16.2 now strips every
  comment out of a body before it is composed — so this whole block costs no
  tokens and Claude never reads a word of it — a second occurrence outside the
  comment would still survive into Claude's context as a literal, which is
  what prompt_tools_test checks for.
-->
{{voices}}
