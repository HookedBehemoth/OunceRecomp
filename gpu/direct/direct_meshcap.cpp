// HOS_DBG=meshcap (direct_meshcap.h).
#include <sys/stat.h>
#include <unistd.h>

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

// (HOS_DBG=meshcap:<present> one frame, every draw: meshcap/frame.bin, as M6e made it. The roll (the Hyrule Field hang):
// meshcap:<first>-<last> every frame of that range, meshcapprog:<program key> only that program's mesh draws, meshcaplost
// nothing read or written while the device lives: the last draws are kept and written when a submit reports the device
// lost. The roll goes to meshcap/roll.bin, many frames in one file (MC_FRAME), a memory range written once while its
// bytes stay the same)
struct Opt {
    int64_t a = -1, b = -1;
    uint64_t prog = 0;
    bool lost = false, roll = false;
};
const Opt& opt() {
    static const Opt o = [] {
        Opt o;
        if (const char* s = dbg_value("meshcap")) {
            o.a = atoll(s);
            const char* d = std::strchr(s, '-');
            o.b = d ? atoll(d + 1) : o.a;
        }
        if (const char* s = dbg_value("meshcapprog")) o.prog = std::strtoull(s, nullptr, 16);
        o.lost = dbg("meshcaplost");
        if (o.lost && o.a < 0) o.a = 0, o.b = INT64_MAX;
        o.roll = o.a >= 0 && (o.lost || o.prog || o.b != o.a);
        return o;
    }();
    return o;
}
int64_t at() { return opt().a; }
constexpr size_t kLostKept = 1200;   // (meshcaplost: the draws kept)

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
bool meshcap_now(uint64_t key) {
    const Opt& o = opt();
    if (o.a < 0) return false;
    const int64_t p = (int64_t)present_count();
    if (!o.roll) return p == o.a && !written;
    return p >= o.a && p <= o.b && (!o.prog || key == o.prog);
}

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
    if (opt().lost && cmds.size() >= kLostKept) cmds.erase(cmds.begin(), cmds.begin() + kLostKept / 4);
    if (cmds.size() >= 20000 || (opt().roll && d.vertex)) return;
    d.d.seq = seq++, d.d.cb_seq = cb_count[d.d.cb]++;
    d.d.present = present_count();
    d.other.seq = d.d.seq, d.other.cb_seq = d.d.cb_seq, d.other.cb = d.d.cb, d.other.key = d.d.key;
    Cmd c{true, std::move(d), {}};
    cmds.push_back(std::move(c));
}

void meshcap_other(uint64_t cb, uint32_t kind, uint64_t key, uint32_t count) {
    if (opt().roll) return;   // (the roll: mesh draws only)
    std::lock_guard lk(mu);
    if (cmds.size() >= 20000) return;
    Cmd c{false, {}, {}};
    c.other = {key, cb, seq++, cb_count[cb]++, kind, count};
    cmds.push_back(std::move(c));
}

namespace {

struct Counts {
    uint32_t n_mesh = 0, n_task = 0;
    uint64_t ssbo_bytes = 0;
};

/// One captured command as its record. add(guest, bytes) -> the blob's id. read: the game's memory is read (false: a
/// uniform buffer of the game's stays unread)
template <class Add> std::vector<uint8_t> record_of(Cmd& c, Add&& add, Counts& n, std::vector<uint64_t>& used) {
    std::vector<uint8_t> o;
    if (!c.mesh) {
        put(o, mcap::McRecord{mcap::MC_OTHER, 0, sizeof c.other});
        put(o, c.other);
        return o;
    }
    MeshCapDraw& m = c.draw;
    n.n_mesh += !m.vertex;
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
        u.blob = b.empty() ? ~0u : add(from_guest ? u.guest : 0, std::move(b));
    }
    if (m.vertex) {   // (M6h: a vertex draw: its uniform buffers, then its place in the frame)
        m.d.nubo = (uint32_t)m.ubos.size();
        put(o, mcap::McRecord{mcap::MC_VDRAW, 0, sizeof m.d + m.ubos.size() * sizeof(mcap::McDrawUbo) + sizeof m.other});
        put(o, m.d);
        for (const auto& u : m.ubos) put(o, u);
        put(o, m.other);
        return o;
    }
    // (the mesh stage's storage buffers: the game's, read now, up to 64 MB each)
    for (mcap::McDrawSsbo& s : m.ssbos) {
        s.blob = ~0u;
        if (!s.guest || !s.size) continue;
        std::vector<uint8_t> b(std::min<uint64_t>(s.size, 64ull << 20));
        if (!host->gpu_read(s.guest, b.data(), b.size())) continue;
        n.ssbo_bytes += b.size();
        s.blob = add(s.guest, std::move(b));
    }
    // (a task+mesh draw's header, map and payload as the GPU left them)
    std::vector<uint32_t> map, payload;
    if (m.d.task) {
        if (const uint8_t* h = resolve(m, m.header_dev, 64)) {
            n.n_task++;
            std::memcpy(m.d.header, h, 64);
            const uint32_t* w = m.d.header;
            const uint64_t ma = w[4] | (uint64_t)w[5] << 32, pa = w[6] | (uint64_t)w[7] << 32;
            uint32_t groups = std::min(w[0], w[8]), tasks = w[9], pw = w[13];
            if (groups > (1u << 20) || tasks > 65536 || pw > 4096) groups = tasks = 0;   // (a slot reset since: not this draw's header)
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
    return o;
}

/// Pointers in the first 4 KB of the storage and uniform buffers read (a pointer table: some mesh stages read through such
/// words, e.g. 40d0c0768e874600): the 4 MB after each, or less where the guest has less, so that the harness has them too.
/// heads: those first 4 KB. Returns the bytes read
template <class Add> uint64_t pointer_targets(const std::vector<std::vector<uint8_t>>& heads, Add&& add, size_t& targets) {
    std::map<uint64_t, uint64_t> want;   // (page -> end)
    for (const std::vector<uint8_t>& b : heads)
        for (size_t o = 0; o + 8 <= std::min<size_t>(b.size(), 4096); o += 8) {
            uint64_t v;
            std::memcpy(&v, b.data() + o, 8);
            if (v >> 32 < 0x10 || v >> 32 >= 0x200 || (v & 3)) continue;
            const uint64_t lo = v & ~0xFFFull;
            uint64_t& e = want[lo];
            e = std::max<uint64_t>(e, lo + (4ull << 20));
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
            add(from, std::move(b));
            break;
        }
    }
    targets = want.size();
    return total;
}

void write_programs(FILE* f, const std::vector<uint64_t>& used, uint32_t& n_prog, std::vector<uint64_t>* done = nullptr) {
    std::lock_guard lk(mu);
    for (uint64_t k : used) {
        if (done && std::find(done->begin(), done->end(), k) != done->end()) continue;
        const auto it = programs.find(k);
        if (it == programs.end()) continue;
        const mcap::McRecord r{mcap::MC_PROGRAM, 0, it->second.size()};
        std::fwrite(&r, sizeof r, 1, f), std::fwrite(it->second.data(), 1, it->second.size(), f);
        n_prog++;
        if (done) done->push_back(k);
    }
}

// ---- the roll: many frames in meshcap/roll.bin ----
struct Roll {
    FILE* f = nullptr;
    bool failed = false;
    struct Ent {
        uint64_t guest, bytes, hash;
    };
    std::vector<Ent> ents;
    std::unordered_map<uint64_t, std::vector<uint32_t>> by_hash;
    std::vector<uint64_t> progs;
    uint64_t written = 0, frames = 0;
} roll;

uint64_t hash_of(const std::vector<uint8_t>& b) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= b.size(); i += 8) {
        uint64_t v;
        std::memcpy(&v, b.data() + i, 8);
        h = (h ^ v) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < b.size(); i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/// One frame's kept draws into the roll: new memory ranges, the draws, the frame's list of ranges; flushed to the disk
void roll_frame(std::vector<Cmd>& v, uint64_t present, const char* how) {
    if (v.empty() || roll.failed) return;
    if (!roll.f) {
        mkdir("meshcap", 0755);
        roll.f = std::fopen("meshcap/roll.bin", "wb");
        if (!roll.f) return roll.failed = true, logf("direct: meshcap: meshcap/roll.bin not written");
        const mcap::McFile hdr{mcap::kMagic, mcap::kVersion, present};
        std::fwrite(&hdr, sizeof hdr, 1, roll.f);
    }
    if (roll.written > (48ull << 30)) return;
    std::vector<uint32_t> ids;                     // (this frame's ranges of the game's memory)
    std::vector<std::vector<uint8_t>> heads;       // (their first 4 KB: the pointer tables)
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> by_range;
    uint64_t fresh = 0;
    const auto add = [&](uint64_t g, std::vector<uint8_t>&& b) -> uint32_t {
        if (g) {
            const auto it = by_range.find({g, b.size()});
            if (it != by_range.end()) return it->second;
        }
        const uint64_t h = hash_of(b) ^ g;
        uint32_t id = ~0u;
        for (uint32_t i : roll.by_hash[h])
            if (roll.ents[i].guest == g && roll.ents[i].bytes == b.size()) id = i;
        if (id == ~0u) {
            id = (uint32_t)roll.ents.size();
            roll.ents.push_back({g, b.size(), h});
            roll.by_hash[h].push_back(id);
            const mcap::McBlob mb{id, 0, g, b.size()};
            const mcap::McRecord r{mcap::MC_BLOB, 0, sizeof mb + mb.bytes};
            std::fwrite(&r, sizeof r, 1, roll.f), std::fwrite(&mb, sizeof mb, 1, roll.f), std::fwrite(b.data(), 1, b.size(), roll.f);
            fresh += b.size(), roll.written += b.size();
        }
        if (g) by_range[{g, b.size()}] = id, ids.push_back(id);
        b.resize(std::min<size_t>(b.size(), 4096));
        heads.push_back(std::move(b));
        return id;
    };
    Counts n;
    std::vector<uint64_t> used;
    std::vector<std::vector<uint8_t>> draws;
    char line[600];
    int k = 0;
    for (Cmd& c : v) {
        draws.push_back(record_of(c, add, n, used));
        if (c.mesh && !c.draw.vertex && k < 500)
            k += std::snprintf(line + k, sizeof line - k, " #%u %016llx groups %u of %u (tasks %u)", c.draw.d.seq, (unsigned long long)c.draw.d.key,
                               c.draw.d.header[0], c.draw.d.header[8], c.draw.d.header[9]);
    }
    size_t targets = 0;
    const std::vector<std::vector<uint8_t>> first = heads;   // (the draws' own buffers: targets are not followed further)
    const uint64_t read = pointer_targets(first, add, targets);
    uint32_t n_prog = 0;
    write_programs(roll.f, used, n_prog, &roll.progs);
    for (const auto& o : draws) std::fwrite(o.data(), 1, o.size(), roll.f);
    const mcap::McFrame fr{present, (uint32_t)ids.size(), 0};
    const mcap::McRecord r{mcap::MC_FRAME, 0, sizeof fr + 4ull * ids.size()};
    std::fwrite(&r, sizeof r, 1, roll.f), std::fwrite(&fr, sizeof fr, 1, roll.f), std::fwrite(ids.data(), 4, ids.size(), roll.f);
    std::fflush(roll.f);
    fsync(fileno(roll.f));
    roll.frames++;
    logf("direct: meshcap roll (%s): present %llu:%s; %zu ranges (%zu pointer targets, %llu MB read), %llu KB new, file %llu MB", how,
         (unsigned long long)present, line, ids.size(), targets, (unsigned long long)(read >> 20), (unsigned long long)(fresh >> 10),
         (unsigned long long)(roll.written >> 20));
}

/// The kept draws of presents [from, to] taken out, a frame at a time into the roll
void roll_take(uint64_t from, uint64_t to, const char* how, uint64_t first = 0) {
    std::vector<Cmd> v;
    {
        std::lock_guard lk(mu);
        for (size_t i = 0; i < cmds.size();)
            if (cmds[i].draw.d.present >= from && cmds[i].draw.d.present <= to) v.push_back(std::move(cmds[i])), cmds.erase(cmds.begin() + i);
            else i++;
    }
    // (in present order; after a loss the frame of the first unfinished command first, then the newer ones, then the older)
    const auto rank = [first](uint64_t p) { return first ? (p >= first ? p - first : (1ull << 40) + first - p) : p; };
    std::stable_sort(v.begin(), v.end(), [&](const Cmd& x, const Cmd& y) { return rank(x.draw.d.present) < rank(y.draw.d.present); });
    for (size_t i = 0; i < v.size();) {
        size_t j = i;
        std::vector<Cmd> fr;
        while (j < v.size() && v[j].draw.d.present == v[i].draw.d.present) fr.push_back(std::move(v[j++]));
        roll_frame(fr, fr[0].draw.d.present, how);
        i = j;
    }
}

}  // namespace

void meshcap_lost(uint64_t first_open_present) {
    if (!opt().lost) return;
    // (the device is lost: nothing of the GPU is asked. The task headers, maps and payloads are host memory the GPU wrote
    // (their slots were not reset: the queue never got past them); the game's memory is read as it is now, seconds after the
    // draw was recorded)
    uint64_t newest = 0;
    {
        std::lock_guard lk(mu);
        for (const Cmd& c : cmds) newest = std::max<uint64_t>(newest, c.draw.d.present);
    }
    const uint64_t from = first_open_present ? (first_open_present > 3 ? first_open_present - 3 : 0) : (newest > 8 ? newest - 8 : 0);
    logf("direct: meshcap: the device is lost: the kept draws of presents %llu.. written (the first unfinished command's present %llu, the newest kept %llu)",
         (unsigned long long)from, (unsigned long long)first_open_present, (unsigned long long)newest);
    roll_take(from, first_open_present ? first_open_present + 3 : newest, "after the loss", first_open_present);
    host->log_flush();
}

void meshcap_after() {
    const Opt& o = opt();
    if (o.a < 0) return;
    const int64_t pc = (int64_t)present_count();
    if (o.lost) {   // (meshcaplosttest:<present>, the loss path tried without a loss: the kept draws written as at a loss)
        static const int64_t test = [] {
            const char* v = dbg_value("meshcaplosttest");
            return v ? atoll(v) : -1;
        }();
        if (test >= 0 && pc == test + 1) meshcap_lost((uint64_t)test - 1);
        return;
    }
    if (o.roll) {
        if (pc - 1 < o.a || pc - 1 > o.b) return;
        vkDeviceWaitIdle(dev.device);
        roll_take(0, (uint64_t)pc - 1, "the frame done");
        if (pc - 1 == o.b) host->log_flush();
        return;
    }
    if (written || pc != at() + 1) return;
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
    Counts n;
    const auto add = [&](uint64_t g, std::vector<uint8_t>&& b) { return blobs.add(g, std::move(b)); };
    for (Cmd& c : v) draws.push_back(record_of(c, add, n, used));
    {
        std::vector<std::vector<uint8_t>> heads;
        for (const auto& b : blobs.bytes) heads.emplace_back(b.begin(), b.begin() + std::min<size_t>(b.size(), 4096));
        size_t targets = 0;
        const uint64_t total = pointer_targets(heads, add, targets);
        logf("direct: meshcap: %zu pointer targets in the buffers' first 4 KB, %llu MB read after them", targets, (unsigned long long)(total >> 20));
    }
    mkdir("meshcap", 0755);
    FILE* f = std::fopen("meshcap/frame.bin", "wb");
    if (!f) return logf("direct: meshcap: meshcap/frame.bin not written");
    const mcap::McFile hdr{mcap::kMagic, mcap::kVersion, (uint64_t)at()};
    std::fwrite(&hdr, sizeof hdr, 1, f);
    uint32_t n_prog = 0;
    write_programs(f, used, n_prog);
    uint64_t blob_bytes = 0;
    for (uint32_t i = 0; i < blobs.bytes.size(); i++) {
        const mcap::McBlob b{i, 0, blobs.guest[i], blobs.bytes[i].size()};
        const mcap::McRecord r{mcap::MC_BLOB, 0, sizeof b + b.bytes};
        std::fwrite(&r, sizeof r, 1, f), std::fwrite(&b, sizeof b, 1, f), std::fwrite(blobs.bytes[i].data(), 1, b.bytes, f);
        blob_bytes += b.bytes;
    }
    for (const auto& o2 : draws) std::fwrite(o2.data(), 1, o2.size(), f);
    std::fclose(f);
    logf("direct: meshcap: present %lld: %zu commands, %u mesh draws (%u task+mesh with their header), %u programs, %zu blobs (%llu MB; storage "
         "%llu MB) in meshcap/frame.bin", (long long)at(), v.size(), n.n_mesh, n.n_task, n_prog, blobs.bytes.size(),
         (unsigned long long)(blob_bytes >> 20), (unsigned long long)(n.ssbo_bytes >> 20));
    host->log_flush();
}

}  // namespace hsd
