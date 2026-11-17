# The asset converters

This page describes the build steps that turn the game's textures, terrain tiles and sky tables into the linear data
the back end uploads. Read it when you set up a new machine, when a texture draws as a placeholder, or before you
change a prepared format.

The back end has no block-linear code, no ASTC decoder and no run-time conversion. Every texture and terrain tile is
prepared by these steps, or it draws as a placeholder and is counted.

## Rules all converters keep

- They read the romfs in place (default: `work/dump/romfs`) and write only into the folder you name. What they write
  is made from the game: it never leaves the PC and is never committed.
- They run in `hookshot-builder` on every CPU; the texture step starts a file only when the memory gate has room
  for it.
- They are not incremental: each run replaces the whole output (the textures take about 18 minutes on 16 threads,
  4 with `--fast`).

## Run the converters

1. Prepare the textures:

   ```
   hookshot-builder textures work/prepared/tex
   ```

   `--fast` uses quicker encoder settings (a quarter of the time; packs about 4 % larger).
   `--only <romfs prefix>` writes a store of those files alone, for a quick check.

2. Prepare this game's own files (the terrain tiles, the resource size table and the sky tables):

   ```
   hookshot-builder overlay work/prepared/terrain
   ```

3. Or do both in a game build: `hookshot-builder build <out> ... --prepared <folder>` writes `<folder>/tex` and
   `<folder>/terrain` after the link.

To check a store against another one, run `hookshot-builder textures --compare <a> <b>`: it compares every index field
except where the frames lie in the packs, and every frame's bytes.

## What the game description sets

`games/<game>/game.toml` names what is this game's in `[assets]`: the romfs file of its zstd dictionaries, the
top-level folders the texture step skips, and the overlay steps the game needs (`tarb_terrain`, `skybin`).

## Textures

The texture step writes every BNTX texture of the romfs: zstd frames in packs, with an index keyed by the BNTX file path
and the texture's index. `ounce/tex/src/store.rs` describes the format.

| Source | Prepared as |
|---|---|
| BC1-BC7, uncompressed formats | kept, deswizzled |
| ASTC, one channel used | BC4 |
| ASTC, two channels used (normal maps) | BC5 |
| other ASTC | BC7 |
| any of the above below 35 dB PSNR | BC7 of the channels read; if still below, uncompressed and exact |

No texture is refused. Each record keeps a channel map, which the back end applies on top of the game's own swizzle.
The host finds a texture's record when the game loads it; see [texture streaming](texture-streaming.md).

## Terrain tiles

The `tarb_terrain` step writes an overlay: each TARB file under the same romfs-relative path, with its GPU
texel blob made linear. Everything else in the file stays as it was. The host serves the overlay in place of the
game's files, so the game loads and decompresses the converted file itself, and the run-time patch receives linear
bytes at the game's `memcpy`; see [terrain tiles and CPU texels](terrain-texels.md).

- A file that is not a valid TARB, or whose blob matches no known layout, is not written; the game's own file is served.
  The three damaged files of the dump are such files.
- Linear rows compress worse, so some converted files are larger than the game's. The game's loader drops a file larger
  than its entry in the resource size table, so the step also writes an adjusted size table into the overlay.

## Sky tables

The `skybin` step writes the environment archive with its sky lookup-table member made linear, into the same overlay
folder.
The member keeps its size. The host uploads it at the game's `memcpy` (`host/direct_sky_lut.c`).

## Point the game at the output

The host finds the store in `work/prepared` beside the build. To use another folder, set `HOS_PREPARED=<folder>`:
`<folder>/tex` is the texture store and `<folder>/terrain` the overlay. `HOS_FS_OVERLAY` adds another overlay root in
front of it.
