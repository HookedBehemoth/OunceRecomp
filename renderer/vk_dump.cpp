// The frame dump (vk_dump.h). The copies as gpu/legacy/vk/vk_dump.cpp makes them (a nearest blit of colour into a small
// RGBA32 image of its numeric kind, depth copied as it is), the statistics line and the PNG as it writes them.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "vk_dump.h"
#include "os.h"
#include "vk_image.h"
#include "vk_check.h"

namespace hsd {
namespace {

// (the dumped pictures' width, at most: 320; debug.dumpw=<w> another (M4g: patterns finer than a thumbnail holds))
uint32_t dump_width() {
    static const uint32_t w = [] {
        const char* v = dbg_value(SET_DBG_DUMPW);
        const int n = v ? atoi(v) : 0;
        return n >= 16 ? (uint32_t)n : 320u;
    }();
    return w;
}
std::mutex mu;   // (the claimed items, the graveyard)
std::vector<DumpItem*> claimed;
std::vector<std::pair<DumpItem*, uint64_t>> graveyard;   // (unclaimed items of freed lists, and the value they wait for)
uint64_t claimed_value = 0;
int seq = 0;
bool written = false;
std::string dir;
std::atomic<int64_t> st_recorded{0}, st_claims{0};

// (debug.dump=<present>[+<count>]: that many frames from there, in one folder, steps numbered on)
int64_t target() {
    static const int64_t t = [] {
        const char* v = dbg_value(SET_DBG_DUMP);
        return v ? atoll(v) : -1;
    }();
    return t;
}
int64_t frames() {
    static const int64_t n = [] {
        const char* v = dbg_value(SET_DBG_DUMP);
        const char* plus = v ? strchr(v, '+') : nullptr;
        return plus ? std::max(1, atoi(plus + 1)) : 1;
    }();
    return n;
}

int kind_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A8B8G8R8_UINT_PACK32: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_A8B8G8R8_SINT_PACK32: return 2;
    default: return 0;
    }
}

void full_barrier(VkCommandBuffer cb) {   // (the dump's own: a debug aid, not in the validation runs)
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}
void to_general(VkCommandBuffer cb, VkImage image) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

/// One target's copy recorded into cb (outside a rendering)
DumpItem* copy(VkCommandBuffer cb, const Target& t, const char* what) {
    const DebugObjects debug_aid;   // (M6b: its copies are not strict's events)
    Image* im = t.im;
    if (!im || im->placeholder || im->samples != VK_SAMPLE_COUNT_1_BIT) return nullptr;
    auto* it = new DumpItem;
    it->cb = cb, it->format = im->format, it->gpu = im->gpu, it->id = im->id;
    it->what = std::string(what) + " #" + std::to_string(im->id);
    it->src_w = std::max(1u, im->width >> t.level), it->src_h = std::max(1u, im->height >> t.level);
    if (!(im->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        if (!(im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return delete it, nullptr;
        it->w = it->src_w, it->h = it->src_h;
        it->depth_bytes = im->format == VK_FORMAT_D16_UNORM ? 2 : 4;
        it->depth_float = im->format == VK_FORMAT_D32_SFLOAT || im->format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        if (!mem_buffer((uint64_t)it->w * it->h * it->depth_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, it->buffer,
                        it->buffer_mem, &it->map))
            return delete it, nullptr;
        full_barrier(cb);
        const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, t.level, t.layer, 1}, {0, 0, 0}, {it->w, it->h, 1}};
        vkCmdCopyImageToBuffer(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, it->buffer, 1, &c);
        full_barrier(cb);
        return it;
    }
    it->kind = kind_of(im->format);
    it->w = std::min(it->src_w, dump_width());
    it->h = std::max(1u, (uint32_t)((uint64_t)it->src_h * it->w / it->src_w));
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = it->kind == 1 ? VK_FORMAT_R32G32B32A32_UINT : it->kind == 2 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT;
    ci.extent = {it->w, it->h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!mem_image(ci, it->image, it->image_mem)) return delete it, nullptr;
    if (!mem_buffer((uint64_t)it->w * it->h * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, it->buffer, it->buffer_mem, &it->map)) {
        mem_image_free(it->image, it->image_mem);
        return delete it, nullptr;
    }
    full_barrier(cb);
    to_general(cb, it->image);
    VkImageBlit b{};
    const bool d3 = im->type == VK_IMAGE_TYPE_3D;
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t.level, d3 ? 0 : t.layer, 1};
    const int32_t z = d3 ? (int32_t)t.layer : 0;
    b.srcOffsets[0] = {0, 0, z};
    b.srcOffsets[1] = {(int32_t)it->src_w, (int32_t)it->src_h, z + 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1] = {(int32_t)it->w, (int32_t)it->h, 1};
    vkCmdBlitImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, it->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    full_barrier(cb);
    const VkBufferImageCopy c{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {it->w, it->h, 1}};
    vkCmdCopyImageToBuffer(cb, it->image, VK_IMAGE_LAYOUT_GENERAL, it->buffer, 1, &c);
    full_barrier(cb);
    return it;
}

void free_item(DumpItem* it) {
    if (it->buffer) mem_buffer_free(it->buffer, it->buffer_mem), it->buffer = VK_NULL_HANDLE;
    if (it->image) mem_image_free(it->image, it->image_mem), it->image = VK_NULL_HANDLE;
}

uint8_t unit(float v) { return (uint8_t)std::clamp((int)std::lround(v * 255.0f), 0, 255); }

bool write_png(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {   // (stored deflate, as vk_replay.cpp's)
    auto crc = [](const uint8_t* p, size_t n, uint32_t c) {
        static uint32_t table[256];
        if (!table[1])
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t k = i;
                for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
                table[i] = k;
            }
        for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c;
    };
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        for (uint32_t x = 0; x < w; x++) raw.insert(raw.end(), rgba + ((size_t)y * w + x) * 4, rgba + ((size_t)y * w + x) * 4 + 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    for (size_t at = 0; at < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n), z.push_back((uint8_t)(n >> 8)), z.push_back((uint8_t)~n), z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + (long)at, raw.begin() + (long)(at + n));
        at += n;
    }
    const uint32_t adler = b << 16 | a;
    for (int i = 3; i >= 0; i--) z.push_back((uint8_t)(adler >> (i * 8)));
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    auto be = [](uint32_t v, uint8_t* o) { o[0] = v >> 24, o[1] = v >> 16, o[2] = v >> 8, o[3] = v; };
    auto chunk = [&](const char* type, const uint8_t* d, size_t n) {
        uint8_t hdr[8];
        be((uint32_t)n, hdr);
        std::memcpy(hdr + 4, type, 4);
        fwrite(hdr, 1, 8, f);
        if (n) fwrite(d, 1, n, f);
        uint8_t c[4];
        be(crc(d, n, crc((const uint8_t*)type, 4, 0xFFFFFFFFu)) ^ 0xFFFFFFFFu, c);
        fwrite(c, 1, 4, f);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {};
    be(w, ihdr), be(h, ihdr + 4);
    ihdr[8] = 8, ihdr[9] = 2;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), z.size());
    chunk("IEND", nullptr, 0);
    return fclose(f) == 0;
}

/// The item's line (vk_dump.cpp write_item's) and its PNG
void write_item(DumpItem& it, int n_seq) {
    mem_invalidate(it.buffer_mem);
    std::vector<float> depth;
    const void* map = it.map;
    if (it.depth_bytes) {   // (depth in red, green and blue, scaled to at most 320 wide)
        const uint32_t w = std::min(it.src_w, dump_width()), h = std::max(1u, (uint32_t)((uint64_t)it.src_h * w / it.src_w));
        depth.assign((size_t)w * h * 4, 1.0f);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const size_t at = (size_t)(y * it.src_h / h) * it.src_w + (size_t)x * it.src_w / w;
                float v;
                if (it.depth_bytes == 2) v = ((const uint16_t*)it.map)[at] / 65535.0f;
                else if (it.depth_float) std::memcpy(&v, (const uint8_t*)it.map + at * 4, 4);
                else v = (((const uint32_t*)it.map)[at] & 0xFFFFFF) / 16777215.0f;
                float* o = &depth[((size_t)y * w + x) * 4];
                o[0] = o[1] = o[2] = v;
            }
        it.w = w, it.h = h, map = depth.data();
    }
    const size_t n = (size_t)it.w * it.h;
    std::vector<uint8_t> rgba(n * 4, 255);
    char line[1024];
    if (it.kind == 0) {
        const float* p = (const float*)map;
        int64_t nan = 0, inf = 0, finite = 0;
        double sum[4]{}, mx[4] = {-1e30, -1e30, -1e30, -1e30}, mn[4] = {1e30, 1e30, 1e30, 1e30};
        for (size_t i = 0; i < n; i++) {
            const float* q = p + i * 4;
            bool bn = false, bi = false;
            for (int c = 0; c < 4; c++) bn |= std::isnan(q[c]), bi |= std::isinf(q[c]);
            if (bn || bi) {
                nan += bn, inf += bi && !bn;
                continue;
            }
            finite++;
            for (int c = 0; c < 4; c++) sum[c] += q[c], mx[c] = std::max(mx[c], (double)q[c]), mn[c] = std::min(mn[c], (double)q[c]);
        }
        const bool hdr = std::max({mx[0], mx[1], mx[2]}) > 1.0;
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < 3; c++) {
                const float v = std::max(p[i * 4 + c], 0.0f);
                rgba[i * 4 + c] = std::isnan(v) ? 255 : unit(hdr ? v / (1.0f + v) : v);
            }
        const double f = finite ? 1.0 / (double)finite : 0.0;
        std::snprintf(line, sizeof line,
                      "vk: dump %03d %s: tex %#llx format %d %ux%u: NaN %lld, Inf %lld of %lld; mean %.4g %.4g %.4g %.4g, "
                      "min %.4g %.4g %.4g %.4g, max %.4g %.4g %.4g %.4g",
                      n_seq, it.what.c_str(), (unsigned long long)it.gpu, (int)it.format, it.src_w, it.src_h, (long long)nan,
                      (long long)inf, (long long)n, sum[0] * f, sum[1] * f, sum[2] * f, sum[3] * f, mn[0], mn[1], mn[2], mn[3],
                      mx[0], mx[1], mx[2], mx[3]);
    } else {
        const int32_t* p = (const int32_t*)map;
        int64_t mx[4] = {INT64_MIN, INT64_MIN, INT64_MIN, INT64_MIN}, mn[4] = {INT64_MAX, INT64_MAX, INT64_MAX, INT64_MAX}, nz = 0;
        auto val = [&](size_t i, int c) { return it.kind == 1 ? (int64_t)(uint32_t)p[i * 4 + c] : (int64_t)p[i * 4 + c]; };
        for (size_t i = 0; i < n; i++) {
            bool z = false;
            for (int c = 0; c < 4; c++) mx[c] = std::max(mx[c], val(i, c)), mn[c] = std::min(mn[c], val(i, c)), z |= c < 3 && val(i, c);
            nz += z;
        }
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < 3; c++) rgba[i * 4 + c] = (uint8_t)((val(i, c) - mn[c]) * 255 / std::max<int64_t>(1, mx[c] - mn[c]));
        std::snprintf(line, sizeof line,
                      "vk: dump %03d %s: tex %#llx format %d %ux%u: %s, %lld of %lld texels not zero; min %lld %lld %lld %lld, "
                      "max %lld %lld %lld %lld",
                      n_seq, it.what.c_str(), (unsigned long long)it.gpu, (int)it.format, it.src_w, it.src_h,
                      it.kind == 1 ? "uint" : "sint", (long long)nz, (long long)n, (long long)mn[0], (long long)mn[1], (long long)mn[2],
                      (long long)mn[3], (long long)mx[0], (long long)mx[1], (long long)mx[2], (long long)mx[3]);
    }
    logf("%s", line);
    char path[1024];
    std::snprintf(path, sizeof path, "%s/%03d_%s_%llx.png", dir.c_str(), n_seq, it.what.c_str(), (unsigned long long)it.gpu);
    for (char* c = path + dir.size() + 1; *c; c++)
        if (*c == ' ' || *c == ':' || *c == '/') *c = '_';
    write_png(path, rgba.data(), it.w, it.h);
    // (debug.dumpraw= a float item's values beside its PNG, <name>.f32: u32 width, u32 height, then RGBA32F rows; the
    // dark values of HDR targets that the PNG rounds to 0. With dumpw:4096, the full-resolution dump tools/direct_picture_check.py packs)
    static const bool raw = dbg(SET_DBG_DUMPRAW);
    if (raw && it.kind == 0) {   // (M6b: depth too, as the frozen back end's RX_VK_DUMP_RAW writes it: its value in R, G and B)
        std::snprintf(path + std::strlen(path) - 4, 5, ".f32");
        if (FILE* f = fopen(path, "wb")) {
            const uint32_t wh[2] = {it.w, it.h};
            fwrite(wh, 4, 2, f);
            fwrite(map, 16, n, f);
            fclose(f);
        }
    }
}

}  // namespace

bool dump_recording() {
    const int64_t t = target();
    if (t < 0 || written) return false;
    const int64_t p = (int64_t)present_count();
    return p >= t - 2 && p < t + frames();
}

void dump_pass(List* l, VkCommandBuffer cb) {
    if (!dump_recording()) return;
    char what[96];
    for (int i = 0; i < l->pass.n; i++)
        if (l->pass.color[i].view) {
            std::snprintf(what, sizeof what, "pass c%d prog %016llx list %s rt %llu", i, (unsigned long long)l->pass.last_prog,
                          l->info.kind == GPU_LIST_GATE ? "gate" : l->info.kind == GPU_LIST_LAYER ? "layer" : "other",
                          (unsigned long long)l->pass.set_rt_at);
            if (DumpItem* it = copy(cb, l->pass.color[i], what)) l->dumps.push_back(it), st_recorded++;
        }
    if (l->pass.depth.view) {
        std::snprintf(what, sizeof what, "pass depth prog %016llx rt %llu", (unsigned long long)l->pass.last_prog,
                      (unsigned long long)l->pass.set_rt_at);
        if (DumpItem* it = copy(cb, l->pass.depth, what)) l->dumps.push_back(it);
    }
}

bool dump_claim(const std::vector<DumpItem*>& items, uint64_t frame) {
    const int64_t t = target();
    if (t < 0 || written || (int64_t)frame < t || (int64_t)frame >= t + frames()) return false;
    std::lock_guard lk(mu);
    st_claims++;
    for (DumpItem* it : items)
        if (!it->claimed) it->claimed = true, claimed.push_back(it);
    return true;
}

void dump_sent(uint64_t value) {
    std::lock_guard lk(mu);
    claimed_value = std::max(claimed_value, value);
}

void dump_present(uint64_t present) {
    const int64_t t = target();
    {
        std::lock_guard lk(mu);
        for (size_t i = 0; i < graveyard.size();)
            if (sync_completed() >= graveyard[i].second) {
                free_item(graveyard[i].first);
                delete graveyard[i].first;
                graveyard[i] = graveyard.back(), graveyard.pop_back();
            } else {
                i++;
            }
    }
    if (t < 0 || written || (int64_t)present < t + frames() + 2) return;   // (its submissions sent by then: some are held at waits)
    uint64_t v;
    std::vector<DumpItem*> items;
    {
        std::lock_guard lk(mu);
        v = claimed_value;
        items.swap(claimed);
    }
    sync_wait_value(v, UINT64_MAX);
    const char* d = dbg_value(SET_DBG_DUMPDIR);
    dir = std::string(d ? d : ".") + "/dump_" + std::to_string(t);
    os_mkdir(dir.c_str());
    logf("vk: dumping the frame after present %lld to %s (direct: %lld frames, %zu items; %lld recorded, %lld submissions "
         "claimed)", (long long)t, dir.c_str(), (long long)frames(), items.size(), (long long)st_recorded.load(), (long long)st_claims.load());
    for (DumpItem* it : items) {
        write_item(*it, seq++);
        free_item(it);   // (the struct stays: its list may still name it)
    }
    logf("vk: dump of the frame after present %lld: %d steps so far", (long long)t, seq);
    written = true;
}

void dump_free(std::vector<DumpItem*>& items) {
    if (items.empty()) return;
    std::lock_guard lk(mu);
    const uint64_t v = sync_pushed();
    for (DumpItem* it : items)
        if (!it->claimed) graveyard.push_back({it, v});
    items.clear();
}

bool dump_write_png(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) { return write_png(path, rgba, w, h); }

}  // namespace hsd
