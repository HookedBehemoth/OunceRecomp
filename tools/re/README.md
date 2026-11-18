# tools/re

## re

`tools/re/re [--game games/oot|games/dkb] <command>` reads one game's executable as compact text, each output
within a fixed budget. Start here; the project skill `.claude/skills/re` and the `re` agent (`.claude/agents/re.md`,
a cheap model) build on it.

| command | what it prints |
|---|---|
| `fn <addr\|label>` | the function's annotated listing: blocks, callee names and their stored notes, strings, globals |
| `tree <fn> [--depth N]` | the callee tree: labelled, SDK, read before (a note), or unread |
| `pseudo <fn> [--set <file\|->]` | the stored pseudo-C (the re agent writes it) |
| `note <fn> [<text\|->]` | the stored summary (name, what, fields, effects, confidence) |
| `callers`, `callees <fn>` | one line per caller (BL, tail B, pointers) or callee |
| `xref <string\|addr>` | code that takes the address (ADRP+ADD/LDR), and pointers to it |
| `field <offset> [--all]` | functions that load, store or add that offset |
| `fieldname <offset> <name>` | a name `fn` shows at that offset |
| `asm <addr> [n]` | raw instructions with their words, for patch sites |
| `pc <fn>` | experimental pseudo-C without a model (pseudo.py) |
| `export` | refresh the lifter's view |

The lifter's view (function bodies, jump tables, PLT, symbols) comes from `ounce-lift re-export` and is cached in
`work/re/<game>/export`, with the call and ADRP indexes beside it. Stored notes and pseudo-C live in
`work/re/<game>/{notes,pseudo}`, keyed by the function's body hash: a changed body shows them as stale. The exefs is
`--exefs`, `OUNCE_RE_EXEFS`, the first line of `work/<game>/dump.txt`, or (OoT) `work/dump.txt`.

## Older scripts

Searches for reverse engineering in the main executable's flat image (`work/re/main.bin`, made from the dump's exefs; `MAIN_BIN=<file>` for
another). Offsets are file offsets (virtual address minus the 0x80020000 base). Run with `work/venv/bin/python`.

| script | what it is |
|---|---|
| `disasm.py <hex_off> [before] [after]` | AArch64 disassembly around an offset, with ADRP+ADD string references named (needs `capstone`) |
| `xref.py <hex_off> ...` or `-s <string> ...` | code that references an address or a string through ADRP+ADD |
| `callers.py <hex_off> ...` | BL/B callers of a function and 8-byte pointers to it |
