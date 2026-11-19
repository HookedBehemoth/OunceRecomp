# tools/dump

Readers and patchers for the game's own file formats, for when you need to look inside a game file or make a test
overlay. They read the user's dump in place (the folder the config names, `ounce config get oot.dump`, with
`exefs/` + `romfs/`) and write only where you point them; what they write is made from game files and
stays on this PC. Run them with `work/venv/bin/python` (needs `zstandard`, `numpy`); `.zs` files are decompressed with the
game's dictionaries in `work/zsdic` (`ZSDIC=<folder>` for another).

| script | what it is |
|---|---|
| `make_chisel_overlay.py` | builds a `fs.overlay` folder whose patched sequence graphs boot straight into a map |
| `make_zoo.py` | builds an actor-zoo overlay from a scene description (`games/oot/zoo/*.toml`): the chiseled boot into an existing map whose bancs hold the scene's actors |
| `ainb.py` | AINB (AI node graph) reader: `ainb.load(bytes)`, `ainb.to_text(graph)`; `python tools/dump/ainb.py <file>` prints it |
| `ainb_patch.py` | patches an AINB's parameters and plugs (`in:`, `prop:`, `plug:`, `unlink:` operations) and writes a copy |
| `byml.py`, `byml_be.py` | BYML readers (little endian; both endiannesses with hash dicts) to Python objects or JSON |
| `byml_write.py` | BYML v7 writer, with a typed reader whose documents it writes back with the same nodes (the game's bancs at their own size) |
| `sarc.py` | SARC archive unpacker |
| `zs.py` | the game's zstd dictionaries: `zs.decompress(bytes)` |
| `bntx.py` | BNTX texture container reader and block-linear deswizzle (`--check` verifies the layout) |
| `bfres.py` | BFRES model reader: shapes, materials, shader assigns (`-v`: options and samplers) |
| `bfsha.py` | BFSHA shader archive and embedded BNSH reader (`--program MODEL IDX` decodes one program's key) |

To make a chiseled boot into Kokiri Forest from a new game and run it:

    work/venv/bin/python -I tools/dump/make_chisel_overlay.py work/overlay/chisel_kokiri --route newgame --banc KokiriForest_Child
    ounce run oot --set fs.overlay=$PWD/work/overlay/chisel_kokiri

`--route load` continues the save instead; `--start NAME` picks a start position (the market overlay is a start in
Hyrule Field at the drawbridge); `--skip-fades` and `--skip-title` cut the loading fades and the title movie.
[Test in the game](../../docs/testing.md#start-in-a-scene) shows how runs and smoke tests use the overlays.
