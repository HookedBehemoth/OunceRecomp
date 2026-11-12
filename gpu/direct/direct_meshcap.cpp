// HOS_DBG=meshcap (direct_meshcap.h).
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

#include "direct_internal.h"
#include "direct_meshcap.h"
#include "direct_shader.h"

namespace hsd {
namespace {

int64_t at() {
    static const int64_t v = [] {
        const char* s = dbg_value("meshcap");
        return s ? atoll(s) : -1;
    }();
    return v;
}

std::mutex mu;
std::unordered_map<uint64_t, std::vector<uint8_t>> programs;   // (key -> its MC_PROGRAM payload)
struct Cmd {
    bool mesh;
    MeshCapDraw draw;
    mcap::McOther other;
};
std::vector<Cmd> cmds;
std::unordered_map<uint64_t, uint32_t> cb_count;
uint32_t seq = 0;
bool written = false;

template <class T> void put(std::vector<uint8_t>& o, const T& v) { o.insert(o.end(), (const uint8_t*)&v, (const uint8_t*)&v + sizeof v); }
void put_bytes(std::vector<uint8_t>& o, const void* p, size_t n) { o.insert(o.end(), (const uint8_t*)p, (const uint8_t*)p + n); }

// (the frame's memory blobs, one per distinct range or content)
struct Blobs {
    std::vector<std::vector<uint8_t>> bytes;
    std::vector<uint64_t> guest;
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> by_range;
    std::unordered_map<uint64_t, std::vector<uint32_t>> by_hash;
    uint32_t add(uint64_t g, std::vector<uint8_t>&& b) {
        if (g) {
            const auto it = by_range.find({g, b.size()});
            if (it != by_range.end()) return it->second;
        }
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint8_t c : b) h = (h ^ c) * 0x100000001b3ull;
        h ^= g;
        for (uint32_t i : by_hash[h])
            if (guest[i] == g && bytes[i] == b) return i;
        const uint32_t id = (uint32_t)bytes.size();
        by_hash[h].push_back(id);
        if (g) by_range[{g, b.size()}] = id;
        bytes.push_back(std::move(b)), guest.push_back(g);
        return id;
    }
};

const uint8_t* resolve(const MeshCapDraw& d, uint64_t dev, uint64_t n) {
    if (d.chunk_host && dev >= d.chunk_dev && dev + n <= d.chunk_dev + d.chunk_bytes) return d.chunk_host + (dev - d.chunk_dev);
    if (d.pool_host && dev >= d.pool_dev && dev + n <= d.pool_dev + d.pool_bytes) return d.pool_host + (dev - d.pool_dev);
    return nullptr;
}

}  // namespace

bool meshcap_on() { return at() >= 0; }
bool meshcap_now() { return at() >= 0 && (int64_t)present_count() == at() && !written; }

void meshcap_program(const DrawProg& d, uint64_t task_code, const McStageIn* st, uint32_t n) {
    if (!meshcap_on()) return;
    std::vector<uint8_t> o;
    mcap::McProgram p{};
    p.key = d.key;
    std::memcpy(p.codes, d.codes, sizeof p.codes);
    p.task_code = task_code;
    p.task = d.task, p.task_threads = d.task_threads, p.task_payload_words = d.task_payload_words, p.push_bytes = d.push_bytes;
    p.nubo = (uint32_t)d.ubos.size(), p.npushed = (uint32_t)d.pushed.size(), p.nstages = n, p.map_size = n ? st[0].map_size : 0;
    put(o, p);
    for (const auto& u : d.ubos) put(o, mcap::McProgUbo{u.stage, u.index, 0, u.bytes});
    for (const auto& q : d.pushed) put(o, mcap::McPushed{q.stage, q.index, q.push_off});
    for (uint32_t i = 0; i < n; i++) {
        const McStageIn& s = st[i];
        put(o, mcap::McStage{s.vk_stage, s.nvn_stage, (uint32_t)s.spirv->size(), s.nmaps, (uint32_t)s.c1->size(), s.threads, s.payload_words, 0});
        put_bytes(o, s.spirv->data(), s.spirv->size() * 4);
        put_bytes(o, s.maps, (size_t)s.nmaps * s.map_size);
        put_bytes(o, s.c1->data(), s.c1->size());
    }
    std::lock_guard lk(mu);
    programs[d.key] = std::move(o);
}

void meshcap_draw(MeshCapDraw&& d) {
    std::lock_guard lk(mu);
    if (cmds.size() >= 20000) return;
    d.d.seq = seq++, d.d.cb_seq = cb_count[d.d.cb]++;
    d.d.present = present_count();
    Cmd c{true, std::move(d), {}};
    cmds.push_back(std::move(c));
}

void meshcap_other(uint64_t cb, uint32_t kind, uint64_t key, uint32_t count) {
    std::lock_guard lk(mu);
    if (cmds.size() >= 20000) return;
    Cmd c{false, {}, {}};
    c.other = {key, cb, seq++, cb_count[cb]++, kind, count};
    cmds.push_back(std::move(c));
}

void meshcap_after() {
    if (at() < 0 || written || (int64_t)present_count() != at() + 1) return;
    std::vector<Cmd> v;
    {
        std::lock_guard lk(mu);
        v.swap(cmds);
        written = true;
    }
    vkDeviceWaitIdle(dev.device);
    Blobs blobs;
    std::vector<std::vector<uint8_t>> draws;
    std::vector<uint64_t> used;
    uint32_t n_mesh = 0, n_task = 0;
    uint64_t ssbo_bytes = 0;
    for (Cmd& c : v) {
        std::vector<uint8_t> o;
        if (!c.mesh) {
            put(o, mcap::McRecord{mcap::MC_OTHER, 0, sizeof c.other});
            put(o, c.other);
            draws.push_back(std::move(o));
            continue;
        }
        MeshCapDraw& m = c.draw;
        n_mesh++;
        if (std::find(used.begin(), used.end(), m.d.key) == used.end()) used.push_back(m.d.key);
        // (uniform buffers: the CPU's copies as made; game buffers and c1 read now)
        for (size_t i = 0; i < m.ubos.size(); i++) {
            mcap::McDrawUbo& u = m.ubos[i];
            std::vector<uint8_t> b = std::move(m.ubo_bytes[i]);
            const bool from_guest = b.empty() && u.guest;
            if (from_guest) {
                b.resize(u.bytes);
                if (!host->gpu_read(u.guest, b.data(), b.size())) b.clear();
            }
            u.blob = b.empty() ? ~0u : blobs.add(from_guest ? u.guest : 0, std::move(b));
        }
        // (the mesh stage's storage buffers: the game's, read now, up to 64 MB each)
        for (mcap::McDrawSsbo& s : m.ssbos) {
            s.blob = ~0u;
            if (!s.guest || !s.size) continue;
            std::vector<uint8_t> b(std::min<uint64_t>(s.size, 64ull << 20));
            if (!host->gpu_read(s.guest, b.data(), b.size())) continue;
            ssbo_bytes += b.size();
            s.blob = blobs.add(s.guest, std::move(b));
        }
        // (a task+mesh draw's header, map and payload as the GPU left them)
        std::vector<uint32_t> map, payload;
        if (m.d.task) {
            if (const uint8_t* h = resolve(m, m.header_dev, 64)) {
                n_task++;
                std::memcpy(m.d.header, h, 64);
                const uint32_t* w = m.d.header;
                const uint64_t ma = w[4] | (uint64_t)w[5] << 32, pa = w[6] | (uint64_t)w[7] << 32;
                const uint32_t groups = std::min(w[0], w[8]), tasks = w[9], pw = w[13];
                if (const uint8_t* mp = resolve(m, ma, 4ull * groups)) map.assign((const uint32_t*)mp, (const uint32_t*)mp + groups);
                if (const uint8_t* pp = resolve(m, pa, 4ull * tasks * pw)) payload.assign((const uint32_t*)pp, (const uint32_t*)pp + (uint64_t)tasks * pw);
            }
        } else if (m.d.indirect && m.d.record) {
            host->gpu_read(m.d.record, m.d.record_words, sizeof m.d.record_words);
        }
        m.d.nubo = (uint32_t)m.ubos.size(), m.d.nssbo = (uint32_t)m.ssbos.size(), m.d.nmap = (uint32_t)map.size(), m.d.npayload = (uint32_t)payload.size();
        const uint64_t bytes = sizeof m.d + m.ubos.size() * sizeof(mcap::McDrawUbo) + m.ssbos.size() * sizeof(mcap::McDrawSsbo) + 4 * (map.size() + payload.size());
        put(o, mcap::McRecord{mcap::MC_DRAW, 0, bytes});
        put(o, m.d);
        for (const auto& u : m.ubos) put(o, u);
        for (const auto& s : m.ssbos) put(o, s);
        put_bytes(o, map.data(), 4 * map.size());
        put_bytes(o, payload.data(), 4 * payload.size());
        draws.push_back(std::move(o));
    }
    // (pointers in the first 4 KB of the storage and uniform buffers read (a pointer table: some mesh stages read through such
    // words, e.g. 40d0c0768e874600): the 4 MB after each, or less where the guest has less, so that the harness has them too)
    {
        std::map<uint64_t, uint64_t> want;   // (page -> end)
        const size_t n0 = blobs.bytes.size();
        for (size_t i = 0; i < n0; i++) {
            const std::vector<uint8_t>& b = blobs.bytes[i];
            for (size_t o = 0; o + 8 <= std::min<size_t>(b.size(), 4096); o += 8) {
                uint64_t v;
                std::memcpy(&v, b.data() + o, 8);
                if (v >> 32 < 0x10 || v >> 32 >= 0x200 || (v & 3)) continue;
                const uint64_t lo = v & ~0xFFFull;
                uint64_t& e = want[lo];
                e = std::max<uint64_t>(e, lo + (4ull << 20));
            }
        }
        uint64_t covered = 0, total = 0;
        for (auto& [lo, hi] : want) {
            if (lo < covered) {
                if (hi <= covered) continue;
            }
            const uint64_t from = std::max(lo, covered);
            if (total > (1ull << 30)) break;
            for (uint64_t n = hi - from; n >= 4096; n >>= 1) {
                std::vector<uint8_t> b(n);
                if (!host->gpu_read(from, b.data(), n)) continue;
                total += n;
                covered = from + n;
                blobs.add(from, std::move(b));
                break;
            }
        }
        logf("direct: meshcap: %zu pointer targets in the buffers' first 4 KB, %llu MB read after them", want.size(), (unsigned long long)(total >> 20));
    }
    mkdir("meshcap", 0755);
    FILE* f = std::fopen("meshcap/frame.bin", "wb");
    if (!f) return logf("direct: meshcap: meshcap/frame.bin not written");
    const mcap::McFile hdr{mcap::kMagic, mcap::kVersion, (uint64_t)at()};
    std::fwrite(&hdr, sizeof hdr, 1, f);
    uint32_t n_prog = 0;
    {
        std::lock_guard lk(mu);
        for (uint64_t k : used) {
            const auto it = programs.find(k);
            if (it == programs.end()) continue;
            const mcap::McRecord r{mcap::MC_PROGRAM, 0, it->second.size()};
            std::fwrite(&r, sizeof r, 1, f), std::fwrite(it->second.data(), 1, it->second.size(), f);
            n_prog++;
        }
    }
    uint64_t blob_bytes = 0;
    for (uint32_t i = 0; i < blobs.bytes.size(); i++) {
        const mcap::McBlob b{i, 0, blobs.guest[i], blobs.bytes[i].size()};
        const mcap::McRecord r{mcap::MC_BLOB, 0, sizeof b + b.bytes};
        std::fwrite(&r, sizeof r, 1, f), std::fwrite(&b, sizeof b, 1, f), std::fwrite(blobs.bytes[i].data(), 1, b.bytes, f);
        blob_bytes += b.bytes;
    }
    for (const auto& o : draws) std::fwrite(o.data(), 1, o.size(), f);
    std::fclose(f);
    logf("direct: meshcap: present %lld: %zu commands, %u mesh draws (%u task+mesh with their header), %u programs, %zu blobs (%llu MB; storage "
         "%llu MB) in meshcap/frame.bin", (long long)at(), v.size(), n_mesh, n_task, n_prog, blobs.bytes.size(),
         (unsigned long long)(blob_bytes >> 20), (unsigned long long)(ssbo_bytes >> 20));
    host->log_flush();
}

}  // namespace hsd
