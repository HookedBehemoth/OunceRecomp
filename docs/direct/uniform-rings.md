# Uniform data and write-once rings

This page describes how OoT's uniform data reaches the GPU. It records why only the bound ranges are copied,
and how the dynamic ring can later be written straight into memory the GPU reads (step 9). Read
it before you patch a uniform writer, change how the gsys heap's general pools are claimed, or map back-end memory
into the guest window. The claim kinds are in [pool claims](pool-claims.md), and the frame's threads are in
[the frame loop](frame-loop.md).

The facts come from reading the game's code and from the census runs in `work/assets/native/uniform-buffers.md`. No
game run was made for this page.

## The uniform data paths

| Path | Made by | Written by | Bound by | Bytes |
|---|---|---|---|---|
| Persistent object blocks | `ubo_object_pool_alloc`, through `ubo_object_block_alloc` and `ubo_heap_block_alloc`, into `gpu_pool_create`'s general pools (flags 0x22: CPU-uncached, GPU-cached) | their owners, in place (below) | `ubo_object_bind_uniform`, `draw_material_bind`, the model shape draw, `draw_deformable_model` | about 40 to 60 KB written a frame; 0.55 to 0.97 MB of distinct content bound |
| The dynamic ring | `agl_dynamic_uniform_block_init` (from `dta_create`): one 16 MB block | `ubo_ring_16mb`, which copies the caller's data in when it allocates | `ubo_object_bind_uniform` | about 26 allocations a frame of 256 to 1,280 bytes (compute and pass constants) |
| Inline updates | the 64 KB block that `dta_create` makes | `nvnCommandBufferUpdateUniformBuffer` in the command stream: about 80 a frame, 2 KB | the 2D and layout path | the back end applies them in GPU order; the CPU never writes the block |
| Transient buffers | `ubo_transient_buffer_wrap` | | | none: its 1,890 wraps a frame are report slots and storage, never uniform buffers |

**Object blocks.** A ubo object holds `copies × elements` members of an aligned size in one block. Its byte at +0x4a
holds the copy count in the low nibble and the current copy in the high nibble. Each owner flips the current copy
(`copy = 1 - copy`) right before it writes, and binders form the address as base + aligned size × (elements × copy +
element). So a block bound this frame is not the one its owner writes next frame. Static models have one copy and a
fixed address. The owners and their bytes a frame (census at 60 fps):

| Block | Size | Writers |
|---|---|---|
| Camera, per view | 13,568 B, 2 copies | `ubo_view_camera_write` (memcpy), `ubo_model_scene_view_write`, `ubo_view_pool_write`; 10.8 KB written, 11.4 KB read back by the same writers |
| Environment, per view | 16,960 B, 2 copies | `ubo_env_block_write`, called by the binder at the block's first bind each frame; 0.8 KB |
| Small per view | 240 B | `ubo_param_dirty_write`, `ubo_view_small_block_write`, f_80680730; 0.5 KB |
| Shape, material, skeleton | 256 B, 976 B, 13,440 B in 1 MB pools | `ubo_shape_block_write`, `ubo_param_dirty_write` (the next frame's calc jobs), f_80bd1d00, f_800a8ae0, f_8051c740, f_800acfa0; 1.8 KB in the field, more in Kokiri, where skeletons animate |
| Deformable per instance | 150 to 180 B | `draw_deformable_model`, the binder itself; 2.5 KB |
| Sky, Vfx3 | 464 to 2,432 B | `sky_inscatter_ubo_write`, `vfx_emitter_ubo_write`, `vfx_emitter_c14_write` |

Camera, shape and skeleton blocks are written after their binds are recorded and before the frame's gate. Anything
that reads uniform contents at record time gets them wrong.

**The dynamic ring.** `ubo_ring_16mb` bumps an offset with a compare-and-swap and starts again at 0 when the request
does not fit. Nothing fences it against the GPU: 16 MB lasts several hundred frames at this rate. The data is complete
when the allocation returns, so the ring is already write-once.

## How far the GPU runs behind

The double-buffering holds when the GPU is at most one frame behind the CPU. The game enforces that itself, and the
back end keeps the guarantee:

- `frame_end_wait_and_gate` waits with `nvnSyncWait` on the previous frame's fence (one of two), then on the window's,
  and only then signals the gate event that the next frame's work waits for.
- A CPU wait on an NVN fence waits until its submission is sent, then for its timeline value on the GPU. The sender
  thread adds no lag that a fence wait does not cover.
- The work after the gate's event wait is held on the host until the CPU signals. Every CPU write before the signal is
  in memory before that work is submitted.
- The present keeps three slots and waits for the slot's fence of three presents ago. Lists reach the queue up to two
  presents after they were begun (the transient pool's `REUSE_AFTER`). Neither loosens the game's own fence waits.

So CPU writes into a mapped block that the GPU reads would race the GPU exactly as they do on the console. The races
the claims check finds today (about one a frame, the next frame's calc jobs writing material parameters while the GPU
draws) would become the console's behavior instead of the submit-time snapshot that Upload gives.

## Placing back-end memory at guest addresses

Lifted code reaches guest address g at host address 0x1000_0000_0000 + g, and the guest window is 512 GB. The game
takes the CPU pointer of a gsys pool from `nvnMemoryPoolMap` (`gpu_pool_create` keeps it in the pool record, and
`ubo_ring_16mb` writes through it), and the GPU address from `nvnMemoryPoolGetBufferAddress`. The runtime chooses
both, so a pool can be given addresses in a range of the window that the back end reserves.

| Option | Where it works | Verdict |
|---|---|---|
| Placed map (`VK_EXT_map_memory_placed`): map device-local, host-visible memory at the reserved range | RADV, ANV and NVK since Mesa 24.1. This PC's RADV has `memoryMapPlaced` and `memoryUnmapReserve`, not `memoryMapRangePlaced` (an allocation maps whole), with 4 KB alignment. NVIDIA lists it for its Linux developer beta drivers only, since 535.43.10 (September 2023); production drivers are unconfirmed | the way on Linux; check the feature on the user's NVIDIA driver |
| Windows: export a Win32 handle, then `MapViewOfFile3` into a placeholder | No source says a Vulkan Win32 memory handle is a file-mapping section; WDDM maps allocations through the graphics kernel. NVIDIA allowed external buffers in host-visible device-local memory only in a May 2026 beta (596.46) | not available; it would depend on external memory, which is fragile on Windows |
| Linux: export an opaque fd or a dma-buf, then `mmap` it with `MAP_FIXED` | On AMD the placed map already does this. NVIDIA's Linux driver refuses host-visible memory types for dma-bufs, and mmap of its exports is undocumented | not needed on AMD, unsafe on NVIDIA |
| No placement: `nvnMemoryPoolMap` returns the driver's pointer minus the window base | Lifted code wraps around correctly, but host code that checks guest ranges (the memcpy and memset replacements, among others) rejects such pointers, and each check would need the alias | a later experiment for Windows, not a plan |
| Host-visible system memory | everywhere | no: NVIDIA's shader loads from system memory cost about 4.5 times a VRAM load, and uniform blocks are read many times a frame (the environment block about 150 times) |

**No external memory.** With the placed map the driver allocates ordinary device-local memory, and only the CPU
mapping's address is chosen. The export options (Win32 handles, dma-bufs) depend on how each driver treats external
memory, which is where NVIDIA and Windows fail.

**CPU reads are the new cost.** Device-local, host-visible memory is write-combined: CPU reads go over PCIe and are
uncached. The camera writers read back 11.4 KB of their blocks a frame, the general pools also hold report slots the
CPU reads, and some writers may read before they modify. A pool stays Upload, or its reader is patched to read
a copy, before it can be mapped.

## Writer reports for uniform data are not planned

Upload copies only the ranges the game binds, 0.55 to 0.97 MB a frame in OoT, of which 40 to 60 KB is written
(work/assets/native/uniform-buffers.md, section 1). Narrowing the copies to writer reports would need patches at about
15 to 25 writers. A CPU profile of Kokiri puts the whole claims machinery at about 1.1 ms of CPU a frame, 0.76 ms of it
in the sender thread's preparing of copies, and 1.95 MB a frame is staged. Narrowing could remove at most about 0.9 ms
(realistically 0.4 to 0.6), all on the asynchronous sender thread, which rarely holds the frame. The bound-range copy
stays, and a writer that needs a report uses `nvn_claim_write`.

## Step 9: the dynamic ring

Step 9 is a later, whole host replacement of `agl::utl::DynamicUniformBlockAllocator`'s alloc (see
[engine classes](engine-classes.md)) writing into a mapped ring. It removes a further 0.1 to 0.3 ms, also on the sender
thread. It is not a general programme of mapped pools: the gsys heap's uniform pools stay Upload. Mapped memory is
Linux only, because Windows has no placed map; there the ring stays Upload.

### Plan for step 9

1. Count CPU reads of the ring's pages. Move the readers it finds out of the ring, or patch them to read a copy.
2. Add a mapped claim kind. The runtime gives a mapped pool its CPU and GPU address in a range of the window that the
   back end reserves.
3. In the back end, allocate an arena of device-local, host-visible, coherent memory, map it whole at its range with
   the placed map, point the address table's granules at the arena's buffer, and skip Upload tracking for it.
4. Replace the alloc by host code that writes into the ring and fences reuse against the GPU. Claim the ring as mapped
   where the device has the placed map, and as Upload elsewhere. DKB has the same allocator.
5. Measure the sender thread's CPU time and the bytes staged before and after.

## DKB

DKB uses the same agl and gsys code at other addresses: `ubo_object_pool_alloc`, `ubo_object_block_alloc_b` (the block
allocator and its pool helper inlined), `gsys_gpu_heap_pool_create`, `agl_dynamic_uniform_block_init` and
`ubo_ring_alloc` (101 callers), in `games/dkb/labels.txt`. Its general pools are claimed Upload by the same rule as
OoT's. Its writers are not traced yet.

## Sources

- [VK_EXT_map_memory_placed](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_map_memory_placed.html)
  and [its proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_map_memory_placed.html): a
  placed map replaces the mappings at its address.
- [NVIDIA's Vulkan developer beta drivers](https://developer.nvidia.com/vulkan-driver): the extension listed for Linux
  (535.43.10); external buffers in host-visible device-local memory (596.46 and 595.44.06, May 2026).
- [Mesa 24.1](https://www.collabora.com/news-and-blog/news-and-events/mesa-241-brings-new-hardware-support-for-arm-and-nvidia-gpus.html):
  the extension in RADV, ANV and NVK.
- [Windows: known problems](../windows.md#known-problems): the cost of NVIDIA's shader loads from system memory.
