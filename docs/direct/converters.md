# The asset converters

This page describes the build step that turns the game's textures into the linear data the back end uploads. Read it
when you set up a new machine, when a texture draws as a placeholder, or before you change the prepared format.

The back end has no block-linear code, no ASTC decoder and no run-time conversion. Every asset texture is prepared by
this step, or it draws as a placeholder and is counted. Terrain tiles and the sky tables are not prepared: the game's
patches make their texels linear when the game loads them (see [terrain tiles and CPU texels](terrain-texels.md)).

## Rules the converter keeps

- It reads the romfs in place (default: the romfs of the game's dump in the config) and writes only into the folder you name. What it writes
  is made from the game: it never leaves the PC and is never committed.
- It runs in `ounce` on every CPU and starts a file only when the memory gate has room for it.
- A conversion is not incremental: it replaces the whole store (about 4 minutes for OoT on 16 threads). `ounce build`
  converts only when the dump's files or the encoder settings changed.

## Run the converter

Every `ounce build <game>` puts the game's store in its game folder's `prepared/`. It keeps the store while the dump and
the settings are unchanged; a worktree's build copies the main checkout's store when that one is current; only
otherwise does it convert. To make a store by hand, for a check:

1. Prepare the textures:

   ```
   ounce tool textures <folder>/tex --game games/<game>
   ```

   `--quality high` uses slower encoder settings (several times the time; smaller packs, higher PSNR).
   `--only <romfs prefix>` writes a store of those files alone, for a quick check.


To check a store against another one, run `ounce tool textures --compare <a> <b>`: it compares every index field
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
| ASTC normal map: linear, two channels read | BC5 |
| other ASTC, linear | BC7 |
| other ASTC, sRGB | BC7 sRGB |
| any ASTC below 35 dB PSNR | encoded again, in the same format, at slower settings |

The back end makes a texture that the game creates in an ASTC format as BC7 (sRGB for sRGB), so the game's copies
from loaded textures into it fit texel for texel. BC7 is the one BC format with ASTC's four 8-bit channels; NVN has no
HDR ASTC formats. A copy, buffer copy or texel write that would mix ASTC data with another format, or a copy off the
4x4 blocks (possible with 5-, 6- and 10-texel ASTC blocks), is a strict event. Each BC5 record holds its normals'
angular error (mean and max degrees, z reconstructed from x and y).

No texture is refused, and every texture keeps its channels where the source has them. The host finds a texture's record when the game loads it; see [texture streaming](texture-streaming.md).

## Point the game at the output

The host finds the store in the game folder's `prepared/`. To use another folder, set `prepared=<folder>`:
`<folder>/tex` is the texture store.
