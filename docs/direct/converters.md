# The asset converters

This page describes the build-time tools that turn the game's textures, terrain tiles and sky tables into the linear
data the back end uploads. Read it when you set up a new machine, when a texture draws as a placeholder, or before you
change a prepared format.

The back end has no block-linear code, no ASTC decoder and no run-time conversion. Every texture and terrain tile is
prepared by these tools, or it draws as a placeholder and is counted.

## Rules all converters keep

- They read the romfs in place (default: the path in `work/dump.txt` plus `/romfs`) and write only under
  `work/prepared/`. What they write is made from the game: it never leaves the PC and is never committed.
- They are incremental: a file that has not changed is skipped. They run niced, with 8 workers by default, so a game
  run that is measuring is not disturbed.
- Each has a `--verify` mode that decodes its output again and compares it with the source.

## Run the converters

1. Prepare the textures:

   ```
   python -I nx2/prepare_textures.py
   python -I nx2/prepare_textures.py --verify 1000
   ```

2. Prepare the terrain tiles (this takes a few minutes for the whole romfs; `--map KokiriForest_01` does one map):

   ```
   python -I nx2/prepare_terrain.py
   python -I nx2/prepare_terrain.py --verify 1000
   ```

3. Prepare the sky tables:

   ```
   python -I nx2/prepare_skybin.py
   ```

`--report` on the texture and terrain tools prints a summary of what is in the output.

## Textures: `nx2/prepare_textures.py`

The tool writes every BNTX texture of the romfs into `work/prepared/tex`: zstd frames in packs, with an index keyed by
the BNTX file path and the texture's index. The format is in the script's docstring.

| Source | Prepared as |
|---|---|
| BC1-BC7, uncompressed formats | kept, deswizzled |
| ASTC, one channel used | BC4 |
| ASTC, two channels used (normal maps) | BC5 |
| other ASTC | BC7 |
| any of the above below 35 dB PSNR | BC7 of the channels read; if still below, uncompressed and exact |

No texture is refused. Each record keeps a channel map, which the back end applies on top of the game's own swizzle.
The host finds a texture's record when the game loads it; see [texture streaming](texture-streaming.md).

## Terrain tiles: `nx2/prepare_terrain.py`

The tool writes an overlay: each TARB file under the same romfs-relative path in `work/prepared/terrain`, with its GPU
texel blob made linear. Everything else in the file stays as it was. The host serves the overlay in place of the
game's files, so the game loads and decompresses the converted file itself, and the run-time patch receives linear
bytes at the game's `memcpy`; see [terrain tiles and CPU texels](terrain-texels.md).

- A file that is not a valid TARB, or whose blob matches no known layout, is not written; the game's own file is served.
  The three damaged files of the dump are such files.
- Linear rows compress worse, so some converted files are larger than the game's. The game's loader drops a file larger
  than its entry in the resource size table, so the tool also writes an adjusted size table into the overlay.

## Sky tables: `nx2/prepare_skybin.py`

The tool writes the environment archive with its sky lookup-table member made linear, into the same overlay folder.
The member keeps its size. The host uploads it at the game's `memcpy` (`host/direct_sky_lut.c`).

## Point the game at the output

The host finds the store in `work/prepared` beside the build. To use another folder, set `HOS_PREPARED=<folder>`:
`<folder>/tex` is the texture store and `<folder>/terrain` the overlay. `HOS_FS_OVERLAY` adds another overlay root in
front of it.
