#pragma once
// (native-vk, RX_VK_DESC_HEAP_BDA=1, needs RX_VK_LAYER0>=1) Storage buffers read through device addresses: the shader takes
// each one's guest address from its constant buffer and translates it through the layer-0 window table
// (eden/.../backend/spirv/bda_emit.h); the CPU no longer builds their descriptors, it keeps the declared range bound
// (vk_draw.cpp build_descriptors) and notes the written ones as before. vk_bda.cpp: the switch, the window table, the null
// window, the check words of RX_VK_BDA_CHECK=1, the counters.
#include <cstddef>
#include <cstdint>

namespace mkwvk {

/// RX_VK_DESC_HEAP_BDA=1 asked (read once)
bool bda_wanted();
/// Asked, and layer 0 is on with a sparse residency that reads unbound pages as 0 (residencyNonResidentStrict) and buffer
/// device addresses: decided once the device is made (false before)
bool bda_on();
/// RX_VK_BDA_CHECK=1 (with bda_on): each access checked against its buffer's size, the misses read every present
bool bda_check_on();
/// RX_VK_DESC_HEAP_BDA=2 (with bda_on): the stages =1 leaves on descriptors for tables, the render area, CAS loops,
/// native_global_bounds or a missing u32 view go by address too (bda_emit.h kWideAllowed, Profile::ssbo_bda_wide)
bool bda_wide();
/// RX_VK_LOG_BDA_WHY=1: each program's stages left on descriptors logged once, with every reason (Info::bda_why), at its layout
bool bda_why_log();
/// The reasons of Info::bda_why as text ("render area, table (count > 1)"), into buf
const char* bda_why_text(uint32_t why, char* buf, size_t n);
/// A stage draw on descriptors (BC_CLASSIC) counted by each of its reasons, logged with the counters every 600 presents
void bda_count_why(uint32_t why);
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
    BC_GUESSED,      // (=2: read-only table entries and candidates of by-address stages: no descriptor, kept bound)
    BC_COUNT
};
void bda_count(BdaCount c, int64_t n = 1);

} // namespace mkwvk
