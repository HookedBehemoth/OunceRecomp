# Engine classes behind the pool patches

This page groups the store-level pool patches by the engine classes they patch. For each class it says whether a
single host-side replacement can stand in for those patches in OoT, DKB and MKW. Read it before you add another
`nvn_claim_write`, `nvn_claim_fill` or `nvn_claim_read` row, or a claim scope around an allocation call. The claim
kinds are explained in [pool claims](pool-claims.md), and the makers in [pool users](pool-users.md).

The facts come from reading the three games' code. No game run was made for this page. The class names come from
the games' own `getName` strings and from the Tears of the Kingdom agl and gsys headers exported in
[aquacluck/ghidra_scripts](https://github.com/aquacluck/ghidra_scripts) (`dt_totk_gdt_export`). That export has no
licence, so this repository cites it and copies nothing from it.

## Summary

Most of the patches are not about the classes' logic. They patch the two places where agl already tells the GPU
what the CPU did: `UniformBlock::flush`, which the game calls after it writes a block in a CPU-cached pool, and the
allocation of a block. Two replacements carry over to every game. The first gives a flush the meaning NVN gives it:
the CPU wrote this range. The second claims a block where it is allocated, from a per-game table of call sites. The
GPU culling lists are the one class that is worth replacing outright, and only OoT has them.

| Class | Games | Methods: functions or inlined | Patches retired | Size | Recommendation |
|---|---|---|---|---|---|
| agl::UniformBlock and ShaderStorageBlock: flush | all three, and TOTK | flush is a function; writes, copy flips and address math are inlined in hundreds of functions | about 20 write reports in `pools.c` | about 30 lines in `nvn_native.c` | do it first, after a census |
| agl::UniformBlock and GPUMemBlockBase: allocation | all three | the allocators are functions (OoT 180, 34 and 110 calls) | 22 claim scopes (44 rows) | about 60 lines, plus a table of call sites per game | do it second |
| gsys GPU culling lists | OoT | the reset is inlined 12 times, plus 2 calls; the header read is a function | all of `mesh_lists.c` and `mesh_args.c`, and the 11 count fills of `actor_records_read_a` | about 150 lines, plus a table of list offsets | do it third; it also fixes the order of the resets |
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

Every writer that the write reports in `pools.c` follow flushes: `cull_veg_task_records_write`,
`depth_grid_camera_write`, `exposure_light_readback`, `render_light_visibility_readback`, the probe writers,
`cull_scene_blocks_init`, `cull_address_block_create`, `cull_geometry_draw_view` and the job scratch's zeroing. The back
end already handles the invalidates as readbacks, but `nvnMemoryPoolFlushMappedRange` does nothing.

**The replacement: a flush is a reported write.** When the pool is claimed, `nvnMemoryPoolFlushMappedRange` and
`nvnBufferFlushMappedRange` report their range as `nvn_claim_write` does. This is NVN's own contract, not
game knowledge, so the back end stays plain and the same code serves every game. Two limits apply:

- A flush covers the whole current copy, rounded to 64 bytes. Where the GPU owns part of that copy, the report hands
  the GPU's bytes back to stale CPU data. The culling lists are such blocks (next section); the vegetation task
  blocks may be.
- Pools that are not CPU-cached (the gsys general pools, flags 0x22, and the 0x12 object blocks) are never flushed.
  For those, the claim can report the pool as CPU-cached, so agl's writers flush what they wrote. That is uniform
  rings' step 8a without writer patches, but it changes what the game sees, so try it separately.

**The second replacement: claim a block at its allocator.** Patch `ubo_object_pool_alloc`, `ubo_object_block_alloc`
and `ubo_object_block_alloc_b` once each. The patch looks up the return address in a per-game table of call sites and
kinds, and runs the allocation inside that claim. The table replaces the claim scopes that sit around those calls
today. Calls that are inside generic makers (`cull_task_block_create`, `gpu_calc_block_create`) stay claimed at the
maker's calls. A block that shares a heap pool with another maker's block is still a risk; whether the heap's
dedicated-pool path can be forced for claimed blocks is open.

## The gsys GPU culling lists

The culling objects (`cull_gpu_object_a_create`, `cull_gpu_object_b_create`) embed list objects: a ring of
ShaderStorageBlocks, the copy count and current copy, and the counts read last. The first two declared members of
each list are the head record and the appended count. Only OoT has these lists; DKB's voxel culling and the
other games' GeometryCullingMgr are different designs.

Its methods:

- **Reset** (flip, read the counts of the copy the GPU filled, zero the head and the count, flush): inlined 12 times
  in `cull_gpu_lists_reset`, and called twice as `cull_gpu_list_reset_one`. The counts are read without an
  invalidate, which is why `mesh_lists.c` names them with `nvn_claim_read`.
- **Set a count and a dispatch record** ({0, 1, 1, 0}): inlined, three of each.
- **Read the headers:** `report_reader_b`, six calls, all from `cull_gpu_lists_reset`.
- **Read and zero the gather lists' counts:** inlined 11 times in `actor_records_read_a`.

A flush cannot replace these patches: the head is 16 bytes of a copy of up to 4.4 MB that the GPU writes.

**The replacement** wraps the methods instead of rewriting them. The game's code still runs, and its stores still land
in guest memory.

1. At the end of the object's maker, name each list's head records as read by the CPU, from a table of the lists'
   offsets in the two objects. Do this once, not at each read.
2. After `cull_gpu_lists_reset` and `actor_records_read_a` return, record the GPU fills for every list they reset, in
   one batch, from the same table.

This retires about 60 At rows. It also puts the fills at one point in the frame, which is where the open ordering
question of the foliage pass (see [pool users](pool-users.md)) can be answered.

## agl::utl::DynamicUniformBlockAllocator

The allocator holds a GPUMemBlockBase (the 16 MB ring), an array of slot objects, a slot index and the ring's
offset. These fields are at the same places in OoT, DKB and TOTK. `agl_dynamic_uniform_block_init` (43 instructions
in all three games) makes it, and `ubo_ring_16mb` (DKB: `ubo_ring_alloc`) bumps the offset, copies the caller's data
in, and hands out the next slot. Both are functions; there are 72 calls of the alloc in OoT and 101 in DKB.

No patch exists for it: the ring is claimed Upload, and its data is complete when the alloc returns. A host alloc
that writes into a mapped ring and fences reuse against the GPU is the natural form of step 9 in
[uniform rings](uniform-rings.md). It takes one address per game and no field table beyond the three offsets. Do it
with step 9, not before.

## Readback objects

- **Exposure light buffers** (`exposure_light_buffers_create`, read by `exposure_light_readback`) and **light
  visibility** (`render_light_visibility_buffer`, `render_light_visibility_readback`): game-side classes in OoT only.
  Each reader invalidates, reads one value, zeroes it, and flushes. The zeroing reports and the exposure record's
  report go with the flush replacement; the maker claims stay.
- **agl::utl::DepthBufferEx** (`depth_grid_slots_create`, `report_reader_a`, `depth_grid_camera_write`): in all three
  games. DKB's voxel culling also reads a coarse depth grid on the CPU. The camera copy's report goes with the flush
  replacement, and the four claim scopes go into the allocation table. The TOTK export does not describe the class.
- **The query object** (`depth_grid_buffers_create`, read by `cull_gpu_dispatch`): OoT only. It is a compute-based
  query, not agl::cull::OcclusionQuery, the NVN-report query that DKB and MKW name and TOTK describes. Its scopes go
  into the allocation table; the scope over the whole maker stays.

None of these needs a class replacement. A host readback ring for the light sums would only move the copy that the
invalidates already make.

## gsys::GeometryCullingMgr

The manager holds the per-view ShaderStorageBlocks (address arrays), a ShaderStorageBlock over one GPUMemBlockBase,
and a bump offset. OoT, DKB and MKW use the same offsets; TOTK is 8 bytes shorter after the first block, as in the
UniformBlock section. `cull_geometry_slot_alloc` is a function in OoT and DKB; DKB inlines the address store into it
as a loop.

Its three patches split between the two shared replacements: the two scopes go into the allocation table, and
`cull_geometry_draw_view` flushes the address array, which covers the address store's report. Check in the census
that DKB's draw flushes the same way before you claim DKB's manager.

## NintendoWare Vfx3 attribute buffers

OoT's effects use NintendoWare Vfx3, and DKB and MKW use nn::vfx2. Vfx3's resource pool is not CPU-cached, and the
dirty-range copy does not flush, so the flush replacement does not cover it. The five setters, the buffer fill and
the dirty-range copy are each a function with one report. That is already one patch per method; keep them.

## Order of work

1. **Census.** In one Kokiri run, log each flush on a claimed pool beside the reports that follow it. Each report the
   flush replacement retires must lie inside a flush of the same call. List the flushes that overlap GPU-owned grains.
2. **Flush as a reported write,** in `nvn_native.c`. Delete the reports that the census shows covered, and confirm
   with the claims check and the golden.
3. **The allocation table** on the three allocators: OoT first, then DKB's claims, then MKW's pools.
4. **The culling list wrapper** in OoT, which replaces `mesh_lists.c` and `mesh_args.c`.
5. **The dynamic ring's host alloc,** together with mapped pools.

The claims at pool makers, the Vfx3 reports, and the scopes at generic makers' calls stay as they are.
