#pragma once
// (native-vk, RX_VK_DESC_HEAP_BDA=1, needs RX_VK_LAYER0>=1) Storage buffers read through device addresses: the shader takes
// each one's guest address from its constant buffer and translates it through the layer-0 window table
// (eden/.../backend/spirv/bda_emit.h); the CPU no longer builds their descriptors, it keeps the declared range bound
// (vk_draw.cpp build_descriptors) and notes the written ones as before. vk_bda.cpp: the switch, the window table, the null
// window, the check words of RX_VK_BDA_CHECK=1, the counters.
#include <cstdint>

namespace mkwvk {

/// RX_VK_DESC_HEAP_BDA=1 asked (read once)
bool bda_wanted();
/// Asked, and layer 0 is on with a sparse residency that reads unbound pages as 0 (residencyNonResidentStrict) and buffer
/// device addresses: decided once the device is made (false before)
bool bda_on();
/// RX_VK_BDA_CHECK=1 (with bda_on): each access checked against its buffer's size, the misses read every present
bool bda_check_on();
/// The window table's device address (u64 per window, bda_emit.h), made at the first call; 0 when off
uint64_t bda_table_address();
/// (vk_pools.cpp, its lock held) window k made at that device address: its table entry
void bda_window_made(uint32_t k, uint64_t address);
/// At each present: RX_VK_BDA_CHECK's misses logged when there are new ones; the counters every 600
void bda_frame();

/// Counters (the replay's thread; plain, on a line of their own: atomics on this path cost ~0.3 us each)
enum BdaCount : int {
    BC_STAGES,       // (stage draws whose storage buffers went through addresses)
    BC_SKIPPED,      // (read-only storage buffers left to the shader: no descriptor, no lookup)
    BC_WRITTEN,      // (written ones: still noted the classic way)
    BC_BOUND_NOW,    // (read-only ones whose range was not bound yet: bound on the CPU before the draw)
    BC_VIRTUAL,      // (addresses from 1 TB on, a virtual pool's: the draw's virtual range)
    BC_VIRTUAL_LOST, // (of them, not translated: a second range in the draw, or not bound; the shader reads 0 there)
    BC_CLASSIC,      // (stage draws with storage buffers left on descriptors: not ssbo_bda)
    BC_COUNT
};
void bda_count(BdaCount c, int64_t n = 1);

} // namespace mkwvk
