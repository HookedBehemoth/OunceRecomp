// (native-vk engine cut E4) the pass table, the switches and counters of the job consumer (eng_pass.h). The consumer itself
// is the replay's NOP_ENG (vk_replay.cpp); the secondary recording's traps are in the draw path (vk_draw.cpp, vk_texture.cpp)
#include "eng_pass.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace mkwvk {

namespace {

bool env_on(const char* n) {
    const char* e = getenv(n);
    return e && atoi(e) != 0;
}

// ---- the device: inline draws and secondaries in one rendering ----
bool m7_listed = false, nested_listed = false;
#ifdef VK_KHR_maintenance7
VkPhysicalDeviceMaintenance7FeaturesKHR m7_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR};
#endif
#ifdef VK_EXT_nested_command_buffer
VkPhysicalDeviceNestedCommandBufferFeaturesEXT nested_feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NESTED_COMMAND_BUFFER_FEATURES_EXT};
#endif

// ---- the pass table: one row per view record (the job's pass), noted at every job call ----
struct Row {
    EngPassFormats fmt{};
    uint32_t site = 0, w = 0, h = 0;
    uint64_t color0 = 0, depth = 0;      // (the targets' guest addresses as last seen)
    int64_t notes = 0, fmt_changes = 0, size_changes = 0, color_changes = 0, depth_changes = 0;
    int64_t first = 0;                   // (the note count when it was first seen: its order)
};
std::shared_mutex table_mu;
std::unordered_map<uint64_t, Row> table;
std::atomic<int64_t> notes_total{0};

std::atomic<int64_t> counts[EC_N];
std::mutex abort_mu;
std::map<std::string, int64_t> aborts;

thread_local bool sec_active = false;
thread_local const char* sec_broken = nullptr;

const char* fmt_name(VkFormat f) {
    switch (f) {
    case VK_FORMAT_UNDEFINED: return "-";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "B10G11R11";
    case VK_FORMAT_D32_SFLOAT: return "D32";
    case VK_FORMAT_D16_UNORM: return "D16";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32S8";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_R32_SFLOAT: return "R32F";
    default: break;
    }
    static thread_local char b[4][16];
    static thread_local int k;
    char* o = b[k++ & 3];
    snprintf(o, 16, "f%d", (int)f);
    return o;
}

void log_table(const char* why) {
    std::shared_lock lk(table_mu);
    std::vector<std::pair<uint64_t, const Row*>> rows;
    for (const auto& [k, r] : table) rows.push_back({k, &r});
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second->first < b.second->first; });
    logf("eng: pass table (%s): %zu passes (view records), %lld notes", why, rows.size(), (long long)notes_total.load());
    for (const auto& [k, r] : rows) {
        char col[96] = "";
        int n = 0;
        for (uint32_t i = 0; i < r->fmt.ncolor && i < 8 && n < 80; i++) n += snprintf(col + n, sizeof col - n, "%s%s", i ? "," : "", fmt_name(r->fmt.color[i]));
        logf("eng:   view %#llx site %#x: %ux%u, %u colour [%s], depth %s, stencil %s, %d samples; %lld notes, changes: formats %lld, "
             "size %lld, colour 0 object %lld, depth object %lld (colour 0 %#llx, depth %#llx)",
             (unsigned long long)k, r->site, r->w, r->h, r->fmt.ncolor, col, fmt_name(r->fmt.depth), fmt_name(r->fmt.stencil),
             (int)r->fmt.samples, (long long)r->notes, (long long)r->fmt_changes, (long long)r->size_changes, (long long)r->color_changes,
             (long long)r->depth_changes, (unsigned long long)r->color0, (unsigned long long)r->depth);
    }
}

} // namespace

bool eng_passes_on() {
    static const bool v = env_on("HOS_ENG_PASSES") || env_on("HOS_ENG_PASS_SELFTEST");
    return v;
}
bool eng_pass_selftest() {
    static const bool v = env_on("HOS_ENG_PASS_SELFTEST");
    return v;
}
bool eng_pass_mixed() {
    static const bool v = [] {
        bool on = false;
#ifdef VK_KHR_maintenance7
        on |= m7_listed && m7_feat.maintenance7;
#endif
#ifdef VK_EXT_nested_command_buffer
        on |= nested_listed && nested_feat.nestedCommandBuffer && nested_feat.nestedCommandBufferRendering;
#endif
        if (env_on("HOS_ENG_PASS_NO_MIX")) on = false;   // (a test: an execute in a rendering of its own, as without the feature)
        if (eng_passes_on())
            logf("eng: inline draws and secondaries %s one rendering (%s)", on ? "mixed in" : "NOT mixed in",
                 on ? (m7_listed ? "VK_KHR_maintenance7" : "VK_EXT_nested_command_buffer") : "each execute in a rendering of its own");
        return on;
    }();
    return v;
}
VkRenderingFlags eng_rendering_flags() {
    if (!eng_passes_on() || !eng_pass_mixed()) return 0;
#ifdef VK_KHR_maintenance7
    return VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT | VK_RENDERING_CONTENTS_INLINE_BIT_KHR;
#else
    return 0;
#endif
}
void eng_pass_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want, void**& tail) {
    if (!eng_passes_on()) return;
    auto has = [&](const char* n) {
        for (const auto& e : exts)
            if (!strcmp(e.extensionName, n)) return true;
        return false;
    };
#ifdef VK_KHR_maintenance7
    if (has(VK_KHR_MAINTENANCE_7_EXTENSION_NAME)) {
        want.push_back(VK_KHR_MAINTENANCE_7_EXTENSION_NAME);
        *tail = &m7_feat, tail = &m7_feat.pNext;
        m7_listed = true;
        return;
    }
#endif
#ifdef VK_EXT_nested_command_buffer
    if (has(VK_EXT_NESTED_COMMAND_BUFFER_EXTENSION_NAME)) {
        want.push_back(VK_EXT_NESTED_COMMAND_BUFFER_EXTENSION_NAME);
        *tail = &nested_feat, tail = &nested_feat.pNext;
        nested_listed = true;
    }
#endif
}

bool eng_formats_equal(const EngPassFormats& a, const EngPassFormats& b) {
    if (a.ncolor != b.ncolor || a.depth != b.depth || a.stencil != b.stencil || a.samples != b.samples) return false;
    for (uint32_t i = 0; i < a.ncolor && i < 8; i++)
        if (a.color[i] != b.color[i]) return false;
    return true;
}

bool eng_pass_formats(uint64_t view_key, EngPassFormats* out) {
    std::shared_lock lk(table_mu);
    auto it = table.find(view_key);
    if (it == table.end()) return false;
    *out = it->second.fmt;
    return true;
}
void eng_pass_note(uint64_t view_key, const EngPassFormats& f) { eng_pass_note_ex(view_key, f, 0, 0, 0, 0, 0); }
void eng_pass_note_ex(uint64_t view_key, const EngPassFormats& f, uint32_t site, uint32_t w, uint32_t h, uint64_t color0,
                      uint64_t depth) {
    const int64_t k = notes_total.fetch_add(1, std::memory_order_relaxed);
    {   // (a row that holds: a shared look first, the table changes only when something did)
        std::shared_lock lk(table_mu);
        auto it = table.find(view_key);
        if (it != table.end()) {
            const Row& r = it->second;
            if (eng_formats_equal(r.fmt, f) && r.w == w && r.h == h && r.color0 == color0 && r.depth == depth && (!site || r.site == site)) {
                const_cast<Row&>(r).notes++;   // (a statistic: a lost increment is harmless)
                return;
            }
        }
    }
    std::unique_lock lk(table_mu);
    auto [it, fresh] = table.try_emplace(view_key);
    Row& r = it->second;
    if (fresh) {
        r.fmt = f, r.site = site, r.w = w, r.h = h, r.color0 = color0, r.depth = depth, r.first = k;
        if (table.size() > 64 && table.size() % 64 == 1) logf("eng: the pass table has %zu views", table.size());
    } else {
        if (!eng_formats_equal(r.fmt, f)) {
            r.fmt_changes++;
            static int told;
            if (told++ < 20)
                logf("eng: view %#llx (%ux%u) changes formats: %u colour %s.., depth %s -> %u colour %s.., depth %s", (unsigned long long)view_key,
                     w, h, r.fmt.ncolor, fmt_name(r.fmt.color[0]), fmt_name(r.fmt.depth), f.ncolor, fmt_name(f.color[0]), fmt_name(f.depth));
        }
        if (r.w != w || r.h != h) r.size_changes++;
        if (r.color0 != color0) r.color_changes++;
        if (r.depth != depth) r.depth_changes++;
        r.fmt = f, r.w = w, r.h = h, r.color0 = color0, r.depth = depth;
        if (site) r.site = site;
    }
    r.notes++;
}

void eng_count(int c, int64_t n) {
    if (c >= 0 && c < EC_N) counts[c].fetch_add(n, std::memory_order_relaxed);
}
void eng_abort_reason(const char* why) {
    std::lock_guard lk(abort_mu);
    aborts[why ? why : "?"]++;
}
void eng_pass_report(double frames) {
    if (!eng_passes_on()) return;
    static int64_t reports;
    reports++;
    if (reports == 5) log_table("after 600 presents");
    if (reports == 30) log_table("after 3600 presents");
    static int64_t last[EC_N];
    int64_t now[EC_N];
    for (int i = 0; i < EC_N; i++) now[i] = counts[i].load(std::memory_order_relaxed);
    const auto d = [&](int i) { return (now[i] - last[i]) / frames; };
    logf("eng passes: a frame: %.1f job calls (%.1f layer lists), %.1f with a view (%.1f without); executed %.1f (%.0f draws); fallen back: "
         "no host job %.1f, formats %.1f, hazard %.1f, not resident %.1f, op %.1f, empty %.1f, mode %.1f, no pass %.1f, abort %.1f, "
         "malformed %.1f; restarts %.1f, state walks %.1f",
         d(EC_CALLS), d(EC_LAYER_CALLS), d(EC_JOBS), d(EC_NO_VIEW), d(EC_EXECUTED), d(EC_EXEC_DRAWS), d(EC_FB_NO_JOB), d(EC_FB_FORMATS),
         d(EC_FB_HAZARD), d(EC_FB_RESIDENT), d(EC_FB_OP), d(EC_FB_EMPTY), d(EC_FB_MODE), d(EC_FB_NOPASS), d(EC_FB_ABORT),
         d(EC_FB_MALFORMED), d(EC_RESTARTS), d(EC_STATE_WALKS));
    std::memcpy(last, now, sizeof last);
    if (reports % 5 == 0) {
        std::lock_guard lk(abort_mu);
        std::string s;
        for (const auto& [k, v] : aborts) s += " " + k + "=" + std::to_string(v);
        if (!s.empty()) logf("eng passes: why the self-test fell back, so far:%s", s.c_str());
    }
}

bool eng_sec_active() { return sec_active; }
const char* eng_sec_broken() { return sec_active ? sec_broken : nullptr; }
void eng_sec_set(bool active) { sec_active = active, sec_broken = nullptr; }
bool eng_sec_trap(const char* why) {
    if (!sec_active) return false;
    if (!sec_broken) sec_broken = why;
    return true;
}

} // namespace mkwvk
