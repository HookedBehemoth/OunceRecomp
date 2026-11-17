# The asset converters

This page describes the build step that turns the game's textures into the linear data the back end uploads. Read it
when you set up a new machine, when a texture draws as a placeholder, or before you change the prepared format.

The back end has no block-linear code, no ASTC decoder and no run-time conversion. Every asset texture is prepared by
this step, or it draws as a placeholder and is counted. Terrain tiles and the sky tables are not prepared: the game's
patches make their texels linear when the game loads them (see [terrain tiles and CPU texels](terrain-texels.md)).

## Rules the converter keeps

- It reads the romfs in place (default: `work/dump/romfs`) and writes only into the folder you name. What it writes
  is made from the game: it never leaves the PC and is never committed.
- It runs in `hookshot-builder` on every CPU and starts a file only when the memory gate has room for it.
- It is not incremental: each run replaces the whole output (about 18 minutes on 16 threads, 4 with `--fast`).

## Run the converter

1. Prepare the textures:

   ```
   hookshot-builder textures work/prepared/tex
   ```

   `--fast` uses quicker encoder settings (a quarter of the time; packs about 4 % larger).
   `--only <romfs prefix>` writes a store of those files alone, for a quick check.

2. Or do it in a game build: `hookshot-builder build <out> ... --prepared <folder>` writes `<folder>/tex` after the
   link.

To check a store against another one, run `hookshot-builder textures --compare <a> <b>`: it compares every index field
except where the frames lie in the packs, and every frame's bytes.

## What the game description sets

`games/<game>/game.toml` names what is this game's in `[assets]`: the romfs file of its zstd dictionaries and the
top-level folders the texture step skips.

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

## Point the game at the output

The host finds the store in `work/prepared` beside the build. To use another folder, set `HOS_PREPARED=<folder>`:
`<folder>/tex` is the texture store.
