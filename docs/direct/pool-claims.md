# Pool claims

This page explains how the GPU gets the game's memory. Every memory pool the game makes is
claimed by a game patch, and the claim says where the pool's GPU copy lives and how data moves between it and the
game's memory. Read it before you add a pool patch, change `renderer/api/nvn_claims.h`, or change how the back end backs
a pool. The order of work is at the end.

## Why claims

The games assume unified memory: the CPU and the GPU read and write the same bytes, and nothing tells the driver
what changed. A PC has two memories. Importing the game's memory into the GPU (dma-bufs on Linux, host pointers on
Windows) would hide that, but it works only where the platform can import memory and the driver reads it fast, and it
puts every texture the game copies, every shader and every command list on the GPU's side for nothing. The back end
imports no guest memory.

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

Every kind the GPU reads is device-local. On NVIDIA the GPU's reads of host-visible system memory are slow enough that
Kokiri ran at 5 fps from it. Upload and Readback pay a staging copy instead.

A pool can need two kinds for parts of it, for example an object block the CPU fills from a template and the GPU then
adds to. The claim is then made on the allocation: the callers of a generic allocator (OoT's `gpu_pool_create`) claim
their own kind. Claims nest: a kind a patch names holds over a generic allocator's rule, and the innermost kind holds
over the kinds around it, so a maker's exception (a Readback buffer among GPU scratch) sits inside its caller's claim.
Between rules, the outermost holds.

## Where Upload data comes from

The back end sees every uniform, vertex, index and indirect range a frame binds, at record time. Those ranges are the
default Upload signal: each list notes them when it records them, and the submission's first chunk copies them in,
before the work that reads them. The game's invalidate calls are never a signal, and its flush calls are one only where
a patch marks them (see Reported writes). Patches report writes only where binds cannot:

- data shaders reach through stored pointers (storage buffers by address), reported by the writer or its allocator;
- load-once data, such as the in-place files' vertex and index pools (a Load once claim), copied a page at a time
  at the first submission that uses it and then excluded from the per-submit copies. In OoT some of those pages
  change 46 to 55 presents after their pool is made, all before their first use (the check finds no write after it in
  Kokiri and Market), so a copy when the pool is made would be stale; no loader-end signal is known yet.

**How the copies run.** An Upload or Readback claim's region is device-local, in the same arenas as VRAM claims, and is
copied whole from guest memory at the first submission after its pool is made. An Upload region keeps a bitmap with a
bit per 16 bytes. The sender thread prepares a submission's copies just before it submits the submission's first chunk,
in queue order. It marks the ranges the submission's lists read in the bitmaps, then clears every marked bit and
copies its bytes from guest memory into a host-visible staging chunk, and from there into the regions, before the
work. No sorting or merging of ranges is needed. Staging is chunks of 16 MB, a copy larger than a chunk split over several; a
submission holds its chunks until it completes, and idle chunks beyond four are freed.

**Event waits the CPU signals.** OoT's frame starts with a gate: the Presentation Thread submits the frame with a GPU
wait on an event, and the main thread signals that event from the CPU at the frame's end. On the console, everything
the CPU writes before its signal is visible to the work after the wait. The game relies on it: about ten uploaded
ranges a frame in Kokiri (in `gpu_pool_create`'s per-view and uniform pools) change between the submit and the signal.
So when the CPU's signal meets a wait that held a submission, the sender compares the ranges that submission copied
with guest memory, 256 bytes at a time, and copies the changed blocks again before the work after the wait. Waits that
another queue's work meets get no such copy: they order nothing for the CPU.

The writes the check still finds after the signal and before the submission completes (about one a frame) are races
in the game itself: they come from the next frame's calc jobs (`job_calc_runner`, through `f_814f2c80`, the material
parameter writer), which run while the GPU draws the frame. The console GPU reads the new value only if its draw comes
after the write. The next frame's submit copies them, so they need no patch.

**Pools both uploaded and written by the GPU.** A 16-byte grain that any GPU work writes (a storage buffer the program
writes, a copy's or a fill's destination) belongs to the GPU from the submission that first writes it: bound ranges
never copy it again, so a re-upload cannot overwrite what the GPU wrote. The CPU's writes there reach the GPU only
through a writer's report (`nvn_claim_write`), and the CPU reads the GPU's only through a Readback claim.

**Reported writes.** A writer patch reports a CPU write after it. The next submission's first copy copies the range in
before its work, whatever the GPU owns, and the copies out of submissions whose first copy came before the report drop
that range: an older GPU value never lands over the CPU's write. OoT's light readers zero the sums they read this way.

Where the writer flushes what it wrote, the patch marks the flush call instead (`nvn_claim_flush_written`), and that
flush's range is the report. A mark needs a census of that call: every write of its class lies inside the flush that
follows it on the same thread, and the range holds no grain GPU work writes. Unmarked flushes are no signal: most cover
whole blocks (megabytes a frame in OoT) or grains the GPU writes, which a report would hand back to stale CPU data. The
census and the classes it rules out are in [engine classes](engine-classes.md).

**Copies out.** A Readback claim's region takes the CPU's writes like an Upload region. The ranges its submission's GPU
work writes (storage written, copy and fill destinations) are copied into staging by a command buffer after the
submission's last work, and into guest memory when it completes, before any CPU wait on it returns (`nvn_wait`,
`nvn_finish`, the sync waits: `vk_readback.cpp`'s order).

**Resets made on the GPU.** Where the CPU resets memory the GPU owns (a list's appended count, an indirect record), the
writer patch keeps the game's store and adds `nvn_claim_fill`: the same words filled on the GPU before the next
submission's work, with a report's protection against older copies out. OoT's culling lists reset this way, from
host code that replaces the culling object's reset function (`cull_lists.c`).

**Ranges the CPU reads of a VRAM pool.** `nvn_claim_read` names a range of a VRAM pool that the CPU reads, such as a
list's header in a list of megabytes. After each submission whose work writes the pool, that range alone is copied out
into guest memory, as a Readback claim's ranges are. OoT names its culling lists' counts and headers once, when the
culling object is made.

**The maker's scope.** The pools a claim's scope makes are copied whole at the first submission after their making.
When a submission's first copy came before the outermost scope ended, they are copied again then, so a maker that fills
its pool after making it (the claim around the whole function) needs no report.

**Storage buffers by stored address.** A program's storage buffer whose address and size are words the game stored in a
uniform buffer, or in the driver bank outside its binding table, counts as bound, read or written as the program
declares it. Global memory a program reaches through an address it loaded is not seen.

**The verifier.** An Upload write nothing reports leaves stale bytes on the GPU, silently. With `debug.claimcheck`,
each submission's lists note the guest ranges they use (uniform, vertex, index, indirect, storage, copies), merged by
region. When its first chunk goes, the guest bytes of the ranges the GPU only reads are kept (VRAM claims' ranges aside:
shaders write those through addresses no binding names, and the owned-page scan below checks them), and a copy of what
their regions hold runs before its work; once it completes the two are compared, and a mismatch is an error line and a
strict event that names the range, its use, the pool with its claim and claiming patch, and the lists. It uses no page
protection. It also confirms the load-once copies. Ranges that any GPU work writes are left out. Once the submission
completes, the check also compares the bytes it copied with guest memory and logs the first ranges the CPU wrote
meanwhile, with their count in its summary line. Every 600 presents it also hashes the pages the GPU owns (a CPU-visible
VRAM claim's, and the GPU-written grains of Upload and Readback claims): a change no report or copy out explains is a
CPU write the GPU never sees, counted as a mismatch and named by pool.

**The measurement.** `debug.claimuse` prints, every 600 presents, the bytes bound a frame per pool group (claiming
patch, flags, claim) and per use, the largest ranges, the pages that changed since their pool was made and between
uses, and the guest data shaders reach through stored addresses (a storage buffer addressed by a word outside the
binding table, or an address held in a small storage buffer of a program that uses global memory).

**The census.** `debug.poolcensus` names who on the CPU writes and reads a pool the GPU uses, and when. It watches the
pools that claim names (`vram`, `upload`, `readback`, `loadonce`; the value `1` picks all four), `by:<patch>` or a hex
address pick, joined by `+`; `whole` also watches the pages no submission binds, which shaders reach through stored
addresses. A page a submission binds faults on its next CPU write, and a page GPU work writes faults on its next CPU read
too, once per present. Each fault is counted by pool maker (its creation call chain) and by writer or reader (function,
guest call chain, thread), and timed against the submissions that use the page: none outstanding, submitted but not
started, or on the GPU, and for reads whether the writing submission completed this present or earlier. A write on a
grain GPU work writes, and whether a writer's report followed it, are noted too. It writes `poolcensus.txt` into the run
folder every 300 presents. It changes no behaviour; the faults cost about 5 µs each. Host code in the executable is named
`f_<nearest lifted function>(exe+<offset>)`: `tools/re/samap.py` names the offset.

## How the GPU finds a claimed pool

Translated shaders read buffers through the address table (`bda_emit.h`): one entry per 64 KB of guest addresses,
holding the distance from a guest address to the device address that serves it:

- A VRAM, Upload or Readback claim gets a region: a buffer the pool's size, bound into an arena of its kind (a few
  large device allocations, the pools sub-allocated). Its granules' entries point into that buffer. Pools that share
  a granule split it into page entries. Memory allocations stay few: OoT makes about 4,574 heap pools, more than the
  common limit of 4,096 allocations, and RADV lists every allocation in every submission (about 0.75 µs each). Buffer
  objects over an arena add neither.
- Textures, Targets and Host claims have no entries: an address there reads the null window, which reads zero.
- Vertex, index and indirect-argument buffers, buffer copies, fills and report writes find their buffer and offset
  by the same lookup on the CPU.

The descriptor heap does not change: textures and samplers were never in pool memory on the GPU's side.

## How each GPU use is served

| Use | Served by |
|---|---|
| Uniform, storage and global buffers in shaders | the address table into the claim's buffer |
| Vertex and index buffers, indirect arguments, draw counts | the claim's buffer |
| Buffer copies and fills (`CopyBufferToBuffer`, `ClearBuffer`) | the claim's buffer |
| Report and occlusion results | the resolve step's records, copied into guest memory when the submit completes, and the claim's buffer where the address has one |
| Render conditions | the report's word in the Readback claim's buffer, on the GPU |
| GPU results the CPU reads (exposure light sums, light visibility) | Readback claims on the allocations, copied when their submit completes |
| CPU_NO_ACCESS pools | VRAM claims |
| The texture heap, shader code | Textures and Host claims: no GPU copy |

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
  Readback (each allocation is a pool of its own). Their readers zero the sums each frame, and patches report those
  stores.
- **Which pools the CPU reads.** A pool the game allocates CPU-cached (0x24) and invalidates before it reads is one whose
  GPU writes the CPU reads. The invalidate is never a signal, but a survey of the invalidates per pool (a 600-present
  run) names the Readback candidates; the claim then goes on their maker.

**Vfx3's particle streams.** NintendoWare Vfx3's resource pool (`gsys_vfx_mgr_init`, 0x22, Upload) holds particle
attribute buffers that draws bind as storage their programs write, and that the CPU also writes. The emitter plugins'
per-particle setters (float to mat4), the new-buffer fill and the dirty-range copy report their writes.

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

**OoT's gsys object blocks.** `ubo_object_block_alloc` makes the heap's 0x24 and 0x12 pools (252 in Kokiri), and each
maker claims its own (`games/oot/patches/pools.c`): a direct call of one of agl's block allocators by a row of the
allocation table ([engine classes](engine-classes.md)), any other maker by a claim around its own call or body. The
kinds come from a Kokiri survey of each pool's binds, page changes and the CPU's invalidates (a CPU-cached pool the CPU
invalidates is one whose GPU writes it reads):

| Maker | Kind | Evidence |
|---|---|---|
| the exposure and light-visibility buffers, `exposure_result_block_create`, `gpu_result_block_b_create`, the depth grid's read block | Readback | the CPU invalidates and reads them every frame |
| `agl_render_dl_buffer`, `agl_driver_mgr_dl`'s lists (0x12) | Host | display lists: never bound |
| the GPU culling object a (`cull_gpu_object_a_create`) | VRAM | the CPU's resets made on the GPU, the counts it reads copied out of their records |
| object b's task records (`f_80c5f870`), gather lists (`f_80c576b0`) and result blocks | Upload, Readback, Readback | the gather lists' zeroing made on the GPU |
| GeometryCullingMgr: the views' arrays, the block | Readback, VRAM | each address store reported; the exposure record in the views' pool reported |
| the vegetation task blocks (`cull_task_block_create`, object b's and `cull_task_block_fill`'s) | VRAM | the CPU's writes reported, among them a GPU address stored later into the shared pool ([pool users](pool-users.md)) |
| the depth grid: constants, slots, the 0x5b40 block | Upload, Readback, VRAM | the per-frame camera copy into the slots reported |
| the occlusion query's records and per-id state (`depth_grid_buffers_create`) | VRAM | filled once after their allocations, inside a scope over the maker that names no kind |
| the probe storage block, both calc blocks (`gpu_calc_block_create`) | VRAM | bake::ModuleMgr's records, the streamed data, the BakeProbe records and the calc fields reported; the 150 MB block's fill is in its claim's scope |
| the save picture (`texdata_serialize_jpg`) | Readback | a copy the CPU waits for, then encodes |
| terrain streams, `f_82c24db0`'s block | Load once | written before their first use (the check finds no later write in Kokiri and Market); as Upload their bound ranges cost about 1 ms of host draw |
| the tile cache, `f_80f6dc20`'s pool, texdata_cpu_lut's table, texdata_init_jpg's buffer | Host | never bound |
| `f_80c62260`, `texdata_storage_block` | VRAM | GPU scratch the CPU fills only when it makes it |
| `f_80c64650` | Upload | written later (`f_80c63e70`'s fill, reported) and bound as storage in Market |
| `f_80cdf2f0`'s blocks, `f_80c5ea90`, the agl/gfx general pool (0x22) | Upload | CPU-written, binds read them |

The census (`debug.poolcensus`) and `games/oot/patches/pools.c` give each maker's claim and its writer patches. The
rules decide the pools no maker claims, by their flags, from the census of those pools in Kokiri and Market:

| Rule | Kind | Evidence |
|---|---|---|
| gsys heap 0x24 (object blocks no maker claims: agl's DTA objects, the texture-data makers, small per-system blocks) | Upload | each written at its making and read by binds, or never used; none read by the CPU |
| nn::gfx 0xa2 (`f_80f6a898`'s 8 MB) | Upload | a few pages the CPU writes, bound |
| Vfx3 0xa4 | Upload | unused in Kokiri and Market, flags as its 0x24 buffers |
| a pool the GPU cannot access (GPU_NO_ACCESS) | Host | by its flags |
| a pool the CPU cannot access (CPU_NO_ACCESS) | VRAM | by its flags |

Any other flags are unclaimed: an error that names the call. OoT's other makers: NDSR's networks' pool (VRAM: the set-up
writes and reads it inside its scope) and `job_list_build_b`'s 16.5 MB scratch (VRAM, the CPU's zeroing reported).

DKB's claims (`games/dkb/patches/pools.c`) come from the census of `ounce test dkb` (the tunnel test: a new game
through the mode select and the punching tutorial into the mining tunnel):

| Maker | Kind | Evidence |
|---|---|---|
| GeometryCullingMgr: the views' arrays, the block | Readback, VRAM | as OoT's; the address store, inlined in `cull_geometry_slot_alloc`, reported |
| `f_807935d8`'s block | Upload | the scene drawers write about 100 pages a present, draws read it as vertex and storage data |
| `f_804ab060`'s block, agl::driver::GraphicsDriverMgr's results block | Readback | GPU work writes them, the CPU reads them |
| Vfx2's pools (`f_808ec234`, `f_808da640`) | Readback | the CPU writes particles, GPU work writes them too and the CPU reads those writes |
| `f_804a0944`'s named blocks | by flags: 0x22 Upload, 0x24 VRAM, 0x12 Upload | 0x22: written every frame, bound; 0x24: 450 MB only GPU work writes; 0x12: unused (unverified) |
| `ndsr_network_setup` | VRAM | as OoT's |
| GraphicsDriverMgr's display lists, `f_8074431c`'s RenderDLBuffer, `f_80743ba8`'s and `f_80732960`'s blocks | Host | never bound |
| `f_807ac5e8`'s data pool | Upload | from the code: mapped and filled buffers (not reached: unverified) |

The rules: the gsys heap's 0x22 and 0x24 pools Upload, nn::gfx 0x22 Upload and 0x24 load once, and the access rules.

Open in DKB's tunnel test (parked; the claims above passed the title-and-tunnel runs, but these are not settled):

- **A GPU hang in the punch tutorial.** A gfx ring timeout at about present 5500, once with a GPUVM fault at device
  address 0x800393dab000 (client TCP, PERMISSION_FAULTS 3): 3 hangs in 7 runs with the claims, before `f_804ab060`'s
  block moved from Host to Readback (GPU work writes it as storage), and 1 in the 3 runs since (a ring timeout at about
  present 5700, the submit failing with a lost device), so that move did not end it; none in 3 runs of the same test
  while DKB's pools were still imported guest memory (af634bf0). Likely a claim's, unproven; the faulting address is not yet mapped back to a pool.
- **The voxel scene renders black** (the geometry stage is not translated): a renderer issue, not a claim's.
- **A gsys 0x24 heap pool the CPU reads** after GPU work writes it (the check: "the CPU reads GPU writes outside a
  Readback claim", pool `0x111105d000+0x4f000` in one run): its maker needs a Readback claim.
- **CPU writes into GPU-owned pages** no report hands over, in another gsys 0x24 heap pool (`+0x40000`) and in
  `f_808da640`'s Vfx2 pool (now Readback; not checked since).

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
| 1 | (done) The claim API: kinds in `nvn_claims.h`, the claim stored on the pool, an unclaimed pool an error. Every OoT and DKB pool claimed at its engine function | 400 lines, mostly patches |
| 2 | (done) Host and Textures claims: shader code, command memory, descriptor pools, texture heaps, in-place texture files. The back end's texture-data-pool and shader-code special cases go | 200 lines, deletes about as many |
| 3 | (done) Regions for every claim the GPU reads, in arenas per kind; the sparse windows go. The address table's entries point into the regions, and the CPU lookup finds them. VRAM claims move first (CPU_NO_ACCESS pools) | 500 lines, deletes about 700 |
| 4 | (done) Readback: copies out of the ranges GPU work writes, reported CPU writes, the light-sum and light-visibility claims; the check's owned pages | 400 lines |
| 5 | (done) The verifier, and the measurement: bytes bound per frame per pool, and the data reached through stored pointers | 250 lines |
| 6 | Upload as bound-range copies at the guest's submit, load-once copies at their claim's end, and writer patches for the exceptions the measurement names | 600 lines, plus the patches |
| 7 | (done) No guest memory imported: the imports, udmabuf and the host-pointer path, `os_guest`'s export, the import probe, their settings and docs go | deletes about 800 lines |
| 8 | The agl texture takeover, then MKW's pools on the same patches | 600 lines across the games |
| 9 | Write-once dynamic uniform ring (below) | to be sized |

### Upload copies the bound ranges

OoT's uniform data is persistent, double-buffered per-object blocks (bound at base + (frame & 1) * stride), written in
place by a few writers. The bound-range copy moves about 0.55 to 0.97 MB a frame, of which only 40 to 60 KB is written
(work/assets/native/uniform-buffers.md, section 1). Writer reports for uniform data are not planned: a CPU profile of
Kokiri puts the whole claims machinery at about 1.1 ms of CPU a frame, 0.76 ms of it in the sender thread's preparing
of copies (1.95 MB a frame is staged), so narrowing could win at most about 0.9 ms, on a thread that rarely holds the
frame. Where a writer must report, it uses `nvn_claim_write`. See [uniform rings](uniform-rings.md).

### Step 9: write-once dynamic uniform ring

Step 9 is a later whole host replacement of `agl::utl::DynamicUniformBlockAllocator`'s alloc, writing into a ring
mapped with `VK_EXT_map_memory_placed`, so the game's data is written once and not copied. It removes a further 0.1 to
0.3 ms of sender-thread CPU. It is Linux only; Windows has no placed map and the ring stays Upload. The gsys heap's
uniform pools stay Upload. The plan is in [uniform rings](uniform-rings.md).

### The NVIDIA check

There is no NVIDIA GPU here; the run comes from the user. On a PC with an NVIDIA GPU (Windows or Linux), build the
branch and run `ounce profile oot --scene kokiri`, then `ounce test oot --name market` three times. Check in the
profile run's log (`work/oot/runs/profile-kokiri/log.txt`):

- the summary line's frame time and GPU busy, against the same run on the commit before the claims moved;
- `direct: claims: pools made by claim:` counts upload, load once and VRAM pools, with `0 failed` regions and
  `spans outside a region 0`; its `uploads since the start:` part names the bytes and the time the copies take;
- `direct: readback a frame:` shows the reports copied into guest memory each frame, with few submissions waiting;
- no `nvn: ERROR` or `direct: ERROR` lines, and no strict events.

Then, once, `ounce run oot --headless --seconds 60 --set debug.claimcheck=1` in Kokiri: its `check since the start:`
lines must report 0 mismatches.

## Deferred

- **Driver-mapped memory for other pools** (`VK_EXT_map_memory_placed`, as step 9 does for the dynamic ring): CPU
  reads of device-local, host-visible memory are uncached, Windows has no placed map, and it cannot serve in-place
  files. Not planned.
