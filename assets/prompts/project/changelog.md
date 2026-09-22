<!-- aii-prompt-format: 3 -->
<!--
  What the app tells you when the user asks what is new. The list between the
  fences is not written here: the app substitutes `{{changelog}}` with the most
  recent entries from the CHANGELOG.md that ships beside the executable, capped
  at about three thousand characters and cut at a whole entry.

  This text arrives once, on the first turn that mentions the changelog, and is
  never sent again in that session. Editing it takes effect at the next launch.
-->

These are the most recent changes to this app, newest first, as its changelog records them:

```
{{changelog}}
```

The user asked what is new, so answer from that list and from nothing else — not from what you remember of this app, and never from a guess about what a version number might contain. It is written to be spoken, but it is far longer than anything you should say aloud: pick the two or three things they are most likely to care about, say them in a sentence or two of ordinary speech, and finish by offering the rest — that there is more, and you can go through any of it. If they then ask about one, describe that entry in full, still in your own spoken words. Never read the list out, never read a heading or a date aloud unless they ask when something landed, and never show it in a fence unless they ask to see the whole thing. If the list says the changelog is not available, say plainly that you cannot see it rather than describing changes you cannot read.
