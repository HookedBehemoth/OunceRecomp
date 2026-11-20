# Pool claims

This page explains how the GPU gets the game's memory without layer 0. Every memory pool the game makes is
claimed by a game patch, and the claim says where the pool's GPU copy lives and how data moves between it and the
game's memory. Read it before you add a pool patch, change `runtime/nvn_claims.h`, or change how the back end backs
a pool. It is also the plan for removing layer 0; the order of work is at the end.

## Why claims

The games assume unified memory: the CPU and the GPU read and write the same bytes, and nothing tells the driver
what changed. A PC has two memories. Layer 0 hid that by importing the game's memory into the GPU (dma-bufs on Linux,
host pointers or regions on Windows). That works only where the platform can import memory and the driver can read
it fast, and it puts every texture the game copies, every shader and every command list on the GPU's side for nothing.

A claim replaces the guess with a decision made where the game decides what a pool is for: at the engine function
that makes it. One patch on an engine allocator claims every pool that allocator makes. A pool no claim covers is an
error: a log line that names the guest call, and a strict event.

## Claim kinds

| Kind | The GPU's copy | How data moves | Used for |
|---|---|---|---|
| Textures | none: the pool is CPU memory only | the texture store, region uploads, the GPU's own writes into images | texture heaps, in-place texture files, glyph caches |
| Targets | none: an address range with no memory | images the back end owns | render-target allocators (agl::utl::DynamicTextureAllocator), render buffers |
| Host | none | the host reads guest memory | shader code, command memory |
| VRAM | device-local memory the back end owns | GPU only: the GPU's copies fill it | pools the CPU cannot access, GPU scratch |
| Upload | memory the back end owns, read by the GPU | the CPU's writes are copied in at the game's flush, or at the submit for the ranges a ring patch reports | uniform rings, vertex and index data, per-frame CPU data |
| Readback | memory the back end owns, written by the GPU | the GPU's writes are copied into the game's memory when their submit completes, before any wait on it returns | report and query memory, GPU results the CPU reads |

A pool can need two kinds for parts of it, for example an object block the CPU fills from a template and the GPU then
adds to. The claim is then made on the allocation, not on the pool, by a patch on the function that hands it out.

## How the GPU finds a claimed pool

Translated shaders read buffers through the address table (`bda_emit.h`): one entry per 64 KB of guest addresses,
holding the distance from a guest address to the device address that serves it. Without layer 0 the table stays,
and only what its entries point at changes:

- A VRAM, Upload or Readback claim gets one buffer of the back end's own, the pool's size. Its granules' entries
  point into that buffer. Pools that share a granule split it into page entries, as regions do today.
- Textures, Targets and Host claims have no entries: an address there reads the null window, which reads zero.
- Vertex, index and indirect-argument buffers, buffer copies, fills and report writes find their buffer and offset
  by the same lookup on the CPU.

The descriptor heap does not change: textures and samplers were never in pool memory on the GPU's side.

## What depends on layer 0 today, and how it is served

| Use | Today | With claims |
|---|---|---|
| Uniform, storage and global buffers in shaders | the address table into the import windows or regions | the table into the claim's buffer |
| Vertex and index buffers, indirect arguments, draw counts | the import windows | the claim's buffer |
| Buffer copies and fills (`CopyBufferToBuffer`, `ClearBuffer`) | the import windows | the claim's buffer |
| Report and occlusion results | a compute step writes 16 bytes into guest memory | the step writes into a Readback claim; the copy to guest memory runs when the submit completes |
| Render conditions | the report's word read through the windows | the Readback claim's buffer, on the GPU |
| GPU results the CPU reads (exposure light sums, light visibility) | the CPU reads imported memory | Readback claims on the allocations; the game invalidates before it reads |
| CPU_NO_ACCESS pools | layer 0's VRAM class | VRAM claims |
| The texture heap, shader code | already left unbound | Textures and Host claims |

## Engine allocators

The three games share agl, sead and gsys. A claim patch goes on the shared engine function, at its address in each
game. Game-specific makers stay per-game patches.

| Engine function | What it makes | Claim | OoT | DKB | MKW |
|---|---|---|---|---|---|
| agl::TextureDataMgr::InitializeHeap | the texture heap (flags 0x24) | Textures | `agl_texture_heap_create` 0x80f6c580 | 0x80577140 | `assets_heap_init` 0x80b4d8c0 |
| agl::utl::DynamicTextureAllocator, pool record | one physical pool (0x18c) and one virtual pool | Targets (done) | `dta_pool_record_create` 0x80c49380 | `dta_init` 0x80737cac | to find |
| gsys GPU heap | 4,574 pools in OoT: 0x22, 0x24, 0xa1, 0x12 | by allocation: Upload, VRAM, Readback | `gpu_pool_create` 0x809de920 | f_80198500 | `gpu_heap_grow` 0x805f72f0 |
| nn::gfx MemoryPool for in-place files | BFRES vertex and index data (0x24), shader binaries (0x64), ui2d (0x21) | Upload (load time), Host, Textures | `gfx_memory_pool_init` 0x80792e30 | 0x8052f8a8 | to find |
| shader archives | shader code (0x62) | Host | `shader_sharcb_load` | f_805ba7c0 | to find |
| sead device and queue pools | command memory (0x12, 0x21) | Host | `sead_device_queue_pools` 0x80f69040 | f_805e7a88 | to find |
| the display's buffers | three 4K window textures (0x21, 95.6 MB) | Targets | f_8114e4f0 | f_80802d48 | at boot, to find |
| descriptor pools | texture and sampler descriptors | Host | `desc_pools_create` 0x80966440 | to find | to find |

Per-game makers:

- **OoT:** the tera manager's pools (`terrain_tera_mgr_init`, through agl's second wrapper); the sky tables and
  terrain tiles are region uploads already.
- **MKW:** the sparse bake arrays, the shadow bake and the rut mask (`sparse_virtual_pool_init`) become plain
  arrays the back end owns, fed by `bake_cell_copy_flush`'s per-layer copies. Virtual pools are otherwise an error.

## The agl texture takeover

Every agl path that makes texture storage becomes the host's: the texture heap, `loadImage_` and its inlined forms,
compaction, rebind and evict, the render-target and render-buffer allocators, the transient pool, in-place BNTX files
(ui2d, fonts, Vfx2), the code-made defaults (agl's TextureInfo path) and the glyph caches. The back end then owns all
texture memory, and the texture heap needs no GPU-visible pool: it stays as CPU bookkeeping, because agl's
compaction and free lists keep their nodes in the heap's own memory (the DTA's physical pool holds only its arenas'
free-list nodes). A Textures claim says exactly that.

## Order of work

Each step is one commit. Each one passes `ounce test oot`, a 60-second DKB run, and a Kokiri profile before and after
(GPU busy and frame time; about 5.3 ms and 6.5 ms before the work).

| Step | What | Size |
|---|---|---|
| 1 | The claim API: kinds in `nvn_claims.h`, the claim stored on the pool, an unclaimed pool an error. Every OoT and DKB pool claimed at its engine function; GPU-visible pools on a temporary `Layer0` kind | 400 lines, mostly patches |
| 2 | Host and Textures claims leave layer 0: shader code, command memory, descriptor pools, texture heaps, in-place texture files. The back end's texture-data-pool and shader-code special cases go | 200 lines, deletes about as many |
| 3 | Back-end buffers for claims: a VkBuffer per claimed pool, the address table's entries pointed into it, the CPU lookup that replaces `layer0_span`. VRAM claims move first (CPU_NO_ACCESS pools) | 500 lines |
| 4 | Readback: GPU writes copied into guest memory when their submit completes, before the guest's wait returns. Reports, queries and the light-sum buffers move | 400 lines |
| 5 | Upload: copies at the game's flush calls (CPU_CACHED pools) and at the submit for ring ranges a patch reports. The uniform rings, object blocks and the in-place files' vertex data move | 700 lines, plus a ring patch per engine allocator |
| 6 | Remove layer 0: the import, the sparse windows, the regions' imports, udmabuf and the host-pointer path, `os_guest`'s export, the import probes, their settings and docs | deletes about 1,500 lines |
| 7 | The agl texture takeover, then MKW's pools on the same patches | 600 lines across the games |

Steps 3 to 5 keep the remaining pools on `Layer0` until their claims move, so each step lands with both games
running.

## Open questions

- Which 0x22 (CPU uncached) pools the CPU writes every frame without a flush call. Those need a ring patch that
  reports the written ranges; measure the flush calls per pool before step 5.
- Whether a pool the CPU fills once at load and then never touches can be claimed as Upload with one copy at its
  first use, or needs a load-end signal from its loader.
