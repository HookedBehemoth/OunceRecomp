# tools/re

## re

`tools/re/re --game <game> <command>` reads one game's executable as compact text, each output but `fn` within a
fixed budget. `--game` (`oot`, `dkb` or `games/<game>`) is required; without it `re` stops and names the
games. The project skill `.claude/skills/re` says how to use it.

| command | what it prints |
|---|---|
| `fn <addr\|label>` | the function's whole annotated listing: blocks, callees with their arguments, strings, globals, NVN procs; an error above 6000 instructions, never a cut |
| `tree <fn> [--depth N]` | the callee tree: labelled, SDK, or proc |
| `callers`, `callees <fn>` | one line per caller (BL, tail B, pointers) or callee |
| `xref <string\|addr>` | code that takes the address (ADRP+ADD/LDR), and pointers to it |
| `field <offset> [--all]` | functions that load, store or add that offset |
| `fieldname <offset> <name>` | a name `fn` shows at that offset |
| `asm <addr> [n]` | raw instructions with their words, for patch sites |
| `export` | refresh the lifter's view |

The lifter's view (function bodies, jump tables, PLT, symbols) comes from `ounce-lift re-export` and is cached in
`work/re/<game>/export`, with the call and ADRP indexes beside it.

`fn`'s header gives the function's argument count: the registers x0-x7 it reads before it writes them. At each `bl`,
`blr` and tail `b`, the listing shows the arguments the callee takes, with the values it tracked in address order
(args.py): the parameters of the NVN proc's typedef in `renderer/api/nvn_api.h` or of the SDK symbol's demangled
signature, else the registers set since the previous call.

Calls through the game's proc tables show the proc's name: `; nvn...` on the `blr` in `fn`, `nvn... [proc]` in `tree`
and `callees`, and `pfn_<proc>` for the slot. The slots come from the game's own loaders (a call with a proc name
string as its argument whose result is stored to a fixed address, as in nvnDeviceGetProcAddress loops), cached in
`work/re/<game>/procs.tsv`.

The exefs is `--exefs`, else the game's dump in the user's config (`ounce config set <game>.dump <folder>`).
