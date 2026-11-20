# Engine classes behind the pool patches

This page groups the store-level pool patches by the engine classes they patch. For each class it says whether a
single host-side replacement can stand in for those patches in OoT, DKB and MKW. Read it before you add another
`nvn_claim_write`, `nvn_claim_flush_written`, `nvn_claim_fill` or `nvn_claim_read` row, or a claim scope around an allocation call. The claim
kinds are explained in [pool claims](pool-claims.md), and the makers in [pool users](pool-users.md).

The facts come from reading the three games' code and, for the flushes, from a census of OoT's flushes in Kokiri. The class names come from
the games' own `getName` strings and from the Tears of the Kingdom agl and gsys headers exported in
[aquacluck/ghidra_scripts](https://github.com/aquacluck/ghidra_scripts) (`dt_totk_gdt_export`). That export has no
licence, so this repository cites it and copies nothing from it.

## Summary

Most of the patches are not about the classes' logic. They patch the two places where agl already tells the GPU
what the CPU did: `UniformBlock::flush`, which the game calls after it writes a block in a CPU-cached pool, and the
allocation of a block. A flush is a reported write only at the flush calls a patch marks, because most of the game's
flushes cover far more than the CPU wrote, or bytes the GPU writes. The second replacement claims a block where it is
allocated, from a per-game table of call sites (done in OoT). The GPU culling lists are the one class that is worth replacing
outright, and only OoT has them; OoT's are replaced by host code.

| Class | Games | Methods: functions or inlined | Patches retired | Size | Recommendation |
|---|---|---|---|---|---|
| agl::UniformBlock and ShaderStorageBlock: flush | all three, and TOTK | flush is a function; writes, copy flips and address math are inlined in hundreds of functions | 8 write reports in OoT's `pools.c`, now marked flush calls | about 15 lines in `nvn_native.c` | done in OoT; DKB and MKW need their own census |
| agl::UniformBlock and GPUMemBlockBase: allocation | all three | the allocators are functions (OoT 180, 34 and 110 calls) | 22 claim scopes (44 rows) in OoT, now table rows | about 60 lines, plus a table of call sites per game | done in OoT; DKB and MKW next |
| gsys GPU culling lists | OoT | the reset is inlined per list, plus 2 calls; the header read is a function; the gather lists' zeroing is inlined in an actor function | `mesh_lists.c` and `mesh_args.c` (about 60 rows), and the 11 count fills of `actor_records_read_a` | about 200 lines and a table of lists | done in OoT: the reset function replaced by host code |
| agl::utl::DynamicUniformBlockAllocator | all three, and TOTK | init and alloc are functions | none | about 120 lines | only together with mapped pools (uniform rings, step 9) |
| Readback objects: exposure lights, light visibility, DepthBufferEx, the query object | DepthBufferEx in all three; the rest OoT | makers and readers are functions | covered by the first two rows | none of its own | keep the maker claims |
| gsys::GeometryCullingMgr | all three, and TOTK | the slot allocator is a function; its address store is inlined in DKB | covered by the first two rows | none of its own | no class replacement |
| NintendoWare Vfx3 attribute buffers | OoT | the setters are functions | none | | keep the store-level reports |

## agl::UniformBlock and agl::ShaderStorageBlock

The "ubo object" of the labels is agl::UniformBlock; agl::ShaderStorageBlock derives from it and adds a block
alignment. An object holds a pointer to its declarations (the members, 12 bytes each: count, offset, element size,
type), its CPU address, its place in a pool (offset, pool, the GPUMemBlockBase that owns the memory), its block
size, the copies and the current copy in one byte, flags, an NVNbuffer and its GPU address. OoT's and DKB's allocators
store the same fields at the same offsets, and MKW's GeometryCullingMgr uses the same object size. TOTK's is the same class with the members from the pool offset on 8 bytes earlier, because the
offset is 32-bit there.

Its methods:

- **Declare:** `ubo_object_init` and `ubo_object_add_member`, functions in every game.
- **Allocate:** `ubo_object_pool_alloc` (object, heap, copies, elements) makes a GPUMemBlockBase through
  `ubo_object_block_alloc`, then `ubo_object_set_buffer` (DKB and MKW: `agl_ssb_set_storage`) makes the buffer. DKB
  inlines the buffer set-up into the allocator, and its `ubo_object_block_alloc_b` inlines the pool helper.
- **Write and flip:** inlined. Writers flip the current copy and compute base + aligned size × (elements × copy +
  element) + member offset themselves. Hundreds of functions do it, so no patch on a method can see the writes.
- **Flush:** `ubo_object_flush`, and the same range computed inline in writers such as `cull_gpu_list_reset_one`. When
  `nvnMemoryPoolGetFlags` says the pool is CPU-cached, the writer flushes the current copy it wrote. Readers call
  `nvnMemoryPoolInvalidateMappedRange` before they read.
- **Bind:** `ubo_object_bind` and `ubo_object_bind_uniform`, functions with about 830 and 250 calls in OoT.

The back end handles the invalidates as readbacks. **A flush is a reported write where a patch marks its call:**
`nvn_claim_flush_written`, placed on the instruction that calls `nvnMemoryPoolFlushMappedRange` or
`nvnBufferFlushMappedRange`, makes that one flush report its range as `nvn_claim_write` does. The back end needs no
game knowledge for it. A census in Kokiri decides each mark, per flush call: every write of the class lies inside the
flush that follows it on the same thread, and the range holds no grain GPU work writes. In OoT, eight calls pass:
`probe_calc_stream_write`, `probe_calc_fields_write`, `probe_storage_records_write`, `cull_veg_task_records_write`, the
two in `cull_scene_blocks_init`, `f_80c63e70` and `cull_address_block_create`.

The other flushes are not reports, for three reasons:

- **They cover far more than the CPU wrote.** agl flushes a whole block: NintendoWare's dynamic buffers flush 2 MB
  about ten times a frame, the culling lists that no GPU work is seen to write about 3.5 MB a frame, and
  `probe_bake_records_write` 156 MB per bake, before it writes. As reports, they would copy about 24 MB a frame.
- **They cover grains the GPU writes.** The culling lists, `f_80c576b0`'s lists, the depth grid's slots (with the
  camera copy in them), the exposure light buffers, the exposure record and the light visibility counters. Their
  writes stay reported or filled by store-level patches, which hand only the CPU's bytes over.
- **They do not exist.** Vfx3's resource pool and the gsys general pools (flags 0x22) are not CPU-cached, so nothing
  flushes them. The claim could report such a pool as CPU-cached so that agl's writers flush what they wrote, but
  that changes what the game sees; it is a separate experiment.

**A block is claimed at its allocator's call.** One patch on each of `ubo_object_pool_alloc`, `ubo_object_block_alloc`
and `ubo_object_block_alloc_b` takes the return address at the function's entry and looks it up in the game's
allocation table (OoT: `games/oot/patches/pools.c`). A row names a call site, its kind and its name, and the
allocation runs inside that claim; a call no row names takes the claims around it, as any other allocation does. Each
row holds the call's instruction word, and the first allocation of a run checks every row against the image: a row that
is not a direct call of one of the allocators stops the game. Only direct calls can be rows: a tail call or an inlined
allocator has no return address of its own. Calls inside generic makers (`cull_task_block_create`,
`gpu_calc_block_create`, `gpu_mem_block_new`) stay claimed at the maker's calls. A block that shares a heap pool with
another maker's block is still a risk; whether the heap's dedicated-pool path can be forced for claimed blocks is open.

## The gsys GPU culling lists

The culling objects (`cull_gpu_object_a_create`, `cull_gpu_object_b_create`) embed list objects: a ring of
ShaderStorageBlocks, the copy count and current copy, the counts read last and a count the CPU sets. Each list declares
up to 12 members: mesh draw records and appended counts. Only OoT has these lists; DKB's voxel culling and the other
games' GeometryCullingMgr are different designs.

Its methods:

- **Reset** (flip, read the counts of the copy the GPU filled, zero records and counts, set a count, set a dispatch
  record to {0, 1, 1, 0}, flush): inlined for each of object a's lists in `cull_gpu_lists_reset`, and called twice as
  `cull_gpu_list_reset_one`. Which members each list resets differs per list.
- **Read the headers:** `report_reader_b`, six calls, all from `cull_gpu_lists_reset`: flip a two-copy UniformBlock,
  read its 0x40-byte header, zero it.
- **Read and zero the gather lists' counts:** inlined in `actor_records_read_a`, among unrelated actor logic. Two of its
  eleven counts are zeroed only when the list's first byte is 1.

A flush cannot stand in for these: the head is 16 bytes of a copy of up to 4.4 MB that the GPU writes.

**Host code over the class (done in OoT, `games/oot/patches/cull_lists.c`).** `cull_gpu_lists_reset` is replaced
whole by host code that works on typed views of the game's objects and a table of object a's lists (offset, members
read, members reset, the set count, the dispatch record). It does what the game did, in the game's order, writes the
same words into guest memory, and makes each reset a GPU fill; it drops the flushes and invalidates, because the
pools are VRAM. When the object is made, the patch on its maker names every record the CPU reads with
`nvn_claim_read`, once, and checks that each block's virtual methods are the ones the game's code assumes (the
alignment getter and the empty invalidate). `actor_records_read_a` mixes too much other logic to replace; one row after
its zeroes makes them on the GPU, reading the same condition as the game.

## agl::utl::DynamicUniformBlockAllocator

The allocator holds a GPUMemBlockBase (the 16 MB ring), an array of slot objects, a slot index and the ring's
offset. These fields are at the same places in OoT, DKB and TOTK. `agl_dynamic_uniform_block_init` (43 instructions
in all three games) makes it, and `ubo_ring_16mb` (DKB: `ubo_ring_alloc`) bumps the offset, copies the caller's data
in, and hands out the next slot. Both are functions; there are 72 calls of the alloc in OoT and 101 in DKB.

No patch exists for it: the ring is claimed Upload, and its data is complete when the alloc returns. A host alloc
that writes into a mapped ring and fences reuse against the GPU is step 9 in [uniform rings](uniform-rings.md). It
takes one address per game and no field table beyond the three offsets. It is a later replacement of the whole alloc,
not a general mapped-pool programme.

## Readback objects

- **Exposure light buffers** (`exposure_light_buffers_create`, read by `exposure_light_readback`) and **light
  visibility** (`render_light_visibility_buffer`, `render_light_visibility_readback`): game-side classes in OoT only.
  Each reader invalidates, reads one value, zeroes it, and flushes. The flushes cover grains the GPU writes, so the
  zeroing reports and the exposure record's report stay, as do the maker claims.
- **agl::utl::DepthBufferEx** (`depth_grid_slots_create`, `report_reader_a`, `depth_grid_camera_write`): in all three
  games. DKB's voxel culling also reads a coarse depth grid on the CPU. The camera copy lies in grains the GPU writes, so its
  report stays; its four allocations are rows of the allocation table. The TOTK export does not describe the class.
- **The query object** (`depth_grid_buffers_create`, read by `cull_gpu_dispatch`): OoT only. It is a compute-based
  query, not agl::cull::OcclusionQuery, the NVN-report query that DKB and MKW name and TOTK describes. Its allocations
  are rows of the allocation table; the scope over the whole maker stays, because the maker fills them after.

None of these needs a class replacement. A host readback ring for the light sums would only move the copy that the
invalidates already make.

## gsys::GeometryCullingMgr

The manager holds the per-view ShaderStorageBlocks (address arrays), a ShaderStorageBlock over one GPUMemBlockBase,
and a bump offset. OoT, DKB and MKW use the same offsets; TOTK is 8 bytes shorter after the first block, as in the
UniformBlock section. `cull_geometry_slot_alloc` is a function in OoT and DKB; DKB inlines the address store into it
as a loop.

Its two allocations are rows of OoT's allocation table. The address store's report stays: Kokiri never runs the store, so no
census covers its flush.

## NintendoWare Vfx3 attribute buffers

OoT's effects use NintendoWare Vfx3, and DKB and MKW use nn::vfx2. Vfx3's resource pool is not CPU-cached, and the
dirty-range copy does not flush, so the flush replacement does not cover it. The five setters, the buffer fill and
the dirty-range copy are each a function with one report. That is already one patch per method; keep them.

## Order of work

1. **Marked flushes** (done in OoT). For DKB and MKW, a census of their flushes against their reports comes first.
2. **The allocation table** on the three allocators (done in OoT). Next DKB's claims, then MKW's pools.
3. **The culling lists** in OoT (done): host code replaces the reset function.
4. **The dynamic ring's host alloc,** together with mapped pools.

The claims at pool makers, the Vfx3 reports, and the scopes at generic makers' calls stay as they are.
