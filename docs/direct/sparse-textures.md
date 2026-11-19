# Sparse textures

This page explains how the back end gives NVN sparse textures their memory: only the pages the game maps are backed,
in device memory the back end owns for each physical pool. Read it before you change `renderer/vk_sparse.cpp`, the
sparse parts of `renderer/vk_image.cpp`, or the virtual pools in `runtime/nvn_native.c`.

## What the game does

A sparse texture (`NVN_TEXTURE_FLAGS_SPARSE`) has its storage in a virtual pool. Nothing backs that storage until the
game maps 64 KiB pages of a physical pool into it with `nvnMemoryPoolMapVirtual`. The game keeps free lists of physical
pages and moves them between textures and places as it streams content in. It fills the textures on the GPU, by copies
from loaded textures, and by rendering into them.

The game works out which pages a tile uses from `nvnTextureGetSparseTileLayout`. Each layer holds its tiled levels'
tiles, level by level and row by row, followed by its mip tail pages. A layer is a whole number of pages, so each layer
has a mip tail of its own.

Mario Kart World uses sparse textures for World_Game03's baked lighting arrays (68 layers of 8192 x 8192), the shadow
bake and the rut mask. In full, they would need about 24 GB.

## How the back end backs them

1. **The image is sparse.** A sparse texture's image is created with sparse binding, residency and aliasing, in the
   format the back end uses for that NVN format (BC7 for ASTC). It has no memory of its own. Unbound tiles read as
   zero.
2. **Each physical pool owns device memory.** The back end allocates it the first time a page of the pool is bound, in
   one allocation per class of image. A class is the number of the image's sparse blocks that one NVN page holds: one
   for a format with NVN's bytes per block, and four for ASTC 8x8 shown as BC7 (a 512 x 512 NVN tile is four 256 x 256
   BC7 blocks). A pool that serves two classes gets two allocations, and the log says so.
3. **A page binds to its place in the pool.** An NVN page's blocks are bound to the place of the physical page it maps,
   in a fixed order. When the game moves a page, its content moves with it, as on the console.
4. **The mip tail.** NVN's mip tail pages of a layer hold the image's tiled levels below NVN's tile size, then the
   image's own mip tail of that layer.
5. **Binds are batched.** The host tells the back end after each change to a virtual pool's page table. The back end
   queues the binds, and makes them in one `vkQueueBindSparse` on layer 0's bind queue just before the next
   submission. The binds wait until all work submitted earlier is done, and every later submission waits for the
   binds.
6. **A finalized physical pool** has its pages unbound wherever they are still mapped. Its memory is freed when those
   unbinds are done.

The device's choice is made once and logged: the bind queue, the memory type, and the checks the device must pass (64
KiB sparse blocks, sparse residency of aliased 2D images, and zero reads of unbound tiles). A sparse texture the back
end cannot bind gets the placeholder and is a strict event.
