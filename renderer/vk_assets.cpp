// Prepared assets (vk_assets.h). The store is the builder's (FORMAT_VERSION 1; ounce/tex/src/store.rs describes the
// format): index.bin and the packs are mapped read-only once; a texture's frames are standard zstd frames (third_party/zstd,
// linked into this library) decompressed straight into the upload's staging, mip by mip, by the thread that initializes
// the texture. Debug aids (HOS_DBG): assetcheck:<n> reads back n prepared images and the sampled terrain and glyph uploads
// after the load and compares them with the store's bytes and the bytes given; assetdump writes a font atlas, terrain
// tiles, a UI texture and normal maps as PNGs into <dumpdir or the run's folder>/assets.

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <zstd.h>

#include "vk_list_api.h"
#include "os.h"
#include "vk_check.h"
#include "vk_assets.h"
#include "vk_cmd.h"
#include "vk_dump.h"
#include "vk_image.h"

namespace hsd {
namespace {

// ---- the store ----
struct Frame {
    uint16_t pack;
    uint32_t csize;
    uint64_t off, raw_off;
    uint32_t raw_size;
};
struct Rec {
    uint32_t file, index, name_off, name_len, nvn_format, vk_format;
    uint8_t kind, status, view, target, map[4], sel[4], dropped, bh, rule;
    uint32_t w, h, d, layers, mips, mip_first, fr_first, fr_n;
    uint64_t raw, hash;
    uint32_t isz, doff;
};
enum { KIND_NONE, KIND_COPY_BC, KIND_COPY, KIND_BC7, KIND_BC5, KIND_BC4, KIND_BC1, KIND_RAW };

struct Store {
    bool ok = false;
    std::string dir;
    const uint8_t* idx = nullptr;
    size_t idx_n = 0;
    uint32_t nfiles = 0, ntex = 0, nmips = 0, nframes = 0, npacks = 0, nstr = 0;
    const uint8_t *files = nullptr, *texs = nullptr, *mipr = nullptr, *framer = nullptr, *str = nullptr;
    std::vector<const uint8_t*> packs;
    std::vector<size_t> pack_n;
    std::vector<os_file*> files_open;   // (frames are read with a read at an offset: one call, not a page fault at a time)
    std::vector<uint32_t> bucket, next;   // (name hash -> first record + 1; chain)
    uint32_t nbucket = 1;
} S;
std::mutex open_mu;

uint32_t name_hash(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}
template <class T> T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
void rec_at(uint32_t i, Rec& r) {
    const uint8_t* p = S.texs + (size_t)i * 128;
    r.file = rd<uint32_t>(p), r.index = rd<uint32_t>(p + 4), r.name_off = rd<uint32_t>(p + 8), r.name_len = rd<uint32_t>(p + 12);
    r.nvn_format = rd<uint32_t>(p + 16), r.vk_format = rd<uint32_t>(p + 20);
    r.kind = p[24], r.status = p[25], r.view = p[26], r.target = p[27];
    std::memcpy(r.map, p + 28, 4), std::memcpy(r.sel, p + 32, 4);
    r.dropped = p[36], r.bh = p[37], r.rule = p[116];
    r.w = rd<uint32_t>(p + 40), r.h = rd<uint32_t>(p + 44), r.d = rd<uint32_t>(p + 48), r.layers = rd<uint32_t>(p + 52);
    r.mips = rd<uint32_t>(p + 56), r.mip_first = rd<uint32_t>(p + 60), r.fr_first = rd<uint32_t>(p + 64), r.fr_n = rd<uint32_t>(p + 68);
    r.raw = rd<uint64_t>(p + 72), r.hash = rd<uint64_t>(p + 80);
    r.isz = rd<uint32_t>(p + 92), r.doff = rd<uint32_t>(p + 96);
}
void frame_at(uint32_t i, Frame& f) {
    const uint8_t* p = S.framer + (size_t)i * 32;
    f.pack = rd<uint16_t>(p), f.csize = rd<uint32_t>(p + 4), f.off = rd<uint64_t>(p + 8), f.raw_off = rd<uint64_t>(p + 16);
    f.raw_size = rd<uint32_t>(p + 24);
}
void mip_at(const Rec& r, uint32_t m, uint64_t& off, uint32_t& layer_bytes) {
    const uint8_t* p = S.mipr + (size_t)(r.mip_first + m) * 16;
    off = rd<uint64_t>(p), layer_bytes = rd<uint32_t>(p + 8);
}
const uint8_t* map_file(const std::string& path, size_t& n) {
    os_file* f = os_file_open(path.c_str(), OS_READ, nullptr);
    if (!f) return nullptr;
    const void* p = os_file_map_read(f, &n);
    os_file_close(f);
    if (!p) return nullptr;
    mem_main_mapped(n);
    return (const uint8_t*)p;
}
/// The record's block size: the kept formats' from the NVN format, the converted ones' from what they became
/// A copied record's block from its BNTX format (the record's nvn_format field holds the BNTX code, e.g. 0x1d01: high byte
/// the type, low byte the variant), as ounce/tex/src/bntx.rs's fmt has it; 0: not a type the converter copies
int bntx_block(uint32_t format, int& w, int& h) {
    struct B { uint8_t type, w, h, bytes; };
    static const B table[] = {{0x02, 1, 1, 1}, {0x07, 1, 1, 2}, {0x09, 1, 1, 2}, {0x0a, 1, 1, 2}, {0x0b, 1, 1, 4}, {0x0c, 1, 1, 4},
                              {0x0e, 1, 1, 4}, {0x0f, 1, 1, 4}, {0x12, 1, 1, 4}, {0x14, 1, 1, 4}, {0x15, 1, 1, 8}, {0x17, 1, 1, 8},
                              {0x19, 1, 1, 16}, {0x1a, 4, 4, 8}, {0x1b, 4, 4, 16}, {0x1c, 4, 4, 16}, {0x1d, 4, 4, 8},
                              {0x1e, 4, 4, 16}, {0x1f, 4, 4, 16}, {0x20, 4, 4, 16}};
    for (const B& b : table)
        if (b.type == (format >> 8 & 0xff)) return w = b.w, h = b.h, b.bytes;
    return 0;
}

bool block_of(const Rec& r, uint32_t& bpb, uint32_t& bw, uint32_t& bh) {
    int w = 1, h = 1, b = 0;
    switch (r.kind) {
    // (M3b: was nvn_block_size(r.nvn_format), an NVN format lookup given the BNTX code: 0 for every copied record, so each
    // keyed as "an asset with no record" and drew the placeholder; the title screen's RGTC textures were three of them)
    case KIND_COPY_BC: case KIND_COPY: b = bntx_block(r.nvn_format, w, h); break;
    case KIND_BC7: case KIND_BC5: b = 16, w = h = 4; break;
    case KIND_BC4: case KIND_BC1: b = 8, w = h = 4; break;
    case KIND_RAW:
        b = r.vk_format == VK_FORMAT_R8_UNORM || r.vk_format == VK_FORMAT_R8_SRGB ? 1 : r.vk_format == VK_FORMAT_R8G8_UNORM ? 2 : 4;
        break;
    default: return false;
    }
    if (b <= 0) return false;
    bpb = (uint32_t)b, bw = (uint32_t)w, bh = (uint32_t)h;
    return true;
}

// ---- links: a texture object -> its load's record (armed by a load or a rebind, taken by the next initialization) ----
struct Link {
    uint32_t rec = 0;   // (+1; 0: a miss)
    const char* why = nullptr;
    bool armed = false;
    uint64_t plain = 0;
};
std::atomic<uint64_t> plain_ids{0};
std::atomic<int64_t> st_plain{0};
std::mutex links_mu;
std::unordered_map<const void*, Link> links;
std::shared_mutex serial_mu;
std::unordered_map<uint64_t, AssetRef> by_serial;
std::atomic<uint64_t> pool_lo{0}, pool_hi{0};

// ---- counters ----
enum { S_STORE, S_TERRAIN, S_GLYPH, S_TEXELS, S_N };
const char* const kSrcNames[S_N] = {"store", "terrain", "glyph", "texel writes"};
int src_index(uint8_t bit) { return bit == W_STORE ? S_STORE : bit == W_TERRAIN ? S_TERRAIN : bit == W_GLYPH ? S_GLYPH : S_TEXELS; }
struct ThreadStat {
    char name[32] = {};
    uint32_t tid = 0;
    std::atomic<int64_t> n[S_N]{}, bytes[S_N]{}, ns[S_N]{};
};
thread_local ThreadStat* me_stat = nullptr;
std::mutex stats_mu;
std::vector<ThreadStat*> all_stats;
std::atomic<uint32_t> present_tid{0};
std::atomic<int64_t> f_n[S_N]{}, f_bytes[S_N]{}, t_n[S_N]{}, t_bytes[S_N]{}, t_ns[S_N]{}, on_present[S_N]{};
std::atomic<int64_t> st_loads{0}, st_inplace{0}, st_prepared{0}, st_miss_name{0}, st_miss_desc{0}, st_miss_place{0},
    st_miss_nobntx{0}, st_miss_nostore{0}, st_rebinds{0}, st_rebind_nolink{0}, st_created{0}, st_unarmed{0}, st_link_desc{0},
    st_unseen{0}, st_fills{0}, st_fill_bytes{0}, st_fill_zbytes{0}, st_fill_ns{0}, st_zstd_ns{0}, st_fill_uploads{0},
    st_read_fail{0}, st_no_tex[S_N]{}, st_skipped[S_N]{}, st_tiles_unconverted{0}, st_jobs{0};
std::atomic<int64_t> st_staging_max{0};
std::atomic<int64_t> st_kept_taken{0}, st_kept_had{0};   // (M6f: GPU_TEXELS_TERRAIN_KEPT)

ThreadStat& thread_stat() {
    if (!me_stat) {
        me_stat = new ThreadStat;
        os_thread_name(os_thread_current(), me_stat->name, sizeof me_stat->name);
        me_stat->tid = os_tid();
        std::lock_guard lk(stats_mu);
        all_stats.push_back(me_stat);
    }
    return *me_stat;
}

// ---- names of misses, each once ----
std::mutex named_mu;
std::unordered_set<std::string> named;
void name_once(const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    {
        std::lock_guard lk(named_mu);
        if (named.size() >= 400 || !named.insert(b).second) return;
    }
    logf("direct: assets: %s", b);
}

// ---- debug checks and dumps (HOS_DBG=assetcheck:<n>, assetdump) ----
struct Check {
    Image* im = nullptr;
    uint32_t rec = 0;   // (a prepared image: compared with the store)
    uint8_t source = 0;
    uint32_t level = 0, layer = 0, layers = 1;
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bytes;   // (a region upload: the bytes given)
    const uint8_t* live = nullptr;  // (a glyph atlas: its guest memory (texel 0, 0), compared whole with the image)
    uint64_t live_row = 0;
    std::string what;
};
std::mutex check_mu;
std::vector<Check> checks, dumps;
int check_n = -1;
bool dump_on = false;
int64_t checked_regions[S_N] = {};
void dbg_read() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* v = dbg_value("assetcheck");
        check_n = v ? std::max(1, atoi(v)) : 0;
        dump_on = dbg("assetdump");
    });
}

}  // namespace

// ---- the store, opened by the host ----
extern "C" __attribute__((visibility("default"))) int gpu_assets_open(const char* dir) {
    std::lock_guard lk(open_mu);
    if (S.ok) return (int)S.ntex;
    if (!dir) return 0;
    S.dir = dir;
    S.idx = map_file(S.dir + "/index.bin", S.idx_n);
    if (!S.idx || S.idx_n < 128 || std::memcmp(S.idx, "HKTXIDX1", 8) || rd<uint32_t>(S.idx + 8) != 1) {
        logf("direct: assets: no prepared texture store at %s (index.bin %s): every asset texture is the placeholder", dir,
             S.idx ? "of another version" : "not found");
        return 0;
    }
    S.nfiles = rd<uint32_t>(S.idx + 24), S.ntex = rd<uint32_t>(S.idx + 28), S.nmips = rd<uint32_t>(S.idx + 32);
    S.nframes = rd<uint32_t>(S.idx + 36), S.npacks = rd<uint32_t>(S.idx + 40), S.nstr = rd<uint32_t>(S.idx + 44);
    uint64_t o[5];
    for (int i = 0; i < 5; i++) o[i] = rd<uint64_t>(S.idx + 48 + 8 * i);
    if (rd<uint16_t>(S.idx + 88) != 64 || rd<uint16_t>(S.idx + 90) != 128 || rd<uint16_t>(S.idx + 92) != 16 ||
        rd<uint16_t>(S.idx + 94) != 32 || o[1] + (uint64_t)S.ntex * 128 > S.idx_n || o[2] + (uint64_t)S.nmips * 16 > S.idx_n ||
        o[3] + (uint64_t)S.nframes * 32 > S.idx_n || o[4] + S.nstr > S.idx_n) {
        logf("direct: assets: %s/index.bin: record sizes or offsets not this reader's", dir);
        return 0;
    }
    S.files = S.idx + o[0], S.texs = S.idx + o[1], S.mipr = S.idx + o[2], S.framer = S.idx + o[3], S.str = S.idx + o[4];
    uint64_t packed = 0;
    for (uint32_t i = 0; i < S.npacks; i++) {
        char p[32];
        std::snprintf(p, sizeof p, "/pack_%03u.bin", i);
        size_t n = 0;
        const uint8_t* m = map_file(S.dir + p, n);
        if (!m || n < 64 || std::memcmp(m, "HKTXPAK1", 8)) logf("direct: assets: %s%s not read: its textures fail", dir, p);
        S.packs.push_back(m && n >= 64 ? m : nullptr), S.pack_n.push_back(m ? n : 0);
        S.files_open.push_back(os_file_open((S.dir + p).c_str(), OS_READ, nullptr));
        packed += n;
    }
    while (S.nbucket < S.ntex * 2) S.nbucket <<= 1;
    S.bucket.assign(S.nbucket, 0), S.next.assign(S.ntex + 1, 0);
    uint32_t ok = 0;
    for (uint32_t i = 0; i < S.ntex; i++) {
        Rec r;
        rec_at(i, r);
        if (r.status || r.name_off + (uint64_t)r.name_len > S.nstr) continue;
        const uint32_t b = name_hash((const char*)S.str + r.name_off, r.name_len) & (S.nbucket - 1);
        S.next[i + 1] = S.bucket[b];
        S.bucket[b] = i + 1;
        ok++;
    }
    S.ok = true;
    logf("direct: assets: the prepared texture store at %s: %u textures (%u usable) of %u files, %u packs (%.1f GB), mapped", dir,
         S.ntex, ok, S.nfiles, S.npacks, packed / 1073741824.0);
    return (int)S.ntex;
}

namespace {
/// A BNTX header at data - off (a texture's mip 0 is at off in its file), inside guest memory
/// ... and the BRTI at index in its texture table has the name (pointers relocated by the loader, or offsets in the file)
bool bntx_at(const uint8_t* data, uint32_t off, uint32_t index, const char* name, uint32_t name_len) {
    const uint8_t* g = host->guest_base();
    if (!data || data < g + off || off < 0x20) return false;
    const uint8_t* b = data - off;
    if (std::memcmp(b, "BNTX", 4) || (rd<uint32_t>(b + 8) & 0xffffff00u) != 0x00040100u ||   // (version 4.1.x)
        index >= rd<uint32_t>(b + 0x24)) return false;
    const uint64_t base = (uint64_t)(b - g), size = host->guest_size();
    auto at = [&](uint64_t p) { return p < base ? p + base : p; };   // (guest address)
    const uint64_t tbl = at(rd<uint64_t>(b + 0x28));
    if (tbl + 8ull * (index + 1) > size) return false;
    const uint64_t brti = at(rd<uint64_t>(g + tbl + 8ull * index));
    if (brti + 0x68 > size || std::memcmp(g + brti, "BRTI", 4)) return false;
    const uint64_t nm = at(rd<uint64_t>(g + brti + 0x60));
    if (nm + 2 + name_len > size || rd<uint16_t>(g + nm) != name_len) return false;
    return !std::memcmp(g + nm + 2, name, name_len);
}
std::atomic<int64_t> st_ambiguous{0}, st_by_name{0};
}  // namespace

namespace {
std::atomic<int64_t> st_rebind_rec{0}, st_rebind_same{0}, st_rebind_changed{0}, st_rebind_kept{0};
/// A rebind whose resource is a file's BRTI with its data there: that record (the texture's data now), else the link as it was
int rebind_load(const gpu_asset_key* k) {
    uint32_t found = 0;
    if (S.ok && k->name && k->data)
        for (uint32_t e = S.bucket[name_hash(k->name, k->name_len) & (S.nbucket - 1)]; e && !found; e = S.next[e]) {
            Rec r;
            rec_at(e - 1, r);
            if (r.name_len != k->name_len || std::memcmp(S.str + r.name_off, k->name, k->name_len) || r.nvn_format != k->format ||
                r.w != k->width || r.h != k->height || r.layers != k->layers || r.mips != k->mips || r.isz != k->image_size)
                continue;
            if (bntx_at((const uint8_t*)k->data, r.doff, r.index, k->name, k->name_len)) found = e;
        }
    st_rebinds.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lk(links_mu);
    auto it = links.find(k->tex);
    if (found) {
        st_rebind_rec.fetch_add(1, std::memory_order_relaxed);
        if (it != links.end() && it->second.rec == found) st_rebind_same.fetch_add(1, std::memory_order_relaxed);
        else st_rebind_changed.fetch_add(1, std::memory_order_relaxed);
        Link l;
        l.rec = found, l.armed = true;
        links[k->tex] = l;
        return 1;
    }
    if (it == links.end()) return st_rebind_nolink.fetch_add(1, std::memory_order_relaxed), 0;
    st_rebind_kept.fetch_add(1, std::memory_order_relaxed);
    it->second.armed = true;
    return it->second.rec ? 1 : 0;
}
}  // namespace

namespace {
std::atomic<int64_t> st_same{0}, st_same_rec{0}, st_same_plain{0}, st_same_nolink{0}, st_same_desc{0};
/// A load with no data whose texels are another texture's (M6f: a UnifyTex alias; the host names its target, the head
/// of its alias chain): that texture's link, when the descriptors agree; else a plain image, counted, named and a strict
/// event (its content is undefined)
int same_load(const gpu_asset_key* k) {
    st_same.fetch_add(1, std::memory_order_relaxed);
    Link l;
    l.armed = true;
    std::lock_guard lk(links_mu);
    auto it = links.find(k->same_as);
    const bool known = it != links.end();
    const char* why = nullptr;
    char whyb[256];
    if (!known) {
        why = "no load of its target seen";
    } else if (it->second.rec) {
        const uint32_t rec = it->second.rec;
        Rec r;
        rec_at(rec - 1, r);
        if (r.nvn_format == k->format && r.w == k->width && r.h == k->height && r.layers == k->layers && r.mips == k->mips &&
            r.isz == k->image_size)
            l.rec = rec;
        else
            std::snprintf(whyb, sizeof whyb, "its record has another descriptor ('%.*s', BNTX format %#x %ux%u x%u, %u mips, %u bytes)",
                          (int)r.name_len, (const char*)S.str + r.name_off, r.nvn_format, r.w, r.h, r.layers, r.mips, r.isz),
            why = whyb;
    } else if (it->second.plain) {
        l.plain = it->second.plain;
    } else {
        why = "its target has no record either";
    }
    if (l.rec) st_same_rec.fetch_add(1, std::memory_order_relaxed), st_prepared.fetch_add(1, std::memory_order_relaxed);
    else if (l.plain) st_same_plain.fetch_add(1, std::memory_order_relaxed);
    else {
        (!known ? st_same_nolink : st_same_desc).fetch_add(1, std::memory_order_relaxed);
        l.plain = plain_ids.fetch_add(1, std::memory_order_relaxed) + 1;
        st_plain.fetch_add(1, std::memory_order_relaxed);
        name_once("'%.*s' (BNTX format %#x %ux%u x%u, %u mips, %u bytes): loaded with no data over another texture's content, but %s: a plain image",
                  k->name ? (int)k->name_len : 0, k->name ? k->name : "", k->format, k->width, k->height, k->layers, k->mips,
                  k->image_size, why);
        strict_event("a texture loaded with no data over a content not resolved (undefined content)", "'%.*s': %s",
                     k->name ? (int)k->name_len : 0, k->name ? k->name : "", why);
    }
    links[k->tex] = l;
    return l.rec ? 1 : 0;
}
}  // namespace

extern "C" __attribute__((visibility("default"))) int gpu_asset_load(const gpu_asset_key* k) {
    if (!k || !k->tex) return 0;
    if (k->path == GPU_ASSET_REBIND) return rebind_load(k);
    if (k->path == GPU_ASSET_SAME) return same_load(k);
    (k->path == GPU_ASSET_INPLACE ? st_inplace : st_loads).fetch_add(1, std::memory_order_relaxed);
    Link l;
    l.armed = true;
    if (!S.ok) {
        l.why = "no prepared store";
        st_miss_nostore.fetch_add(1, std::memory_order_relaxed);
    } else if (!k->name) {
        l.why = "its data is not from a BNTX";
        st_miss_nobntx.fetch_add(1, std::memory_order_relaxed);
        name_once("a texture load whose data is not from a BNTX: '%.*s' BNTX format %#x %ux%u x%u, %u mips, %u bytes: the placeholder",
                  k->name ? (int)k->name_len : 0, k->name ? k->name : "", k->format, k->width, k->height, k->layers, k->mips, k->image_size);
    } else {
        int why = 1;   // (1 no record of the name, 2 of the descriptor, 3 of its place in the file)
        uint64_t hash = 0;
        for (uint32_t e = S.bucket[name_hash(k->name, k->name_len) & (S.nbucket - 1)]; e; e = S.next[e]) {
            Rec r;
            rec_at(e - 1, r);
            if (r.name_len != k->name_len || std::memcmp(S.str + r.name_off, k->name, k->name_len)) continue;
            why = std::max(why, 2);
            if (r.nvn_format != k->format || r.w != k->width || r.h != k->height || r.layers != k->layers || r.mips != k->mips ||
                r.isz != k->image_size)
                continue;
            why = std::max(why, 3);
            // (a load with no data (src 0: the game fills the slot some other way, or never) is told by its name and
            // descriptor alone, when every record of them has the one content)
            const bool nodata = !k->data_offset && !k->data;
            if (!nodata && (k->data_offset ? r.index != k->index || r.doff != k->data_offset
                                           : !bntx_at((const uint8_t*)k->data, r.doff, r.index, k->name, k->name_len)))
                continue;
            if (!l.rec) {
                l.rec = e, hash = r.hash;
            } else if (r.hash != hash) {   // (two contents match: a load with data takes the first; one without none)
                st_ambiguous.fetch_add(1, std::memory_order_relaxed);
                name_once("'%.*s': records of two contents match its load%s", (int)k->name_len, k->name,
                          nodata ? " (it has no data): the placeholder" : ": the first taken");
                if (nodata) l.rec = 0, why = 4;
                break;
            }
        }
        if (l.rec && !k->data_offset && !k->data) st_by_name.fetch_add(1, std::memory_order_relaxed);
        if (l.rec) {
            st_prepared.fetch_add(1, std::memory_order_relaxed);
        } else if (!k->data_offset && !k->data && why == 1) {
            // (no data at its load and no BNTX of its name: not an asset; written by the GPU (copies, rendering), so a plain
            // image, kept across its moves; counted and named)
            l.plain = plain_ids.fetch_add(1, std::memory_order_relaxed) + 1;
            st_plain.fetch_add(1, std::memory_order_relaxed);
            name_once("'%.*s' (BNTX format %#x %ux%u x%u, %u mips): loaded with no data and in no BNTX: a plain image", (int)k->name_len,
                      k->name, k->format, k->width, k->height, k->layers, k->mips);
        } else {
            l.why = why == 1 ? "no record of its name" : why == 2 ? "no record of its descriptor"
                  : why == 3 ? "no record of its place in its file" : "no data and records of two contents";
            (why == 1 ? st_miss_name : why == 2 ? st_miss_desc : st_miss_place).fetch_add(1, std::memory_order_relaxed);
            name_once("'%.*s' (BNTX format %#x %ux%u x%u, %u mips, #%d at %#x in its BNTX, %s): %s: the placeholder", (int)k->name_len,
                      k->name, k->format, k->width, k->height, k->layers, k->mips, k->data_offset ? (int)k->index : -1, k->data_offset,
                      k->path == GPU_ASSET_INPLACE ? "in place" : "loaded", l.why);
        }
    }
    std::lock_guard lk(links_mu);
    links[k->tex] = l;
    return l.rec ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) void gpu_asset_rebind(const void* tex) {
    st_rebinds.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lk(links_mu);
    auto it = links.find(tex);
    if (it == links.end()) return void(st_rebind_nolink.fetch_add(1, std::memory_order_relaxed));
    it->second.armed = true;
}

extern "C" __attribute__((visibility("default"))) void gpu_asset_pool(uint64_t lo, uint64_t hi) {
    pool_lo.store(lo, std::memory_order_relaxed);
    pool_hi.store(hi, std::memory_order_release);
    logf("direct: assets: the asset heap's pool %#llx + %llu MB: a texture there with no load seen is the placeholder",
         (unsigned long long)lo, (unsigned long long)((hi - lo) >> 20));
}

std::mutex tdp_mu;   // (M4f: the texture data pools named at their creation; a few)
std::vector<std::pair<uint64_t, uint64_t>> tdp;
bool assets_pool_is(uint64_t gpu, uint64_t size) {
    std::lock_guard lk(tdp_mu);
    for (const auto& [lo, hi] : tdp)
        if (lo == gpu && hi == gpu + size) return true;
    return false;
}
extern "C" __attribute__((visibility("default"))) void gpu_texture_data_pool(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(tdp_mu);
    for (auto& p : tdp)
        if (p.first == lo) return void(p.second = hi);
    tdp.push_back({lo, hi});
}

extern "C" __attribute__((visibility("default"))) void gpu_texels(uint32_t source, const void* tex, unsigned level, unsigned layer0,
                                                                  const void* region, const void* src, uint64_t row, uint64_t img) {
    const nvn_tex* t = (const nvn_tex*)tex;
    const NVNcopyRegion* r = (const NVNcopyRegion*)region;
    const bool kept = source == GPU_TEXELS_TERRAIN_KEPT;
    const int si = source == GPU_TEXELS_TERRAIN || kept ? S_TERRAIN : source == GPU_TEXELS_GLYPH ? S_GLYPH : S_TEXELS;
    if (!t || !r) return;
    if (kept) {   // (M6f: the storage's bytes for a texture made there again: only into an image that holds nothing else)
        Image* im = image_find(t);
        if (!im || im->placeholder || im->written.load(std::memory_order_relaxed)) return void(st_kept_had.fetch_add(1, std::memory_order_relaxed));
        st_kept_taken.fetch_add(1, std::memory_order_relaxed);
    }
    if (!src) {   // (a tile file that was not converted: its image is drawn as the placeholder)
        if (Image* im = image_find(t); im && !im->placeholder) im->no_content = true;
        st_tiles_unconverted.fetch_add(1, std::memory_order_relaxed);
        name_once("a %s not converted (NVN format %#x %ux%u x%u at %#llx): the placeholder",
                  si == S_TERRAIN ? "terrain tile" : "prepared file's texture", t->d.format, t->d.width,
                  t->d.height, t->d.depth, (unsigned long long)t->gpu);
        return;
    }
    if (!image_upload_region(t, level, layer0, r, (const uint8_t*)src, row, img,
                            si == S_TERRAIN ? W_TERRAIN : si == S_GLYPH ? W_GLYPH : W_TEXELS)) {
        st_skipped[si].fetch_add(1, std::memory_order_relaxed);
        name_once("a %s upload skipped (no image or outside it): NVN format %#x %ux%u, region %d,%d %dx%d x%d", kSrcNames[si],
                  t->d.format, t->d.width, t->d.height, r->xoffset, r->yoffset, r->width, r->height, r->depth);
        return;
    }
    if (Image* im = image_find(t); im) im->no_content = false;
}

// ---- the image side ----
bool assets_created(const nvn_tex* t, AssetRef& a) {
    if (!t || !t->serial) return false;
    Link l;
    bool found = false;
    {
        std::lock_guard lk(links_mu);
        auto it = links.find((const void*)t);
        if (it != links.end()) {
            found = true;
            if (!it->second.armed) st_unarmed.fetch_add(1, std::memory_order_relaxed);
            it->second.armed = false;
            l = it->second;
        }
    }
    if (!found) {
        const uint64_t hi = pool_hi.load(std::memory_order_acquire);
        if (!hi || t->gpu < pool_lo.load(std::memory_order_relaxed) || t->gpu >= hi) return false;
        l.why = "in the asset heap with no load seen";
        st_unseen.fetch_add(1, std::memory_order_relaxed);
        name_once("a texture in the asset heap with no load seen: NVN format %#x %ux%ux%u, %u levels, at %#llx: the placeholder",
                  t->d.format, t->d.width, t->d.height, t->d.depth, t->d.levels, (unsigned long long)t->gpu);
    }
    st_created.fetch_add(1, std::memory_order_relaxed);
    a.rec = l.rec, a.why = l.why, a.plain = l.plain;
    if (a.rec) {
        Rec r;
        rec_at(a.rec - 1, r);
        const uint32_t layers = t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY || t->d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY ? t->d.depth
                              : t->d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : 1;
        if (t->d.width != r.w || t->d.height != r.h || std::max(1u, t->d.levels) != r.mips || layers != r.layers || t->L.size != r.isz) {
            st_link_desc.fetch_add(1, std::memory_order_relaxed);
            name_once("'%.*s': the texture initialized (%ux%u x%u, %u levels, %llu bytes) is not its record's (%ux%u x%u, %u, %u): "
                      "the placeholder", (int)r.name_len, (const char*)S.str + r.name_off, t->d.width, t->d.height, layers,
                      t->d.levels, (unsigned long long)t->L.size, r.w, r.h, r.layers, r.mips, r.isz);
            a.rec = 0, a.why = "initialized with another descriptor than its record's";
        }
    }
    std::unique_lock lk(serial_mu);
    by_serial[t->serial] = a;
    return true;
}

bool assets_of(const nvn_tex* t, AssetRef& a) {
    if (!t || !t->serial) return false;
    std::shared_lock lk(serial_mu);
    if (by_serial.empty()) return false;
    auto it = by_serial.find(t->serial);
    if (it == by_serial.end()) return false;
    a = it->second;
    return true;
}

void assets_released(const nvn_tex* t) {
    if (!t || !t->serial) return;
    std::unique_lock lk(serial_mu);
    by_serial.erase(t->serial);
}

bool assets_desc(uint32_t rec, AssetDesc& d) {
    if (!S.ok || !rec || rec > S.ntex) return false;
    Rec r;
    rec_at(rec - 1, r);
    if (!block_of(r, d.bpb, d.bw, d.bh) || !r.vk_format || !r.mips) return false;
    d.key = r.hash ? r.hash : 0xA55E700000000000ull | rec;
    d.format = (VkFormat)r.vk_format;
    d.view = (VkImageViewType)r.view;
    d.width = std::max(1u, r.w), d.height = std::max(1u, r.h), d.layers = std::max(1u, r.layers), d.mips = r.mips;
    std::memcpy(d.map, r.map, 4);
    return true;
}

const char* assets_name(uint32_t rec, uint32_t* len) {
    if (!S.ok || !rec || rec > S.ntex) return *len = 0, "";
    Rec r;
    rec_at(rec - 1, r);
    *len = r.name_len;
    return (const char*)S.str + r.name_off;
}

namespace {
thread_local ZSTD_DCtx* tl_dctx = nullptr;
thread_local std::vector<uint8_t> tl_scratch, tl_z;
constexpr uint64_t kGroup = 16ull << 20;   // (an upload holds whole mips up to this; a bigger mip is one upload)

/// The record's frames that overlap [a, b) of its raw data, into dst (= raw a); false: a pack not readable or a bad frame
bool read_range(const Rec& r, uint64_t a, uint64_t b, uint8_t* dst, int64_t& zbytes) {
    if (!tl_dctx && !(tl_dctx = ZSTD_createDCtx())) return false;
    for (uint32_t i = 0; i < r.fr_n; i++) {
        Frame f;
        frame_at(r.fr_first + i, f);
        const uint64_t fa = f.raw_off, fb = f.raw_off + f.raw_size;
        if (fb <= a || fa >= b) continue;
        if (f.pack >= S.packs.size() || !S.packs[f.pack] || f.off + f.csize > S.pack_n[f.pack] || fb > r.raw) return false;
        if (tl_z.size() < f.csize) {
            const size_t was = tl_z.capacity();
            tl_z.resize(std::max<size_t>(f.csize, 1u << 20));
            mem_account_main(MO_M_SCRATCH, (int64_t)tl_z.capacity() - (int64_t)was);
        }
        os_file* const pack = S.files_open[f.pack];
        if (!pack || os_file_read_at(pack, tl_z.data(), f.csize, f.off) != (int64_t)f.csize) return false;
        const uint8_t* z = tl_z.data();
        zbytes += f.csize;
        if (fa >= a && fb <= b) {
            const size_t n = ZSTD_decompressDCtx(tl_dctx, dst + (fa - a), f.raw_size, z, f.csize);
            if (ZSTD_isError(n) || n != f.raw_size) return false;
        } else {   // (a frame across the upload's edge: through host memory)
            if (tl_scratch.size() < f.raw_size) {
                const size_t was = tl_scratch.capacity();
                tl_scratch.resize(f.raw_size);
                mem_account_main(MO_M_SCRATCH, (int64_t)tl_scratch.capacity() - (int64_t)was);
            }
            const size_t n = ZSTD_decompressDCtx(tl_dctx, tl_scratch.data(), f.raw_size, z, f.csize);
            if (ZSTD_isError(n) || n != f.raw_size) return false;
            const uint64_t lo = std::max(a, fa), hi = std::min(b, fb);
            std::memcpy(dst + (lo - a), tl_scratch.data() + (lo - fa), hi - lo);
        }
    }
    return true;
}

void hold_for_checks(Image& im, uint32_t rec) {
    dbg_read();
    if (!check_n && !dump_on) return;
    std::lock_guard lk(check_mu);
    static std::mt19937_64 rng(12345);
    uint32_t nl = 0;
    const char* nm = assets_name(rec, &nl);
    const std::string name(nm, nl);
    const bool nrm = name.size() > 4 && name.compare(name.size() - 4, 4, "_Nrm") == 0;
    if (check_n && (checks.size() < (size_t)check_n * 4 ? rng() % 8 == 0 : false)) {
        Check c;
        c.im = &im, c.rec = rec, c.what = name;
        image_hold(&im);
        checks.push_back(std::move(c));
    }
    if (dump_on) {   // (normal maps, colour BC7 ones and UI textures, up to two each)
        static int n_nrm = 0, n_bc7 = 0, n_ui = 0;
        Rec r;
        rec_at(rec - 1, r);
        const uint8_t* fp = S.files + (size_t)r.file * 64;
        const bool ui = rd<uint32_t>(fp + 4) >= 3 && !std::memcmp(S.str + rd<uint32_t>(fp), "UI/", 3) && im.width >= 128;
        const bool bc7 = !ui && im.format == VK_FORMAT_BC7_SRGB_BLOCK && im.width >= 256;
        if ((nrm && n_nrm < 2) || (bc7 && n_bc7 < 2) || (ui && n_ui < 2)) {
            (nrm ? n_nrm : ui ? n_ui : n_bc7)++;
            Check c;
            c.im = &im, c.rec = rec, c.what = (nrm ? "normal_" : ui ? "ui_" : "colour_") + name;
            image_hold(&im);
            dumps.push_back(std::move(c));
        }
    }
}
}  // namespace

bool assets_fill(Image& im, uint32_t rec) {
    const int64_t t0 = host->now_ns();
    Rec r;
    rec_at(rec - 1, r);
    int64_t zbytes = 0, zns = 0, uploads = 0;
    bool ok = true, first = true;
    for (uint32_t m0 = 0; m0 < r.mips && ok;) {
        // (whole mips into one upload, up to kGroup bytes; the mips' raw ranges are in order)
        uint64_t a = 0, la = 0;
        uint32_t lb = 0;
        mip_at(r, m0, a, lb);
        uint32_t m1 = m0;
        uint64_t b = a;
        while (m1 < r.mips) {
            uint64_t o;
            mip_at(r, m1, o, lb);
            const uint64_t e = o + (uint64_t)lb * r.layers;
            if (m1 > m0 && e - a > kGroup) break;
            b = e, m1++;
        }
        (void)la;
        if (b > r.raw || b <= a) {
            ok = false;
            break;
        }
        Upload u = upload_begin(b - a);
        if (!u.map) {
            upload_end(u);
            ok = false;
            break;
        }
        for (int64_t m = st_staging_max.load(); (int64_t)(b - a) > m && !st_staging_max.compare_exchange_weak(m, (int64_t)(b - a));) {}
        const int64_t z0 = host->now_ns();
        ok = read_range(r, a, b, u.map, zbytes);
        zns += host->now_ns() - z0;
        if (first) {   // (a new image: undefined to GENERAL before its first copy)
            VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            ib.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            ib.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = im.image;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            d.imageMemoryBarrierCount = 1;
            d.pImageMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(u.cb, &d);
            first = false;
        }
        if (ok) {
            VkBufferImageCopy c[16];
            uint32_t n = 0;
            for (uint32_t m = m0; m < m1; m++) {
                uint64_t o;
                mip_at(r, m, o, lb);
                VkBufferImageCopy& x = c[n++];
                x = VkBufferImageCopy{};
                x.bufferOffset = o - a;
                x.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, im.layers};
                x.imageExtent = {std::max(1u, im.width >> m), std::max(1u, im.height >> m), 1};
                if (n == 16 || m + 1 == m1) {
                    vkCmdCopyBufferToImage(u.cb, u.staging, im.image, VK_IMAGE_LAYOUT_GENERAL, n, c);
                    n = 0;
                }
            }
        }
        upload_end(u);
        uploads++;
        m0 = m1;
    }
    const int64_t dt = host->now_ns() - t0;
    if (!ok) {
        st_read_fail.fetch_add(1, std::memory_order_relaxed);
        name_once("'%.*s': its frames could not be read from the store: an empty image", (int)r.name_len, (const char*)S.str + r.name_off);
        return false;
    }
    st_fills.fetch_add(1, std::memory_order_relaxed);
    st_fill_bytes.fetch_add((int64_t)r.raw, std::memory_order_relaxed);
    st_fill_zbytes.fetch_add(zbytes, std::memory_order_relaxed);
    st_fill_ns.fetch_add(dt, std::memory_order_relaxed);
    st_zstd_ns.fetch_add(zns, std::memory_order_relaxed);
    st_fill_uploads.fetch_add(uploads, std::memory_order_relaxed);
    assets_note_upload(W_STORE, r.raw, dt);
    hold_for_checks(im, rec);
    return true;
}

void assets_note_upload(uint8_t source, uint64_t bytes, int64_t ns) {
    const int i = src_index(source);
    ThreadStat& s = thread_stat();
    s.n[i].fetch_add(1, std::memory_order_relaxed);
    s.bytes[i].fetch_add((int64_t)bytes, std::memory_order_relaxed);
    s.ns[i].fetch_add(ns, std::memory_order_relaxed);
    f_n[i].fetch_add(1, std::memory_order_relaxed);
    f_bytes[i].fetch_add((int64_t)bytes, std::memory_order_relaxed);
    t_n[i].fetch_add(1, std::memory_order_relaxed);
    t_bytes[i].fetch_add((int64_t)bytes, std::memory_order_relaxed);
    t_ns[i].fetch_add(ns, std::memory_order_relaxed);
    if (s.tid && s.tid == present_tid.load(std::memory_order_relaxed)) on_present[i].fetch_add(1, std::memory_order_relaxed);
}

void assets_note_placeholder(const nvn_tex* t, const char* why) {
    (void)t;
    (void)why;   // (named where the miss was found: the load, or the initialization)
}

void assets_job_list() { st_jobs.fetch_add(1, std::memory_order_relaxed); }

// ---- region checks (image_upload_region's bytes, sampled) ----
void assets_check_region(Image* im, uint8_t source, uint32_t level, uint32_t layer, uint32_t layers, int32_t x, int32_t y, uint32_t w,
                         uint32_t h, const uint8_t* bytes, uint64_t n, const uint8_t* src, uint64_t row) {
    dbg_read();
    const int si = src_index(source);
    if ((!check_n && !dump_on) || si == S_STORE || si == S_TEXELS || n > (8u << 20)) return;
    std::lock_guard lk(check_mu);
    static int seen[S_N] = {}, held_dump[S_N] = {};
    const int k = seen[si]++;
    if (check_n && (k < 16 || k % 64 == 0) && checked_regions[si] < check_n) {
        Check c;
        c.im = im, c.source = source, c.level = level, c.layer = layer, c.layers = layers, c.x = x, c.y = y, c.w = w, c.h = h;
        if (si == S_GLYPH && im->bpb == 1 && im->bw == 1) c.live = src - (uint64_t)y * row - x, c.live_row = row;
        else c.bytes.assign(bytes, bytes + n);
        c.what = kSrcNames[si];
        image_hold(im);
        checks.push_back(std::move(c));
        checked_regions[si]++;
    }
    if (dump_on && held_dump[si] < (si == S_TERRAIN ? 3 : 1)) {   // (terrain tiles, the first font atlas: dumped after the load)
        held_dump[si]++;
        Check c;
        c.im = im, c.source = source, c.layer = layer;
        c.what = si == S_TERRAIN ? "terrain_tile" : "font_atlas";
        image_hold(im);
        dumps.push_back(std::move(c));
    }
}

namespace {

/// One readback of the image's level / layers / region into host memory (the calling thread's upload, waited for)
bool readback(Image* im, uint32_t level, uint32_t layer, uint32_t layers, int32_t x, int32_t y, uint32_t w, uint32_t h, uint64_t bytes,
              std::vector<uint8_t>& out) {
    VkBuffer buf = VK_NULL_HANDLE;
    Mem* mem = nullptr;
    void* map = nullptr;
    if (!mem_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, buf, mem, &map) || !map) return false;
    Upload u = upload_begin(0);
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(u.cb, &d);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, layers};
    c.imageOffset = {x, y, 0};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(u.cb, im->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    const uint64_t v = upload_end(u);
    const bool ok = v && sync_wait_value(v, 10ull * 1000000000ull);
    if (ok) {
        mem_invalidate(mem);
        out.assign((const uint8_t*)map, (const uint8_t*)map + bytes);
    }
    mem_buffer_free(buf, mem);
    return ok;
}

void run_checks() {
    std::vector<Check> todo;
    {
        std::lock_guard lk(check_mu);
        todo.swap(checks);
    }
    int64_t n[S_N] = {}, good[S_N] = {}, bytes = 0;
    std::mt19937_64 rng(777);
    std::shuffle(todo.begin(), todo.end(), rng);
    int prepared = 0;
    for (Check& c : todo) {
        const int si = c.rec ? S_STORE : src_index(c.source);
        if (c.rec && prepared >= check_n) {
            image_unhold(c.im);
            continue;
        }
        bool ok = true;
        if (c.rec) {
            prepared++;
            Rec r;
            rec_at(c.rec - 1, r);
            std::vector<uint8_t> want(r.raw), got;
            int64_t z = 0;
            ok = read_range(r, 0, r.raw, want.data(), z);
            for (uint32_t m = 0; ok && m < r.mips; m++) {
                uint64_t o;
                uint32_t lb;
                mip_at(r, m, o, lb);
                const uint64_t mb = (uint64_t)lb * r.layers;
                ok = readback(c.im, m, 0, c.im->layers, 0, 0, std::max(1u, c.im->width >> m), std::max(1u, c.im->height >> m), mb, got) &&
                     !std::memcmp(got.data(), want.data() + o, mb);
                if (!ok) logf("direct: assetcheck: '%s' (vk format %d %ux%u x%u, %u mips) mip %u differs from the store", c.what.c_str(),
                              (int)c.im->format, c.im->width, c.im->height, c.im->layers, r.mips, m);
                bytes += (int64_t)mb;
            }
        } else if (c.live) {   // (the whole atlas against its guest memory now: the CPU's copy is the truth)
            std::vector<uint8_t> got;
            const uint32_t w = c.im->width, h = c.im->height;
            ok = readback(c.im, 0, 0, 1, 0, 0, w, h, (uint64_t)w * h, got);
            int64_t bad = 0;
            for (uint32_t y = 0; ok && y < h; y++)
                for (uint32_t x = 0; x < w; x++) bad += got[(size_t)y * w + x] != c.live[y * c.live_row + x];
            if (bad) logf("direct: assetcheck: glyph atlas image #%u (%ux%u): %lld texels differ from its guest memory", c.im->id, w, h,
                          (long long)bad);
            ok = ok && !bad;
            bytes += (int64_t)w * h;
        } else {
            std::vector<uint8_t> got;
            ok = readback(c.im, c.level, c.layer, c.layers, c.x, c.y, c.w, c.h, c.bytes.size(), got) && got == c.bytes;
            if (!ok) logf("direct: assetcheck: a %s upload (image #%u, %d,%d %ux%u x%u) differs from the bytes given (or was written "
                          "again since)", c.what.c_str(), c.im->id, c.x, c.y, c.w, c.h, c.layers);
            bytes += (int64_t)c.bytes.size();
        }
        n[si]++, good[si] += ok;
        image_unhold(c.im);
    }
    logf("direct: assetcheck: prepared images read back %lld, equal to the store's bytes %lld; terrain uploads %lld, equal %lld; glyph "
         "uploads %lld, equal %lld (%.1f MB compared)", (long long)n[S_STORE], (long long)good[S_STORE], (long long)n[S_TERRAIN],
         (long long)good[S_TERRAIN], (long long)n[S_GLYPH], (long long)good[S_GLYPH], bytes / 1048576.0);
}

void run_dumps() {
    std::vector<Check> todo;
    {
        std::lock_guard lk(check_mu);
        todo.swap(dumps);
    }
    const char* dd = dbg_value("dumpdir");
    const std::string dir = std::string(dd ? dd : ".") + "/assets";
    os_mkdir(dir.c_str());
    int written = 0;
    for (Check& c : todo) {
        Image* im = c.im;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, im->format, &fp);
        const uint32_t sw = im->width, sh = im->height;
        const uint32_t scale = std::max(1u, (std::max(sw, sh) + 1023) / 1024);
        const uint32_t w = std::max(1u, sw / scale), h = std::max(1u, sh / scale);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
            logf("direct: assetdump: %s (vk format %d) cannot be blitted: not written", c.what.c_str(), (int)im->format);
            image_unhold(im);
            continue;
        }
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R8G8B8A8_UNORM, ci.extent = {w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkImage tmp = VK_NULL_HANDLE;
        Mem* tmem = nullptr;
        VkBuffer buf = VK_NULL_HANDLE;
        Mem* bmem = nullptr;
        void* map = nullptr;
        if (!mem_image(ci, tmp, tmem) || !mem_buffer((uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, buf, bmem, &map)) {
            image_unhold(im);
            continue;
        }
        Upload u = upload_begin(0);
        VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ib.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        ib.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = tmp;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1;
        d.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(u.cb, &d);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c.layer, 1};
        bl.srcOffsets[1] = {(int32_t)sw, (int32_t)sh, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
        vkCmdBlitImage(u.cb, im->image, VK_IMAGE_LAYOUT_GENERAL, tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d2.memoryBarrierCount = 1;
        d2.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(u.cb, &d2);
        const VkBufferImageCopy cp{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(u.cb, tmp, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &cp);
        const uint64_t v = upload_end(u);
        if (v && sync_wait_value(v, 10ull * 1000000000ull)) {
            mem_invalidate(bmem);
            std::vector<uint8_t> px((const uint8_t*)map, (const uint8_t*)map + (size_t)w * h * 4);
            const bool one = im->format == VK_FORMAT_R8_UNORM || im->format == VK_FORMAT_R8_SRGB || im->format == VK_FORMAT_BC4_UNORM_BLOCK ||
                             im->format == VK_FORMAT_R32_SFLOAT;
            if (one)
                for (size_t i = 0; i < px.size(); i += 4) px[i + 1] = px[i + 2] = px[i];   // (one channel: grey)
            char path[1024];
            std::snprintf(path, sizeof path, "%s/%02d_%s_%ux%u_vk%d_l%u.png", dir.c_str(), written, c.what.c_str(), sw, sh,
                          (int)im->format, c.layer);
            for (char* p = path + dir.size() + 1; *p; p++)
                if (*p == '/' || *p == ':' || *p == ' ') *p = '_';
            if (dump_write_png(path, px.data(), w, h)) written++;
        }
        mem_buffer_free(buf, bmem);
        mem_image_free(tmp, tmem);
        image_unhold(im);
    }
    logf("direct: assetdump: %d images written to %s", written, dir.c_str());
}

int64_t load_t0 = 0;
uint64_t first_play = 0;
bool checks_started = false;

}  // namespace

void assets_present(uint64_t present) {
    if (present == 1) {
        present_tid.store(os_tid(), std::memory_order_relaxed);
        load_t0 = host->now_ns();
    }
    const int64_t jobs = st_jobs.exchange(0, std::memory_order_relaxed);
    if (!first_play && present > 1 && jobs >= 50) {   // (the scene's render jobs: the first frame in play)
        first_play = present;
        char b[2048];
        int k = std::snprintf(b, sizeof b, "direct: assets: the first frame in play at present %llu, %.2f s after present 1; at the load: ",
                              (unsigned long long)present, (host->now_ns() - load_t0) / 1e9);
        for (int i = 0; i < S_N; i++)
            k += std::snprintf(b + k, k < (int)sizeof b ? sizeof b - k : 0, "%s %lld uploads (%.1f MB, %.0f ms)%s", kSrcNames[i],
                               (long long)t_n[i].load(), t_bytes[i].load() / 1048576.0, t_ns[i].load() / 1e6, i + 1 < S_N ? ", " : "");
        logf("%s", b);
    }
    dbg_read();
    if ((check_n || dump_on) && first_play && !checks_started && present >= first_play + 600) {
        checks_started = true;
        os_thread_release(os_thread_start([](void*) {
            if (dump_on) run_dumps();
            if (check_n) run_checks();
        }, nullptr, 0, "direct checks"));
    }
}

void assets_stats(char* out, size_t cap, double frames) {
    int k = std::snprintf(out, cap, "assets: loads %lld, in place %lld: prepared %lld (by name and descriptor alone, no data: %lld), no data and no BNTX (plain images) %lld; placeholders: no record of the name %lld, of the "
                          "descriptor %lld, of the place %lld (ambiguous %lld), not from a BNTX %lld, no store %lld, in the heap unseen %lld, descriptor "
                          "at initialization %lld; initializations %lld (not armed %lld), rebinds %lld (a file's record %lld: the same %lld, another %lld; the link kept %lld; no link %lld); fills %lld (%.0f MB "
                          "from %.0f MB of frames, %lld uploads, staging peak %.1f MB; %.0f ms, of it zstd %.0f ms), failed %lld; "
                          "uploads a frame:",
                          (long long)st_loads.load(), (long long)st_inplace.load(), (long long)st_prepared.load(), (long long)st_by_name.load(), (long long)st_plain.load(),
                          (long long)st_miss_name.load(), (long long)st_miss_desc.load(), (long long)st_miss_place.load(), (long long)st_ambiguous.load(),
                          (long long)st_miss_nobntx.load(), (long long)st_miss_nostore.load(), (long long)st_unseen.load(),
                          (long long)st_link_desc.load(), (long long)st_created.load(), (long long)st_unarmed.load(),
                          (long long)st_rebinds.load(), (long long)st_rebind_rec.load(), (long long)st_rebind_same.load(),
                          (long long)st_rebind_changed.load(), (long long)st_rebind_kept.load(), (long long)st_rebind_nolink.load(), (long long)st_fills.load(),
                          st_fill_bytes.load() / 1048576.0, st_fill_zbytes.load() / 1048576.0, (long long)st_fill_uploads.load(),
                          st_staging_max.load() / 1048576.0, st_fill_ns.load() / 1e6, st_zstd_ns.load() / 1e6, (long long)st_read_fail.load());
    for (int i = 0; i < S_N; i++)
        k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %s %.2f (%.1f KB)", kSrcNames[i], f_n[i].exchange(0) / frames,
                           f_bytes[i].exchange(0) / 1024.0 / frames);
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; loads with no data over another texture's content %lld: its record %lld, its "
                       "plain image %lld, no load of its target %lld, another descriptor %lld", (long long)st_same.load(), (long long)st_same_rec.load(),
                       (long long)st_same_plain.load(), (long long)st_same_nolink.load(), (long long)st_same_desc.load());
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; on the Presentation Thread:");
    for (int i = 0; i < S_N; i++) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %s %lld", kSrcNames[i], (long long)on_present[i].load());
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; terrain tiles not converted %lld, uploads skipped terrain %lld glyph %lld; "
                       "a tile's bytes kept for a texture made there again: taken %lld, its image had content %lld; "
                       "by thread (uploads, MB, ms):", (long long)st_tiles_unconverted.load(), (long long)st_skipped[S_TERRAIN].load(),
                       (long long)st_skipped[S_GLYPH].load(), (long long)st_kept_taken.load(), (long long)st_kept_had.load());
    std::lock_guard lk(stats_mu);
    for (ThreadStat* s : all_stats) {
        int64_t n = 0, b = 0, ns = 0;
        for (int i = 0; i < S_N; i++) n += s->n[i].load(), b += s->bytes[i].load(), ns += s->ns[i].load();
        if (k < (int)cap - 120)
            k += std::snprintf(out + k, cap - k, " [%s%s] %lld %.1f %.0f;", s->name, s->tid == present_tid.load() ? " = Presentation" : "",
                               (long long)n, b / 1048576.0, ns / 1e6);
    }
}

}  // namespace hsd
