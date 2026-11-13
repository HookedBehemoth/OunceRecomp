# The game's texture streaming: what the code does (M6g, 2026-11-13)

Research only: no back-end or host code was changed. Static analysis of this build (Ghidra decompiles made on demand
with `work/ghidra/decomp.sh`, kept under `work/proto/streaming/g1..g4/`, not committed), the lifted code, the romfs
parameter files, and the logs already on disk (`work/play/log.txt`, `work/runs/*/log.txt`). No game run was needed.
Addresses are main's VA (the lifter's `f_<addr>`). The Ghidra project has been rebased to the same VA since 2026-11-11
22:46, so `decomp.sh` now takes the VA itself; the "VA - 0x20000" rule in `work/assets/native/asset-path.md` applies only
to the decompiles made before that, in `work/proto/assets/ghidra/`. Class names come from strings (thread, heap and
parameter names); the agl RTTI is stripped.

**VERIFIED** means read in the decompile or lifted code at the given address, or read from a romfs file or a log. How is
said where it matters. **INFERRED** means concluded from the code's shape or from naming, and not confirmed.

## 0. Short

- **The game streams whole textures, not mip levels.** Each texture is loaded at its full size with every mip, and its
  view always covers mips 0..N-1 (VERIFIED, 2.2). No code changes a resident texture's mip range, and no texture is first
  registered at low detail. The 341x341 and 768x768 textures in the play log are full-size textures of the romfs: 816
  BNTX textures are 341 wide.
- **"No data" loads are aliases.** `System/Gfx/UnifyTex.Product.100.byml` maps 5,186 texture names to 333 shared
  targets. A name in that table is never loaded. It gets an agl::TextureData of its own that shares its target's heap
  slot and copies its target's BRTI. Its `loadImage_` runs with src 0 (VERIFIED, 2.3). Every name the play log reports
  as "loaded with no data ... another descriptor" is in that table, and the descriptor logged for it is its target's: for
  example `Land_LakeHrl_MatsuCliff_F_AO` -> `Land_HrlField_MatsuCliff_L_AO`, 341x341, 9 mips.
- **Unloading is a cache.** When a texture's last reference goes, the texture is not freed. It goes on an LRU list,
  stays initialized and registered, and keeps its texels. It is destroyed (nvnTextureFinalize, its descriptor slot and
  heap block freed) only under memory pressure: when an allocation fails, or when the texture heap has less than
  `TextureCacheRejectionThreshold` (120 MB) free. A scene's cache can also be cleared on purpose (VERIFIED, 2.6, 3).
- **Budget:** one 2,400 MB `TextureHeap` plus a 32 MB `TextureCompactionWorkHeap`. Together they are pool 127,
  0x98000000 bytes. The heap holds at most `TexatureHandleNum` = 10,240 textures (sic, the game's spelling). The values
  come from romfs parameter files (VERIFIED, 3).
- **Compaction never stops.** The texture heap is a compactible heap. A compaction thread moves blocks every frame.
  The frame update then rebinds each moved texture, and every alias of it, at its new address: Finalize + Initialize +
  RegisterTexture under the same descriptor id. That is the ~3,600 rebinds (and "packaged" copies) per 600 presents in
  the play log (2.5).
- **"Replace"** swaps an already-loaded texture's content for a newly loaded BNTX of the same name, in two delayed
  phases. It is not streaming, and no run so far has reached it (2.7).
- **Terrain tiles are a separate system:** tera's own manager, its own 201 MB pool and its own memcpy (5).

## 1. The objects

| Object (name from strings) | Where | What it is |
|---|---|---|
| `shaftTextureResourceMgr` (game, class `TextureResourceMgr` from its own strings) | init `f_809ee0f0`; request `f_805c5ca0`; registration from a file `f_83ad27c0`; fallback alias `f_83ad22d0` | The game's texture front end. It resolves a name through the UnifyTex table (map at `+0x190`, used when `+0x2a9 == 1`). It finds an existing texture or loads `Tex/<name>.bntx` (or `<dir>/<name>.bntx`) through the resource system (`f_801b4260`, heap name "shaftTextureResourceMgr"), then registers it and creates the aliases. A "TextureResourceMgr::Retry" thread exists only on the develop path: `f_809ee0f0` creates it only when its third path string is not empty, and `f_831dd780` passes "" (INFERRED: absent in this build's run) |
| `agl::TextureDataMgr` (singleton `*0x84ca9bf0`) | heap init `f_80f6c580` ("agl::TextureDataMgr::InitializeHeap"), called once from the gfx system init `f_831dd780+0x278` | Owns every TextureData: the name dictionary, the LRU cache, the compactible heap (pool 127), the compaction and Replace threads, and eviction |
| agl::TextureData (`td`, 0x1d0 bytes + user data; allocated by `f_8051d820` from the "aglTextureDataMgr::Unit" heap) | create `f_80798d00`, load `f_80798690`, init `f_807987d0`, rebind `f_807982f0`, destroy `f_8079bbe0` | One texture: its slot, its BRTI copy, its NVN texture and view, its descriptor id, its name, its alias links |
| `aglTextureDataMgr::CompactibleHeap` (`mgr+0x40`) | made in `f_80f6c580` (`f_80f6dcf0`); alloc `f_80799630` -> `f_80799bd0`; free vtable `+0x48` | Texel memory. Blocks are described by 0x70-byte records with an owner tag (`f_83538b70`: the td's offset from mgr / 8) |
| NVN memory pool (pool 127) | `f_80f6c9d0(mgr+0x170, size, mem, ...)`, nvnMemoryPoolInitialize returns to `0x80f6cb20` | The heap's storage, `TextureHeap + TextureCompactionWorkHeap` bytes. **`f_80f6c9d0` is agl's generic pool wrapper**: it is also called by the tera manager (`f_809e3350+0x334` -> `f_80f6cf50`) and by Replace requests (`f_83537b20`), so lr `0x80f6cb20` alone does not identify the texture heap (6) |
| `aglTextureDataMgr::Compaction` thread (`mgr+0x300`, priority 25) | handler `f_807da3f0` -> `f_807da480` (states 0/3 `f_807dbd60`, 1 `f_807dbf90`, 2 `f_807db4a0`) | Plans and moves blocks |
| `aglTextureDataMgr::Replace` thread (`mgr+0x330`, priority 25; made because the init flag at param `+0x48` is 1) | handler `f_83537580` (message 200) -> `f_835385f0` | Runs Replace requests and the delayed frees of slots, a frame-counted step each time it is woken |
| Per-frame update | `f_80797240(mgr)`, called from `f_831dfe10+0x64` (gfx system). Threads: the rebind hook logs show sead::MainThread and ModuleSystemWorker1-5 | Wakes compaction (message 100), wakes Replace (message 200) when `mgr+0x338` is set, applies finished compaction (`f_80797da0`), and evicts by threshold |
| Descriptor allocator (`*0x84e9fca8`) | register `f_80064920` (-> nvnTexturePoolRegisterTexture); release `f_8079c360` | Refcounted descriptor ids. A released id goes onto one of two free lists (`+0xa00`, chosen by the byte at `+0xa60`). INFERRED: frame-parity deferred reuse |

### 1.1 agl::TextureDataMgr layout (as far as used)

| Offset | Content | How known |
|---|---|---|
| +0x00 | name dictionary root (tree of 0x30-byte nodes: +0x18 crc32 of the name, +0x20 name, +0x28 td) | lookups in `f_8051d560`, `f_80517d80` |
| +0x08 / +0x18 / +0x1c | free dictionary nodes / count / capacity (= TexatureHandleNum) | `f_80f6c940` |
| +0x20 / +0x30 / +0x34 | the LRU list of unreferenced textures: list header, count, node offset in td (the node is at td+0x48) | `f_80517d80` pushes, `f_83537760` / `f_80797240` pop, `f_8051d560` unlinks |
| +0x38 | the td allocator ("aglTextureDataMgr::Unit", N x 0x3d8) | `f_80f6c580` |
| +0x40 | the CompactibleHeap | `f_80f6c580` |
| +0x50 | the heap context passed to `loadImage_` as x1: ctx+0x130 = the NVN pool (mgr+0x180), ctx+0x280 = the CPU base of the heap memory (mgr+0x2d0) | `f_80798690`, `f_807982f0` |
| +0x170 | the pool wrapper (its +0x10 is the NVN pool) | `f_80f6c9d0` call |
| +0x300, +0x308 | compaction thread; compaction on | `f_80f6c580`, `f_80797240` |
| +0x330, +0x338 | Replace thread; "work pending" (set by `f_83537870`, cleared when `f_835385f0` finds nothing to do) | |
| +0x33c | how many cached textures an eviction pass looks at before it simply takes the LRU (default N/8 = 1,280) | `f_80f6c580`, `f_80797240` |
| +0x340 | the rejection threshold in bytes (`TextureCacheRejectionThreshold` << 20) | `f_831dd780`, `f_80797240` |
| +0x350/+0x360, +0x368/+0x378, +0x380/+0x390 | the Replace request lists: swap, copy, retire (each request has a frame countdown at +0x2d0 and its target td at +0x2d8) | `f_83538060`, `f_835381e0`, `f_83538390` |
| +0x3a0/+0x3b0 | delayed slot frees (records of 0x30 bytes, countdown 3) | `f_83537870`, `f_83538470` |
| +0x3d0, +0x3f0, +0x410 | mutexes: main, delayed frees, update | |

### 1.2 agl::TextureData layout

| Offset | Content | How known |
|---|---|---|
| +0x00 u16 | reference count | acquire / release |
| +0x02 u8 | flags: bit 0 slot allocated, bit 1 registered (NVN texture initialized, descriptor taken), bit 2 in the dictionary, bit 3 ready (other loaders of the same name sleep until it is set) | `f_80798d00`, `f_80798690`, `f_8079c220` |
| +0x18 | **slot**: the heap block. The texels start at +0x10 into it. An alias holds its target's value | `f_80798690`, `f_807974f0` |
| +0x20 | heap pointer, low 3 bits flags | `f_8079c220` |
| +0x28, +0x2c | owner tag; slot size | `f_83537870`, `f_83537d90` |
| +0x30.. +0x44 | listener list (each listener is called after a load, a rebind or an alias retarget), count, node offset | `f_807982f0`, `f_807974f0` |
| +0x48 | LRU node | |
| +0x58 (0xa0 bytes) | **the BRTI head copy** (TextureInfo): +0x6e mip count (BRTI+0x16), +0x74 format, +0x7c/+0x80 width/height, +0x88 layers, +0xa8 image size, +0xac alignment, +0xb8 -> name, +0xc0 -> data block (its +0x10 is the slot), +0xc8 -> mip pointer table | asset-path.md 7.3 (A0 trace), `f_80798d00` |
| +0xd8 | gfx Texture. **NVNtexture = [td+0xd8]+0x10** | A0: 4,499 of 4,499 |
| +0xe8 | descriptor id (-1: none). Kept through rebinds | `f_807982f0`, `f_8079c360` |
| +0xf8 | name: u16 length, then the characters | `f_80798d00` |
| +0x188 / +0x190 | **alias links**: prev / next. A non-alias head has prev 0. Aliases hang off their target's +0x188 node | `f_807974f0`, `f_80797da0`, `f_8079c220` |
| +0x198 | the Replace request in progress | `f_83538060` |

## 2. The life of a texture

### 2.1 Request and load (synchronous, on the requesting thread)

The steps, VERIFIED by the code at each address. The play log shows loads on sead::MainThread, SceneLowPriority,
OptimizeChangeBanc, bake::ModuleMgr, BaseProcCreateAndDeleteThread and PreActorModelCompose.

1. `f_805c5ca0` (TextureResourceMgr) takes a name. If the name is in UnifyTex (murmur3 lookup in `+0x190`), the
   *target* name is used from here on. If the target is in the agl dictionary, its reference count goes up; when it
   was 0, the texture leaves the LRU list and its block is marked in use again (`f_8079aa20(slot, 0)`). The texture is
   then used as it is: no NVN call.
2. Otherwise the file is loaded: `"%s/%s.bntx"` through `f_801b4260`. That is the ARes loader and host zstd of
   asset-path.md 3.1.
3. `f_83ad27c0(ctx, BRTI of the file, status)` -> `f_8051d560(mgr, BRTI, key, listener)` (acquire). The acquire looks the
   name up again, then:
   - `f_8051d820`: allocates a td; when the td heap is full it evicts the LRU texture (`f_83537760`) and tries again.
   - `f_80798d00` (createTextureData_):
     - allocates the slot, `f_80799630(td, heap, BRTI+0x50 size, BRTI+0x54 alignment)`. On failure it looks the name up
       once more, then evicts one LRU texture (`f_83537760`) and retries. With nothing left to evict it fails (-2);
     - copies the name to td+0xf8 and the 0xa0-byte BRTI to td+0x58;
     - points the mip table at the slot: `[td+0xc8][i] = slot + 0x10 + mip_off[i] - mip_off[0]`;
     - calls `f_80798690(td, ctx, src = **(BRTI+0x70))`, src being the file's mip-0 data.
4. `f_80798690` (loadImage_) does the following:
   - `memcpy([td+0x18]+0x10, src, [td+0xa8])` (the call returns to 0x80798700): the whole image, every mip and layer;
   - a memmove into a second CPU mapping, if the pool has one (`0x807987c4`), and a flush (`0x807987a4`);
   - then, with or without src, `f_807987d0(td+0x58)`, which:
     - runs nn::gfx `TextureImpl::Initialize` (`f_80798920`): builder SetDevice, builder from the TextureInfo
       (`f_812a3680`), SetStorage(pool 127, slot + 0x10 - heap base), the packaged layout when bit 5 of the info's byte +0x10 is
       set (`f_812a5950`), and **nvnTextureInitialize** (LR 0x807989e8);
     - initializes the TextureView (`f_80798a20`): SetLevels(base 0, count = BRTI+0x16), SetLayers(0, BRTI layers),
       the swizzle, the depth mode, the target;
     - takes a descriptor (`f_80064920` -> **nvnTexturePoolRegisterTexture**);
   - sets the registered flag.
5. Back in `f_80798d00`, the td goes into the dictionary. If another thread finished the same name first, this td is
   destroyed (`f_8079bbe0`, which includes nvnTextureFinalize) and the other one is returned. The ready bit is then set.
   Callers that are not the creator sleep until it is (`f_809467e0` loop).
6. The resource handle is released after registration (`f_8080b4c0`). INFERRED: the decompressed file buffer is freed
   there.

Other entries into `f_80798d00` / `f_80798690`: `f_809ee850` -> `f_809ee930` (a TextureData made from a TextureInfo
struct with src at `+0x48`, e.g. DummyTextureForEvent from 0x809ee9a8). The in-place paths (`f_80798b60` ui2d and
`f_80792bd0` BNTX in place) initialize NVN textures over a file's own memory and are not part of the TextureDataMgr
heap.

### 2.2 No partial residency

- The copy size is the full image size `[td+0xa8]` = BRTI+0x50 (VERIFIED, A0: equal on every load).
- The texture is initialized from the full TextureInfo.
- The view is SetLevels(0, BRTI mip count) (VERIFIED, `f_807987d0` stores BRTI+0x16 as the view's level count and 0 as
  its base).
- No code path found writes a smaller mip count, a base level or a min LOD clamp per texture. There is no "resident mip
  range" field in the td.

The only level-of-detail choices are higher up, in *which* texture is asked for. `IsEnableBakeTextureLOD` (bake
textures, `f_8083b770`, `f_81227610`) and `lod_ignore_low_resolutions_for_handheld` (`f_831ee700`) are of that kind:
neither was traced (INFERRED from their names).

### 2.3 Aliases: the "no data" loads

`f_807974f0(mgr, aliasKey, targetKey, listener, status)` (VERIFIED):

1. Look the target up in the dictionary. Without it, fail (-2).
2. Look the alias name up:
   - **existing alias A (a retarget):**
     - `f_8079c110`: drops any Replace request on A;
     - `f_83537870`: if A owns a slot of its own and is referenced, the slot goes to the delayed-free list (3 Replace
       steps);
     - `f_8079c220(A, 0)`: unregisters A (descriptor release, **nvnTextureFinalize** of A's object), unlinks A from its
       old chain and clears its slot;
   - **no alias yet:** a new td of 0x1d0 bytes (`f_8051d820`).
3. Append A to the target S's chain at S+0x188, then copy S's slot (+0x18), slot size (+0x2c) and heap (+0x20).
4. `f_80798d00(mgr, &A, BRTI = S+0x58, src = 0, aliasKey, listener)`. The slot is already set, so nothing is allocated.
   The BRTI copied is the target's, so **the alias has its target's descriptor**. `f_80798690` sees src 0: no copy, then
   nvnTextureInitialize over S's storage and a descriptor of its own.
5. Call A's listeners.

Callers:

| Caller | When |
|---|---|
| `f_83ad27c0+0x4d4` | right after the target is acquired for a UnifyTex name (same thread, same call) |
| `f_83ad22d0` | a fallback alias to TextureResourceMgr's default key (`+0x208`) when a texture cannot be had; it also queues a retry when the Retry ring exists |
| `f_805c5ca0+0x1188..` | a missing file, aliased to `DummyTextureForEvent` (the crc of that name is computed inline) |
| `f_81154da0`, `f_831ded30` | |

The table: `System/Gfx/UnifyTex.Product.100.byml` maps 5,186 names to 333 targets (VERIFIED, read with
`work/tools/byml.py`). No target is itself an alias. No alias name is a BNTX in the romfs, and every target is one. The
play-log names (Kokiri, the market, Hyrule Field):

| Alias (play log) | Target (UnifyTex) | Target's BNTX descriptor (scan) | Logged descriptor |
|---|---|---|---|
| `Player_Child_Hairs_Alb` | `Mt_Hairs_Player_Adult_Alb` | 768x768, 10 mips, 0x2006 | (M4g: same slot, same size) |
| `Land_LakeHrl_MatsuCliff_F_Nrm` | `Land_HrlField_MatsuCliff_L_Nrm` | 768x768, 10, 0x2f01 | 768x768, 10, 0x2f01 |
| `Land_LakeHrl_MatsuCliff_F_AO` | `Land_HrlField_MatsuCliff_L_AO` | 341x341, 9, 0x1d01 | 341x341, 9, 0x1d01 |
| `CmnTex_Map_HrlFieldRock_A_DetailMap_Alb` | `CmnTex_Map_DetailMapA_Alb` | 341x341, 9, 0x3206 | 341x341, 9, 0x3206 |
| `CmnTex_Rock_LakeHrl_Stone_A_Nrm` | `CmnTex_Rock_Graveyard_Stone_A_Nrm` | 1536x1536, 11, 0x2f01 | same |
| `Rock_WaterTemple_BrickWall_A_Rgh` | `Rock_WaterTemple_BrickFloor_C_Rgh` | 768x768, 10, 0x1d01 | same |
| `CmnTex_Zora_Ground_C_01_Alb` | `Rock_Kokiri_Pebble_C_Alb` | 1024x1024, 11, 0x3106 | same |
| `RBake_Scene_Market_Child_G_9_8_d0_t0` | `RBake_Scene_BarinadeStage_G_1_0_d0_t0` | 8x8, 1, 0x1e01 | same |
| `Rock_Pot_A_Rgh`, `Mt_Item_DekuStick_Trs`, `Fire_Thick_Burst_00_Dst` | `Rock_Pot_A_02_Rgh`, `CmnTex_Map_CityPartsFar_Carriage_A_Mtl`, `Fire_Burst_SingleShot_S_01_Dst` | | |

So "the previous load at the same slot" (M4g's reading) holds on the console because an alias *is* its target's slot.
The target is known exactly, from the chain (walk +0x188 back to the node whose prev is 0; that node is the target's
+0x188), from `f_807974f0`'s x2 key, or from UnifyTex.

### 2.4 Destroying an alias, or its target

`f_8079c220(td, final)` (VERIFIED):

1. Unregister: `f_8079c360`, which releases the descriptor and runs `f_8079c480` -> **nvnTextureFinalize** and
   `f_8079c470` (the view).
2. Then one of three things happens to the slot:
   - a td with no aliases frees its heap block (heap vtable +0x48, under the heap's write lock);
   - **a target that still has aliases hands the block to its next alias**, which becomes the new head (the owner tag is
     rewritten, `f_83538b70`);
   - an alias just unlinks.

An alias is destroyed as soon as its last reference goes (`f_80517d80`): it is not cached.

### 2.5 Moves: compaction

1. The compaction thread plans and moves blocks (`f_807dbd60` / `f_807dbf90` / `f_807db4a0`): `memcpy(new, old, n)`
   when `heap+0x90` has no copier object and `worker+0x1f21 == 1` (the host clears that byte). Each moved block's
   record gets its new address (`[e+0x40]`).
2. The next frame update applies the moves (`f_80797240` -> `f_80797da0`, when heap state `+0xa6 == 1`, on
   MainThread / ModuleSystemWorker). For each moved block owned by a td (owner tag -> td = mgr + tag * 8), unless the
   block is locked (`f_807998c0`):
   - `td+0x18 = new block`, then `f_807981f0(td, mgr+0x50)`. That copies into the second CPU mapping if there is one,
     then calls `f_807982f0(td, td+0x18, td+0x58, ctx)`;
   - **then the same for every alias** in the chain (`td = [td+0x190] - 0x188`).
3. `f_807982f0` (rebind) on a registered texture:
   - rebuilds the mip table and the data block's base;
   - builds a builder from the live NVN texture (its Get* calls);
   - SetStorage(pool, new offset);
   - SetPackagedTextureData with the texels' own CPU address (`0x84cae130(builder, offset + heap base)`), and the
     packaged layout when BRTI flag bit 0 at td+0x68 is set. INFERRED that `0x84cae130` is SetPackagedTextureData: in
     the play log the host's "packaged" counter equals its rebind counter at every line (2,466 / 2,466, 6,286 / 6,286,
     7,522 / 7,522);
   - then, under the gfx mutex (`*0x84e9f6c8+0x240`): **nvnTextureFinalize, nvnTextureInitialize,
     nvnTexturePoolRegisterTexture with the same id** (td+0xe8);
   - then the listeners.
4. Unregistered: `f_807987d0`, a fresh initialization with a new descriptor.

The update wakes compaction every frame while it is on. The policy for starting a new compaction (`f_807da590`, state
3) was not read. In the play log, between two of the host's 10-second memory lines in
Hyrule Field, the compaction steps went from 1,665 to 3,261 and the rebinds from 2,466 to 6,286.

### 2.6 Release, cache, eviction

- **Release** (`f_80517d80(mgr, key, listener)`, VERIFIED): unlink the listener, decrement the count. At 0 with no
  listeners left:
  - an alias is destroyed;
  - a texture is marked evictable (`f_8079aa20(slot, 1)`) and pushed on the LRU list. It keeps its NVN texture, its
    descriptor and its texels.
- **Re-acquire** from the cache: the count goes back up and the texture leaves the list; no NVN call (2.1, step 1).
- **Eviction** (`f_8079bbe0`: unlink from the dictionary, `f_8079c220(td, 1)`, free the td) is reached from:

  | Caller | Trigger |
  |---|---|
  | `f_83537760` | an allocation failed (td or slot): pop the oldest entry of the LRU list and destroy it |
  | `f_80797240` (each frame) | compaction idle, heap free (`heap+0x1ee8`) < `mgr+0x340` (120 MB), and the list not empty. If fewer than `mgr+0x33c` textures are cached, it first destroys the two cached textures at the lowest and highest heap addresses, then pops the LRU until free >= threshold |
  | `f_8079bb50` | destroy every cached texture. Called from `f_832ee450` (INFERRED: the sequence command `SeqOneShotClearTextureCacheAll`, whose name getter is the next function), from `f_80ed94f0`, and at shutdown (`f_83537290`) |
  | `f_80517d80` | aliases, at once |

- The LRU order (VERIFIED in `f_83488f10`): release pushes at one end, eviction pops from the other, so the texture
  released longest ago goes first.

### 2.7 Replace (content swap)

When a file is loaded for a name that is already a texture and the request's flag (request `+0x18`) is set,
`f_83ad27c0` calls `f_83537e30`. That makes a request (`f_83537a70` -> `f_83537b20`): the new BRTI at r+0x290, the new
texels at r+0x2a8, and an NVN pool of its own over them (`f_80f6c9d0`). The Replace thread then runs the request in
three steps, three frames apart (countdown at r+0x2d0):

1. Swap, `f_83538060`:
   - `f_8079c360` unregisters the texture (nvnTextureFinalize);
   - `f_83536f30` copies the new BRTI head into td+0x58;
   - `f_807982f0(td, r+0x2a8, r+0x290, r)` re-initializes it **over the request's pool**, with a new descriptor.
2. Copy, `f_835381e0` -> `f_83537d90`: a new heap slot, `memcpy` of the texels into it, then `f_807981f0` rebinds the
   texture into the heap (Finalize + Initialize + Register, the same id).
3. Retire, `f_83538390`: the request's pool is finalized (`f_83537c50` -> `f_80941b20`) and the request freed.

The host counted no `f_83537d90` in any run ("replaced 0" in the play log; A0's trace). INFERRED: a reload path, not
used in normal play.

### 2.8 Threads, and what is synchronous

| Work | Thread | Sync |
|---|---|---|
| request, file read, decompression, slot, copy, NVN init, aliases | the requesting thread (MainThread, SceneLowPriority, OptimizeChangeBanc, bake::ModuleMgr, BaseProcCreateAndDeleteThread, PreActorModelCompose ...) | synchronous: the texture is complete when the call returns; other requesters of the name sleep on the ready bit |
| block moves | aglTextureDataMgr::Compaction | asynchronous to the frame; the bytes move before the NVN texture does |
| compaction apply (rebinds), threshold eviction | the frame update (MainThread / ModuleSystemWorker, `f_831dfe10`) | once a frame |
| Replace steps, delayed slot frees | aglTextureDataMgr::Replace (message 200, once a frame when work is pending) | frame-counted (3) |
| eviction on allocation failure | the requesting thread | synchronous |

## 3. The budget

| Limit | Value | Where |
|---|---|---|
| texture heap | **2,457,600 KB = 2,400 MB** (`TextureHeap`) | `System/HeapSize/Product.Nin_Ounce_NVN2.HeapDef.byml`, read in `f_831dd780` (key string 0x83e2352c) through `f_805c2630`; default 0x10000000 |
| compaction work area | **32,768 KB = 32 MB** (`TextureCompactionWorkHeap`) | same file, key 0x83cc983f; default 0x2000000 |
| pool 127 | 0x96000000 + 0x2000000 = **0x98000000** (2,432 MB), which matches the logged pool | `f_80f6c580` -> `f_80f6c9d0` |
| texture count | **`TexatureHandleNum` = 10,240** (default 1,024) | `System/GameGraphics/ModuleInitializeParam/Default.game__gfx__ModuleInitializeParam.bgyml` in `Pack/Bootup.Nin_Ounce_NVN2.pack.zs`; getter `f_831cb8b0` (+0xc0) |
| cache rejection threshold | **`TextureCacheRejectionThreshold` = 120 MB** (default 0: no threshold eviction) | same file; getter `f_831cb910` (+0xc4), `mgr+0x340` |
| compaction jobs | `TextureCompactionJobNum`, default 32 (not set in the file) | getter `f_80a78b80` (+0xc8) |
| preloaded textures | `PreLoadTexture`: 25 foliage textures (`Work/Model/System/Foliage/output/textures/*.ftxb`) | same file; what keeps them was not traced |
| eviction batch | `mgr+0x33c`, N/8 = 1,280 | `f_80f6c580` |

Eviction is driven by memory only: an allocation failure, or less than 120 MB free. Neither distance nor time appears
in agl. Distance-driven streaming is above it: banc/area loading (OptimizeChangeBanc) and the bake modules decide which
names are requested and released. That layer was not traced.

## 4. What identifies a texture

| Key | Stable across | Changes at | Notes |
|---|---|---|---|
| td pointer | the td's life: moves, retargets, Replace | destroy (the memory is reused) | the agl handle |
| NVNtexture `[td+0xd8]+0x10` | the td's life | re-initialized in place by every rebind, alias retarget and Replace step | the object the host sees in nvnTextureInitialize/Finalize |
| descriptor id td+0xe8 | rebinds (re-registered with the same id) | Replace step 1, alias retarget, destroy | a released id is reused later (INFERRED: after a frame-parity delay) |
| name td+0xf8, crc32 key | always | never | an alias's name is not a BNTX name; its target's is |
| BRTI td+0x58 | everything but Replace | Replace step 1 | an alias's is its target's |
| slot td+0x18 | until the next compaction apply | every move; for an alias, also retarget | shared by a target and all its aliases |
| source BNTX | only during `f_80798690` (src: the file buffer) | the buffer is gone afterwards (INFERRED) | the host's record lookup must happen at the load |
| resident mips | always all: BRTI+0x16 = td+0x6e | never | there is no resident-range field |

For a prepared-store record: the head texture's record follows from its load (name + descriptor + place in the file,
as today). An alias's record is its target's record, and the target is known exactly: the head of the alias's +0x188
chain, or `f_807974f0`'s target key. The descriptor check then holds by construction.

## 5. Terrain tiles: a separate system

- The tera manager (`f_809e3350`) makes its own pools through the same agl wrapper (`f_809e3350+0x334` -> `f_80f6cf50`
  -> `f_80f6c9d0`). These are the "201 MB at 0x11685d0000" and "2 MB at 0x11681b6000" pools the host logs at the scene
  load, on SceneLowPriority, as "texture data pools". 0x11685d0000 is the tile pool of asset-path.md section 5.
- Tiles are TARB files read by tera::FileResource. Their texels are memcpy'd at `f_80c17040+0x544`
  (docs/direct/m0-terrain-texels.md).
- There is no TextureData, no UnifyTex, no LRU and no compaction. The tile texture objects are initialized again over
  storage the game does not copy into again (M6f, by run).
- (VERIFIED: the call chain and the pool address; the tile reuse policy was not read.)

## 6. The host's hook points against the code

Hooks on native-vk (`host/direct_assets.c`, `host/direct_tex_dedup.c`, `host/direct_diag_nodata.c`,
`host/direct_texwatch.c`, `host/nvn_native.c`). The m6f worktree differs only in the dedup key (`k.content` instead of
`k.tex`) and in the tile bytes kept by address.

| Hook | What the game state is there | Host assumption | What the code says |
|---|---|---|---|
| `ENTRY_HOOK(0x80798690)` loadImage_ with src | a head texture's full load, every mip, on the requesting thread; td not yet in the dictionary (it may lose a race and be destroyed right after) | the load names the record by src's BNTX | correct |
| same, src 0 | **an alias** (or a TextureInfo load with no data, `f_809ee930`), sharing its target's slot and BRTI | `direct_tex_dedup_nodata`: the content is "the last load with data, or the last rebind, noted at this slot address" | the content is the alias's target S. The target is in the td itself (+0x188 chain), so no guess is needed. The slot map can name another texture: the play log's "its record has another descriptor" cases all have S's descriptor and a different noted content. Which of the map's writers left a stale entry was not settled statically. Candidates: (a) the rebind hook notes **each alias** at the new base after its head (`f_80797da0` walks the chain), so the last alias moved wins, and an alias whose own link was a plain image poisons the slot for the next one; (b) between the compaction thread's move and the frame's apply, S+0x18 still holds the old block, which the heap may already have given to another texture |
| `ENTRY_HOOK(0x807982f0)` rebind | compaction apply (the frame update, every alias too), Replace step 1 (a new BRTI over a request's pool, not the heap) and step 2 | "the object is initialized again: the same record"; a file BRTI in res means streamed data | correct for compaction (res is td+0x58, the same descriptor). For Replace step 1, res is a file BRTI and the storage is the request's own pool: a new record. "Streamed" is the wrong word: there is no mip streaming |
| `ENTRY_HOOK(0x807da480)` compaction step | the compaction thread, any state | clearing `worker+0x1f21` stops the texel copies | correct while `heap+0x90` (a copier object) is null; with a copier the copy is not a memcpy |
| `ENTRY_HOOK(0x83537d90)` replaceLoad_ | Replace step 2 | counted | correct; step 1 (`f_83538060`) is not hooked and comes first |
| `ENTRY_HOOK(0x80798b60)`, `(0x80792bd0)`, `(0x80798920)` | in-place files (ui2d, 74 BNTX) | | outside TextureDataMgr; not cached, moved or aliased |
| nvnTextureBuilderSetPackagedTextureData + nvnTextureInitialize (`direct_assets_skip_packaged`) | in rebinds: the storage's own CPU address | "packaged" = ui2d sources | after the in-place loads, **every packaged init is a compaction rebind**: the counter equals the rebind counter |
| nvnMemoryPoolInitialize at lr `0x80f6cb20` (`direct_assets_pool_made` -> `hsd_texture_data_pool`) | agl's generic pool wrapper `f_80f6c9d0` | "agl::TextureDataMgr, the only pool made from there" | five callers. The 201 MB and 2 MB pools named at the scene load are tera's, and a Replace request makes one too. Only the call from `f_80f6c580` (return 0x80f6c680) is the texture heap |
| plan, "residency follows the game's registrations; no eviction" | registered ≠ in use | | a texture at reference count 0 stays initialized and registered in the cache until memory pressure. On the console the cache is bounded by the 2,400 MB heap. A host image per registered texture is bounded only by what the host does at nvnTextureFinalize, which comes late (2.6) |
| (not hooked) `f_807974f0` alias create/retarget, `f_80517d80` release, `f_8079bbe0` destroy | | | see 7 |

## 7. What a host needs

The smallest set of game events that describes every TextureDataMgr texture's residency. Each is a function entry or
exit, with what it carries. The NVN-level calls already hooked (nvnTextureInitialize / Finalize, RegisterTexture) see
the same events only as anonymous object re-initializations.

| Event | Function (where to observe) | Carries |
|---|---|---|
| created with data, all mips | `f_80798690` entry with x2 != 0 (or `f_80798d00` exit, after a race is resolved) | td, slot = [td+0x18]+0x10, BRTI td+0x58, src (its BNTX at src - data offset), name td+0xf8, NVNtexture [td+0xd8]+0x10 |
| created as an alias of S | `f_807974f0` (x1 alias key, x2 target key); inside it `f_80798690` with x2 = 0 | A, S (= the head of A+0x188), the shared slot, S's BRTI |
| alias retargeted | `f_807974f0` with an existing alias: `f_8079c220(A, 0)` then the alias create | A's old target and new target; A's object finalized, then initialized again |
| moved | `f_807981f0(td, ctx)` (compaction apply from `f_80797da0`, each alias after its head; Replace step 2) | td, new slot td+0x18. Bytes unchanged, descriptor id unchanged |
| content replaced | `f_83538060` (step 1: new BRTI, request pool), `f_83537d90` + `f_807981f0` (step 2: into the heap), `f_83538390` (request retired) | td, new BRTI r+0x290, new data r+0x2a8 |
| released to the cache (evictable, still resident) | `f_80517d80` when the count reaches 0 (an alias is destroyed there instead) | td |
| taken from the cache | acquire paths with the count 0 -> 1 (`f_8051d560`, `f_805c5ca0`, `f_80798d00`, `f_809ee930`), visible as `f_8079aa20(slot, 0)` | td |
| destroyed | `f_8079bbe0(mgr, td)` -> `f_8079c220(td, 1)`: nvnTextureFinalize, descriptor released, block freed or handed to the next alias | td, and whether the block survives (aliases left) |
| texel bytes moved (no NVN effect yet) | the compaction movers (`0x807dbe4c`, `0x807dc300`, `0x807dc680` and `f_807db4a0`) | old and new address, size |

What the game does today, in one line each:

- loads are synchronous and whole;
- moves are asynchronous for the bytes and once a frame for the NVN objects;
- Replace takes three frames per step;
- unloads are deferred into an LRU cache;
- eviction is synchronous under allocation pressure, or once a frame under the 120 MB threshold.

## 8. Not known

- What decides compaction (`f_807da590`'s policy), and whether `heap+0x90` (a copier object) is ever set.
- Which writer leaves the host's slot map stale in the play-log cases (6). It needs one run with the slot map's writes
  logged against `f_807974f0`'s target.
- Who sets the TextureResourceMgr request flag that makes Replace requests (`+0x18`), and whether any scene does.
- The descriptor id's reuse delay (`f_8079c360`'s two free lists).
- The bake LOD and the banc streaming above agl, which decide what is requested and released, and so the churn rate.
  How `PreLoadTexture` textures are held.
- Whether the decompressed BNTX file buffer is freed right after registration (`f_8080b4c0`).
- Exact names: the classes are named from strings; `0x84cae130` = SetPackagedTextureData is INFERRED from counts.
