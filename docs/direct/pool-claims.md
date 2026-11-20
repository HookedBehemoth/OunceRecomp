# Pool claims

This page explains how the GPU gets the game's memory without layer 0. Every memory pool the game makes is
claimed by a game patch, and the claim says where the pool's GPU copy lives and how data moves between it and the
game's memory. Read it before you add a pool patch, change `renderer/api/nvn_claims.h`, or change how the back end backs
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
| Upload | device-local memory the back end owns | staged copies of the ranges the CPU wrote, at the guest's submit (see below) | uniform rings, vertex and index data, per-frame CPU data |
| Readback | device-local memory the back end owns | a staged copy-out of the GPU's writes into the game's memory when their submit completes, before any wait on it returns | report and query memory, GPU results the CPU reads |
| Load once | device-local memory the back end owns | each page copied from the game's memory the first time a submission uses it, then never again | the in-place files' vertex and index data |

Every kind the GPU reads is device-local. Host-visible memory keeps the cost layer 0 has on NVIDIA, where the GPU reads
system memory: Kokiri runs at 5 fps there. Upload and Readback pay a staging copy instead.

A pool can need two kinds for parts of it, for example an object block the CPU fills from a template and the GPU then
adds to. The claim is then made on the allocation: the callers of a generic allocator (OoT's `gpu_pool_create`) claim
their own kind, and claim nesting (the outermost claim holds) gives the caller's kind to the pools made for it. A block
that is both Upload and written by the GPU is claimed Readback with an upload at its fill: the CPU's template is copied
in once, when the patch on its filler reports it, and never again from bound ranges, so a later copy cannot overwrite
what the GPU added.

## Where Upload data comes from

The back end sees every uniform, vertex, index and indirect range a frame binds, at record time. Those ranges are the
default Upload signal: their granules are marked when bound and copied in at the guest's submit, before the work that
reads them. The game's flush and invalidate calls are never a signal. Patches report writes only where binds cannot:

- data shaders reach through stored pointers (storage buffers by address), reported by the writer or its allocator;
- load-once data, such as the in-place files' vertex and index pools (a Load once claim), copied a page at a time
  at the first submission that uses it and then excluded from the per-submit copies. In OoT some of those pages
  change 46 to 55 presents after their pool is made, all before their first use (the check finds no write after it in
  Kokiri and Market), so a copy when the pool is made would be stale; no loader-end signal is known yet.

**How the copies run.** An Upload or Readback claim's region is device-local, in the same arenas as VRAM claims, and is
copied whole from guest memory at the first submission after its pool is made. At each submission, when its first
chunk goes to the queue, the merged ranges its lists read in Upload regions are copied from guest memory into a
host-visible staging chunk and from there into the regions, before its work. A staging chunk is reused once the GPU is
past its submission.

**Pools both uploaded and written by the GPU.** A page that any GPU work writes (a storage buffer the program writes,
a copy's or a fill's destination, a report) belongs to the GPU from the submission that first writes it: bound
ranges never copy it again, so a re-upload cannot overwrite what the GPU added. The CPU's writes there reach the GPU only
through a writer's report, and the CPU reads the GPU's only through a Readback range.

A report from a ring or an allocator arrives before the write. With two guest queues, an async-compute submit can fall
between the allocation and the write, so a report is kept live until its ring flips, not consumed by the next submit.

**The verifier.** An Upload write nothing reports leaves stale bytes on the GPU, silently. With `debug.claimcheck`,
each submission's lists note the guest ranges they use (uniform, vertex, index, indirect, storage, copies), merged by
region. When its first chunk goes, the guest bytes of the ranges the GPU only reads are kept, and a copy of what
their regions hold runs before its work; once it completes the two are compared, and a mismatch is an error line and a
strict event that names the range, its use, the pool with its claim and claiming patch, and the lists. It uses no page
protection. It also confirms the load-once copies. Ranges in layer-0 claims that any GPU work writes are left out.

**The measurement.** `debug.claimuse` prints, every 600 presents, the bytes bound a frame per pool group (claiming
patch, flags, claim) and per use, the largest ranges, the pages that changed since their pool was made and between
uses, and the guest data shaders reach through stored addresses (a storage buffer addressed by a word outside the
binding table, or an address held in a small storage buffer of a program that uses global memory).

## How the GPU finds a claimed pool

Translated shaders read buffers through the address table (`bda_emit.h`): one entry per 64 KB of guest addresses,
holding the distance from a guest address to the device address that serves it. Without layer 0 the table stays,
and only what its entries point at changes:

- A VRAM, Upload or Readback claim gets a region: a buffer the pool's size, bound into an arena of its kind (a few
  large device allocations, the pools sub-allocated). Its granules' entries point into that buffer. Pools that share
  a granule split it into page entries. Memory allocations stay few: OoT makes about 4,574 heap pools, more than the
  common limit of 4,096 allocations, and RADV lists every allocation in every submission (about 0.75 µs each). Buffer
  objects over an arena add neither.
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
| GPU results the CPU reads (exposure light sums, light visibility) | the CPU reads imported memory | Readback claims on the allocations, copied when their submit completes |
| CPU_NO_ACCESS pools | layer 0's VRAM class | VRAM claims |
| The texture heap, shader code | already left unbound | Textures and Host claims |

## What the GPU writes and the CPU reads

Measured in OoT's Kokiri test (`debug.replog`, `nvn.log_pools`):

- **Event words** are written by the host, in submission order, not by the GPU: no readback.
- **Reports and queries** live in the gsys heap's general pools (flags 0x22, six pools in Kokiri), between other
  allocations, and in DKB also in 32-byte buffers at the end of sead's command memory (a Host claim). The back end
  writes each report itself: the resolve step writes its 16 bytes into the command slot's out records, which are
  copied into guest memory when the submission completes, before any wait on it returns (`vk_readback.cpp`), and
  also into the claim's region when the address has one, where render conditions read it. No claim is needed for
  them.
- **GPU results through storage buffers** (the exposure light sums, the light visibility counters) are allocations
  inside the same heap. Their makers (`exposure_light_buffers_create`, `render_light_visibility_buffer`) claim them as
  Readback ranges by allocation. The light visibility counters are also zeroed by the CPU each frame
  (`render_light_visibility_readback`), so their reader is patched to report that write as an upload, and a copy-out
  from a submission recorded before it must not land after it.

Readback is therefore by range, not by pool: a registry of guest ranges, each copied from its region to staging after
the submissions that may write it and into guest memory once they complete, before any wait on them returns.

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
| sead device and queue pools | events (0x12), command memory (0x12), shader scratch (0x21) | Host, Host, VRAM (each call) | `sead_device_queue_pools` 0x80f69040 | `gfx_system_init` 0x805e7a88 | to find |
| the display's buffers | three 4K window textures (0x21, 95.6 MB) | Targets | `display_buffers_pool_init` 0x8114e4f0 | 0x80802d48 | at boot, to find |
| descriptor pools | texture and sampler descriptors | Host | `desc_pools_create` 0x80966440 | 0x807a6234 | to find |

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
| 1 | (done) The claim API: kinds in `nvn_claims.h`, the claim stored on the pool, an unclaimed pool an error. Every OoT and DKB pool claimed at its engine function; GPU-visible pools on a temporary `Layer0` kind | 400 lines, mostly patches |
| 2 | (done) Host and Textures claims leave layer 0: shader code, command memory, descriptor pools, texture heaps, in-place texture files. The back end's texture-data-pool and shader-code special cases go | 200 lines, deletes about as many |
| 3 | (done) Regions for every claim the GPU reads, in arenas per kind; the sparse windows go. The address table's entries point into the regions, and the CPU lookup replaces `layer0_span`. VRAM claims move first (CPU_NO_ACCESS pools) | 500 lines, deletes about 700 |
| 4 | Readback by range: the report slots the back end writes, and the light-sum and light-visibility allocations their makers claim, copied into guest memory when their submissions complete, before the guest's wait returns. Lands with step 6, when their pools become device-local; until then the import keeps them coherent. Then an NVIDIA Kokiri run (below) | 400 lines |
| 5 | (done) The verifier, and the measurement: bytes bound per frame per pool, and the data reached through stored pointers | 250 lines |
| 6 | Upload as bound-range copies at the guest's submit, load-once copies at their claim's end, and writer patches for the exceptions the measurement names | 600 lines, plus the patches |
| 7 | Remove layer 0: the imports, udmabuf and the host-pointer path, `os_guest`'s export, the import probe, their settings and docs | deletes about 800 lines |
| 8 | The agl texture takeover, then MKW's pools on the same patches | 600 lines across the games |

Steps 3 to 6 keep the remaining pools on `Layer0` until their claims move, so each step lands with both games
running. Expect a small regression on AMD, where layer 0 was zero-copy: Upload and Readback add copies.

### The NVIDIA check after step 4

There is no NVIDIA GPU here; the run comes from the user. On a Windows PC with an NVIDIA GPU, build the branch and
run `ounce profile oot --scene kokiri`. Check:

- the summary line's frame time and GPU busy, against the same run before step 4;
- `direct: claims:` names the light-sum and report pools under readback, with no failed regions;
- no `nvn: ERROR` or `direct: ERROR` lines, and no strict events.

## Deferred

- **Driver-mapped memory as the game's storage** for patch-owned pools (`VK_EXT_map_memory_placed`): the guest's
  writes would land in GPU memory with no copy. Only Mesa has it; NVIDIA is unconfirmed and Windows lacks it, CPU reads
  of VRAM are slow, and it cannot serve in-place files. Not planned.
