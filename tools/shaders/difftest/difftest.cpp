// difftest: the differential between two fronts of the translator (work/shader-plan section 3, D1-D3), each stage
// translated by front A and front B (seed.h: old or new) and compared; old against old is the harness's own check.
//
//   difftest micro <seed>... --out <micro.seed>      D1's programs: one compute stage per masked instruction encoding
//   difftest micro --insts <file> --out <micro.seed>  the same from a file of instructions (32 hex digits a line)
//   difftest d1 <micro.seed> [--inputs K]            each micro program run on edge-value inputs, A against B
//   difftest d2 <seed>                                Shader::Info of each stage, field by field
//   difftest d3 <seed> [--inputs K]                   each stage run in irexec with K input seeds, A against B
//   (d1/d3 --show: input 0's trace states as "# <side> ..." lines, a micro program's registers by name)
//   common: --a <front> --b <front> (default old, old), --first F --count C, --alias b=a (B's bank b read as A's a:
//   irexec's bank_alias and D2's bank numbers; --a old --b new means --alias 24=17 unless one is given)
// d1/d2/d3 print a JSON line a stage ("index", "a"/"b": "ok" or the error, "equal", the first difference); "@@stage <i>"
// goes to stderr before each (tools/shaders/difftest/difftest.py runs them sharded and sums them up).
//
// A micro program loads every register its instruction may name from c[1] (GPRs per lane, so 32 lanes are 32 operand
// tuples; uniform registers and both predicate files too), runs the instruction as found in the seed, and stores the
// same registers to shared memory (layout below); register windows are the 8 GPRs and 4 uniform registers (up to UR27)
// from each of the fields at bits 16, 24, 32 and 64, which over-covers (an immediate's low byte opens a window too).
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "../irexec/irexec.h"
#include "../seed.h"

namespace {
using u128 = unsigned __int128;

// ---- micro programs ----

// (c1 and shared memory: lane l's GPR n at l*kLane + 4n (n < 216), its P k at l*kLane + 0x360 + 4k; UR n at kUniform
// + 4n, UP k at kUniform + 0x100 + 4k)
constexpr uint32_t kLane = 0x380, kPreds = 0x360, kUniform = 0x7000, kUPreds = kUniform + 0x100, kC1Size = kUPreds + 0x20;
constexpr uint32_t kGprs = 216;   // (R216 up is the lowering's)
constexpr uint32_t kSharedSize = 0x7200;
constexpr uint32_t kTestAt = 0xd0;   // (the control section's word holding the index of the instruction under test)

u128 hex128(const char* s) {
    u128 x = 0;
    for (; *s; s++) {
        const char c = *s;
        if (c >= '0' && c <= '9') x = x << 4 | (u128)(c - '0');
        else if (c >= 'a' && c <= 'f') x = x << 4 | (u128)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') x = x << 4 | (u128)(c - 'A' + 10);
    }
    return x;
}
std::string hex(u128 x) {
    char b[33];
    std::snprintf(b, sizeof b, "%016" PRIx64 "%016" PRIx64, (uint64_t)(x >> 64), (uint64_t)x);
    return b;
}
uint32_t get(u128 x, int at, int n) { return (uint32_t)(x >> at) & (uint32_t)((1ull << n) - 1); }
u128 set(u128 x, int at, int n, uint64_t v) {
    const u128 m = (((u128)1 << n) - 1) << at;
    return (x & ~m) | (((u128)v << at) & m);
}

// (the scaffold's instructions: real encodings from the seeds, fields per NAK sm70_encode.rs)
namespace tpl {
const u128 ldc = hex128("000e6200000008000200000000067b82");     // LDC R, c[b][Ra + o]: Rd 16, Ra 24, o 38-53, b 54-58
const u128 uldc = hex128("000fe40000000a0000019c0000047ab9");    // ULDC.64 (B32 with 73-75 = 4)
const u128 s2r = hex128("000e2800000000000000000000057919");     // S2R R, SR_LANEID (SR at 72-79)
const u128 imad = hex128("000fca00078e02ffbb40e64d20207824");    // IMAD R, Ra, imm32, RZ
const u128 isetp = hex128("000fda0003f06070000000ff0300720c");   // ISETP.GE.U32.AND P, PT, Ra, RZ, PT (cmp 76-78)
const u128 uisetp = hex128("000fe4000bf052700000003f0600728c");  // UISETP.NE.U32.AND UP, UPT, URa, URZ, UPT
const u128 sel = hex128("000fe4000400000000000001ff0e7807");     // SEL R, RZ, 1, !P (pred 87-90)
const u128 usel = hex128("000fe400080000000000003004047887");    // USEL UR, URa, imm, UP
const u128 movu = hex128("000fe40008000f000000000500137c02");    // MOV R, UR (UR at 32-37)
const u128 sts = hex128("000fe200000008000000101201007388");     // STS [Ra + o], Rb: Ra 24, Rb 32, o 40-63
const u128 exit = hex128("000fec0003800000000000000000794d");    // EXIT
const u128 bra = hex128("000fea00038000000000231000000947");     // BRA (offset in words at 34-81)
}  // namespace tpl

u128 ldc(uint32_t rd, uint32_t ra, uint32_t off) {
    return set(set(set(set(tpl::ldc, 16, 8, rd), 24, 8, ra), 38, 16, off), 54, 5, 1);
}
u128 uldc(uint32_t urd, uint32_t off) { return set(set(set(set(tpl::uldc, 16, 6, urd), 38, 16, off), 54, 5, 1), 73, 3, 4); }
u128 s2r_lane(uint32_t rd) { return set(set(tpl::s2r, 16, 8, rd), 72, 8, 0); }
u128 imad_imm(uint32_t rd, uint32_t ra, uint32_t imm) { return set(set(set(tpl::imad, 16, 8, rd), 24, 8, ra), 32, 32, imm); }
u128 isetp_ne(uint32_t p, uint32_t ra) { return set(set(set(tpl::isetp, 76, 3, 5), 24, 8, ra), 81, 3, p); }
u128 uisetp_ne(uint32_t up, uint32_t ura) { return set(set(tpl::uisetp, 24, 6, ura), 81, 3, up); }
u128 sel_pred(uint32_t rd, uint32_t p) { return set(set(set(tpl::sel, 16, 8, rd), 87, 3, p), 90, 1, 1); }
u128 usel_pred(uint32_t urd, uint32_t up) {
    return set(set(set(set(set(tpl::usel, 16, 6, urd), 24, 6, 63), 32, 32, 1), 87, 3, up), 90, 1, 1);
}
u128 mov_ur(uint32_t rd, uint32_t ur) { return set(set(tpl::movu, 16, 8, rd), 32, 6, ur); }
u128 sts(uint32_t ra, uint32_t rb, uint32_t off) { return set(set(set(tpl::sts, 24, 8, ra), 32, 8, rb), 40, 24, off); }
u128 bra_self() { return set(set(set(tpl::bra, 12, 4, 7), 32, 2, 0), 34, 48, (uint64_t)-4 & ((1ull << 48) - 1)); }

/// Control flow and the ops that change the program's shape: not micro programs (D3 covers them)
bool control(uint32_t op) {
    return (op & 0xff0) == 0x940 || (op & 0xff0) == 0x950 || op == 0x355 || op == 0x356 || op == 0x357 || op == 0x957 || op == 0x34e;
}

/// The instruction's modifier encoding: guard, register, operand and scheduling fields cleared (opcodes.py's), with
/// whether each register field is RZ or URZ kept
u128 masked(u128 x) {
    const u128 operands = ((u128)0xf << 12) | ((u128)0xff << 16) | ((u128)0xff << 24) | ((u128)0xffffffffu << 32) | ((u128)0xff << 64) |
                          ((((u128)1 << 23) - 1) << 105);
    u128 k = x & ~operands;
    int bit = 105;
    for (int at : {16, 24, 32, 64}) {
        k = set(k, bit++, 1, get(x, at, 8) == 255);
        k = set(k, bit++, 1, get(x, at, 6) == 63);
    }
    return k;
}

struct Micro {
    std::vector<u128> insts;
    uint32_t test = 0;   // (index of the instruction under test)
};
Micro micro(u128 x) {
    std::set<uint32_t> gprs, urs;
    for (int at : {16, 24, 32, 64}) {
        const uint32_t r = get(x, at, 8), u = get(x, at, 6);
        for (uint32_t n = r; r != 255 && n < r + 8; n++)
            if (n < kGprs) gprs.insert(n);
        for (uint32_t n = u; u != 63 && n < u + 4; n++)
            if (n < 28) urs.insert(n);
    }
    // (the lowering parks UR28 up in the GPRs below R216: uniform windows stop at UR27, the temporaries stay below R160)
    std::vector<uint32_t> temps;
    for (uint32_t n = 159; temps.size() < 2; n--)
        if (!gprs.count(n)) temps.push_back(n);
    uint32_t urt = 0;
    while (urs.count(urt)) urt++;
    const uint32_t rt = temps[0], ru = temps[1];
    Micro m;
    auto& v = m.insts;
    v.push_back(s2r_lane(rt));
    v.push_back(imad_imm(rt, rt, kLane));
    for (uint32_t k = 0; k < 7; k++) v.push_back(ldc(ru, rt, kPreds + 4 * k)), v.push_back(isetp_ne(k, ru));
    for (uint32_t k = 0; k < 7; k++) v.push_back(uldc(urt, kUPreds + 4 * k)), v.push_back(uisetp_ne(k, urt));
    for (uint32_t n : urs) v.push_back(uldc(n, kUniform + 4 * n));
    for (uint32_t n : gprs) v.push_back(ldc(n, rt, 4 * n));
    m.test = (uint32_t)v.size();
    v.push_back(x);
    v.push_back(s2r_lane(rt));
    v.push_back(imad_imm(rt, rt, kLane));
    for (uint32_t n : gprs) v.push_back(sts(rt, n, 4 * n));
    for (uint32_t k = 0; k < 7; k++) v.push_back(sel_pred(ru, k)), v.push_back(sts(rt, ru, kPreds + 4 * k));
    for (uint32_t n : urs) v.push_back(mov_ur(ru, n)), v.push_back(sts(255, ru, kUniform + 4 * n));
    for (uint32_t k = 0; k < 7; k++) v.push_back(usel_pred(urt, k)), v.push_back(mov_ur(ru, urt)), v.push_back(sts(255, ru, kUPreds + 4 * k));
    v.push_back(tpl::exit);
    v.push_back(bra_self());
    return m;
}

/// The micro program as a compute stage: a control section (offline::load's fields) and the code area
seed::Written micro_stage(u128 x) {
    const Micro m = micro(x);
    seed::Written w;
    w.stage = 5;
    w.ctl.assign(offline::kControl, 0);
    const uint32_t len = (uint32_t)m.insts.size() * 16, c1o = len;   // (c1 empty here: each run gives its own)
    const uint16_t kind = 5;
    const uint32_t local[3] = {32, 1, 1}, shared = kSharedSize;
    std::memcpy(&w.ctl[0x38], &len, 4), std::memcpy(&w.ctl[0x40], &c1o, 4), std::memcpy(&w.ctl[0x56], &kind, 2);
    std::memcpy(&w.ctl[0x58], local, 12), std::memcpy(&w.ctl[0x64], &shared, 4), std::memcpy(&w.ctl[kTestAt], &m.test, 4);
    w.code.assign(len, 0);
    std::memcpy(w.code.data(), m.insts.data(), len);
    return w;
}

/// Input k's c1: edge values (0, -0, INT_MIN/MAX, NaNs, infinities, denormals, f16 pairs) spread over lanes and
/// registers so each pair of operands meets in several combinations; inputs from 6 up are random words
std::vector<uint8_t> micro_c1(uint32_t k) {
    static constexpr uint32_t edges[] = {
        0x00000000, 0x80000000, 0x7fffffff, 0xffffffff, 0x00000001, 0x7fc00000, 0xffc00000, 0x7f800001, 0x7f800000, 0xff800000,
        0x007fffff, 0x807fffff, 0x00800000, 0x3f800000, 0xbf800000, 0x3f000000, 0x4b000000, 0x4f000000, 0xcf000000, 0x7f7fffff,
        0x7c00fc00, 0x7e000001, 0x3c00bc00, 0x80008000, 0x0000ffff, 0x00010000, 0x0000001f, 0x00000020, 0x12345678, 0x9abcdef0};
    constexpr uint32_t n = sizeof edges / sizeof edges[0];
    static constexpr uint32_t stride[] = {1, 7, 11, 13, 17, 3};
    std::vector<uint8_t> c1(kC1Size);
    for (uint32_t w = 0; w < kC1Size / 4; w++) {
        const uint32_t lane = w * 4 < kUniform ? w * 4 / kLane : 0, slot = w * 4 < kUniform ? w % (kLane / 4) : w;
        uint32_t v;
        if (k < 6) {
            v = edges[(lane * 7 + slot * (stride[k] + lane) + k) % n];   // (the step between two registers varies by lane)
        } else {
            uint64_t z = ((uint64_t)k << 32 | w) * 0x9e3779b97f4a7c15ull;
            z = (z ^ (z >> 29)) * 0xbf58476d1ce4e5b9ull;
            v = (uint32_t)(z ^ (z >> 32));
        }
        std::memcpy(&c1[w * 4], &v, 4);
    }
    return c1;
}

/// A shared-memory byte address of a micro program as the register it holds (trace items are word addresses)
std::string micro_name(uint64_t addr) {
    if (addr >= kUPreds) return fmt::format("UP{}", (addr - kUPreds) / 4);
    if (addr >= kUniform) return fmt::format("UR{}", (addr - kUniform) / 4);
    const uint64_t lane = addr / kLane, at = addr % kLane;
    return at >= kPreds ? fmt::format("P{} lane {}", (at - kPreds) / 4, lane) : fmt::format("R{} lane {}", at / 4, lane);
}

// ---- comparing traces ----

std::string json(const std::string& s) {
    std::string o = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if ((unsigned char)c < 0x20) o += fmt::format("\\u{:04x}", (int)c);
        else o += c;
    }
    return o + "\"";
}

/// A trace state value as its items ("key=value" words; a value without '=' is one item), in address order
struct Shorter {   // (hex without leading zeros: shorter is smaller)
    bool operator()(const std::string& x, const std::string& y) const { return x.size() != y.size() ? x.size() < y.size() : x < y; }
};
using Items = std::map<std::string, std::string, Shorter>;
Items items(const std::string& v) {
    Items out;
    std::istringstream in(v);
    std::string w;
    while (in >> w) {
        const size_t eq = w.find('=');
        if (eq == std::string::npos) out[w] = "";
        else out[w.substr(0, eq)] = w.substr(eq + 1);
    }
    return out;
}

struct Difference {
    std::string key, item, a, b;
    size_t items = 0;   // (items of that key that differ)
};
/// The first state key the traces differ in, and in it the first item
std::optional<Difference> compare(const irexec::Trace& a, const irexec::Trace& b) {
    std::set<std::string> keys;
    for (const auto& [k, _] : a.state) keys.insert(k);
    for (const auto& [k, _] : b.state) keys.insert(k);
    for (const auto& k : keys) {
        const auto ia = a.state.find(k), ib = b.state.find(k);
        const std::string va = ia == a.state.end() ? std::string("<none>") : ia->second, vb = ib == b.state.end() ? std::string("<none>") : ib->second;
        if (va == vb) continue;
        Difference d;
        d.key = k;
        const auto ma = items(va), mb = items(vb);
        std::set<std::string, Shorter> all;
        for (const auto& [x, _] : ma) all.insert(x);
        for (const auto& [x, _] : mb) all.insert(x);
        for (const auto& x : all) {
            const auto xa = ma.find(x), xb = mb.find(x);
            const std::string sa = xa == ma.end() ? "<none>" : xa->second, sb = xb == mb.end() ? "<none>" : xb->second;
            if (sa == sb) continue;
            if (!d.items++) d.item = x, d.a = sa, d.b = sb;
        }
        return d;
    }
    return std::nullopt;
}

// ---- Info, field by field ----

using Fields = std::map<std::string, std::string>;
struct Banks {
    std::map<uint32_t, uint32_t> alias;
    uint32_t operator()(uint32_t b) const {
        const auto it = alias.find(b);
        return it == alias.end() ? b : it->second;
    }
};
template <size_t N>
std::string bits(const std::bitset<N>& b) {
    std::string s;
    for (size_t i = 0; i < N; i++)
        if (b[i]) s += fmt::format("{} ", i);
    return s;
}
Fields info_fields(const Shader::IR::Program& p, const Banks& bank) {
    const Shader::Info& i = p.info;
    Fields f;
    const auto put = [&](const std::string& k, auto v) { f[k] = fmt::format("{}", v); };
#define B(x) put(#x, (int)i.x)
    B(is_task), B(is_mesh), B(uses_workgroup_id), B(uses_num_workgroups), B(uses_local_invocation_id), B(uses_invocation_id);
    B(uses_invocation_info), B(uses_sample_id), B(uses_is_helper_invocation), B(uses_subgroup_invocation_id), B(uses_subgroup_shuffles);
    B(uses_quad_shuffles), B(loads_indexed_attributes), B(stores_sample_mask), B(stores_frag_depth), B(stores_tess_level_outer);
    B(stores_tess_level_inner), B(stores_indexed_attributes), B(stores_global_memory), B(native_global_bounds), B(uses_local_memory);
    B(uses_fp16), B(uses_cooperative_matrix), B(uses_fp64), B(uses_fp16_denorms_flush), B(uses_fp16_denorms_preserve);
    B(uses_fp32_denorms_flush), B(uses_fp32_denorms_preserve), B(uses_int8), B(uses_int16), B(uses_int64), B(uses_image_1d);
    B(uses_sampled_1d), B(uses_sparse_residency), B(uses_demote_to_helper_invocation), B(uses_subgroup_vote), B(uses_subgroup_mask);
    B(uses_subgroup_reduce), B(maximal_reconvergence), B(uses_fswzadd), B(uses_derivatives), B(uses_typeless_image_reads);
    B(uses_typeless_image_writes), B(uses_image_buffers), B(uses_shared_increment), B(uses_shared_decrement), B(uses_global_increment);
    B(uses_global_decrement), B(uses_atomic_f32_add), B(uses_atomic_f16x2_add), B(uses_atomic_f16x2_min), B(uses_atomic_f16x2_max);
    B(uses_atomic_f32x2_add), B(uses_atomic_f32x2_min), B(uses_atomic_f32x2_max), B(uses_atomic_s32_min), B(uses_atomic_s32_max);
    B(uses_int64_bit_atomics), B(uses_global_memory), B(uses_atomic_image_u32), B(uses_shadow_lod), B(uses_rescaling_uniform);
    B(uses_cbuf_indirect), B(uses_render_area), B(ssbo_bda), B(bda_why), B(used_constant_buffer_types), B(used_storage_buffer_types);
    B(used_indirect_cbuf_types), B(nvn_buffer_base), B(num_global_tables), B(requires_layer_emulation), B(emulated_layer);
    B(used_clip_distances);
#undef B
    for (size_t k = 0; k < i.uses_patches.size(); k++)
        if (i.uses_patches[k]) put(fmt::format("uses_patches[{}]", k), 1);
    for (size_t k = 0; k < i.interpolation.size(); k++)
        if (i.interpolation[k] != Shader::Interpolation::Smooth) put(fmt::format("interpolation[{}]", k), (int)i.interpolation[k]);
    put("loads", bits(i.loads.mask)), put("stores", bits(i.stores.mask)), put("passthrough", bits(i.passthrough.mask));
    for (const auto& [a, b] : i.legacy_stores_mapping) put(fmt::format("legacy_stores_mapping[{}]", (int)a), (int)b);
    for (size_t k = 0; k < 8; k++)
        if (i.stores_frag_color[k] || i.frag_color_components[k])
            put(fmt::format("frag_color[{}]", k), fmt::format("{} {:x}", (int)i.stores_frag_color[k], i.frag_color_components[k]));
    uint32_t mask = 0;
    for (uint32_t b = 0; b < 32; b++)
        if (i.constant_buffer_mask >> b & 1) mask |= 1u << bank(b);
    put("constant_buffer_mask", fmt::format("{:x}", mask));
    for (uint32_t b = 0; b < i.constant_buffer_used_sizes.size(); b++)
        if (i.constant_buffer_used_sizes[b]) put(fmt::format("constant_buffer_used_sizes[{}]", bank(b)), i.constant_buffer_used_sizes[b]);
    for (uint32_t k = 0; k < i.num_global_tables && k < i.global_tables.size(); k++)
        put(fmt::format("global_tables[{}]", k), fmt::format("{} {:x}", bank(i.global_tables[k].first), i.global_tables[k].second));
    put("nvn_buffer_used", i.nvn_buffer_used.to_string());
    const auto list = [&](const char* name, const auto& v, auto&& one) {
        put(fmt::format("{}.size", name), v.size());
        for (size_t k = 0; k < v.size(); k++) one(fmt::format("{}[{}].", name, k), v[k]);
    };
    list("constant_buffer_descriptors", i.constant_buffer_descriptors, [&](const std::string& n, const auto& d) {
        put(n + "index", bank(d.index)), put(n + "count", d.count);
    });
    list("storage_buffers_descriptors", i.storage_buffers_descriptors, [&](const std::string& n, const auto& d) {
        put(n + "cbuf", fmt::format("{} {:x}", bank(d.cbuf_index), d.cbuf_offset)), put(n + "count", d.count), put(n + "is_written", (int)d.is_written);
    });
    const auto handle = [&](const std::string& n, const auto& d) {
        put(n + "cbuf", fmt::format("{} {:x} {}", bank(d.cbuf_index), d.cbuf_offset, d.shift_left));
        if (d.has_secondary) put(n + "secondary", fmt::format("{} {:x} {}", bank(d.secondary_cbuf_index), d.secondary_cbuf_offset, d.secondary_shift_left));
        put(n + "count", fmt::format("{} {}", d.count, d.size_shift)), put(n + "handle_program", d.handle_program);
    };
    list("texture_buffer_descriptors", i.texture_buffer_descriptors, handle);
    list("image_buffer_descriptors", i.image_buffer_descriptors, [&](const std::string& n, const auto& d) {
        put(n + "cbuf", fmt::format("{} {:x}", bank(d.cbuf_index), d.cbuf_offset));
        put(n + "kind", fmt::format("{} {} {} {}", (int)d.format, (int)d.is_written, (int)d.is_read, (int)d.is_integer));
        put(n + "count", fmt::format("{} {}", d.count, d.size_shift)), put(n + "handle_program", d.handle_program);
    });
    list("texture_descriptors", i.texture_descriptors, [&](const std::string& n, const auto& d) {
        handle(n, d);
        put(n + "kind", fmt::format("{} {} {} {} {}", (int)d.type, (int)d.is_depth, (int)d.is_multisample, (int)d.is_integer, (int)d.type_fixed));
    });
    list("image_descriptors", i.image_descriptors, [&](const std::string& n, const auto& d) {
        put(n + "cbuf", fmt::format("{} {:x}", bank(d.cbuf_index), d.cbuf_offset));
        put(n + "kind", fmt::format("{} {} {} {} {}", (int)d.type, (int)d.format, (int)d.is_written, (int)d.is_read, (int)d.is_integer));
        put(n + "count", fmt::format("{} {}", d.count, d.size_shift)), put(n + "handle_program", d.handle_program);
    });
    put("handle_programs.size", i.handle_programs.size());
    for (size_t k = 0; k < i.handle_programs.size(); k++) {
        std::string s;
        for (const uint32_t w : i.handle_programs[k]) s += fmt::format("{:x} ", w);
        put(fmt::format("handle_programs[{}]", k), s);
    }
    put("program.workgroup_size", fmt::format("{} {} {}", p.workgroup_size[0], p.workgroup_size[1], p.workgroup_size[2]));
    put("program.output", fmt::format("{} {} {}", (int)p.output_topology, p.output_vertices, p.invocations));
    put("program.memory", fmt::format("local {} shared {}", p.local_memory_size, p.shared_memory_size));
    put("program.geometry_passthrough", (int)p.is_geometry_passthrough);
    put("program.mesh", fmt::format("{} {} {} {}", p.mesh.payload_words, p.mesh.max_vertices, p.mesh.max_primitives, p.mesh.topology));
    return f;
}

// ---- the modes ----

struct Run {
    const seed::Front* a = &seed::kOld;
    const seed::Front* b = &seed::kOld;
    irexec::Options options_b;   // (B's bank_alias)
    Banks banks_b;
    int first = 0, count = 1 << 30, inputs = -1;   // (inputs: d1 8, d3 3 unless given)
    bool show = false;                             // (--show: input 0's states, a micro program's as registers)
};

struct Side {
    seed::Translated t;
    std::shared_ptr<irexec::Compiled> c;
    std::string error;
};
Side make(const seed::Seed& s, int id, const seed::Front& f, const irexec::Options* opt) {
    Side x;
    try {
        x.t = seed::translate(s, id, f);
        if (opt) x.c = irexec::compile(x.t.t->program, *opt);
    } catch (const std::exception& e) {
        x.error = e.what();
        if (x.error.empty()) x.error = "error";
    }
    return x;
}
std::string status(const Side& x) { return x.error.empty() ? "ok" : x.error; }

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

/// d1 and d3: both sides run on each input; the first difference, or why the runs are not comparable
int runs(const seed::Seed& s, const Run& r, bool micro_mode) {
    const int end = std::min<int>(r.first + r.count, (int)s.stages.size());
    const irexec::Options none;
    std::vector<std::vector<uint8_t>> c1s;
    if (micro_mode)
        for (int k = 0; k < r.inputs; k++) c1s.push_back(micro_c1((uint32_t)k));
    for (int id = r.first; id < end; id++) {
        std::fprintf(stderr, "@@stage %d\n", id);
        const seed::Stage& st = s.stages[(size_t)id];
        const auto t0 = std::chrono::steady_clock::now();
        Side a = make(s, id, *r.a, &none), b = make(s, id, *r.b, &r.options_b);
        std::string line = fmt::format("{{\"index\": {}, \"key\": \"{:016x}\", \"kind\": {}", id, st.key(), st.stage);
        if (micro_mode) {
            uint32_t at;
            std::memcpy(&at, st.ctl + kTestAt, 4);
            u128 x;
            std::memcpy(&x, st.code + 16 * at, 16);
            line += fmt::format(", \"op\": \"{:03x}\", \"inst\": \"{}\"", get(x, 0, 12), hex(x));
        }
        line += fmt::format(", \"a\": {}, \"b\": {}", json(status(a)), json(status(b)));
        if (a.error.empty() && b.error.empty()) {
            int capped = 0, undefined = 0, compared = 0;
            std::optional<Difference> first;
            int first_input = -1;
            std::string run_error;
            for (int k = 0; k < r.inputs && run_error.empty(); k++) {
                irexec::Inputs in;
                in.seed = (uint64_t)k;
                in.c1 = micro_mode ? &c1s[(size_t)k] : &a.t.code.c1;
                irexec::Trace ta, tb;
                try {
                    ta = irexec::run(*a.c, in);
                    in.c1 = micro_mode ? &c1s[(size_t)k] : &b.t.code.c1;
                    tb = irexec::run(*b.c, in);
                } catch (const std::exception& e) {
                    run_error = e.what();
                    break;
                }
                if (ta.capped || tb.capped) {
                    capped++;
                    continue;
                }
                if (ta.undefined_reads || tb.undefined_reads) undefined++;
                compared++;
                if (r.show && k == 0)
                    for (const auto* t : {&ta, &tb})
                        for (const auto& [key, v] : t->state) {
                            if (!micro_mode || key != "mem.shared") {
                                std::printf("# %s %s %s\n", t == &ta ? "a" : "b", key.c_str(), v.c_str());
                                continue;
                            }
                            for (const auto& [addr, w] : items(v))
                                std::printf("# %s %s = %s\n", t == &ta ? "a" : "b", micro_name(4 * std::stoull(addr, nullptr, 16)).c_str(), w.c_str());
                        }
                if (first) continue;
                if (auto d = compare(ta, tb)) first = d, first_input = k;
            }
            line += fmt::format(", \"compared\": {}, \"capped\": {}, \"undefined\": {}", compared, capped, undefined);
            if (!run_error.empty()) line += fmt::format(", \"run_error\": {}", json(run_error));
            else line += fmt::format(", \"equal\": {}", first ? "false" : "true");
            if (first) {
                std::string item = first->item;
                if (micro_mode && first->key == "mem.shared") item = micro_name(4 * std::stoull(item, nullptr, 16));
                line += fmt::format(", \"diff\": {{\"input\": {}, \"key\": {}, \"item\": {}, \"a\": {}, \"b\": {}, \"items\": {}}}", first_input,
                                    json(first->key), json(item), json(first->a.substr(0, 200)), json(first->b.substr(0, 200)), first->items);
            }
        }
        line += fmt::format(", \"ms\": {:.1f}}}", ms_since(t0));
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }
    return 0;
}

int d2(const seed::Seed& s, const Run& r) {
    const int end = std::min<int>(r.first + r.count, (int)s.stages.size());
    for (int id = r.first; id < end; id++) {
        std::fprintf(stderr, "@@stage %d\n", id);
        const seed::Stage& st = s.stages[(size_t)id];
        Side a = make(s, id, *r.a, nullptr), b = make(s, id, *r.b, nullptr);
        std::string line = fmt::format("{{\"index\": {}, \"key\": \"{:016x}\", \"kind\": {}, \"a\": {}, \"b\": {}", id, st.key(), st.stage,
                                       json(status(a)), json(status(b)));
        if (a.error.empty() && b.error.empty()) {
            const Fields fa = info_fields(a.t.t->program, Banks{}), fb = info_fields(b.t.t->program, r.banks_b);
            std::set<std::string> keys;
            for (const auto& [k, _] : fa) keys.insert(k);
            for (const auto& [k, _] : fb) keys.insert(k);
            std::string diffs;
            for (const auto& k : keys) {
                const auto ia = fa.find(k), ib = fb.find(k);
                const std::string va = ia == fa.end() ? "<none>" : ia->second, vb = ib == fb.end() ? "<none>" : ib->second;
                if (va == vb) continue;
                diffs += fmt::format("{}{}: [{}, {}]", diffs.empty() ? "" : ", ", json(k), json(va.substr(0, 200)), json(vb.substr(0, 200)));
            }
            line += fmt::format(", \"fields\": {}, \"equal\": {}, \"diffs\": {{{}}}", fa.size(), diffs.empty() ? "true" : "false", diffs);
        }
        std::printf("%s}\n", line.c_str());
        std::fflush(stdout);
    }
    return 0;
}

/// micro: the masked encodings of the seeds' instructions (each program up to its end, as offline::load reads it), the
/// first instance of each whose register fields lie below R192 (else the first) as its micro program; a listing
/// "<index> <opcode> <masked> <instruction> <instructions> <stages> <kinds>" goes beside the micro seed (.tsv)
int make_micro(const std::vector<std::string>& seeds, const std::string& insts_file, const std::string& out) {
    struct Enc {
        u128 rep = 0;
        bool low = false;
        uint64_t n = 0, stages = 0;
        std::set<uint32_t> kinds;
        size_t order = 0;
    };
    std::map<u128, Enc> encs;
    const auto low = [](u128 x) {
        for (int at : {16, 24, 32, 64})
            if (get(x, at, 8) >= 192 && get(x, at, 8) != 255) return false;
        return true;
    };
    size_t control_n = 0;
    const auto add = [&](u128 x, uint32_t kind, std::set<u128>& here) {
        if (control(get(x, 0, 12))) return control_n++, void();
        const u128 k = masked(x);
        Enc& e = encs[k];
        if (!e.n) e.rep = x, e.low = low(x), e.order = encs.size();
        else if (!e.low && low(x)) e.rep = x, e.low = true;
        e.n++, e.kinds.insert(kind);
        if (here.insert(k).second) e.stages++;
    };
    for (const auto& path : seeds) {
        seed::Seed s;
        if (!seed::read(path.c_str(), s)) return std::fprintf(stderr, "cannot read the seed %s\n", path.c_str()), 1;
        for (const seed::Stage& st : s.stages) {
            const uint32_t hdr = st.stage == 5 ? 0 : offline::kAmpereHeader;
            uint32_t len;
            std::memcpy(&len, st.ctl + 0x38, 4);
            if (st.stage != 5 && len <= hdr + 16) continue;
            Shader::Ampere::ProgramCaptureState cap;
            std::set<u128> here;
            for (uint32_t i = 0; hdr + (i + 1) * 16 <= len && hdr + (i + 1) * 16 <= st.code_n; i++) {
                Shader::Ampere::Instruction insn;
                std::memcpy(&insn, st.code + hdr + i * 16, 16);
                u128 x;
                std::memcpy(&x, &insn, 16);
                add(x, st.stage, here);
                const auto res = cap.Observe(insn, i);
                if (res != Shader::Ampere::ProgramCaptureState::Result::Continue) break;
            }
        }
    }
    if (!insts_file.empty()) {
        FILE* f = std::fopen(insts_file.c_str(), "r");
        if (!f) return std::fprintf(stderr, "cannot read %s\n", insts_file.c_str()), 1;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            if (line[0] == '#' || std::strlen(line) < 32) continue;
            std::set<u128> here;
            add(hex128(std::string(line, 32).c_str()), 5, here);
        }
        std::fclose(f);
    }
    std::vector<const Enc*> order;
    for (const auto& [k, e] : encs) order.push_back(&e);
    std::sort(order.begin(), order.end(), [](const Enc* x, const Enc* y) {
        const uint32_t ox = get(x->rep, 0, 12), oy = get(y->rep, 0, 12);
        return ox != oy ? ox < oy : x->n != y->n ? x->n > y->n : x->order < y->order;
    });
    std::vector<seed::Written> stages;
    FILE* tsv = std::fopen((out + ".tsv").c_str(), "w");
    if (!tsv) return std::fprintf(stderr, "cannot write %s.tsv\n", out.c_str()), 1;
    for (size_t i = 0; i < order.size(); i++) {
        const Enc& e = *order[i];
        stages.push_back(micro_stage(e.rep));
        std::string kinds;
        for (uint32_t k : e.kinds) kinds += fmt::format("{}{}", kinds.empty() ? "" : ",", k);
        std::fprintf(tsv, "%zu\t%03x\t%s\t%s\t%" PRIu64 "\t%" PRIu64 "\t%s\n", i, get(e.rep, 0, 12), hex(masked(e.rep)).c_str(), hex(e.rep).c_str(),
                     e.n, e.stages, kinds.c_str());
    }
    std::fclose(tsv);
    if (!seed::write(out.c_str(), stages)) return std::fprintf(stderr, "cannot write %s\n", out.c_str()), 1;
    std::printf("%zu encodings (%zu control flow instructions left out) -> %s\n", order.size(), control_n, out.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return std::fprintf(stderr, "difftest micro|d1|d2|d3 <seed>... [options] (see difftest.cpp)\n"), 2;
    const std::string mode = argv[1];
    std::vector<std::string> paths;
    std::string out, insts;
    Run r;
    bool alias_given = false;
    try {
        for (int i = 2; i < argc; i++) {
            const std::string a = argv[i];
            const bool more = i + 1 < argc;
            if (a == "--a" && more) r.a = &seed::front(argv[++i]);
            else if (a == "--b" && more) r.b = &seed::front(argv[++i]);
            else if (a == "--first" && more) r.first = std::atoi(argv[++i]);
            else if (a == "--count" && more) r.count = std::atoi(argv[++i]);
            else if (a == "--inputs" && more) r.inputs = std::atoi(argv[++i]);
            else if (a == "--out" && more) out = argv[++i];
            else if (a == "--show") r.show = true;
            else if (a == "--insts" && more) insts = argv[++i];
            else if (a == "--alias" && more) {
                const std::string v = argv[++i];
                const size_t eq = v.find('=');
                if (eq == std::string::npos) return std::fprintf(stderr, "--alias <b>=<a>\n"), 2;
                const uint32_t from = (uint32_t)std::stoul(v.substr(0, eq)), to = (uint32_t)std::stoul(v.substr(eq + 1));
                r.options_b.bank_alias[from] = to, r.banks_b.alias[from] = to, alias_given = true;
            } else if (a.rfind("--", 0) == 0) return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
            else paths.push_back(a);
        }
        if (!alias_given && r.a == &seed::kOld && r.b == &seed::kNew) r.options_b.bank_alias[24] = 17, r.banks_b.alias[24] = 17;
        if (mode == "micro") {
            if (out.empty()) return std::fprintf(stderr, "micro: --out <micro.seed>\n"), 2;
            return make_micro(paths, insts, out);
        }
        if (paths.size() != 1) return std::fprintf(stderr, "%s: one seed\n", mode.c_str()), 2;
        seed::Seed s;
        if (!seed::read(paths[0].c_str(), s)) return std::fprintf(stderr, "cannot read the seed %s\n", paths[0].c_str()), 1;
        if (r.inputs < 0) r.inputs = mode == "d1" ? 8 : 3;
        if (mode == "d1") return runs(s, r, true);
        if (mode == "d2") return d2(s, r);
        if (mode == "d3") return runs(s, r, false);
        return std::fprintf(stderr, "unknown mode %s\n", mode.c_str()), 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
