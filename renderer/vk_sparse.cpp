// NVN sparse textures (vk_sparse.h, docs/direct/sparse-textures.md).
//
// The game's model of a sparse texture's storage (MKW sparse_texture_init, from nvnTextureGetSparseTileLayout) is 64 KiB
// pages: each layer has its tiled levels' tiles in level and row order, then its mip tail pages, and layers follow one
// another (pages a layer = the storage's pages / layers). The image's own tiles are Vulkan's sparse blocks. A page of NVN
// holds a whole number of them (`units`: 1 when the image's format has NVN's bytes per block, 4 for ASTC 8x8 shown as
// BC7), so each physical pool has device memory of `units` x 64 KiB for each of its pages, one backing a class of
// images, and an NVN page's Vulkan blocks are bound to its place in that backing in a fixed order. A tail page holds,
// in order, Vulkan's tiled levels that NVN's tail covers (BC7's 256-texel level), then Vulkan's mip tail of that layer.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "vk_check.h"
#include "vk_image.h"
#include "vk_marks.h"
#include "vk_sparse.h"

namespace hsd {
namespace {

using clk = std::chrono::steady_clock;
constexpr uint64_t kPage = 0x10000;     // (NVN's page; the device's sparse block must be the same)
constexpr uint32_t kPhysical = 0x100;   // (NVN_MEMORY_POOL_FLAGS_PHYSICAL)
constexpr uint32_t kTailPiece = ~0u;    // (a tail unit that is a piece of Vulkan's mip tail)

/// A physical pool's device memory for one class of images: `units` 64 KiB units for each of its pages
struct Backing {
    uint32_t units = 0;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint64_t bytes = 0;
};
struct Pool {
    uint64_t base = 0, size = 0;
    std::vector<Backing> backings;
    uint64_t free_at = 0;   // (finalized: the bind value after which its memory goes; 0 while its unbinds are queued)
};
struct Level {   // (one of NVN's tiled levels)
    uint32_t first, tx, ty;
};
struct TailUnit {   // (a unit of a layer's tail pages: a Vulkan block of a level, or a 64 KiB piece of its mip tail)
    uint32_t level, x, y;
};
struct Sparse {
    Image* im = nullptr;
    uint64_t gpu = 0, pages = 0;
    uint32_t layers = 1, ppl = 0, tpl = 0, tail_pl = 0;   // (pages a layer; of them tiles, then tail pages)
    uint32_t gw = 0, gh = 0, kx = 1, ky = 1, units = 1;   // (Vulkan's block in texels; blocks across and down an NVN tile)
    std::vector<Level> levels;
    std::vector<TailUnit> tail;
    uint64_t tail_off = 0, tail_size = 0, tail_stride = 0;
    bool single_tail = false;
    uint64_t full_bytes = 0;   // (the image fully resident)
    std::vector<uint64_t> phys;   // (each page's physical guest address; 0 unbound)
    uint64_t bound = 0;           // (pages bound)
    uint64_t last_bind = 0;       // (the bind value that last bound its tiles)
};

// (the device's choice, made once: choose())
int chosen = -1;   // (-1 not yet, 0 sparse textures cannot be bound here, 1 they can)
uint32_t mem_type = UINT32_MAX;
VkSemaphore bind_sem = VK_NULL_HANDLE;

std::mutex mu;   // (everything below; taken after images_mu, before dev.sparse_mu)
std::map<uint64_t, Pool> pools;   // (live physical pools by base)
std::list<Pool> dead_pools;       // (finalized, their memory not freed yet)
std::multimap<uint64_t, Sparse*> by_gpu;
std::unordered_map<Image*, Sparse*> by_image;
uint64_t max_span = 0;
uint64_t bind_value = 0;   // (bind_sem's last signal)
// the queued binds, one for each place (a later bind of a place replaces the earlier one)
struct BindKey {
    VkImage image;
    uint64_t a, b;
    bool operator==(const BindKey& o) const { return image == o.image && a == o.a && b == o.b; }
};
struct BindKeyHash {
    size_t operator()(const BindKey& k) const {
        uint64_t h = (uint64_t)(uintptr_t)k.image * 0x9e3779b97f4a7c15ull ^ k.a * 0xff51afd7ed558ccdull ^ k.b;
        return (size_t)(h ^ h >> 29);
    }
};
struct ImageBind {
    Sparse* s;
    VkSparseImageMemoryBind b;
};
struct OpaqueBind {
    Sparse* s;
    VkSparseMemoryBind b;
};
std::vector<ImageBind> ibinds;
std::vector<OpaqueBind> obinds;
std::unordered_map<BindKey, size_t, BindKeyHash> queued_at;   // (index into ibinds, or obinds | 1 << 63)
std::atomic<bool> queued{false};

int64_t st_binds = 0, st_unbinds = 0, st_calls = 0, st_largest = 0, st_outside = 0, st_refused = 0, st_backings = 0;
double st_bind_ms = 0;

bool choose() {
    if (chosen >= 0) return chosen == 1;
    chosen = 0;
    const char* why = nullptr;
    if (!dev.sparse_queue) why = "no queue for sparse binding";
    else if (!dev.features.sparseBinding || !dev.features.sparseResidencyImage2D || !dev.features.sparseResidencyAliased)
        why = "no sparse residency of aliased 2D images";
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(dev.physical, &p);
    if (!why && !p.sparseProperties.residencyNonResidentStrict) why = "unbound tiles do not read as zero";
    // (the memory type: what a sparse BC7 array takes, device-local and not host-visible first)
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_BC7_UNORM_BLOCK;
    ci.extent = {1024, 1024, 1};
    ci.mipLevels = 1, ci.arrayLayers = 2;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkMemoryRequirements req{};
    if (!why) {
        VkImage probe = VK_NULL_HANDLE;
        if (vkCreateImage(dev.device, &ci, nullptr, &probe) != VK_SUCCESS) why = "no sparse BC7 image";
        else vkGetImageMemoryRequirements(dev.device, probe, &req), vkDestroyImage(dev.device, probe, nullptr);
    }
    if (!why && req.alignment != kPage) why = "a sparse block that is not 64 KiB";
    for (int pass = 0; !why && pass < 2 && mem_type == UINT32_MAX; pass++)
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && mem_type == UINT32_MAX; i++) {
            const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits >> i & 1) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (pass || !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))
                mem_type = i;
        }
    if (!why && mem_type == UINT32_MAX) why = "no device-local memory type for sparse images";
    if (!why) {
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        si.pNext = &ti;
        if (vkCreateSemaphore(dev.device, &si, nullptr, &bind_sem) != VK_SUCCESS) why = "no timeline semaphore for the binds";
    }
    if (why) {
        logf("direct: sparse textures: not bound on this device (%s): they get the placeholder", why);
        return false;
    }
    logf("direct: sparse textures: tiles bound on layer 0's bind queue%s, to the physical pools' device memory of type %u "
         "(flags %#x), in 64 KiB blocks; unbound tiles read as zero", dev.sparse_shared ? " (the graphics queue)" : "", mem_type,
         dev.memory.memoryTypes[mem_type].propertyFlags);
    chosen = 1;
    return true;
}

/// A level's Vulkan block bound to mem at off (VK_NULL_HANDLE: unbound), queued
void queue_block(Sparse& s, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, VkDeviceMemory mem, uint64_t off) {
    const uint32_t lw = std::max(1u, s.im->width >> level), lh = std::max(1u, s.im->height >> level);
    if (x >= lw || y >= lh) return;
    VkSparseImageMemoryBind b{};
    b.subresource = {s.im->aspect, level, layer};
    b.offset = {(int32_t)x, (int32_t)y, 0};
    b.extent = {std::min(s.gw, lw - x), std::min(s.gh, lh - y), 1};
    b.memory = mem, b.memoryOffset = mem ? off : 0;
    const BindKey k{s.im->image, (uint64_t)level << 32 | layer, (uint64_t)x << 32 | y};
    auto [it, fresh] = queued_at.emplace(k, ibinds.size());
    if (fresh) ibinds.push_back({&s, b});
    else ibinds[it->second] = {&s, b};
    (mem ? st_binds : st_unbinds)++;
}
void queue_tail_piece(Sparse& s, uint32_t layer, uint32_t piece, VkDeviceMemory mem, uint64_t off) {
    VkSparseMemoryBind b{};
    b.resourceOffset = s.tail_off + (s.single_tail ? 0 : layer * s.tail_stride) + piece * kPage;
    b.size = std::min<uint64_t>(kPage, s.tail_size - piece * kPage);
    b.memory = mem, b.memoryOffset = mem ? off : 0;
    const BindKey k{s.im->image, b.resourceOffset, ~0ull};
    auto [it, fresh] = queued_at.emplace(k, obinds.size() | 1ull << 63);
    if (fresh) obinds.push_back({&s, b});
    else obinds[it->second & ~(1ull << 63)] = {&s, b};
    (mem ? st_binds : st_unbinds)++;
}

/// Page p's Vulkan blocks bound to mem from off on, `units` 64 KiB units (VK_NULL_HANDLE: unbound), queued
void queue_page(Sparse& s, uint64_t p, VkDeviceMemory mem, uint64_t off) {
    const uint32_t layer = (uint32_t)(p / s.ppl), q = (uint32_t)(p % s.ppl);
    if (layer >= s.layers) return;   // (past the last layer: the storage's rounding)
    if (q < s.tpl) {
        uint32_t l = 0;
        while (l + 1 < s.levels.size() && s.levels[l + 1].first <= q) l++;
        const uint32_t idx = q - s.levels[l].first, tx = idx % s.levels[l].tx, ty = idx / s.levels[l].tx;
        for (uint32_t j = 0; j < s.ky; j++)
            for (uint32_t i = 0; i < s.kx; i++)
                queue_block(s, l, layer, (tx * s.kx + i) * s.gw, (ty * s.ky + j) * s.gh, mem, off + (j * s.kx + i) * kPage);
        return;
    }
    const uint64_t first = (uint64_t)(q - s.tpl) * s.units;
    for (uint64_t u = first; u < first + s.units && u < s.tail.size(); u++) {
        const TailUnit& t = s.tail[u];
        if (t.level != kTailPiece) queue_block(s, t.level, layer, t.x, t.y, mem, off + (u - first) * kPage);
        else if (!s.single_tail || layer == 0) queue_tail_piece(s, layer, t.x, mem, off + (u - first) * kPage);
    }
}

Pool* pool_of(uint64_t phys) {
    auto it = pools.upper_bound(phys);
    if (it == pools.begin()) return nullptr;
    --it;
    return phys < it->second.base + it->second.size ? &it->second : nullptr;
}
/// The pool's backing for images of `units` units a page, allocated at its first use
VkDeviceMemory backing(Pool& pl, uint32_t units) {
    for (const Backing& b : pl.backings)
        if (b.units == units) return b.mem;
    Backing b;
    b.units = units;
    b.bytes = pl.size / kPage * units * kPage;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = b.bytes;
    ai.memoryTypeIndex = mem_type;
    if (vkAllocateMemory(dev.device, &ai, nullptr, &b.mem) != VK_SUCCESS) {
        logf("direct: sparse textures: %llu MB of device memory for physical pool %#llx not allocated",
             (unsigned long long)(b.bytes >> 20), (unsigned long long)pl.base);
        strict_event("a physical pool's device memory not allocated", "pool %#llx, %llu bytes", (unsigned long long)pl.base,
                     (unsigned long long)b.bytes);
        return VK_NULL_HANDLE;
    }
    object_made(1, "a physical pool's device memory");
    mem_account(MO_SPARSE, mem_type, (int64_t)b.bytes);
    st_backings++;
    logf("direct: sparse textures: physical pool %#llx (%llu MB): %llu MB of device memory for images of %u x 64 KiB a page%s",
         (unsigned long long)pl.base, (unsigned long long)(pl.size >> 20), (unsigned long long)(b.bytes >> 20), units,
         pl.backings.empty() ? "" : " (a second class of images in this pool)");
    pl.backings.push_back(b);
    return b.mem;
}

/// Page p of s now maps to physical guest address ph (0: unmapped)
void set_page(Sparse& s, uint64_t p, uint64_t ph) {
    if (s.phys[p] == ph) return;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint64_t off = 0;
    if (ph) {
        Pool* pl = pool_of(ph);
        if (!pl) {
            if (st_outside++ < 4)
                logf("direct: sparse textures: image #%u's page %llu maps to %#llx, in no physical pool: unbound", s.im->id,
                     (unsigned long long)p, (unsigned long long)ph);
            strict_event("a sparse texture's page in no physical pool", "image #%u page %llu -> %#llx", s.im->id,
                         (unsigned long long)p, (unsigned long long)ph);
            ph = 0;
        } else {
            mem = backing(*pl, s.units);
            if (!mem) ph = 0;
            off = (ph - pl->base) / kPage * s.units * kPage;
        }
    }
    if (s.phys[p] == ph) return;
    s.bound += (ph != 0) - (s.phys[p] != 0);
    s.phys[p] = ph;
    queue_page(s, p, mem, off);
    queued.store(true, std::memory_order_release);
}

bool refuse(const nvn_tex* t, const char* why) {
    st_refused++;
    logf("direct: sparse texture %#llx %ux%ux%u, %u levels, NVN format %#x, target %d: not bound (%s): the placeholder",
         (unsigned long long)t->gpu, t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.format, t->d.target, why);
    strict_event("a sparse texture the back end cannot bind", "texture %#llx NVN format %#x: %s", (unsigned long long)t->gpu,
                 t->d.format, why);
    return false;
}

}  // namespace

bool sparse_image_made(Image* im, const nvn_tex* t) {
    std::lock_guard lk(mu);
    if (!choose()) return refuse(t, "sparse images are not bound on this device");
    const nvn_texdesc& d = t->d;
    if (im->type != VK_IMAGE_TYPE_2D || im->samples != VK_SAMPLE_COUNT_1_BIT) return refuse(t, "not a 2D single-sample texture");
    if (t->gpu % kPage) return refuse(t, "storage not on a 64 KiB page");
    auto s = std::make_unique<Sparse>();
    s->im = im;
    s->gpu = t->gpu;
    s->layers = im->layers;
    s->pages = (t->L.size + kPage - 1) / kPage;
    s->ppl = (uint32_t)(s->pages / s->layers);
    // NVN's tiles: its tile layout's shape, its tiled levels (those a tile fits in), each level's tiles in rows
    int bw = 1, bh = 1;
    const int bpb = host->fmt_bpb(d.format, &bw, &bh);
    int tbw, tbh;
    nvn_sparse_tile_blocks(bpb, &tbw, &tbh);
    const uint32_t ntw = (uint32_t)(tbw * bw), nth = (uint32_t)(tbh * bh);
    for (uint32_t l = 0; l < im->levels; l++) {
        const uint32_t lw = std::max(1u, im->width >> l), lh = std::max(1u, im->height >> l);
        if (lw < ntw || lh < nth) break;
        s->levels.push_back({s->tpl, (lw + ntw - 1) / ntw, (lh + nth - 1) / nth});
        s->tpl += s->levels.back().tx * s->levels.back().ty;
    }
    if (s->levels.empty()) return refuse(t, "no level a tile fits in");
    if (s->tpl > s->ppl) return refuse(t, "more tiles a layer than pages");
    s->tail_pl = s->ppl - s->tpl;
    // Vulkan's blocks and mip tail
    uint32_t n = 0;
    vkGetImageSparseMemoryRequirements(dev.device, im->image, &n, nullptr);
    std::vector<VkSparseImageMemoryRequirements> reqs(n);
    vkGetImageSparseMemoryRequirements(dev.device, im->image, &n, reqs.data());
    const VkSparseImageMemoryRequirements* r = nullptr;
    for (const VkSparseImageMemoryRequirements& q : reqs) {
        if (q.formatProperties.aspectMask & VK_IMAGE_ASPECT_METADATA_BIT) return refuse(t, "its image has a metadata aspect");
        if (q.formatProperties.aspectMask & im->aspect) r = &q;
    }
    if (!r) return refuse(t, "the driver gives no sparse layout of its image");
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev.device, im->image, &mr);
    if (mr.alignment != kPage || !(mr.memoryTypeBits >> mem_type & 1)) return refuse(t, "its blocks are not 64 KiB of the chosen memory");
    s->full_bytes = mr.size;
    // (the granularity in texels: a 64 KiB block of the image's format)
    s->gw = r->formatProperties.imageGranularity.width, s->gh = r->formatProperties.imageGranularity.height;
    if ((uint64_t)(s->gw / im->bw) * (s->gh / im->bh) * im->bpb != kPage) s->gw *= im->bw, s->gh *= im->bh;
    if (!s->gw || !s->gh || ntw % s->gw || nth % s->gh) return refuse(t, "NVN's tile is not a whole number of the image's blocks");
    s->kx = ntw / s->gw, s->ky = nth / s->gh, s->units = s->kx * s->ky;
    const uint32_t vk_tail = std::min(r->imageMipTailFirstLod, im->levels);
    if (vk_tail < s->levels.size()) return refuse(t, "a level NVN tiles is in the image's mip tail");
    s->single_tail = r->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT;
    s->tail_off = r->imageMipTailOffset, s->tail_size = vk_tail < im->levels ? r->imageMipTailSize : 0;
    s->tail_stride = r->imageMipTailStride;
    for (uint32_t l = (uint32_t)s->levels.size(); l < vk_tail; l++) {   // (NVN's tail: the image's tiled levels in it, then its mip tail)
        const uint32_t lw = std::max(1u, im->width >> l), lh = std::max(1u, im->height >> l);
        for (uint32_t y = 0; y < lh; y += s->gh)
            for (uint32_t x = 0; x < lw; x += s->gw) s->tail.push_back({l, x, y});
    }
    for (uint32_t k = 0; k * kPage < s->tail_size; k++) s->tail.push_back({kTailPiece, k, 0});
    if (s->tail.size() > (uint64_t)s->tail_pl * s->units) return refuse(t, "its mip tail needs more than NVN's tail pages hold");
    s->phys.assign(s->pages, 0);
    Sparse* sp = s.release();
    by_gpu.emplace(sp->gpu, sp);
    by_image[im] = sp;
    max_span = std::max(max_span, sp->pages * kPage);
    static int told = 0;
    if (told++ < 12)
        logf("direct: sparse texture %#llx %ux%u x%u, %u levels, NVN format %#x as vk %d: %llu pages (%u a layer: %u tiles, %u "
             "tail), NVN tile %ux%u = %u x %ux%u blocks of the image, its mip tail from level %u (%llu bytes%s); %llu MB fully "
             "resident", (unsigned long long)sp->gpu, im->width, im->height, im->layers, im->levels, d.format, (int)im->format,
             (unsigned long long)sp->pages, sp->ppl, sp->tpl, sp->tail_pl, ntw, nth, sp->units, sp->gw, sp->gh, vk_tail,
             (unsigned long long)sp->tail_size, sp->single_tail ? ", one for all layers" : "", (unsigned long long)(sp->full_bytes >> 20));
    for (uint64_t p = 0; p < sp->pages; p++) {   // (the pages the game has mapped already)
        const uint64_t g = host->gpu_guest(sp->gpu + p * kPage);
        if (g) set_page(*sp, p, g);
    }
    return true;
}

void sparse_image_gone(Image* im) {
    std::unique_lock lk(mu);
    auto it = by_image.find(im);
    if (it == by_image.end()) return;
    Sparse* s = it->second;
    by_image.erase(it);
    for (auto g = by_gpu.lower_bound(s->gpu); g != by_gpu.end() && g->first == s->gpu; ++g)
        if (g->second == s) {
            by_gpu.erase(g);
            break;
        }
    for (ImageBind& b : ibinds)
        if (b.s == s) b.s = nullptr;
    for (OpaqueBind& b : obinds)
        if (b.s == s) b.s = nullptr;
    for (auto q = queued_at.begin(); q != queued_at.end();)
        q = q->first.image == im->image ? queued_at.erase(q) : std::next(q);
    const uint64_t wait = s->last_bind;
    delete s;
    lk.unlock();
    if (wait) {   // (its last binds made before it is destroyed)
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1, wi.pSemaphores = &bind_sem, wi.pValues = &wait;
        vkWaitSemaphores(dev.device, &wi, UINT64_MAX);
    }
}

void sparse_pool(uint64_t gpu, uint64_t size, uint32_t flags, int on) {
    if (!(flags & kPhysical)) return;
    std::lock_guard lk(mu);
    if (on) {
        Pool& pl = pools[gpu];
        pl.base = gpu, pl.size = size;
        return;
    }
    auto it = pools.find(gpu);
    if (it == pools.end()) return;
    for (auto& [g, s] : by_gpu)   // (its pages unbound wherever they are still mapped)
        for (uint64_t p = 0; p < s->pages; p++)
            if (s->phys[p] >= gpu && s->phys[p] < gpu + size) set_page(*s, p, 0);
    if (!it->second.backings.empty()) {   // (freed after the next bind call: sparse_flush, sparse_sweep)
        dead_pools.push_back(std::move(it->second));
        queued.store(true, std::memory_order_release);
    }
    pools.erase(it);
}

void sparse_mapped(uint64_t gpu, uint64_t size, uint64_t phys) {
    std::lock_guard lk(mu);
    if (by_gpu.empty()) return;
    for (auto it = by_gpu.lower_bound(gpu > max_span ? gpu - max_span : 0); it != by_gpu.end() && it->first < gpu + size; ++it) {
        Sparse& s = *it->second;
        const uint64_t lo = std::max(gpu, s.gpu), hi = std::min(gpu + size, s.gpu + s.pages * kPage);
        for (uint64_t a = lo; a < hi; a += kPage) set_page(s, (a - s.gpu) / kPage, phys ? phys + (a - gpu) : 0);
    }
}

bool sparse_flush(VkSemaphore timeline, uint64_t after, VkSemaphore* sem, uint64_t* value) {
    if (!queued.load(std::memory_order_acquire)) return false;
    std::lock_guard lk(mu);
    queued.store(false, std::memory_order_relaxed);
    std::sort(ibinds.begin(), ibinds.end(), [](const ImageBind& a, const ImageBind& b) { return a.s < b.s; });
    std::sort(obinds.begin(), obinds.end(), [](const OpaqueBind& a, const OpaqueBind& b) { return a.s < b.s; });
    std::vector<VkSparseImageMemoryBind> iflat;
    std::vector<VkSparseMemoryBind> oflat;
    std::vector<std::pair<Sparse*, size_t>> iruns, oruns;   // (each image's first bind)
    for (const ImageBind& b : ibinds)
        if (b.s) {
            if (iruns.empty() || iruns.back().first != b.s) iruns.push_back({b.s, iflat.size()});
            iflat.push_back(b.b);
        }
    for (const OpaqueBind& b : obinds)
        if (b.s) {
            if (oruns.empty() || oruns.back().first != b.s) oruns.push_back({b.s, oflat.size()});
            oflat.push_back(b.b);
        }
    ibinds.clear(), obinds.clear(), queued_at.clear();
    const bool any = !iflat.empty() || !oflat.empty();
    std::vector<VkSparseImageMemoryBindInfo> ii;
    std::vector<VkSparseImageOpaqueMemoryBindInfo> oi;
    for (size_t i = 0; i < iruns.size(); i++) {
        const size_t end = i + 1 < iruns.size() ? iruns[i + 1].second : iflat.size();
        ii.push_back({iruns[i].first->im->image, (uint32_t)(end - iruns[i].second), iflat.data() + iruns[i].second});
    }
    for (size_t i = 0; i < oruns.size(); i++) {
        const size_t end = i + 1 < oruns.size() ? oruns[i + 1].second : oflat.size();
        oi.push_back({oruns[i].first->im->image, (uint32_t)(end - oruns[i].second), oflat.data() + oruns[i].second});
    }
    if (any) {
        const uint64_t signal = bind_value + 1;
        VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        ts.waitSemaphoreValueCount = after ? 1 : 0, ts.pWaitSemaphoreValues = &after;
        ts.signalSemaphoreValueCount = 1, ts.pSignalSemaphoreValues = &signal;
        VkBindSparseInfo si{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
        si.pNext = &ts;
        si.waitSemaphoreCount = after ? 1 : 0, si.pWaitSemaphores = &timeline;
        si.imageBindCount = (uint32_t)ii.size(), si.pImageBinds = ii.data();
        si.imageOpaqueBindCount = (uint32_t)oi.size(), si.pImageOpaqueBinds = oi.data();
        si.signalSemaphoreCount = 1, si.pSignalSemaphores = &bind_sem;
        const auto t0 = clk::now();
        VkResult r;
        if (dev.sparse_shared) {
            r = vkQueueBindSparse(dev.sparse_queue, 1, &si, VK_NULL_HANDLE);
        } else {
            std::lock_guard qk(dev.sparse_mu);
            r = vkQueueBindSparse(dev.sparse_queue, 1, &si, VK_NULL_HANDLE);
        }
        st_bind_ms += std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        if (r != VK_SUCCESS) {
            logf("direct: sparse textures: vkQueueBindSparse of %zu binds failed (VkResult %d)", iflat.size() + oflat.size(), (int)r);
            if (r == VK_ERROR_DEVICE_LOST) device_lost("a sparse bind");
            return false;
        }
        bind_value = signal;
        st_calls++, st_largest = std::max<int64_t>(st_largest, (int64_t)(iflat.size() + oflat.size()));
        for (const auto& [s, at] : iruns) s->last_bind = bind_value;
        for (const auto& [s, at] : oruns) s->last_bind = bind_value;
    }
    for (Pool& pl : dead_pools)
        if (!pl.free_at) pl.free_at = std::max<uint64_t>(bind_value, 1);
    if (!any) return false;
    *sem = bind_sem, *value = bind_value;
    return true;
}

void sparse_sweep() {
    std::lock_guard lk(mu);
    if (dead_pools.empty() || !bind_sem) return;
    uint64_t done = 0;
    vkGetSemaphoreCounterValue(dev.device, bind_sem, &done);
    for (auto it = dead_pools.begin(); it != dead_pools.end();) {
        if (!it->free_at || it->free_at > done) {
            ++it;
            continue;
        }
        for (Backing& b : it->backings) {
            vkFreeMemory(dev.device, b.mem, nullptr);
            mem_account(MO_SPARSE, mem_type, -(int64_t)b.bytes);
        }
        it = dead_pools.erase(it);
    }
}

void sparse_stats(char* out, size_t cap) {
    std::lock_guard lk(mu);
    uint64_t pages = 0, bound = 0, full = 0, mem = 0, dead = 0, pool_bytes = 0;
    for (const auto& [im, s] : by_image) pages += s->pages, bound += s->bound, full += s->full_bytes;
    for (const auto& [g, pl] : pools) {
        pool_bytes += pl.size;
        for (const Backing& b : pl.backings) mem += b.bytes;
    }
    for (const Pool& pl : dead_pools)
        for (const Backing& b : pl.backings) dead += b.bytes;
    std::snprintf(out, cap, "sparse textures: %zu images (%llu MB if fully resident), %llu of their %llu pages bound; %zu physical "
                  "pools (%llu MB), their device memory %llu MB in %lld backings made since the start, %llu MB of finalized pools "
                  "not freed yet; since the start %lld binds and %lld unbinds in %lld bind calls (the largest %lld, %.1f ms in "
                  "vkQueueBindSparse), %lld pages mapped outside a physical pool, %lld textures not bound",
                  by_image.size(), (unsigned long long)(full >> 20), (unsigned long long)bound, (unsigned long long)pages,
                  pools.size(), (unsigned long long)(pool_bytes >> 20), (unsigned long long)(mem >> 20), (long long)st_backings,
                  (unsigned long long)(dead >> 20), (long long)st_binds, (long long)st_unbinds, (long long)st_calls,
                  (long long)st_largest, st_bind_ms, (long long)st_outside, (long long)st_refused);
}

}  // namespace hsd
