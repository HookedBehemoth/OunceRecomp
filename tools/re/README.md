# tools/re

## re

`tools/re/re [--game games/oot|games/dkb] <command>` reads one game's executable as compact text, each output
within a fixed budget. Start here; the project skill `.claude/skills/re` and the `re` agent (`.claude/agents/re.md`,
a cheap model) build on it.

| command | what it prints |
|---|---|
| `fn <addr\|label>` | the function's annotated listing: blocks, callee names (with a stored reading's name), strings, globals, NVN procs |
| `tree <fn> [--depth N]` | the callee tree: labelled, SDK, read (the stored pseudo-C's name), or unread |
| `pseudo <fn> [--set <file\|->]` | the stored pseudo-C (the re agent writes it) |
| `callers`, `callees <fn>` | one line per caller (BL, tail B, pointers) or callee |
| `xref <string\|addr>` | code that takes the address (ADRP+ADD/LDR), and pointers to it |
| `field <offset> [--all]` | functions that load, store or add that offset |
| `fieldname <offset> <name>` | a name `fn` shows at that offset |
| `asm <addr> [n]` | raw instructions with their words, for patch sites |
| `pc <fn>` | experimental pseudo-C without a model (pseudo.py) |
| `export` | refresh the lifter's view |

The lifter's view (function bodies, jump tables, PLT, symbols) comes from `ounce-lift re-export` and is cached in
`work/re/<game>/export`, with the call and ADRP indexes beside it. Stored pseudo-C lives in `work/re/<game>/pseudo`,
keyed by the function's body hash: a changed body shows it as stale. Its signature line carries the suggested name
that `fn` and `tree` show for the function.

Calls through the game's proc tables show the proc's name: `nvnCommandBufferBindVertexBuffer(...)` in `pc`, `; nvn...`
on the `blr` in `fn`, and `pfn_<proc>` for the slot. The slots come from the game's own loaders (a call with a proc
name string as its argument whose result is stored to a fixed address, as in nvnDeviceGetProcAddress loops), cached in
`work/re/<game>/procs.tsv`.

The exefs is `--exefs`, `OUNCE_RE_EXEFS`, the first line of `work/<game>/dump.txt`, or (OoT) `work/dump.txt`.
