# The asset converters

This page describes the build step that turns the game's textures into the linear data the back end uploads. Read it
when you set up a new machine, when a texture draws as a placeholder, or before you change the prepared format.

The back end has no block-linear code, no ASTC decoder and no run-time conversion. Every asset texture is prepared by
this step, or it draws as a placeholder and is counted. Terrain tiles and the sky tables are not prepared: the game's
patches make their texels linear when the game loads them (see [terrain tiles and CPU texels](terrain-texels.md)).

## Rules the converter keeps

- It reads the romfs in place (default: `work/dump/romfs`) and writes only into the folder you name. What it writes
  is made from the game: it never leaves the PC and is never committed.
- It runs in `ounce-builder` on every CPU and starts a file only when the memory gate has room for it.
- It is not incremental: each run replaces the whole output (about 4 minutes for OoT on 16 threads).

## Run the converter

1. Prepare the textures:

   ```
   ounce-builder textures work/prepared/tex
   ```

   `--quality high` uses slower encoder settings (several times the time; smaller packs, higher PSNR).
   `--only <romfs prefix>` writes a store of those files alone, for a quick check.

2. Or do it in a game build: `ounce-builder build <out> ... --dump <dump>` writes `<out>/prepared/tex`, and
   `--prepared <folder>` writes `<folder>/tex` from `work/dump/romfs`.

To check a store against another one, run `ounce-builder textures --compare <a> <b>`: it compares every index field
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
| ASTC (every block size), linear | BC7 |
| ASTC (every block size), sRGB | BC7 sRGB |

The prepared format follows from the NVN format alone, never from the content. The back end makes a texture that the
game creates in an ASTC format in the same format, so the game's copies from loaded textures into it fit texel for
texel. BC7 is the one BC format with ASTC's four 8-bit channels; NVN has no HDR ASTC formats. A copy, buffer copy or
texel write that would mix ASTC data with another format, or a copy off BC7's 4x4 blocks (possible with 5-, 6- and
10-texel ASTC blocks), is a strict event.

No texture is refused, and every texture keeps its channels where the source has them. The host finds a texture's record when the game loads it; see [texture streaming](texture-streaming.md).

## Point the game at the output

The host finds the store in the game folder's `prepared/`, else in `work/prepared` beside the build. To use another folder, set `HOS_PREPARED=<folder>`:
`<folder>/tex` is the texture store.
