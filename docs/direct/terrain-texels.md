# Terrain tiles and CPU-written texels

This page covers the texels that do not come from the asset texture store: terrain tiles, the sky lookup tables, the
font glyph cache and the textures the game fills through NVN's texel calls. Read it when a texture shows black,
garbage or a placeholder, or before you add a patch that hands texels to the back end.

The rule behind all of them: the back end never reads texture storage in guest memory. Texels reach an image only
through an NVN call the host forwards or through a game patch that hands the bytes over (`gpu_texels`).

## Terrain tiles

Terrain tiles are TARB files under `TerrainArc/` (about 51,000), read by the terrain manager, not by agl's texture
manager. Each file holds the tile's GPU texels in the console's block-linear layout. The game copies that blob with
one `memcpy` into the tile texture's storage.

1. The game loads and decompresses the file itself, as shipped.
2. A patch in `games/oot/patches/assets.c` replaces that `memcpy`: it makes level 0 of the blob linear, using the layout
   of the texture the blob is copied into, and uploads it into the tile's image.
3. A blob that is not the size of its texture's storage draws the placeholder and is counted.

The work runs on the terrain loader's thread (`tera::FileResourceMgr`), not the frame's; a tile takes well under a
millisecond. The game's own linear branch cannot carry the data: it refuses every compressed format, and three of the
five tile kinds have no CPU copy to upload.

## The sky lookup tables

At boot and at each scene load, the game copies a block-linear blob of three RGBA16F textures out of
`Env/GameScene.Nin_Ounce_NVN2.genvb.zs`. `games/oot/patches/sky_lut.c` makes each part linear in the same way and uploads it
instead of the `memcpy`. Without it, the scene's ambient light is black. Log line: "direct patch sky lut" (6 uploads
at boot, 6 at each scene load).

## The font glyph cache

The font renderer clears a glyph's cell in a small pitch-linear R8 atlas and plots the glyph into it with plain CPU
stores: no NVN call carries those bytes. A patch on the shelf packer notes each cell, and a patch after the glyph
writer uploads the cell's rows to the atlas image (`games/oot/patches/assets.c`). The labels start with `font_`.

## Texels through NVN calls

These need nothing special: the bytes arrive linear in the call, and the back end copies them into the image region.

- `WriteTexels` and `FlushTexels`: primitive and procedural textures, the JPEG initializer, the terrain polish step.
- Movie frames; see [the movie player](movie-player.md).
- `ReadTexels` goes the other way and reads back from the image.

## Texture storage used as plain memory

The SMAA pass allocates `ndsr::smaa_edge_list` as a texture only for its memory: it takes the texture's storage
address with `nvnTextureGetTextureAddress` and uses it as a buffer, and never binds the handle. No patch handles this
yet; it is on the open-work list in [the direct back end plan](../direct-backend-plan.md).

## Find a new CPU writer

If a texture shows wrong content and no NVN call explains it:

1. Run with `HOS_DBG=texwatch`. Every texture's storage pages are write-protected, and each writer is logged once with
   its guest return address ("texwatch: ...").
2. Identify the function, add its label to `games/oot/labels.txt`, and write a patch that hands the bytes to the
   back end at that writer.
