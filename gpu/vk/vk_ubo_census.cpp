// (native-vk) The uniform buffer census, RX_VK_UBO_CENSUS=<file> (work/assets/native/uniform-buffers.md): for every constant
// buffer a draw's or dispatch's descriptors read, one binary record of where it came from and what was in it as the replay
// read it. host/nvn_native.c appends a trailer to each uniform buffer bind when the switch is on (nvn_p_ubo_census: the
// guest binding site, an id into <file>.sites, and a hash of the range as it was when the bind was recorded); the replay
// keeps the trailer with the bind (ubo_census_bind) and build_descriptors notes each buffer it resolved (ubo_census_note).
// RX_VK_UBO_CENSUS_FRAMES=<from>:<to> limits it to those presents. Off: nothing is read or written.
// The records are analysed by work/proto/uniform-buffers/ubo_census.py (game-derived: they stay on the PC).
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "vk_draw.h"
#include "vk_shader.h"
#include "vk_ubo_census.h"

namespace mkwvk {
extern int64_t presents_now;   // (vk_replay.cpp)

namespace {

struct UboCensusRec {   // (80 bytes, little-endian; the analysis script's layout)
    uint32_t frame, seq, site, size, full;
    uint8_t stage, slot, kind, flags;   // kind 0 a user buffer (c3..c18), 1 c1, 2 bank c0, 3 bank c2
    uint32_t pad;
    uint64_t prog, addr, hash, hash_full;
    uint8_t head[16];
};
static_assert(sizeof(UboCensusRec) == 80);

struct Site {
    uint32_t site = 0;
    uint64_t rec_hash = 0;
    bool known = false;
};
struct Sites {
    Site s[kNvnStages][16];
};

std::mutex census_mu;
FILE* census_file = nullptr;
FILE* census_bytes = nullptr;   // (RX_VK_UBO_CENSUS_BYTES=1: <file>.bytes, the whole range of each key's first read a frame)
std::unordered_map<uint64_t, int64_t> census_bytes_seen;   // (key -> the frame it was written last)
std::unordered_map<const NvnState*, Sites> census_sites;
int64_t census_from = 0, census_to = INT64_MAX;
uint32_t census_seq = 0;
int64_t census_frame = -1;

uint64_t census_hash(const uint8_t* p, uint64_t n) {   // (host/nvn_native.c's ubo_hash)
    uint64_t h = 0xcbf29ce484222325ull;
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h ^ n;
}

bool census_window() {
    return presents_now >= census_from && presents_now < census_to;
}

} // namespace

bool ubo_census_on() {
    static const bool on = [] {
        const char* f = getenv("RX_VK_UBO_CENSUS");
        if (!f || !*f) return false;
        census_file = fopen(f, "wb");
        if (!census_file) return false;
        static char buf[1 << 20];
        setvbuf(census_file, buf, _IOFBF, sizeof buf);
        if (const char* w = getenv("RX_VK_UBO_CENSUS_FRAMES")) {
            long long a = 0, b = 0;
            if (sscanf(w, "%lld:%lld", &a, &b) == 2) census_from = a, census_to = b;
        }
        if (const char* b = getenv("RX_VK_UBO_CENSUS_BYTES"); b && *b == '1') {
            std::string bp = std::string(f) + ".bytes";
            census_bytes = fopen(bp.c_str(), "wb");
        }
        logf("vk: uniform buffer census to %s, presents %lld..%lld", f, (long long)census_from, (long long)census_to);
        return true;
    }();
    return on;
}

void ubo_census_bind(const NvnState* s, int stage, int index, const uint8_t* payload, uint32_t avail) {
    if (stage < 0 || stage >= kNvnStages || index < 0 || index >= 16) return;
    Site site;
    if (avail >= sizeof(nvn_p_bind_buf) + sizeof(nvn_p_ubo_census)) {
        nvn_p_ubo_census t;
        std::memcpy(&t, payload + sizeof(nvn_p_bind_buf), sizeof t);
        if (t.magic == NVN_UBO_CENSUS_MAGIC) site.site = t.site, site.rec_hash = t.rec_hash, site.known = true;
    }
    std::lock_guard lk(census_mu);
    census_sites[s].s[stage][index] = site;
}

void ubo_census_begin() {
    std::lock_guard lk(census_mu);
    if (census_frame != presents_now) {   // (a new frame: what the last one wrote goes out, the run may stop any time)
        if (census_frame >= census_from && census_frame < census_to) {
            fflush(census_file);
            if (census_bytes) fflush(census_bytes);
        }
        census_frame = presents_now, census_seq = 0;
    }
    census_seq++;
}

void ubo_census_note(const NvnState& s, const Program& p, const Stage& st, uint32_t index, const VkDescriptorBufferInfo& bi) {
    if (!census_window() || !bi.buffer) return;
    UboCensusRec r{};
    r.frame = (uint32_t)presents_now;
    r.stage = (uint8_t)st.nvn_stage, r.slot = (uint8_t)index;
    r.prog = p.key;
    r.size = (uint32_t)bi.range;
    if (p.compute) r.flags |= 8;
    static thread_local std::vector<uint8_t> tmp;
    const int ns = std::clamp(st.nvn_stage, 0, kNvnStages - 1);
    if (index == 0 || index == 2) {
        r.kind = index == 0 ? 2 : 3;
        tmp.assign(bi.range, 0);
        driver_bank_fill(s, (int)index, st.nvn_stage, tmp.data(), (uint32_t)bi.range);
        r.hash = census_hash(tmp.data(), tmp.size());
    } else if (index == 1) {
        r.kind = 1;
        r.hash = census_hash(st.c1.data(), st.c1.size());
        r.full = (uint32_t)st.c1.size();
        tmp.assign(st.c1.begin(), st.c1.end());
    } else if (index >= 3 && index < 3 + 16) {
        const nvn_p_bind_buf& u = s.ubo[ns][index - 3];
        r.addr = u.addr;
        r.full = (uint32_t)u.size;
        const uint64_t full = std::min<uint64_t>(std::max<uint64_t>(u.size, bi.range), 0x10000);
        tmp.assign(full, 0);
        if (!ub_read(u.addr, tmp.data(), full)) r.flags |= 16;   // (not readable)
        r.hash = census_hash(tmp.data(), std::min<uint64_t>(bi.range, full));
        r.hash_full = census_hash(tmp.data(), std::min<uint64_t>(u.size, 0x10000));
    } else {
        return;
    }
    std::memcpy(r.head, tmp.data(), std::min<size_t>(sizeof r.head, tmp.size()));
    std::lock_guard lk(census_mu);
    r.seq = census_seq;
    if (index >= 3) {
        auto it = census_sites.find(&s);
        if (it != census_sites.end()) {
            const Site& site = it->second.s[ns][index - 3];
            if (site.known) {
                r.site = site.site;
                r.flags |= 4;
                if (site.rec_hash == r.hash_full) r.flags |= 1;   // (as it was when the bind was recorded)
            }
        }
    }
    fwrite(&r, sizeof r, 1, census_file);
    if (census_bytes) {   // (frame, site, stage, slot, kind, size; then the bytes the shader reads, at most 16 KB)
        const uint64_t key = (uint64_t)r.site << 24 | (uint64_t)r.stage << 16 | (uint64_t)r.slot << 8 | r.kind;
        int64_t& last = census_bytes_seen[key ^ (r.kind == 1 ? r.prog : 0)];
        if (last != presents_now + 1) {
            last = presents_now + 1;
            const uint32_t n = std::min<uint32_t>(r.size, std::min<uint32_t>((uint32_t)tmp.size(), 0x4000));
            const uint32_t hdr[6] = {r.frame, r.site, r.stage, r.slot, r.kind, n};
            fwrite(hdr, sizeof hdr, 1, census_bytes);
            fwrite(tmp.data(), 1, n, census_bytes);
        }
    }
}

} // namespace mkwvk
