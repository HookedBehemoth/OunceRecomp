// (native-vk, RX_VK_DESC_HEAP_BDA=1) storage buffers through device addresses: the host side (vk_bda.h). The shader side is
// eden/shader_recompiler/backend/spirv/bda_emit.h, the draw side vk_draw.cpp build_descriptors.
//
// The window table: one u64 per layer-0 window (512, guest [0, 1 TB)), then the null window's entry for addresses from 1 TB
// on, then RX_VK_BDA_CHECK's words and scratch (bda_emit.h kCheckOffset, kScratchOffset). Host-visible: an entry is written
// when vk_pools.cpp makes its window, which is before any recording can name an address in it (the draws' ranges are bound
// on the CPU first, and a window is made by its first bind). Entries of windows not made point at the null window: a 4 GB
// sparse buffer with nothing bound, where this GPU reads 0 and drops stores and atomics (residencyNonResidentStrict,
// work/proto/bda/bda_poc.c), so a stray address reads 0 as the classic path's robust descriptors did.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "vk_bda.h"
#include "vk_internal.h"
#include "vk_pools.h"
#include "shader_recompiler/backend/spirv/bda_emit.h"

namespace mkwvk {

namespace {
namespace B = Shader::Backend::SPIRV::Bda;

bool env_on(const char* name) {
    const char* e = getenv(name);
    return e && *e && atoi(e) != 0;
}

std::atomic<uint64_t*> table{nullptr};   // (mapped; set before the entries are filled)
uint64_t table_address = 0, null_address = 0;
VkBuffer table_buffer = VK_NULL_HANDLE, null_window = VK_NULL_HANDLE;
VmaAllocation table_alloc = nullptr;
struct alignas(64) Counts {
    int64_t v[BC_COUNT]{};
};
Counts counts;
const char* const kNames[BC_COUNT] = {"stage draws by address", "read-only skipped", "written (noted)", "bound on the CPU first",
                                      "virtual (the draw's range)", "virtual lost (read 0)", "stage draws on descriptors"};

bool make_table() {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    bi.size = 4ull << 30;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(dev.device, &bi, nullptr, &null_window) != VK_SUCCESS) {
        logf("vk: storage buffers by address off: no null window (a 4 GB sparse buffer)");
        return false;
    }
    VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    da.buffer = null_window;
    null_address = vkGetBufferDeviceAddress(dev.device, &da);
    void* mapped = nullptr;
    if (!create_buffer(B::kTableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true,
                       table_buffer, table_alloc, &mapped, true) ||
        !mapped || !null_address) {
        logf("vk: storage buffers by address off: no window table");
        return false;
    }
    std::memset(mapped, 0, B::kTableBytes);
    uint64_t* t = (uint64_t*)mapped;
    for (uint32_t k = 0; k <= B::kNullEntry; k++) t[k] = null_address;
    da.buffer = table_buffer;
    table_address = vkGetBufferDeviceAddress(dev.device, &da);
    table.store(t, std::memory_order_release);   // (windows made from now on write their own entries)
    uint32_t made = 0;
    for (uint32_t k = 0; k < B::kWindows; k++)
        if (const uint64_t a = layer0_window_address(k)) t[k] = a, made++;
    logf("vk: storage buffers by address: window table at %#llx (%u windows made), null window at %#llx%s",
         (unsigned long long)table_address, made, (unsigned long long)null_address,
         bda_check_on() ? "; RX_VK_BDA_CHECK: accesses checked against their buffers' sizes" : "");
    return true;
}

} // namespace

bool bda_wanted() {
    static const bool on = env_on("RX_VK_DESC_HEAP_BDA");
    return on;
}

bool bda_on() {
    if (!bda_wanted()) return false;
    static std::atomic<int> state{-1};
    const int s = state.load(std::memory_order_acquire);
    if (s >= 0) return s != 0;
    if (!dev.device) return false;
    static std::mutex once;
    std::lock_guard lk(once);
    if (state.load(std::memory_order_relaxed) >= 0) return state.load(std::memory_order_relaxed) != 0;
    const char* why = nullptr;
    if (!layer0_on()) why = "needs layer 0 (RX_VK_LAYER0=1 or 2)";
    else if (!dev.props.sparseProperties.residencyNonResidentStrict) why = "unbound sparse pages do not read 0 on this GPU";
    else if (!vkGetBufferDeviceAddress) why = "no buffer device addresses";
    if (!why && !make_table()) why = "the window table could not be made";
    if (why) logf("vk: storage buffers by address (RX_VK_DESC_HEAP_BDA=1) off: %s", why);
    state.store(why ? 0 : 1, std::memory_order_release);
    return !why;
}

bool bda_check_on() {
    static const bool on = bda_wanted() && env_on("RX_VK_BDA_CHECK");
    return on;
}

uint64_t bda_table_address() {
    return bda_on() ? table_address : 0;
}

void bda_window_made(uint32_t k, uint64_t address) {
    if (uint64_t* t = table.load(std::memory_order_acquire); t && k < B::kWindows) t[k] = address;
}

void bda_count(BdaCount c, int64_t n) {
    counts.v[c] += n;
}

void bda_frame() {
    uint64_t* t = table.load(std::memory_order_relaxed);
    if (!t) return;
    if (bda_check_on()) {   // (the GPU's words as they are now: a monitor, read every present, never reset)
        static uint32_t seen = 0, told = 0;
        const volatile uint32_t* w = (const volatile uint32_t*)((const uint8_t*)t + B::kCheckOffset);
        const uint32_t n = w[B::kCheckCount];
        if (n != seen && told < 200) {
            told++;
            logf("vk: RX_VK_BDA_CHECK: %u storage accesses out of their buffers' range so far (+%u); first program %08x, last %08x "
                 "(offset %#x, bound %#x)",
                 n, n - seen, w[B::kCheckFirst], w[B::kCheckLast], w[B::kCheckLastOffset], w[B::kCheckLastBound]);
            seen = n;
        }
    }
    static int64_t frames = 0, last[BC_COUNT]{};
    if (++frames % 600) return;
    char line[600];
    int len = snprintf(line, sizeof line, "vk: storage buffers by address, last 600 presents:");
    for (int i = 0; i < BC_COUNT && len < 520; i++) {
        len += snprintf(line + len, sizeof line - len, " %s %lld%s", kNames[i], (long long)(counts.v[i] - last[i]),
                        i + 1 < BC_COUNT ? "," : "");
        last[i] = counts.v[i];
    }
    if (bda_check_on()) {
        const volatile uint32_t* w = (const volatile uint32_t*)((const uint8_t*)t + B::kCheckOffset);
        len += snprintf(line + len, sizeof line - len, "; out of range so far %u", w[B::kCheckCount]);
    }
    logf("%s", line);
}

} // namespace mkwvk
