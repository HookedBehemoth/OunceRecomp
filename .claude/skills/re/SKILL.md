---
name: re
description: Reverse engineering of game code (games/oot, games/dkb) with tools/re/re and the re subagent. Use before reading any game function, answering what game code does, finding callers, strings, or fields, or adding labels to games/<game>/labels.txt.
---

# Reverse engineering game code

Reading disassembly in the main session is the most expensive way to understand game code: most of the tokens are
addresses and instructions. Delegate the reading and keep only the pseudo-C.

## Order of work

1. Read `games/<game>/labels.txt` first (grep it for the subsystem). A label with a note may already answer the
   question.
2. Read the stored readings: `tools/re/re --game <g> pseudo <fn>` prints the pseudo-C an earlier agent wrote, and
   `re tree <fn>` shows which callees are labelled, SDK, read (`[read: <suggested name>]`) or `[UNREAD]`.
3. Delegate what is unread. Start an Agent with `subagent_type: "re"` and a narrow prompt: the game, the function
   (address or label) or the question. It reads the listings bottom-up on Sonnet, stores the pseudo-C keyed by
   the function's body hash, and returns only the pseudo-C: the suggested name on the signature line, the confidence in
   the comment above it, `/* ? */` at uncertain spots. Several independent functions can go to parallel re agents.
4. Search yourself only with the one-line commands: `re callers`, `re callees`, `re xref <string|addr>`,
   `re field <offset>`. They are cheap and capped.
5. Read the listing yourself (`re fn`) only to check a claim before you rely on it: a label, a patch, or a
   conclusion the user acts on. `re asm <addr> [n]` (raw words) is for patch sites only, where `word()` checks need
   the exact encoding.

## Labels

- Add a label only after you checked the claim against `re fn`'s listing (or the stored pseudo-C against the listing
  at the lines that matter). A re agent's pseudo-C alone is a reading, not evidence.
- Follow docs/reverse-engineering.md, "Label a function": our description, a subsystem prefix, `# <file>: what`, `# ?`
  for a doubt. Check with `python -I tools/re/labels.py --game games/<g> check`.
- Name a field you are sure of with `re fieldname <offset> <name>` only when the offset is distinctive (well above
  0x100); `re fn` then annotates it everywhere.

## Cost tips

- The listing of a typical function is 30-100 lines. `re fn` prints the whole function, never a cut; above 6000
  instructions it exits with an error instead. Do not page through disassembly with `re asm`.
- Calls through the game's NVN proc slots are named (`nvnCommandBufferBindVertexBuffer`) in `fn`, `tree`, `callees`,
  `pc` and the stored pseudo-C; a call through `g_<addr>` is not through a known proc slot.
- Do not dump whole call trees into the main session. Give the re agent the tree and the question.
- `re pc <fn>` is an experimental deterministic pseudo-C printer (no model). It is cheaper than the listing, but its
  conditions across blocks and its call arguments can be wrong. Check it against `re fn` before you rely on it.
- Nothing here sends game data anywhere but the session's own model. Do not upload listings elsewhere.
