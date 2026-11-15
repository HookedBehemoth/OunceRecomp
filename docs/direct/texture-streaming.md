# Texture streaming

This page explains how the game loads, caches, moves and frees its asset textures, and how the host and the back end
follow those events without keeping a CPU copy of any texel. Read it before you change `host/direct_assets.c`,
`host/direct_texdata.c`, `host/direct_tex_dedup.c` or the back end's image lifetime (`gpu/direct/direct_image.cpp`).

The game's texture code is agl's `TextureDataMgr`. Its functions carry the prefixes `texdata_`, `texheap_`, `texres_`
and `gfx_texture_` in `games/oot/labels.txt`.

## How the game streams textures

- **Whole textures, never mip ranges.** Each texture loads at full size with every mip level, synchronously, on the
  requesting thread. No code changes a resident texture's mip range.
- **"No data" loads are aliases.** `System/Gfx/UnifyTex.Product.100.byml` maps about 5,000 texture names to about 330
  shared targets. An alias gets an object of its own that shares its target's heap slot and description; its load has
  no source data.
- **Unloading is a cache.** When a texture's last reference goes, it moves to an LRU list and stays initialized and
  registered. It is destroyed only under memory pressure: when an allocation fails, or when the texture heap has less
  than `TextureCacheRejectionThreshold` (120 MB) free.
- **Compaction never stops.** The texture heap is compactible. A compaction thread moves blocks every frame, and the
  frame update re-initializes each moved texture and its aliases at the new address, under the same descriptor id.
- **Replace** swaps an already-loaded texture's content for a new file of the same name. No run so far has reached it.
- **The budget** is one 2,400 MB `TextureHeap` plus a 32 MB compaction work heap, and at most 10,240 textures. The
  values come from romfs parameter files.

Terrain tiles do not use this system; see [terrain tiles and CPU texels](terrain-texels.md).

## How the back end follows it

The back end never reads texture storage. It follows the game's own texture events, hooked in the host:

| Game event | What happens |
|---|---|
| load with data | the host finds the texture's record in the prepared store and hands it to the back end before `nvnTextureInitialize`; the game's texel copy into its heap is skipped (`host/direct_assets.c`) |
| load of an alias | the alias's target is read from the alias's own chain and handed over as the same image (`host/direct_tex_dedup.c`) |
| compaction move | the texture object keeps its identity, so its image and registration follow it; the movers copy no bytes |
| released to the cache, taken back | counted; the image stays, because the game may use the texture again without a load |
| destroyed | its `nvnTextureFinalize` releases the image; the image goes once no texture uses it and the frames that may read it have completed |

The table of texture states lives in `host/direct_texdata.c`. The back end holds no eviction policy of its own.

### Eviction by free VRAM

The game decides eviction by its heap's free bytes, which say nothing about VRAM on a PC (prepared BCn images can be
larger than the console's bytes, and the console's bytes are never copied). A patch on the game's per-frame update
raises the cache threshold by however much free VRAM is short of a 1.5 GB margin. The game then evicts LRU textures in
its own order, through its own destroy. Free VRAM is the back end's figure: the driver's budget minus usage. Look for
the log line "assets: VRAM eviction patch".

This patch can destroy a texture still sampled by draws of the same present; it is on the open-work list in
[the direct back end plan](../direct-backend-plan.md).

## What identifies a texture

| Key | Stable across | Changes at |
|---|---|---|
| the `TextureData` pointer | moves, alias retargets, Replace | destroy (the memory is reused) |
| the NVN texture object | the texture's life | re-initialized in place by every move and Replace step |
| the descriptor id | moves | Replace, alias retarget, destroy; a freed id is reused after a delay |
| the name | always | never; an alias's name is not a file name, its target's is |
| the heap slot | until the next compaction | every move |

The source file exists only during the load, so the host must find the prepared record at the load.

## What will surprise you

- **Prepared textures need their channel map.** The prepared store keeps a texture's channels in its own order, so every
  sampled view of a prepared image applies the game's swizzle on top of the store's. Without it, static meshes come out
  under-lit. `HOS_DBG=nochanmap` turns the map off, for a comparison.
- **The game renders into compressed textures.** It encodes its noise volumes to BC4 on the GPU, through a view of an
  uncompressed format of the block's size. A compressed image that is not from the prepared store is therefore made
  view-compatible and usable as a color attachment.
- **Persistent descriptor ids are reused.** agl's descriptor ring frees an id after the frames in flight and hands it
  out again within seconds. The back end changes a heap entry only when the game registers an id again; NVN has no
  "unregister".

## Debug switches

| Switch | Effect |
|---|---|
| `HOS_DBG=noassets` | turns the asset patches off, for a before/after comparison |
| `HOS_DBG=nodata[:<name part>]` | logs every load with no data; with a name part, watches those textures' slots for CPU writes |
| `HOS_DBG=texwatch` | finds code that writes texels with the CPU (each write to a texture's pages faults once) |
| `HOS_PREPARED=<folder>` | the prepared store's folder (default `work/prepared` beside the build) |
