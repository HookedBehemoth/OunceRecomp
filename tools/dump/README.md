# tools/dump

Readers and patchers for the game's own file formats. They read the user's dump in place (the folder named in
`work/dump.txt`, `exefs/` + `romfs/`) and write only where you point them; what they write is made from game files and
stays on this PC. Run them with `work/venv/bin/python` (needs `zstandard`, `numpy`); `.zs` files are decompressed with the
game's dictionaries in `work/zsdic` (`ZSDIC=<folder>` for another).

| script | what it is |
|---|---|
| `make_chisel_overlay.py` | builds a `HOS_FS_OVERLAY` folder whose patched sequence graphs boot straight into a map |
| `ainb.py` | AINB (AI node graph) reader: `ainb.load(bytes)`, `ainb.to_text(graph)`; `python tools/dump/ainb.py <file>` prints it |
| `ainb_patch.py` | patches an AINB's parameters and plugs (`in:`, `prop:`, `plug:`, `unlink:` operations) and writes a copy |
| `byml.py`, `byml_be.py` | BYML readers (little endian; both endiannesses with hash dicts) to Python objects or JSON |
| `sarc.py` | SARC archive unpacker |
| `zs.py` | the game's zstd dictionaries: `zs.decompress(bytes)` |
| `bntx.py` | BNTX texture container reader and block-linear deswizzle (`--check` verifies the layout) |
| `bfres.py` | BFRES model reader: shapes, materials, shader assigns (`-v`: options and samplers) |
| `bfsha.py` | BFSHA shader archive and embedded BNSH reader (`--program MODEL IDX` decodes one program's key) |

How the `work/overlay/chisel_*` folders are made, for example the Kokiri start from a new game:

    work/venv/bin/python tools/dump/make_chisel_overlay.py work/overlay/chisel_kokiri --route newgame --banc KokiriForest_Child
    OVERLAY=$PWD/work/overlay/chisel_kokiri scripts/run-vk.sh name 60 -- RX_BACKEND=direct

`--route load` continues the save instead; `--start NAME` picks a StartPos (the market overlay is a start in
Hyrule Field at the drawbridge, see `docs/native-vk.md`); `--skip-fades` and `--skip-title` cut the loading fades and the title movie.
