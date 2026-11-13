// The list calls (direct_api.h) at M1: every NVN record is counted by op and dropped, but the ops the frame level needs
// (event signals and waits, reports, calls), which hsd_submit runs (direct_sync.cpp). The counts of the lists ended,
// by kind, of their records, by op, and of the return addresses each kind was begun from, go in the 600-present line.
#include <cstdio>
#include <cstring>
#include <mutex>
#include <chrono>
#include <thread>

#include "direct_assets.h"
#include "direct_cmd.h"
#include "direct_compute.h"
#include "direct_draw.h"
#include "direct_dump.h"
#include "direct_internal.h"
#include "direct_lists.h"
#include "direct_pass.h"
#include "direct_query.h"

namespace hsd {
namespace {

static_assert(NOP_COUNT_ == 55, "rx_nvn.h's ops changed: the names below too");
const char* const kOpNames[NOP_COUNT_] = {
    "END", "JUMP", "BEGIN", "CALL", "SIGNAL_EVENT", "REPORT_COUNTER", "COPY_B2B", "UPDATE_UB", "SET_RT", "CLEAR_COLOR",
    "CLEAR_DS", "VIEWPORTS", "SCISSOR", "DEPTH_RANGE", "BIND_PROGRAM", "BIND_STATE", "BIND_VATTR", "BIND_VSTREAM", "BIND_VBUF",
    "BIND_UBO", "BIND_SSBO", "BIND_TEX", "BIND_SAMP", "STENCIL", "POLY_OFFSET", "DRAW", "DRAW_IDX", "DISPATCH", "BARRIER",
    "SET_TEX_POOL", "SET_SAMP_POOL", "COPY_B2T", "COPY_STRIDE", "BLEND_COLOR", "COPY_T2B", "BIND_IMG", "COPY_T2T",
    "RESET_COUNTER", "CLEAR_TEX", "DEPTH_CLAMP", "RENDER_ENABLE", "RASTER_DISCARD", "RENDER_COND", "PRIMITIVE_RESTART",
    "BIND_BUFS", "DRAW_INDIRECT", "DISPATCH_INDIRECT", "DRAW_MESH", "WAIT_EVENT", "PUSH_CONST", "CLEAR_BUF", "WAIT_FENCE",
    "PKT", "JOB", "ENG"};
const char* const kKindNames[HSD_LIST_KINDS] = {"unclassified", "load", "job", "layer", "gate", "compute", "head"};

// (the recording threads' time in hsd_emit and hsd_list_call, in TSC ticks, by the list's kind: summed per thread, added up
// every 256 records)
std::atomic<int64_t> st_emit_ticks[HSD_LIST_KINDS], st_call_ticks[HSD_LIST_KINDS];
thread_local uint64_t my_ticks[HSD_LIST_KINDS], my_call_ticks[HSD_LIST_KINDS];
thread_local uint32_t my_n = 0;
void flush_ticks() {
    for (int k = 0; k < HSD_LIST_KINDS; k++) {
        if (my_ticks[k]) st_emit_ticks[k].fetch_add((int64_t)my_ticks[k], std::memory_order_relaxed), my_ticks[k] = 0;
        if (my_call_ticks[k]) st_call_ticks[k].fetch_add((int64_t)my_call_ticks[k], std::memory_order_relaxed), my_call_ticks[k] = 0;
    }
    my_n = 0;
}
void emit_ticks(uint32_t kind, uint64_t t) {
    my_ticks[kind] += t;
    if (++my_n == 256) flush_ticks();
}
void call_ticks(uint32_t kind, uint64_t t) {
    my_call_ticks[kind] += t;
    if (++my_n == 256) flush_ticks();
}
double tsc_per_ns() {
    static const double r = [] {
        const auto a = std::chrono::steady_clock::now();
        const uint64_t t = __builtin_ia32_rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - a).count();
        return (double)(__builtin_ia32_rdtsc() - t) / ns;
    }();
    return r;
}
std::atomic<int64_t> st_pkt_bytes{0}, st_pkt_lists{0}, st_pkt_calls{0}, st_pkt_ops{0}, st_pkt_nested{0}, st_pkt_unended{0};
std::atomic<int64_t> st_ops[NOP_COUNT_], st_kinds[HSD_LIST_KINDS], st_calls{0}, st_live{0}, st_dropped{0}, st_bad_op{0};

// (the return addresses each kind was begun from, and how their lists were used: a table that only grows, read without a
// lock; the counts since the start and since the last line)
constexpr int kSites = 1024;
struct Site {
    std::atomic<uint64_t> key{0};
    std::atomic<uint32_t> kind{0}, load_kind{0};
    std::atomic<int64_t> n{0}, submitted{0}, called_by[HSD_LIST_KINDS]{};
    int64_t last_n = 0;   // (the 600-present line's: its thread's alone)
};
Site sites[kSites];
int32_t site_note(const hsd_list_info& i) {
    const uint64_t key = i.lr ^ ((uint64_t)i.kind << 56);
    for (uint32_t k = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 54), tries = 0; tries < kSites; k = (k + 1) & (kSites - 1), tries++) {
        Site& s = sites[k];
        uint64_t have = s.key.load(std::memory_order_acquire);
        if (!have) {
            if (s.key.compare_exchange_strong(have, key, std::memory_order_acq_rel)) {   // (the key holds the kind; these for the line)
                s.kind.store(i.kind, std::memory_order_relaxed), s.load_kind.store(i.load_kind, std::memory_order_relaxed);
                logf("direct: a list of kind %s begun from %#llx (fp %#llx)%s", kKindNames[i.kind], (unsigned long long)i.lr,
                     (unsigned long long)i.fp, i.kind == HSD_LIST_PACKET ? ": not classified" : "");
                have = key;
            }
        }
        if (have == key) {
            s.n.fetch_add(1, std::memory_order_relaxed);
            return (int32_t)k;
        }
    }
    return -1;
}
}  // namespace

void list_ref(List* l) { l->refs.fetch_add(1, std::memory_order_relaxed); }
void list_unref(List* l) {
    if (l->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    std::vector<List*> work{l};   // (freed ones: no recursion through long call chains)
    while (!work.empty()) {
        List* x = work.back();
        work.pop_back();
        for (const FrameOp& o : x->ops)
            if (o.op == NOP_CALL && o.callee->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) work.push_back(o.callee);
        draw_list_free(x);
        compute_list_free(x);
        query_list_free(x);
        if (x->packet && x->ended) st_pkt_bytes.fetch_sub((int64_t)x->pkt.size(), std::memory_order_relaxed), st_pkt_lists.fetch_sub(1, std::memory_order_relaxed);
        if (x->slot) cmd_slot_drop(x->slot);
        dump_free(x->dumps);
        if (x->counted) mem_account_main(MO_M_LISTS, -(int64_t)x->counted);
        delete x;
        st_live.fetch_sub(1, std::memory_order_relaxed);
    }
}
void list_submitted(const List* l) {
    if (l->site >= 0) sites[l->site].submitted.fetch_add(1, std::memory_order_relaxed);
}
void list_flatten(const List* l, std::vector<const FrameOp*>& out) {
    for (const FrameOp& o : l->ops) {
        if (o.op == NOP_CALL) list_flatten(o.callee, out);
        else out.push_back(&o);
    }
}

void list_items(const List* l, std::vector<SubItem>& out, std::vector<CmdSlot*>& slots, std::vector<DumpItem*>* dumps) {
    for (const List* j : l->executed)   // (secondaries run inside its buffers: their slots are held by this submission too)
        if (j->slot) slots.push_back(j->slot);
    if (l->has_cmds && l->slot) slots.push_back(l->slot);
    size_t oi = 0;
    auto ops_to = [&](size_t seg) {   // (the ops recorded before segment seg: a callee with commands is its segment)
        for (; oi < l->ops.size() && l->ops[oi].seg <= seg; oi++) {
            const FrameOp& o = l->ops[oi];
            if (o.op != NOP_CALL) out.push_back({&o, VK_NULL_HANDLE});
            else if (!o.callee->has_cmds) list_items(o.callee, out, slots, dumps);
        }
    };
    if (l->has_cmds)
        for (size_t i = 0; i < l->segs.size(); i++) {
            ops_to(i);
            const Seg& s = l->segs[i];
            if (s.callee) {
                list_items(s.callee, out, slots, dumps);
                continue;
            }
            out.push_back({nullptr, s.cb});
            if (dumps)
                for (DumpItem* d : l->dumps)
                    if (d->cb == s.cb) dumps->push_back(d);
        }
    ops_to(SIZE_MAX);
}

void list_buffers(const List* l, std::vector<VkCommandBuffer>& out, std::vector<CmdSlot*>& slots, std::vector<DumpItem*>* dumps) {
    for (const List* j : l->executed)   // (M3a: secondaries run inside its buffers: their slots are held by this submission too)
        if (j->slot) slots.push_back(j->slot);
    if (!l->has_cmds) return;
    if (l->slot) slots.push_back(l->slot);
    for (const Seg& s : l->segs) {
        if (s.callee) {
            list_buffers(s.callee, out, slots, dumps);
            continue;
        }
        out.push_back(s.cb);
        if (dumps)
            for (DumpItem* d : l->dumps)
                if (d->cb == s.cb) dumps->push_back(d);
    }
}

void lists_stats(char* out, size_t cap, double frames) {
    int k = std::snprintf(out, cap, "lists a frame:");
    for (int i = 0; i < HSD_LIST_KINDS; i++)
        k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %s %.1f", kKindNames[i], st_kinds[i].exchange(0) / frames);
    double all = 0, by[HSD_LIST_KINDS], calls[HSD_LIST_KINDS];
    for (int i = 0; i < HSD_LIST_KINDS; i++) {
        by[i] = st_emit_ticks[i].exchange(0) / tsc_per_ns() / 1e6 / frames, calls[i] = st_call_ticks[i].exchange(0) / tsc_per_ns() / 1e6 / frames;
        all += by[i];
    }
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, ", %.3f ms a frame in hsd_emit (all recording threads; by kind:", all);
    for (int i = 0; i < HSD_LIST_KINDS; i++)
        if (by[i] > 0) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %s %.3f", kKindNames[i], by[i]);
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "; in hsd_list_call by the caller's kind:");
    for (int i = 0; i < HSD_LIST_KINDS; i++)
        if (calls[i] > 0) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %s %.3f", kKindNames[i], calls[i]);
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "); packets: %lld live (%.1f MB), %.1f calls a frame expanding %.1f ops, "
                       "%.1f called by packets (in place), %lld called before their end", (long long)st_pkt_lists.load(),
                       st_pkt_bytes.load() / 1048576.0, st_pkt_calls.exchange(0) / frames, st_pkt_ops.exchange(0) / frames,
                       st_pkt_nested.exchange(0) / frames, (long long)st_pkt_unended.load());
    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, ", calls %.1f; %lld lists live, %lld dropped unended, %lld records "
                       "of no op or outside a list; ops a frame:", st_calls.exchange(0) / frames, (long long)st_live.load(),
                       (long long)st_dropped.load(), (long long)st_bad_op.load());
    for (int i = 0; i < NOP_COUNT_; i++) {
        const int64_t n = st_ops[i].exchange(0);
        if (n) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %s %.1f", kOpNames[i], n / frames);
    }
}
/// The return addresses seen, by kind: lists begun from each a frame since the last line (and since the start), how often
/// one was called by a list of each kind and submitted (since the start)
void sites_stats(char* out, size_t cap, double frames) {
    int k = std::snprintf(out, cap, "lists by return address, a frame (since the start; called by kind, submitted):");
    for (int kind = 0; kind < HSD_LIST_KINDS; kind++)
        for (Site& s : sites) {
            const uint64_t key = s.key.load(std::memory_order_acquire);
            if (!key || (key >> 56) != (uint64_t)kind) continue;
            const int64_t n = s.n.load(std::memory_order_relaxed);
            k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %s", kKindNames[kind]);
            if (kind == HSD_LIST_LOAD) k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, "/%u", s.load_kind.load());
            k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " %#llx %.2f (%lld;", (unsigned long long)(key & ((1ull << 56) - 1)),
                               (n - s.last_n) / frames, (long long)n);
            s.last_n = n;
            for (int c = 0; c < HSD_LIST_KINDS; c++)
                if (const int64_t b = s.called_by[c].load(std::memory_order_relaxed))
                    k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " by %s %lld", kKindNames[c], (long long)b);
            k += std::snprintf(out + k, k < (int)cap ? cap - k : 0, " submitted %lld);", (long long)s.submitted.load());
        }
}

}  // namespace hsd

using namespace hsd;

extern "C" __attribute__((visibility("default"))) void* hsd_list_begin(const hsd_list_info* info) {
    List* l = new List;
    l->info = *info;
    if (l->info.kind >= HSD_LIST_KINDS) l->info.kind = HSD_LIST_PACKET;
    l->site = site_note(l->info);
    const uint32_t k = l->info.kind;
    l->rec = k == HSD_LIST_LAYER || k == HSD_LIST_GATE || k == HSD_LIST_COMPUTE || k == HSD_LIST_HEAD;
    l->packet = k == HSD_LIST_LOAD || k == HSD_LIST_PACKET;
    if (k == HSD_LIST_JOB) pass_job_begin(l);   // (M3a: its pass's formats known: a rendering secondary)
    st_live.fetch_add(1, std::memory_order_relaxed);
    return l;
}

namespace hsd {
/// One record into a list that records (its pass, its state, its frame-level ops): the record's own list, or the caller a
/// packet is expanded into
void emit_one(List* l, uint32_t op, const void* payload, uint32_t size) {
    pass_emit(l, op, payload, size);
    FrameOp o;
    switch (op) {
    case NOP_SIGNAL_EVENT:
    case NOP_WAIT_EVENT: {   // (nvn_p_signal and nvn_p_wait_event are the same three fields)
        nvn_p_signal s;
        if (size < sizeof s) break;
        std::memcpy(&s, payload, sizeof s);
        pass_cut(l);   // (M4a: its list's commands before it and after it are separate command buffers: the submit sends them apart)
        o.op = op, o.addr = s.gpu, o.mode = s.mode, o.value = s.value, o.seg = (uint32_t)l->segs.size();
        l->ops.push_back(o);
        break;
    }
    // (M5: REPORT_COUNTER and RESET_COUNTER are GPU work in the list's command buffers: direct_query.cpp)
    default: break;
    }
}
}  // namespace hsd

namespace {
/// (M3a) a packet's op: {op, size} and the payload, 8-byte aligned. Ops with nothing for a caller are left out
void pkt_append(List* l, uint32_t op, const void* payload, uint32_t size) {
    switch (op) {
    case NOP_END: case NOP_JUMP: case NOP_BEGIN: case NOP_PKT: case NOP_JOB: case NOP_ENG: return;
    default: break;
    }
    const size_t at = l->pkt.size(), padded = (size + 7) & ~(size_t)7;
    l->pkt.resize(at + 8 + padded);
    const uint32_t h[2] = {op, size};
    std::memcpy(l->pkt.data() + at, h, 8);
    if (size) std::memcpy(l->pkt.data() + at + 8, payload, size);
    l->pkt_ops++;
}
}  // namespace

extern "C" __attribute__((visibility("default"))) void hsd_emit(void* list, uint32_t op, const void* payload, uint32_t size) {
    List* l = (List*)list;
    if (!l || op >= NOP_COUNT_) {
        st_bad_op.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    l->counts[op]++;
    const uint64_t t0 = __builtin_ia32_rdtsc();
    if (l->packet) pkt_append(l, op, payload, size);
    else emit_one(l, op, payload, size);
    emit_ticks(l->info.kind, __builtin_ia32_rdtsc() - t0);
}

extern "C" __attribute__((visibility("default"))) void hsd_list_call(void* list, void* const* callees, uint32_t n) {
    List* l = (List*)list;
    if (!l) return;
    const uint64_t t0 = __builtin_ia32_rdtsc();
    struct Timed {
        uint32_t kind;
        uint64_t t0;
        ~Timed() { call_ticks(kind, __builtin_ia32_rdtsc() - t0); }
    } timed{l->info.kind, t0};
    for (uint32_t i = 0; i < n; i++) {
        List* c = (List*)callees[i];
        if (!c) continue;
        if (c->packet) {   // (M3a: a packet's ops go into the caller: its state, its command buffer, its frame-level ops)
            l->counts[NOP_CALL]++;
            if (c->site >= 0) sites[c->site].called_by[l->info.kind].fetch_add(1, std::memory_order_relaxed);
            if (!c->ended) {
                st_pkt_unended.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (l->packet) {   // (a packet calling one: the callee's ops in place)
                l->pkt.insert(l->pkt.end(), c->pkt.begin(), c->pkt.end());
                l->pkt_ops += c->pkt_ops;
                st_pkt_nested.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            st_pkt_calls.fetch_add(1, std::memory_order_relaxed);
            st_pkt_ops.fetch_add(c->pkt_ops, std::memory_order_relaxed);
            for (size_t at = 0; at + 8 <= c->pkt.size();) {
                uint32_t h[2];
                std::memcpy(h, c->pkt.data() + at, 8);
                emit_one(l, h[0], c->pkt.data() + at + 8, h[1]);
                at += 8 + ((h[1] + 7) & ~7u);
            }
            continue;
        }
        pass_call(l, c);
        list_ref(c);
        if (c->site >= 0) sites[c->site].called_by[l->info.kind].fetch_add(1, std::memory_order_relaxed);
        FrameOp o;
        o.op = NOP_CALL, o.callee = c, o.seg = (uint32_t)l->segs.size();
        l->ops.push_back(o);
        l->counts[NOP_CALL]++;
    }
}

extern "C" __attribute__((visibility("default"))) void hsd_list_end(void* list) {
    List* l = (List*)list;
    if (!l) return;
    pass_list_end(l);
    if (!l->counted) {
        l->counted = sizeof(List) + l->pkt.capacity() + l->ops.capacity() * sizeof(FrameOp) + l->segs.capacity() * sizeof(Seg) +
                     l->executed.capacity() * sizeof(const List*);
        mem_account_main(MO_M_LISTS, (int64_t)l->counted);
    }
    if (l->packet) {
        l->pkt.shrink_to_fit();
        st_pkt_bytes.fetch_add((int64_t)l->pkt.size(), std::memory_order_relaxed);
        st_pkt_lists.fetch_add(1, std::memory_order_relaxed);
    }
    l->ended = true;
    if (l->has_cmds && !l->counts[NOP_DRAW] && !l->counts[NOP_DRAW_IDX] && !l->counts[NOP_CLEAR_COLOR] && !l->counts[NOP_CLEAR_DS] &&
        !l->counts[NOP_COPY_T2T] && !l->counts[NOP_COPY_B2T] && !l->counts[NOP_CALL] && !l->counts[NOP_DISPATCH] &&
        !l->counts[NOP_DISPATCH_INDIRECT] && !l->counts[NOP_COPY_B2B] && !l->counts[NOP_CLEAR_BUF] && present_count() > 1200) {
        static std::atomic<int> told{0};   // (a list with command buffers and none of the ops that need one: named)
        if (told++ < 12) {
            char b[600];
            int k = std::snprintf(b, sizeof b, "direct: list of kind %s from %#llx has command buffers but records:", kKindNames[l->info.kind],
                                  (unsigned long long)l->info.lr);
            for (int i = 0; i < NOP_COUNT_ && k < 560; i++)
                if (l->counts[i]) k += std::snprintf(b + k, sizeof b - k, " %s %u", kOpNames[i], l->counts[i]);
            logf("%s", b);
        }
    }
    st_kinds[l->info.kind].fetch_add(1, std::memory_order_relaxed);
    if (l->info.kind == HSD_LIST_JOB) assets_job_list();   // (A-rt: the first frame in play)
    for (int i = 0; i < NOP_COUNT_; i++)
        if (l->counts[i]) st_ops[i].fetch_add(l->counts[i], std::memory_order_relaxed);
    st_calls.fetch_add(l->counts[NOP_CALL], std::memory_order_relaxed);
}

extern "C" __attribute__((visibility("default"))) void hsd_list_released(void* list) {
    List* l = (List*)list;
    if (!l) return;
    if (!l->ended) st_dropped.fetch_add(1, std::memory_order_relaxed);
    list_unref(l);
}
