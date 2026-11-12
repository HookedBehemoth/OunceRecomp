// The list calls (direct_api.h) at M1: every NVN record is counted by op and dropped, but the ops the frame level needs
// (event signals and waits, reports, calls), which hsd_submit runs (direct_sync.cpp). The counts of the lists ended,
// by kind, of their records, by op, and of the return addresses each kind was begun from, go in the 600-present line.
#include <cstdio>
#include <cstring>
#include <mutex>

#include "direct_assets.h"
#include "direct_cmd.h"
#include "direct_dump.h"
#include "direct_internal.h"
#include "direct_lists.h"
#include "direct_pass.h"

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
        if (x->slot) cmd_slot_drop(x->slot);
        dump_free(x->dumps);
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

void list_buffers(const List* l, std::vector<VkCommandBuffer>& out, std::vector<CmdSlot*>& slots, std::vector<DumpItem*>* dumps) {
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
    st_live.fetch_add(1, std::memory_order_relaxed);
    return l;
}

extern "C" __attribute__((visibility("default"))) void hsd_emit(void* list, uint32_t op, const void* payload, uint32_t size) {
    List* l = (List*)list;
    if (!l || op >= NOP_COUNT_) {
        st_bad_op.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    l->counts[op]++;
    pass_emit(l, op, payload, size);
    FrameOp o;
    switch (op) {
    case NOP_SIGNAL_EVENT:
    case NOP_WAIT_EVENT: {   // (nvn_p_signal and nvn_p_wait_event are the same three fields)
        nvn_p_signal s;
        if (size < sizeof s) break;
        std::memcpy(&s, payload, sizeof s);
        o.op = op, o.addr = s.gpu, o.mode = s.mode, o.value = s.value;
        l->ops.push_back(o);
        break;
    }
    case NOP_REPORT_COUNTER:
    case NOP_RESET_COUNTER:
        if (size < sizeof o.counter) break;
        std::memcpy(&o.counter, payload, sizeof o.counter);
        o.op = op;
        l->ops.push_back(o);
        break;
    default: break;
    }
}

extern "C" __attribute__((visibility("default"))) void hsd_list_call(void* list, void* const* callees, uint32_t n) {
    List* l = (List*)list;
    if (!l) return;
    for (uint32_t i = 0; i < n; i++) {
        List* c = (List*)callees[i];
        if (!c) continue;
        pass_call(l, c);
        list_ref(c);
        if (c->site >= 0) sites[c->site].called_by[l->info.kind].fetch_add(1, std::memory_order_relaxed);
        FrameOp o;
        o.op = NOP_CALL, o.callee = c;
        l->ops.push_back(o);
        l->counts[NOP_CALL]++;
    }
}

extern "C" __attribute__((visibility("default"))) void hsd_list_end(void* list) {
    List* l = (List*)list;
    if (!l) return;
    pass_list_end(l);
    l->ended = true;
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
