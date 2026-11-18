// The NVN state of a list (direct_state.h): the ops as the frozen job walk applies them (gpu/legacy/vk/eng_job.cpp 956-1086), and
// the graphics stages' driver banks (gpu/legacy/vk/vk_bank.cpp driver_bank_word / driver_bank_fill, graphics stages only: compute's
// packed bank and the task / mesh pointer table are not this stage's).
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

#include "direct_counters.h"
#include "direct_state.h"

namespace hsd {
namespace {

enum { ST_DEFERRED, ST_DEFERRED_BUFS, ST_CONDS, ST_COUNT };
struct StateStats;
ThreadCounters<StateStats, ST_COUNT> st;

template <class T>
T payload(const void* p, uint32_t size) {
    T v{};
    std::memcpy(&v, p, std::min<size_t>(sizeof v, size));
    return v;
}

constexpr uint32_t kSsboTable[kStages] = {0x110, 0x510, 0x410, 0x210, 0x310, 0, 0x670, 0x770};

}  // namespace

bool state_emit(DState& s, uint32_t op, const void* a, uint32_t size) {
    switch (op) {   // (M3b: the ops the dynamic state is made from move its generation)
    case NOP_VIEWPORTS: case NOP_SCISSOR: case NOP_DEPTH_RANGE: case NOP_BIND_STATE: case NOP_STENCIL: case NOP_POLY_OFFSET:
    case NOP_BLEND_COLOR: case NOP_PRIMITIVE_RESTART: case NOP_DEPTH_CLAMP: case NOP_RASTER_DISCARD: s.dyn_gen++; break;
    default: break;
    }
    switch (op) {
    case NOP_VIEWPORTS: {
        const auto v = payload<nvn_p_viewports>(a, size);
        if (v.first == 0 && v.n > 0) std::memcpy(s.viewport, v.v[0], 16);
        return true;
    }
    case NOP_SCISSOR: {
        const auto c = payload<nvn_p_scissor>(a, size);
        s.scissor[0] = c.x, s.scissor[1] = c.y, s.scissor[2] = c.w, s.scissor[3] = c.h;
        return true;
    }
    case NOP_DEPTH_RANGE: {
        const auto d = payload<nvn_p_depth_range>(a, size);
        s.depth_near = d.n, s.depth_far = d.f;
        return true;
    }
    case NOP_BIND_PROGRAM: {
        const auto b = payload<nvn_p_bind_program>(a, size);
        // (M4a: a compute program is bound beside the graphics one, not over it: direct_compute.cpp keeps it)
        if ((b.stages & NVN_SHADER_STAGE_COMPUTE_BIT) && !(b.stages & ~NVN_SHADER_STAGE_COMPUTE_BIT)) return true;
        const bool had = s.has_prog;
        const uint64_t was = s.prog.serial;
        if (b.prog) s.prog = *b.prog;   // (the guest object read now: its stages are set before it is bound)
        s.has_prog = b.prog != nullptr && b.prog->nstage > 0;
        if (had != s.has_prog || was != s.prog.serial) s.prog_gen++;   // (M3b: the program a draw picks by is its serial's)
        return true;
    }
    case NOP_BIND_STATE: {
        const auto b = payload<nvn_p_bind_state>(a, size);
        switch (b.kind) {
        case 0: {
            nvn_blend_st st;
            std::memcpy(&st, b.bytes, sizeof st);
            s.blend[st.target & 7] = st;
            break;
        }
        case 1: std::memcpy(&s.color, b.bytes, sizeof s.color); break;
        case 2: std::memcpy(&s.chmask, b.bytes, sizeof s.chmask); break;
        case 3: std::memcpy(&s.ms, b.bytes, sizeof s.ms); break;
        case 4: std::memcpy(&s.poly, b.bytes, sizeof s.poly); break;
        case 5: std::memcpy(&s.ds, b.bytes, sizeof s.ds); break;
        default: break;
        }
        return true;
    }
    case NOP_BIND_VATTR: {
        const auto v = payload<nvn_p_bind_vattr>(a, size);
        uint32_t was[16];
        std::memcpy(was, s.vattr, sizeof was);
        const int nwas = s.nvattr;
        std::memset(s.vattr, 0, sizeof s.vattr);
        for (int i = 0; i < v.n && i < 16; i++) s.vattr[i] = v.st[i];
        s.nvattr = std::clamp(v.n, 0, 16);
        if (nwas != s.nvattr || std::memcmp(was, s.vattr, sizeof was)) s.vtx_gen++, s.vi_gen++;   // (M3b: the pick reads the attributes only)
        return true;
    }
    case NOP_BIND_VSTREAM: {
        const auto v = payload<nvn_p_bind_vstream>(a, size);
        for (int i = 0; i < v.n && v.first + i < 16; i++)
            if (v.first + i >= 0 && std::memcmp(&s.vstream[v.first + i], &v.st[i], sizeof v.st[i]))
                s.vstream[v.first + i] = v.st[i], s.vi_gen++;
        return true;   // (M3b: no pick generation: the pick does not read streams; the vertex input is set from them at each draw)
    }
    case NOP_BIND_VBUF: {
        const auto v = payload<nvn_p_bind_vbuf>(a, size);
        for (int i = 0; i < v.n && v.first + i < 16; i++)
            if (v.first + i >= 0) s.vbuf[v.first + i] = v.r[i];
        return true;   // (M3b: no generation: the pick does not read buffers; a draw takes them through its spans)
    }
    case NOP_BIND_BUFS: {   // (deferred ranges: the values recorded; their array is the game's to keep. Counted)
        const auto b = payload<nvn_p_bind_bufs>(a, size);
        if (b.stage < 0 || b.stage >= kStages) return true;
        bool changed = false;   // (M3b: the generations move only when a binding does: a rebind of the same is common)
        for (int i = 0; i < b.n && i < 16; i++) {
            const int index = b.first + i;
            if (index < 0 || index >= 16) continue;
            nvn_p_bind_buf& u = (b.ssbo ? s.ssbo : s.ubo)[b.stage][index];
            changed |= u.addr != b.r[i].address || u.size != b.r[i].size;
            u.stage = b.stage, u.index = index, u.addr = b.r[i].address, u.size = b.r[i].size;
        }
        if (b.src) st.add(ST_DEFERRED_BUFS);
        if (changed && b.ssbo && b.stage < kBankStages) s.bank_gen[b.stage]++;
        if (changed && !b.ssbo) s.ubo_gen++;
        return true;
    }
    case NOP_BIND_UBO: case NOP_BIND_SSBO: {
        const auto b = payload<nvn_p_bind_buf>(a, size);
        if (b.stage >= 0 && b.stage < kStages && b.index >= 0 && b.index < 16) {
            nvn_p_bind_buf& u = (op == NOP_BIND_UBO ? s.ubo : s.ssbo)[b.stage][b.index];
            const bool changed = u.addr != b.addr || u.size != b.size;
            u = b;
            if (changed && op == NOP_BIND_UBO) s.ubo_gen++;
            else if (changed && b.stage < kBankStages) s.bank_gen[b.stage]++;
        }
        return true;
    }
    case NOP_BIND_TEX: case NOP_BIND_SAMP: case NOP_BIND_IMG: {
        const auto b = payload<nvn_p_bind_handles>(a, size);
        if (b.stage < 0 || b.stage >= kBankStages || b.first < 0) return true;
        uint32_t* to = op == NOP_BIND_TEX ? s.tex[b.stage] : op == NOP_BIND_SAMP ? s.samp[b.stage] : s.img[b.stage];
        const int lim = op == NOP_BIND_IMG ? 32 : 128;
        const int n = std::clamp(std::min(b.n, lim - b.first), 0, 32);
        bool changed = false;
        for (int k = 0; k < n; k++) changed |= to[b.first + k] != (uint32_t)b.h[k], to[b.first + k] = (uint32_t)b.h[k];
        if (b.stage < kBankStages) {
            uint64_t* src = op == NOP_BIND_TEX ? s.tex_src[b.stage] : op == NOP_BIND_SAMP ? s.samp_src[b.stage] : s.img_src[b.stage];
            for (int k = 0; k < n; k++) {
                const uint64_t was = src[b.first + k], now = b.src ? b.src + 8ull * k : 0;
                if (!was != !now) s.deferred[b.stage] += now ? 1 : (uint32_t)-1;
                changed |= was != now;
                src[b.first + k] = now;
            }
            if (changed) s.bank_gen[b.stage]++;
        }
        if (b.src) st.add(ST_DEFERRED, n);
        return true;
    }
    case NOP_STENCIL: {
        const auto st = payload<nvn_p_stencil>(a, size);
        for (int f = 0; f < 2; f++) {
            if (!(st.face & (1 << f))) continue;
            if (st.which == 0) s.stencil_value_mask[f] = (uint32_t)st.value_mask;
            else if (st.which == 1) s.stencil_mask[f] = (uint32_t)st.mask;
            else s.stencil_ref[f] = (uint32_t)st.ref;
        }
        return true;
    }
    case NOP_POLY_OFFSET: {
        const auto o = payload<nvn_p_poly_offset>(a, size);
        s.poly_offset[0] = o.factor, s.poly_offset[1] = o.units, s.poly_offset[2] = o.clamp;
        return true;
    }
    case NOP_BLEND_COLOR: std::memcpy(s.blend_color, payload<nvn_p_blend_color>(a, size).rgba, 16); return true;
    case NOP_PRIMITIVE_RESTART: s.restart = payload<nvn_p_restart>(a, size).enable != 0; return true;
    case NOP_DEPTH_CLAMP: s.depth_clamp = payload<nvn_p_flag>(a, size).value != 0; return true;
    case NOP_RASTER_DISCARD: s.raster_discard = payload<nvn_p_flag>(a, size).value != 0; return true;
    case NOP_RENDER_ENABLE:
        s.render_enabled = payload<nvn_p_flag>(a, size).value != 0;
        s.conditional = false;
        return true;
    case NOP_RENDER_COND:   // (M5's: the GPU's counters decide. Until then the draws under one are skipped, counted)
        s.conditional = true;
        st.add(ST_CONDS);
        return true;
    default: return false;
    }
}

uint32_t bank_word(const DState& s, int bank, int stage, uint32_t offset) {
    if (stage < 0 || stage >= kBankStages || offset + 4 > 0xA00 || (offset & 3)) return 0;
    auto lo = [](uint64_t v) { return (uint32_t)v; };
    if (bank == 2) {   // (handles' low words, 8 bytes a slot: textures 0x20 (32) and 0x168 (128), images 0x120 (9), samplers 0x568)
        if (offset & 7) return 0;
        if (offset >= 0x568 && offset < 0x568 + 8 * 128) return lo(s.samp[stage][(offset - 0x568) / 8]);
        if (offset >= 0x168 && offset < 0x168 + 8 * 128) return lo(s.tex[stage][(offset - 0x168) / 8]);
        if (offset >= 0x120 && offset < 0x120 + 8 * 9) return lo(s.img[stage][(offset - 0x120) / 8]);
        if (offset >= 0x20 && offset < 0x20 + 8 * 32) return lo(s.tex[stage][(offset - 0x20) / 8]);
        return 0;
    }
    if (bank != 0) return 0;
    const uint32_t base = kSsboTable[stage];
    if (base && offset >= base && offset < base + 16 * 16) {
        const nvn_p_bind_buf& e = s.ssbo[stage][(offset - base) / 16];
        switch ((offset - base) % 16) {
        case 0: return lo(e.addr);
        case 4: return (uint32_t)(e.addr >> 32);
        case 8: return (uint32_t)e.size;
        default: return 0;
        }
    }
    if (stage == 0 && offset == 0x610) return std::bit_cast<uint32_t>(2047.9375f);
    return 0;
}

void bank_fill(const DState& s, int bank, int stage, uint8_t* d, uint32_t n) {
    std::memset(d, 0, n);
    if (stage < 0 || stage >= kBankStages) return;
    auto put = [&](uint32_t off, uint32_t v) {
        if (off + 4 <= n) std::memcpy(d + off, &v, 4);
    };
    if (bank == 2) {
        for (uint32_t i = 0; i < 32 && 0x20 + 8 * i < n; i++) put(0x20 + 8 * i, (uint32_t)s.tex[stage][i]);
        for (uint32_t i = 0; i < 9 && 0x120 + 8 * i < n; i++) put(0x120 + 8 * i, (uint32_t)s.img[stage][i]);
        for (uint32_t i = 0; i < 128 && 0x168 + 8 * i < n; i++) put(0x168 + 8 * i, (uint32_t)s.tex[stage][i]);
        for (uint32_t i = 0; i < 128 && 0x568 + 8 * i < n; i++) put(0x568 + 8 * i, (uint32_t)s.samp[stage][i]);
        return;
    }
    if (bank != 0) return;
    if (const uint32_t base = kSsboTable[stage])
        for (uint32_t i = 0; i < 16 && base + 16 * i < n; i++) {
            const nvn_p_bind_buf& e = s.ssbo[stage][i];
            put(base + 16 * i, (uint32_t)e.addr), put(base + 16 * i + 4, (uint32_t)(e.addr >> 32)), put(base + 16 * i + 8, (uint32_t)e.size);
        }
    if (stage == 0) put(0x610, std::bit_cast<uint32_t>(2047.9375f));
}

void state_stats(char* out, size_t cap, double frames) {
    std::snprintf(out, cap, "state: deferred handle binds %.1f a frame, deferred buffer binds %.1f, render conditions %.1f",
                  st.take(ST_DEFERRED) / frames, st.take(ST_DEFERRED_BUFS) / frames, st.take(ST_CONDS) / frames);
}

}  // namespace hsd
