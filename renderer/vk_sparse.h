// NVN sparse textures (docs/direct/sparse-textures.md). A texture with NVN_TEXTURE_FLAGS_SPARSE has a sparse image with no
// memory of its own. Its tiles are bound to device memory the back end allocates for each physical pool, at the place of
// the physical page the game maps there with nvnMemoryPoolMapVirtual, so a page's content follows it when the game
// moves it. The binds are queued and made on the bind queue just before the next submission, which waits for them.
#pragma once

#include "vk_internal.h"

namespace hsd {

struct Image;

/// A sparse texture's image made, and bound where the host's page table maps its pages now (images_mu held). false:
/// the back end cannot bind its tiles (logged and a strict event): the caller destroys it
bool sparse_image_made(Image* im, const nvn_tex* t);
/// Its image destroyed (images_mu held): its queued binds dropped, and a bind of it on the bind queue waited for
void sparse_image_gone(Image* im);
/// A memory pool made or finalized (nvn_pool_native): physical pools are the pages sparse textures map. At a finalize
/// the tiles still bound to it are unbound, and its memory is freed once the GPU is past them
void sparse_pool(uint64_t gpu, uint64_t size, uint32_t flags, int on);
/// nvn_vmap_mapped: virtual [gpu, gpu + size) maps to guest memory from phys on (0: unmapped)
void sparse_mapped(uint64_t gpu, uint64_t size, uint64_t phys);
/// (vk_sync's sender, queue_mu held) The queued binds made on the bind queue once the timeline reaches `after` (all work
/// submitted before is done with the old binding). true: *sem reaches *value when they are made, and every submission
/// from now on is behind them
bool sparse_flush(VkSemaphore timeline, uint64_t after, VkSemaphore* sem, uint64_t* value);
/// (each present) The memory of finalized pools freed once their unbinds are done
void sparse_sweep();
void sparse_stats(char* out, size_t cap);

}  // namespace hsd
