---
name: re
description: Reverse engineering of game code (games/oot, games/dkb) with tools/re/re. Use before reading any game function, answering what game code does, finding callers, strings, or fields, or adding labels to games/<game>/labels.txt.
---

# Reverse engineering game code

Read the game's code yourself with `tools/re/re --game <oot|dkb> <command>`. Do not delegate it. `--game` is required:
there is no default game.

## Order of work

1. Read `games/<game>/labels.txt` first (grep it for the subsystem). A label with a note may already answer the
   question.
2. Find the code with the one-line commands (each after `re --game <game>`): `re callers <fn>`, `re tree <fn> [--depth N]`, `re xref <string|addr>`,
   `re field <offset>`.
3. Read the function with `re fn <fn>`: one instruction a line, the blocks marked, and at every call the callee's
   name and its arguments. The arguments are named from the NVN headers or the SDK symbol's signature when there is
   one, else shown as the registers set since the previous call. Values are constants, `&label`, `"string"`,
   `&pfn_<proc>`, `argN` (the function's own argument, unchanged), `sp+0x..` (a local), `[x19+0x20]` (a value just
   loaded), or the register itself when the listing does not know it.

## Labels

- Add a label only after you read the listing at the lines that support it.
- Follow docs/reverse-engineering.md, "Label a function": our description, a subsystem prefix, `# <file>: what`, `# ?`
  for a doubt. Check with `python -I tools/re/labels.py --game games/<g> check`.
- Name a field you are sure of with `re fieldname <offset> <name>` only when the offset is distinctive (well above
  0x100); `re fn` then annotates it everywhere.

## Cost tips

- `re fn` prints the whole function, never a cut; above 6000 instructions it exits with an error instead. Read the
  callees that matter one by one, bottom-up, rather than paging through disassembly with `re asm`, which is for patch
  sites only.
- Nothing here sends game data anywhere but the session's own model. Do not upload listings elsewhere.
