// (native-vk, RX_VK_L1_CENSUS=1) the layer-1 census: see vk_l1_census.h
#include "vk_l1_census.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#if defined(__linux__)
#include <dlfcn.h>
#endif

#include "vk_internal.h"

namespace mkwvk {
namespace l1c {

const bool on = [] {
    const char* e = getenv("RX_VK_L1_CENSUS");
    return e && *e && *e != '0';
}();

namespace {

// (rx_nvn.h's NOP_* in their order: the op names of the time line)
const char* const kOpNames[] = {"end", "jump", "begin", "call", "signal_event", "report_counter", "copy_b2b", "update_ub", "set_rt",
                                "clear_color", "clear_ds", "viewports", "scissor", "depth_range", "bind_program", "bind_state",
                                "bind_vattr", "bind_vstream", "bind_vbuf", "bind_ubo", "bind_ssbo", "bind_tex", "bind_samp", "stencil",
                                "poly_offset", "draw", "draw_idx", "dispatch", "barrier", "set_tex_pool", "set_samp_pool", "copy_b2t",
                                "copy_stride", "blend_color", "copy_t2b", "bind_img", "copy_t2t", "reset_counter", "clear_tex",
                                "depth_clamp", "render_enable", "raster_discard", "render_cond", "primitive_restart", "bind_bufs",
                                "draw_indirect", "dispatch_indirect", "draw_mesh", "wait_event", "push_const", "clear_buf",
                                "wait_fence"};
constexpr uint32_t kOps = (uint32_t)(sizeof kOpNames / sizeof kOpNames[0]);
enum : uint32_t { OP_SIGNAL = 4, OP_COPY_B2B = 6, OP_CLEAR_COLOR = 9, OP_CLEAR_DS = 10, OP_DRAW = 25, OP_DRAW_IDX = 26, OP_DISPATCH = 27,
                  OP_COPY_B2T = 31, OP_COPY_T2B = 34, OP_COPY_T2T = 36, OP_CLEAR_TEX = 38, OP_DRAW_INDIRECT = 45,
                  OP_DISPATCH_INDIRECT = 46, OP_DRAW_MESH = 47, OP_CLEAR_BUF = 50 };

struct Access {
    uint64_t lo, hi;      // (a guest range; images: lo is the texture, hi 0)
    bool img, write;
    uint8_t kind;
};

/// Guest ranges as a union of disjoint intervals (start -> end)
struct Ranges {
    std::map<uint64_t, uint64_t> m;
    bool overlaps(uint64_t lo, uint64_t hi) const {
        if (m.empty() || hi <= lo) return false;
        auto it = m.upper_bound(lo);
        if (it != m.begin() && std::prev(it)->second > lo) return true;
        return it != m.end() && it->first < hi;
    }
    void add(uint64_t lo, uint64_t hi) {
        if (hi <= lo) return;
        auto it = m.upper_bound(lo);
        if (it != m.begin() && std::prev(it)->second >= lo) {
            --it;
            lo = it->first, hi = std::max(hi, it->second);
            it = m.erase(it);
        }
        while (it != m.end() && it->first <= hi) hi = std::max(hi, it->second), it = m.erase(it);
        m.emplace(lo, hi);
    }
    void clear() { m.clear(); }
    bool empty() const { return m.empty(); }
};

struct ImgUse {
    uint8_t wkinds = 0, rkinds = 0;   // (bits of Kind)
    uint64_t pass = 0;                // (the pass whose attachment writes these are)
};

/// What the commands of a stretch wrote and read
struct Set {
    Ranges w[K_COUNT], r;
    std::unordered_map<const void*, ImgUse> imgs;
    bool any = false;
    void clear() {
        for (Ranges& x : w) x.clear();
        r.clear(), imgs.clear(), any = false;
    }
    void merge(const Set& o) {
        for (int k = 0; k < K_COUNT; k++)
            for (const auto& [lo, hi] : o.w[k].m) w[k].add(lo, hi);
        for (const auto& [lo, hi] : o.r.m) r.add(lo, hi);
        for (const auto& [t, u] : o.imgs) {
            ImgUse& v = imgs[t];
            v.wkinds |= u.wkinds, v.rkinds |= u.rkinds, v.pass = std::max(v.pass, u.pass);
        }
        any |= o.any;
    }
};

// hazard classes: writer kind x reader kind (K_COUNT readers), then write-after-read
constexpr int kClasses = K_COUNT * K_COUNT + 1;
constexpr int kWar = K_COUNT * K_COUNT;
const char* const kKindName[] = {"attachment", "shader", "transfer"};

/// The hazard class of a command's accesses against what a set holds (-1: none): the first found, writes first
int hazard(const Set& s, const std::vector<Access>& acc, uint64_t pass_now) {
    if (!s.any) return -1;
    int war = -1;
    for (const Access& a : acc) {
        if (a.img) {
            auto it = s.imgs.find((const void*)(uintptr_t)a.lo);
            if (it == s.imgs.end()) continue;
            const ImgUse& u = it->second;
            // (one pass's attachments: its draws are in rasterization order)
            const bool same_pass_att = a.kind == K_ATT && u.pass == pass_now && pass_now;
            for (int k = 0; k < K_COUNT; k++)
                if ((u.wkinds >> k & 1) && !(same_pass_att && k == K_ATT)) return k * K_COUNT + a.kind;
            if (a.write && u.rkinds && !(same_pass_att && u.rkinds == (1u << K_ATT))) war = kWar;
            continue;
        }
        for (int k = 0; k < K_COUNT; k++)
            if (s.w[k].overlaps(a.lo, a.hi)) return k * K_COUNT + a.kind;
        if (a.write && s.r.overlaps(a.lo, a.hi)) war = kWar;
    }
    return war;
}

void add(Set& s, const std::vector<Access>& acc, uint64_t pass_now) {
    for (const Access& a : acc) {
        if (a.img) {
            ImgUse& u = s.imgs[(const void*)(uintptr_t)a.lo];
            if (a.write) u.wkinds |= (uint8_t)(1u << a.kind);
            else u.rkinds |= (uint8_t)(1u << a.kind);
            if (a.kind == K_ATT) u.pass = pass_now;
        } else if (a.write) {
            s.w[a.kind].add(a.lo, a.hi);
        } else {
            s.r.add(a.lo, a.hi);
        }
    }
    s.any |= !acc.empty();
}

// ---- the state (the replay is one thread at a time: replay_mu) ----
bool collecting = false;
uint8_t cur_kind = K_SHADER;
std::vector<Access> pending, last_desc;
size_t desc_mark = SIZE_MAX;
uint64_t pass_serial = 0;
std::vector<const void*> pass_att;

Set ideal;                    // (since the last ideal barrier)
Set prev_seg, cur_seg;        // (between the last two recorded barriers, and since the last one)
bool cur_needed = false;      // (the last recorded barrier ordered a hazard between them)
int cur_class = -1;
const char* cur_why = nullptr;
const void* cur_ra = nullptr;

// counters (since the start; the report takes differences)
struct Counts {
    int64_t ideal[kClasses]{};
    int64_t ideal_in_pass = 0;     // (an ideal barrier between two draws of one pass: a pass break)
    int64_t commands = 0, barriers = 0, needed = 0, no_command = 0;
    int64_t op_n[kOps + 1]{}, op_ns[kOps + 1]{};
    int64_t handles[4]{}, ssbos[4]{};   // (cbuf 0/2, 1, 3+, other)
};
Counts c, last;
struct WhyCount { int64_t n = 0, needed = 0, no_command = 0; int64_t cls[kClasses]{}; };
std::map<std::pair<std::string, uintptr_t>, WhyCount> by_why, by_why_last;
bool last_cmd_in_pass = false;

void finish_barrier() {   // (the recorded barrier before this one: needed or not)
    if (!cur_why) return;
    std::string why = cur_why;
    const uintptr_t ra = std::strstr(cur_why, "(other)") ? (uintptr_t)cur_ra : 0;
    WhyCount& w = by_why[{why, ra}];
    w.n++;
    if (!cur_seg.any) w.no_command++, c.no_command++;
    else if (cur_needed) w.needed++, c.needed++, w.cls[cur_class >= 0 ? cur_class : kWar]++;
}

int src_slot(uint32_t cbuf) { return cbuf == 0 || cbuf == 2 ? 0 : cbuf == 1 ? 1 : cbuf >= 3 && cbuf < 19 ? 2 : 3; }

std::string where(uintptr_t ra) {
    char b[160];
#if defined(__linux__)
    Dl_info info{};
    if (ra && dladdr((const void*)ra, &info) && info.dli_fbase) {
        const char* f = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
        snprintf(b, sizeof b, "%s+%#lx%s%s", f ? f + 1 : "?", (unsigned long)(ra - (uintptr_t)info.dli_fbase),
                 info.dli_sname ? " " : "", info.dli_sname ? info.dli_sname : "");
        return b;
    }
#endif
    snprintf(b, sizeof b, "%#llx", (unsigned long long)ra);
    return b;
}

}  // namespace

void op_begin(uint32_t op) {
    switch (op) {
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_INDIRECT: case OP_DRAW_MESH: case OP_DISPATCH: case OP_DISPATCH_INDIRECT:
    case OP_CLEAR_COLOR: case OP_CLEAR_DS:
        collecting = true, cur_kind = K_SHADER;
        break;
    case OP_SIGNAL: case OP_COPY_B2B: case OP_CLEAR_BUF: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX:
        collecting = true, cur_kind = K_TRANSFER;
        break;
    default: collecting = false; break;
    }
    pending.clear();
}

void op_done(uint32_t op, int64_t ns) {
    const uint32_t k = op < kOps ? op : kOps;
    c.op_n[k]++, c.op_ns[k] += ns;
    if (!collecting) return;
    collecting = false;
    const bool draw_op = op == OP_DRAW || op == OP_DRAW_IDX || op == OP_DRAW_INDIRECT || op == OP_DRAW_MESH || op == OP_CLEAR_COLOR ||
                         op == OP_CLEAR_DS;
    if (draw_op)   // (the pass's attachments, written by each of its draws)
        for (const void* t : pass_att) pending.push_back({(uint64_t)(uintptr_t)t, 0, true, true, K_ATT});
    if (pending.empty()) return;
    c.commands++;
    // (neighbour tracker: against the stretch before the last recorded barrier)
    if (!cur_needed) {
        const int h = hazard(prev_seg, pending, pass_serial);
        if (h >= 0) cur_needed = true, cur_class = h;
    }
    add(cur_seg, pending, pass_serial);
    // (ideal tracker)
    const int h = hazard(ideal, pending, pass_serial);
    if (h >= 0) {
        c.ideal[h]++;
        if (draw_op && last_cmd_in_pass && pass_att.size()) c.ideal_in_pass++;
        ideal.clear();
    }
    add(ideal, pending, pass_serial);
    last_cmd_in_pass = draw_op;
    pending.clear();
}

void buf(uint64_t gpu, uint64_t size, bool write) {
    if (!collecting || !size) return;
    pending.push_back({gpu, gpu + size, false, write, write ? cur_kind : (cur_kind == K_TRANSFER ? (uint8_t)K_TRANSFER : (uint8_t)K_SHADER)});
}

void img(const void* tex, bool write) {
    if (!collecting || !tex) return;
    pending.push_back({(uint64_t)(uintptr_t)tex, 0, true, write, cur_kind});
}

void pass_begin() {
    pass_serial++;
    pass_att.clear();
    last_cmd_in_pass = false;
}

void pass_attachment(const void* tex) {
    if (tex) pass_att.push_back(tex);
}

void desc_begin() {
    if (collecting) desc_mark = pending.size();
}

void desc_end() {
    if (!collecting || desc_mark == SIZE_MAX || desc_mark > pending.size()) return;
    last_desc.assign(pending.begin() + (ptrdiff_t)desc_mark, pending.end());
    desc_mark = SIZE_MAX;
}

void desc_reuse() {
    if (collecting) pending.insert(pending.end(), last_desc.begin(), last_desc.end());
}

void barrier(const char* why, const void* ra) {
    c.barriers++;
    finish_barrier();
    if (cur_seg.any) prev_seg = std::move(cur_seg);   // (an empty stretch: the one before still stands against the next)
    cur_seg.clear();
    cur_needed = false, cur_class = -1;
    cur_why = why ? why : "barrier (other)";
    cur_ra = ra;
    // (a barrier ends any pass: the next draws' attachment writes are in a new one, begin_pass notes them)
}

void handle_src(uint32_t cbuf, uint32_t n) { c.handles[src_slot(cbuf)] += n; }
void ssbo_src(uint32_t cbuf) { c.ssbos[src_slot(cbuf)]++; }

void report(double frames) {
    if (!on || frames <= 0) return;
    char line[1600];
    const auto pf = [&](int64_t now, int64_t was) { return (double)(now - was) / frames; };
    int n = snprintf(line, sizeof line, "vk: l1 census: a frame: %.1f commands, %.1f barriers recorded (%.1f needed by their neighbours, "
                     "%.1f with no command since the one before); ideal %.1f:", pf(c.commands, last.commands), pf(c.barriers, last.barriers),
                     pf(c.needed, last.needed), pf(c.no_command, last.no_command), [&] {
                         int64_t s = 0, l = 0;
                         for (int i = 0; i < kClasses; i++) s += c.ideal[i], l += last.ideal[i];
                         return pf(s, l);
                     }());
    for (int i = 0; i < kClasses; i++) {
        const double v = pf(c.ideal[i], last.ideal[i]);
        if (v < 0.05) continue;
        if (i == kWar) n += snprintf(line + n, sizeof line - n, " write-after-read %.1f;", v);
        else n += snprintf(line + n, sizeof line - n, " %s->%s %.1f;", kKindName[i / K_COUNT], kKindName[i % K_COUNT], v);
    }
    n += snprintf(line + n, sizeof line - n, " of them inside a pass %.1f", pf(c.ideal_in_pass, last.ideal_in_pass));
    logf("%s", line);
    // (recorded barriers by reason; the unlabelled by caller)
    std::vector<std::pair<int64_t, std::string>> rows;
    for (auto& [key, w] : by_why) {
        const WhyCount& o = by_why_last[key];
        const int64_t dn = w.n - o.n;
        if (dn <= 0) continue;
        char b[600];
        int k = snprintf(b, sizeof b, "%7.1f a frame, needed %5.1f, no command %5.1f: %s%s%s", dn / frames, (w.needed - o.needed) / frames,
                         (w.no_command - o.no_command) / frames, key.first.c_str(), key.second ? " at " : "",
                         key.second ? where(key.second).c_str() : "");
        for (int i = 0; i < kClasses; i++) {
            const int64_t d = w.cls[i] - o.cls[i];
            if (d <= 0 || k > (int)sizeof b - 60) continue;
            if (i == kWar) k += snprintf(b + k, sizeof b - k, " [WAR %.1f]", d / frames);
            else k += snprintf(b + k, sizeof b - k, " [%s->%s %.1f]", kKindName[i / K_COUNT], kKindName[i % K_COUNT], d / frames);
        }
        rows.push_back({dn, b});
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < rows.size() && i < 24; i++) logf("vk: l1 census: barrier %s", rows[i].second.c_str());
    by_why_last = by_why;
    // (the replay's time by op: ms a frame, ops a frame)
    std::vector<std::pair<double, uint32_t>> ops;
    for (uint32_t i = 0; i <= kOps; i++)
        if (c.op_n[i] - last.op_n[i]) ops.push_back({(double)(c.op_ns[i] - last.op_ns[i]), i});
    std::sort(ops.begin(), ops.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    double total = 0;
    for (const auto& o : ops) total += o.first;
    n = snprintf(line, sizeof line, "vk: l1 census: replay ops %.2f ms a frame:", total / frames / 1e6);
    for (const auto& [ns, i] : ops) {
        if (n > (int)sizeof line - 64) break;
        n += snprintf(line + n, sizeof line - n, " %s %.3f ms (%.0f)", i < kOps ? kOpNames[i] : "other", ns / frames / 1e6,
                      pf(c.op_n[i], last.op_n[i]));
    }
    logf("%s", line);
    logf("vk: l1 census: texture/image handles a frame from banks c0/c2 %.1f, c1 %.1f, user uniform buffers %.1f, other %.1f; storage "
         "buffer addresses from c0/c2 %.1f, c1 %.1f, user uniform buffers %.1f, other %.1f", pf(c.handles[0], last.handles[0]),
         pf(c.handles[1], last.handles[1]), pf(c.handles[2], last.handles[2]), pf(c.handles[3], last.handles[3]),
         pf(c.ssbos[0], last.ssbos[0]), pf(c.ssbos[1], last.ssbos[1]), pf(c.ssbos[2], last.ssbos[2]), pf(c.ssbos[3], last.ssbos[3]));
    last = c;
}

}  // namespace l1c
}  // namespace mkwvk
