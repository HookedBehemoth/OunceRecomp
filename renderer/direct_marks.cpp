// GPU breadcrumbs for a lost device (direct_marks.h).
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "direct_internal.h"
#include "direct_marks.h"

namespace hsd {

bool marks_enabled = false;

namespace {

constexpr uint32_t kEntries = 1u << 18;   // (about 150 frames of commands: an entry is long finished when it is used again)

struct Entry {
    std::atomic<uint64_t> seq{0};   // (the command's number + 1; 0: never used)
    const char* what = nullptr;
    uint64_t key = 0, lr = 0, present = 0;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint32_t kind = 0, a = 0, b = 0;
    const uint8_t* header = nullptr;
};

bool asked = false;
VkBuffer buffer = VK_NULL_HANDLE;
VkDeviceMemory memory = VK_NULL_HANDLE;
volatile uint32_t* words = nullptr;   // (2 a command: top of pipe, bottom of pipe)
Entry* entries = nullptr;
std::atomic<uint64_t> next{0};
PFN_vkCmdWriteBufferMarker2AMD pMarker = nullptr;

}  // namespace

void marks_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want) {
    if (!(asked = dbg("marks"))) return;
    for (const auto& e : exts)
        if (!std::strcmp(e.extensionName, VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) {
            want.push_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
            return;
        }
    logf("direct: marks: the device has no %s: no breadcrumbs", VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
    asked = false;
}

void marks_init() {
    if (!asked) return;
    pMarker = (PFN_vkCmdWriteBufferMarker2AMD)vkGetDeviceProcAddr(dev.device, "vkCmdWriteBufferMarker2AMD");
    // (host memory, not device-local: a reset loses VRAM, and the markers are read after one)
    void* map = nullptr;
    const auto pBind = (PFN_vkBindBufferMemory)vkGetDeviceProcAddr(dev.device, "vkBindBufferMemory");
    const auto pMap = (PFN_vkMapMemory)vkGetDeviceProcAddr(dev.device, "vkMapMemory");
    bool ok = pMarker && pBind && pMap;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 8ull * kEntries;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ok = ok && vkCreateBuffer(dev.device, &bi, nullptr, &buffer) == VK_SUCCESS;
    uint32_t type = UINT32_MAX;
    if (ok) {
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev.device, buffer, &mr);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++) {
            const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
            if ((mr.memoryTypeBits >> i & 1) && (f & want) == want && !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
        }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size, ai.memoryTypeIndex = type;
        ok = type != UINT32_MAX && vkAllocateMemory(dev.device, &ai, nullptr, &memory) == VK_SUCCESS &&
             pBind(dev.device, buffer, memory, 0) == VK_SUCCESS && pMap(dev.device, memory, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS;
        if (memory) mem_account(MO_OTHER, type, (int64_t)mr.size);
    }
    if (!ok) {
        logf("direct: marks: no marker function or host-memory buffer: no breadcrumbs");
        return;
    }
    std::memset(map, 0, 8ull * kEntries);
    words = (volatile uint32_t*)map;
    entries = new Entry[kEntries];
    marks_enabled = true;
    logf("direct: marks (HOS_DBG=marks): every draw and dispatch bracketed by top- and bottom-of-pipe markers (%u entries); "
         "a lost device logs the commands started and not finished", kEntries);
}

uint32_t mark_before(VkCommandBuffer cb, const char* what, uint64_t key, uint32_t kind, uint64_t lr, uint32_t a, uint32_t b) {
    const uint64_t s = next.fetch_add(1, std::memory_order_relaxed);
    const uint32_t i = (uint32_t)(s % kEntries);
    Entry& e = entries[i];
    e.seq.store(0, std::memory_order_relaxed);
    words[2 * i] = words[2 * i + 1] = 0;
    e.header = nullptr;
    e.what = what, e.key = key, e.lr = lr, e.present = present_count(), e.cb = cb, e.kind = kind, e.a = a, e.b = b;
    e.seq.store(s + 1, std::memory_order_release);
    pMarker(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, buffer, 8ull * i, (uint32_t)(s + 1));
    return i;
}

thread_local const uint8_t* task_last_header = nullptr;
void mark_header(uint32_t i, const void* h) { entries[i].header = (const uint8_t*)h; }

void mark_after(VkCommandBuffer cb, uint32_t i) {
    pMarker(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, buffer, 8ull * i + 4, (uint32_t)entries[i].seq.load(std::memory_order_relaxed));
}


void device_lost(const char* why) {
    // (one thread reports; a second submit that fails meanwhile waits here for the first one's abort: it must not end the
    // process under the report and the capture being written)
    static std::mutex one;
    one.lock();
    marks_report(why);
    host->log_flush();
    abort();
}

void marks_report(const char* why) {
    if (!marks_enabled) return;
    struct Row {
        uint64_t seq;
        uint32_t i;
    };
    std::vector<Row> open;
    uint64_t done_most = 0, done = 0;
    for (uint32_t i = 0; i < kEntries; i++) {
        const uint64_t s = entries[i].seq.load(std::memory_order_acquire);
        if (!s) continue;
        const uint32_t top = words[2 * i], bottom = words[2 * i + 1];
        if (bottom == (uint32_t)s) done++, done_most = std::max(done_most, s);
        else if (top == (uint32_t)s) open.push_back({s, i});
    }
    std::sort(open.begin(), open.end(), [](const Row& x, const Row& y) { return x.seq < y.seq; });
    logf("direct: marks at %s: %zu commands started and not finished (the newest finished: #%llu; %llu entries finished in the ring; "
         "%llu commands recorded)", why, open.size(), (unsigned long long)done_most, (unsigned long long)done,
         (unsigned long long)next.load());
    size_t n = 0;
    for (const Row& r : open) {
        const Entry& e = entries[r.i];
        // (the finished command recorded just before it in the same command buffer, if the ring still has it)
        uint64_t prev = 0;
        for (uint64_t s = r.seq - 1; s > 0 && r.seq - s < 4096; s--) {
            const Entry& p = entries[(s - 1) % kEntries];
            if (p.seq.load(std::memory_order_relaxed) == s && p.cb == e.cb) {
                prev = s;
                break;
            }
        }
        logf("direct: marks: open #%llu %s key %016llx list kind %u lr %#llx present %llu cb %p (%u, %u); before it in its command buffer #%llu%s",
             (unsigned long long)r.seq, e.what, (unsigned long long)e.key, e.kind, (unsigned long long)e.lr, (unsigned long long)e.present,
             (void*)e.cb, e.a, e.b, (unsigned long long)prev,
             prev ? (words[2 * ((prev - 1) % kEntries) + 1] == (uint32_t)prev ? " (finished)" : " (not finished)") : "");
        if (e.header && n < 4) {   // (the task header the mesh draw read, its map and its first task's payload)
            uint32_t h[16];
            std::memcpy(h, e.header, sizeof h);
            logf("direct: marks:   header: groups %u y %u z %u taken %u map %#x%08x payload %#x%08x cap %u tasks %u dispatch %u %u %u payload words %u threads %u",
                 h[0], h[1], h[2], h[3], h[5], h[4], h[7], h[6], h[8], h[9], h[10], h[11], h[12], h[13], h[14]);
            const uint32_t* m = (const uint32_t*)(e.header + 64);
            char b[700];
            int k = 0;
            const uint32_t ng = std::min<uint32_t>(h[0], 48);
            for (uint32_t g = 0; g < ng && h[0] <= h[8]; g++) k += std::snprintf(b + k, sizeof b - k, " %x", m[g]);
            logf("direct: marks:   map (%u of %u):%s", ng, h[0], b);
            if (h[0] <= h[8] && h[9] && h[13] && h[13] < 4096) {
                const uint32_t* pl = m + h[8];
                k = 0;
                for (uint32_t w = 0; w < std::min<uint32_t>(h[13], 24); w++) k += std::snprintf(b + k, sizeof b - k, " %x", pl[w]);
                logf("direct: marks:   payload of task 0 (%u words):%s", h[13], b);
            }
        }
        if (++n == 60) break;
    }
}

}  // namespace hsd
