# Reverse engineering

This page shows you where to look when you need to understand a piece of the game's code or data, and how to record
what you found so profiles and backtraces show it. Read it before you analyze a game function or write a patch for one.

Everything these tools read or write comes from your dump and stays on your PC.

## Read a function

- **Lifted output:** `ounce/target/release/ounce-lift dump --game games/oot <address or label>` writes the guest code, the IR
  and the LLVM IR to a folder per address and prints the function's summary
  ([Build and iterate](lifter-build.md#inspect-a-function)).
- **A patch site:** `ounce/target/release/ounce-lift site --game games/oot <address or label>` shows the instruction, the paths
  into it and the registers live there ([Write a patch](patches.md)).
- **Exports and module layout:** `work/<game>/lift/image/image.json` lists the image's modules and exported symbols. The main
  module is loaded at `0x80020000`.
- **Searches in the main module:** the scripts in `tools/re` disassemble around an offset, find code that references
  an address or a string, and find a function's callers ([tools/re](../tools/re/README.md)).
- **The game's files:** `tools/dump` reads the game's formats (BNTX, BFRES, BFSHA, AINB, BYML, SARC, `.zs`) and
  patches sequence graphs for test boots ([tools/dump](../tools/dump/README.md)).

A call site, a return address or a patch site lies inside a function. To find which, look the address up in
the build's link map, `work/<game>/lift/<exe>.map`.

## Label a function

Lifted functions are named `f_<guest address>`. `games/oot/labels.txt` gives the ones we understand a label. After
the link, `tools/re/labels.py` renames their symbols to `<label>.f_<address>`, so profiles, backtraces and debuggers show
what a function is. Only the symbol table changes; a label costs no rebuild.

Add a label whenever an analysis identifies a function, in the same commit as the note that documents it. Follow these
rules:

- A label is our description, not the game's or the SDK's name, and never C++ mangling. Use lower case, digits and
  underscores, with a subsystem prefix first; the file's header lists the prefixes. Each label is unique.
- A label says what the note says and no more. When a role is all you know, say the role (`report_reader_b`,
  `_caller`). Mark a doubt, or notes that disagree, with `# ?`.
- The address must be a function's entry in this lift. A mid-function site names the function that contains it, with
  the site in the note.
- End each entry with `# <file>[ section]: what`, naming the place that documents it.

To check the list, run `python -I tools/re/labels.py --game games/oot check`. `name <address> ...` prints the labels of
addresses for scripts.
