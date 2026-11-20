# Pool users

This page describes what the game does with the gsys object blocks that are still on layer 0, and how each one can fit
a claim kind. Read it before you claim one of these makers in `games/oot/patches/pools.c`, or before you write a patch
that changes how the game shares memory between the CPU and the GPU. The claim kinds and the plan are in
[pool claims](pool-claims.md).

The findings come from reading the game's code without running it. Each table ends with what a runtime census must
confirm before a claim moves.

## The rule for these makers

The games assume unified memory. When a maker's data flow fits a claim kind as it is, claim it. When the game shares
memory in a way a discrete GPU does badly, change the game's code with a patch: move a CPU reset to a GPU fill, read a
small copy instead of a whole list, or report a CPU write where the writer makes it. The back end stays plain.

Two facts about the back end matter for every table below:

- A claim that copies from guest memory (Upload, Readback, Load once, and VRAM for a pool the CPU can access) copies the
  whole pool at the first submission after the pool is made. Writes the CPU makes while it creates a pool arrive.
- A 16-byte grain that GPU work writes as storage belongs to the GPU from then on. Bound ranges never copy it again, and
  only a reported write (`nvn_claim_write`) moves a later CPU write there. Writes that a shader makes through an address
  it loaded from memory are not seen as writes.

## gsys::GeometryCullingMgr (`gpu_result_block_a_create`)

| Question | Answer |
|---|---|
| System and data | gsys's geometry culling, made by `job_list_build_b` into the job list's +0x3b78. Two kinds of memory: per-view ubo objects (0x98 each, double-buffered), each an array of GeometryCullingUnitNum 8-byte GPU addresses; and one block of GeometryCullingBufferSize bytes (+0xb8) with an agl buffer over it (+0x28). |
| CPU users | `cull_cpu_shapes` (the calc jobs, every frame) calls `cull_geometry_slot_alloc` per shape: a bump allocation in the block, then `cull_geometry_store_address` writes the slice's address into the view's array and adds to the view's count. |
| GPU users | `cull_geometry_draw_view` draws a view's units as points and binds the address array as a storage buffer. Its program writes each shape's slice of the block through those addresses. The shape draws then read the slices. |
| Flow per frame | CPU writes addresses, then the GPU draws points that write the block through those addresses, then the GPU's shape draws read the block. No CPU reader was found. |
| Desktop match | Not as one claim. Split it: the block is GPU scratch (VRAM, `ubo_object_block_alloc_b` at 0x80ecdd24). The address arrays are Upload (`ubo_object_pool_alloc` at 0x80ecdcc0), with a reported write of count × 8 bytes in `cull_geometry_draw_view` before its draw. That report makes the arrays correct even if the program's storage binding counts as written. |
| Confidence and census | Medium. The likely cause of the black scene under Readback: the program's address-array binding counts as written, the grains become the GPU's, and the CPU's new addresses never arrive. The check skips GPU-written ranges, so it stays clean. Confirm whether that binding counts as storage the program writes, and name the invalidate that the survey counted for this maker. |

DKB has the same object: `cull_geometry_mgr_memory` (0x804c0d84), made by 0x804bd9cc.

## The depth grid: DepthBufferEx (`depth_grid_slots_create`)

| Question | Answer |
|---|---|
| System and data | agl::utl::DepthBufferEx, the coarse occlusion grid: a double-buffered constants block (0x390), two grid slots (0x3970 each: the camera, 32 × 18 tile pairs, a frame stamp), and a 0x5b40 block. |
| CPU users | At creation the CPU copies a template into each slot and zeroes the 0x5b40 block. Each frame, `report_reader_a` reads the newest slot whose stamp matches, into the object's own arrays. It writes nothing back to the slots. |
| GPU users | The occlusion-grid dispatch writes the slot (header, grid, stamp); the GPU culling reads it in the same frame. |
| Flow per frame | GPU writes slot N, then the CPU reads it one or two frames later; the stamp makes any age safe. |
| Desktop match | Readback as it is. No CPU write of the slots after creation was found, and the creation writes arrive with the first copy. |
| Confidence and census | Medium. Watch the slot pages after creation for a CPU writer, and the constants block: if a program binds it as written storage, the CPU's per-frame camera stops arriving. |

## The query object (`depth_grid_buffers_create`)

| Question | Answer |
|---|---|
| System and data | A 0x330 object that `texdata_model_procedural` keeps at +0xc80, made with 5000 and 5000. +0x8: 5000 records of 0x90 bytes, zeroed (the 0xb0000 pool). +0x180: 5000 × 16 bytes set to 1e6 (the 0x14000 pool). +0xf0: 7000 × 16 bytes, double-buffered (the 0x37000 block). +0x2a0: constants. A free list of 4999 ids. |
| CPU users | `cull_gpu_dispatch` gathers up to 2000 pending queries a frame, writes their 16-byte records into the +0xf0 half, flushes, dispatches, and later invalidates and reads that half. |
| GPU users | The dispatch's program reads the records and the constants, and keeps per-id state in +0x8 and +0x180 (the occlusion-culling program writes a 0x14000 buffer; see work/assets/native/readback.md). |
| Flow per frame | CPU writes the query records, then the GPU culls and writes results, then the CPU reads them in a later frame. |
| Desktop match | +0xf0 is Readback (claimed today). +0x8 and +0x180 fit VRAM: the CPU writes them only at creation, and a CPU-visible VRAM pool is copied once. |
| Confidence and census | Medium. Confirm that no CPU write reaches +0x8 or +0x180 after creation (an id's release or reuse would be one). |

## The GPU culling lists (`cull_gpu_object_a_create`, `cull_gpu_object_b_create`, `cull_gpu_results_create`)

| Question | Answer |
|---|---|
| System and data | The game's GPU culling. Object a holds the lists, instance buffers and indirect arguments, and six double-buffered result lists. Object b is the vegetation culling ("GatherLeaf", "GatherUndergrowth", "GatherTurf"). Each list starts with records whose words are the mesh-draw group count and the appended count. |
| CPU users | `cull_gpu_lists_reset` (main thread, at the frame's start, through `cull_frame_begin`) and `cull_gpu_list_reset_one` zero each list's first record, in both objects. `report_reader_b` reads the result lists' 0x40-byte headers two frames later and zeroes them. `mesh_drawer_d` uses those counts while it records draws. |
| GPU users | The culling programs append with atomics (group count by atomicMax, items by atomicAdd), reset the other records themselves, and bind whole lists as written storage. Indirect mesh draws read the records. |
| Flow per frame | CPU zeroes the first records, then the GPU culling appends, then indirect draws read the records; two frames later the CPU reads and zeroes the headers. |
| Desktop match | No. The CPU resets data the GPU owns, and the CPU reads 0x40 bytes out of lists of up to 4.4 MB. Change the game: replace the resets with GPU fills ({0, 1, 1, 0} per first record) at the start of the culling, and copy the six headers into a small Readback ring after the culling, which `report_reader_b` reads. The lists then become VRAM. |
| Confidence and census | High for the flow (the existing patches in `mesh_lists.c`, `mesh_args.c` and `pools.c` document the stores). The foliage pass's 6.1 ms under VRAM is not explained: the resets are reported, and reports reach CPU-visible VRAM. The likely cause is order: a reported reset is copied at the next submission, which may be the previous frame's. Count resets and culling dispatches per submission, and compare the appended counts with layer 0. |

## The vegetation task blocks (`cull_task_block_create`)

`cull_task_block_create` makes a gsys ubo object block: a double-buffered parameter layout (a count at layout +4,
0x40-byte records at +0x4c, strided arrays at +0x10, +0x40, +0x58, +0x70) in a pool it grows from the gsys heap. It has
four calls: two in `cull_gpu_object_b_create` (object b's task records) and two in `cull_task_block_fill`, which
`cull_scene_blocks_init` runs on SceneLowPriority at a scene's load.

| Question | Answer |
|---|---|
| System and data | Object b's blocks: the task count and records of the vegetation culling. `cull_task_block_fill`'s: per-scene vegetation parameters, written once. The heap pool `cull_task_block_fill` grows is shared: `cull_address_block_create`'s one-word block (an 8-byte GPU address) lands in it later. |
| CPU users | `cull_veg_task_records_write` (main thread, each frame) writes object b's count and count x 0x40 bytes of records, both reported. `cull_task_block_fill` writes its block once, after making it, in the same call. `cull_address_block_create` (SceneLowPriority, after `cull_task_block_fill`) stores a ubo object's GPU address into its one-word block (0x80c64a28). No CPU reader: the census under layer 0 and under VRAM counts no CPU read of either block. |
| GPU users | Compute only. `a54f19e927d8faef` and `383a566958350efe` reach object b's blocks as storage buffers whose address and size words sit in a uniform buffer (c4+0x40 and c4+0x80), both declared written. Programs that store to global memory (`3e6fe279463b020e`, `4247f1b4c3ce6cf3`, `5b9ba94d1c822e80`, `78ccf14779ec3efb` and others) hold the blocks' base addresses in their small storage buffers, and `cull_address_block_create`'s word is one of those addresses. |
| Flow per frame | CPU writes object b's count and records, the culling dispatches read them and write the GPU's part of the block, and later dispatches read that. `cull_task_block_fill`'s block is read only, through the stored address. |
| Desktop match | VRAM for all four, with the CPU's writes reported. |
| What broke before, and the changes | As Upload or VRAM, the scene's address word never reached the GPU: it is written outside the maker's scope, into a pool another maker grew, so shaders read address 0 and drew no grass or leaves. It is now reported. The storage buffers addressed through a uniform buffer were not counted as GPU writes. They are now counted for draws and dispatches, so Upload would no longer copy over what the GPU wrote. The check compared VRAM ranges, which global stores change rightly. It now leaves them to the owned-page scan, which finds the CPU writes no report hands over. |

`cull_scene_blocks_init`'s own two block sets (VRAM) are filled 16 bytes an entry after the claim's scope. The patch
gathers each loop's range and reports it where the loop's paths meet.

## The dedicated-pool path (lr 0x809df2ac)

| Question | Answer |
|---|---|
| System and data | Not a maker. 0x809df2ac is in `gpu_pool_create`, after its third `nvnMemoryPoolInitialize`: the path for a pool that holds exactly one block. The block's maker is the caller of `ubo_object_block_alloc` or `dta_alloc`, so each is claimed there. |
| CPU users | Those of the block's maker. |
| GPU users | Those of the block's maker. |
| Flow per frame | The 0xcc000 pool made every frame on SceneLowPriority is one maker's block, made and freed each frame. Static reading did not identify it. Candidates are the texture-storage makers through `texdata_block_alloc` and `ubo_object_block_alloc` (the JPEG initializer, `texdata_snapshot_textures`, f_809e7200 and f_809e80f0). |
| Desktop match | Claim the maker. A pool made every frame should become a block the patch keeps instead. |
| Confidence and census | High for the path; the maker is open. The census should record the guest return chain of each 0x809df2ac pool. |

## The probe module (`gpu_calc_block_create`, `storage_block_create`)

| Question | Answer |
|---|---|
| System and data | One module (vtable 0x84c7d108) makes both. Its scene step `probe_module_scene_init` runs `probe_storage_init` (the 0x581000 storage block, zeroed at creation) and `probe_bake_load` ("BakeProbe": baked probe files). Its job `probe_calc_block_init` makes `gpu_calc_block_create`'s 150 MB block, split into five ubo objects that the CPU fills at creation. |
| CPU users | At creation: header copies, data copies and zeroing. In play: about 26 pages a frame in the 150 MB block (survey); the writer is not identified. |
| GPU users | Shaders reach the block through stored addresses, not bindings. |
| Flow per frame | Probe data is streamed into the block by the CPU and read by shaders through addresses. |
| Desktop match | Upload fits only with writer reports, because no binding names the ranges. The patch goes on the streaming writer once the census names it. If `ounce profile` waits for a value the GPU writes into these blocks, that range needs Readback. |
| Confidence and census | Low: what the probes hold is unknown. Page-watch the 150 MB block and the 0x581000 block to name the writers. If profile runs never settle, check whether the loader polls a GPU-written word in them. |

`texdata_storage_block` (0x30e000) is `texdata_model_procedural`'s, through `texdata_block_alloc`; its users are not
traced yet.

## Vfx3 particle records (`gsys_vfx_mgr_init`)

| Question | Answer |
|---|---|
| System and data | NintendoWare Vfx3's resource pool (0x22, Upload): particle records of 0x100 bytes. |
| CPU users | `vfx_particle_init_callbacks`, when a particle is emitted, runs the emitter plugins' callbacks; the slot-0x380 call at 0x81261e08 writes the new particle's record. |
| GPU users | The particle draws bind the records as storage the program writes. |
| Flow per frame | CPU writes a record at emission, then the GPU updates and draws it. |
| Desktop match | Yes, with a reported write: after the callback, report the new particle's record (0x100 bytes). |
| Confidence and census | Medium. The census's page watch named the writer; confirm the record address the callback writes, to place the report. |

## The save picture (`texdata_serialize_jpg`)

| Question | Answer |
|---|---|
| System and data | agl::utl::TextureDataSerializerJPG: a block that receives the picture's pixels. |
| CPU users | After `texdata_copy_to_buffer` returns, the CPU reads the pixels and encodes them with nn::image's JPEG encoder. |
| GPU users | `texdata_copy_to_buffer` copies the texture into the block, submits and finishes. |
| Flow per frame | Once per capture: GPU copy, finish, CPU read. |
| Desktop match | Readback as it is: the copy's destination is copied out before the finish returns. |
| Confidence and census | High. Confirm with a save. DKB's is 0x81663430. |

## Game patches made, and what is open

1. **The culling lists:** the CPU's resets are made on the GPU (`nvn_claim_fill`), and only the records the CPU reads are
   copied out (`nvn_claim_read`): object a is VRAM. Object b is claimed by maker, its task blocks VRAM (above).
2. **GeometryCullingMgr split:** the views' arrays are Readback with each address store reported (their small pool also
   holds the exposure record, whose writes are reported), the block VRAM.
3. **Vfx3:** the plugin setters, the new-buffer fill and the dirty-range copy report their writes.
4. **The probe module:** the storage block is VRAM with bake::ModuleMgr's records reported. The 150 MB calc block's
   writers report, but a scene's load writes it again outside its maker's scope: it stays on layer 0.
5. **The depth grid:** its slots are Readback with the camera copy reported. The query's records and per-id state stay on
   layer 0 until the writer that the census does not attribute is found.
6. **The per-frame dedicated pool:** open.
