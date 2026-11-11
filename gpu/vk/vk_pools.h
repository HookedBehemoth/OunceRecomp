#pragma once
// (native-vk, layer 0: gpu/vk/vk_pools.cpp, RX_VK_LAYER0=1) guest memory in window buffers: see layer0_windows.h
#include <cstdint>
#include "vk_internal.h"

namespace mkwvk {

/// RX_VK_LAYER0=1 and the device can (sparse residency of aliased buffers, guest memory as dma-bufs); false until the device
/// is made. Read once (the switch), then the device's answer is kept
bool layer0_on();
/// The window holding guest [guest, guest + size) at its offset there, its chunks bound at the first use; empty outside the
/// game's pools, for a range past its pool's end or longer than 2 GB (as the old lookup). layer0_buffer: with the device address
BufferRef layer0_bind(uint64_t guest, uint64_t size);
BufferRef layer0_buffer(uint64_t guest, uint64_t size);
/// A pool the game made (nvnMemoryPoolInitialize): its chunks bound now with RX_VK_LAYER0_EAGER=1
void layer0_pool_created(uint64_t gpu, uint64_t size);
/// Any chunk of guest [lo, hi) bound (its pages pinned: the host zeroes them instead of discarding them)
bool layer0_bound(uint64_t lo, uint64_t hi);
/// The page table's entries for every chunk bound (memory_guest_pages, when it makes the table)
void layer0_fill_table();
/// At each submission's start: counts every 600
void layer0_submission();

// (vk_memory.cpp, for vk_pools.cpp)
VkDeviceMemory memory_guest_dma_buf(uint64_t lo, uint64_t size);
uint64_t memory_dma_buf_limit();
bool memory_game_pool(uint64_t guest, uint64_t* lo, uint64_t* hi);
void guest_pages_set(uint64_t lo, uint64_t hi, uint64_t address);

} // namespace mkwvk
