// Late scheduling of a translated stage's SPIR-V (direct_spv_late.h): the translator's register-pressure pass. Stands alone:
// tools/meshlab/spvlate.cpp builds it into a command, tests/test_direct_spv_late.py checks it, tools/meshlab/late_gate.py
// holds RADV's compile of every stage against it.
#include "direct_spv_late.h"

#include <algorithm>
#include <cstdio>

namespace hsd {
namespace {

struct Inst {
    uint32_t op, at, wc;
};

constexpr uint32_t kNone = ~0u, kPhysical = 5349, kFunctionClass = 7, kImmortal = 1u << 20;

bool pure(uint32_t op) {
    // (OpExtInst, the access chains, composites and copies; conversions; arithmetic; logic and comparisons; shifts and bits)
    return op == 12 || op == 65 || op == 66 || op == 67 || (op >= 77 && op <= 83) || (op >= 109 && op <= 124) || (op >= 126 && op <= 152) ||
           (op >= 154 && op <= 191) || (op >= 194 && op <= 205);
}
bool atomic(uint32_t op) { return op >= 227 && op <= 242; }
bool subgroup(uint32_t op) { return op >= 333 && op <= 366; }           // (OpGroupNonUniform...)
bool read_only_class(uint32_t c) { return c == 0 || c == 1 || c == 2 || c == 9; }   // (UniformConstant, Input, Uniform, PushConstant)
/// (the instructions of a function body that make no value: stores, image writes, emits, barriers, merges, labels,
/// terminators, lifetimes, lines; every other one has its type at word 1 and its result at word 2)
bool has_result(uint32_t op) {
    switch (op) {
    case 0: case 8: case 317: case 62: case 63: case 64: case 99: case 218: case 219: case 220: case 221: case 224: case 225: case 228:
    case 246: case 247: case 248: case 249: case 250: case 251: case 252: case 253: case 254: case 255: case 256: case 257: case 318:
    case 4416: case 5380: case 5294: case 5295: case 54: case 56:
        return false;
    default:
        return true;
    }
}
/// (what a function may hold and still only compute: labels, phis, variables, merges, branches, returns, lines)
bool plain_control(uint32_t op) {
    switch (op) {
    case 0: case 8: case 317: case 54: case 55: case 56: case 59: case 245: case 246: case 247: case 248: case 249: case 250: case 251:
    case 253: case 254: case 255:
        return true;
    default:
        return false;
    }
}
/// May word j of an instruction be an id? A literal taken for an id only adds an ordering constraint or a use; an id taken
/// for a literal would lose one: so every word not known to be a literal is an id
bool maybe_id(uint32_t op, uint32_t j) {
    switch (op) {
    case 81: case 61: return j == 3;              // (OpCompositeExtract, OpLoad)
    case 79: case 82: return j == 3 || j == 4;    // (OpVectorShuffle, OpCompositeInsert)
    case 62: return j == 1 || j == 2;             // (OpStore)
    case 12: return j != 4;                       // (OpExtInst: the instruction's number)
    case 250: case 251: return j == 1;            // (OpBranchConditional, OpSwitch: labels are not values)
    case 246: case 247: case 249: return false;
    default: return true;
    }
}

struct Func {
    uint32_t id = 0;
    size_t first = 0, last = 0;
    bool effects = false;          // (it does more than compute: writes, atomics, barriers, outputs, ends an invocation)
    bool reads_physical = false;   // (it reads memory by address)
    bool reads_other = false;      // (it reads memory the stage can write, other than by address)
    std::vector<uint32_t> calls;
};

struct Block {
    uint32_t first = 0, last = 0;   // (instructions, inclusive)
    uint32_t label = 0;
    std::vector<uint32_t> succ;
};

}  // namespace

uint32_t spv_schedule_late(std::vector<uint32_t>& words, LateResult* result, const LateOptions& options) {
    if (result) *result = {};
    if (words.size() < 5 || words[0] != 0x07230203) return 0;
    const uint32_t bound = words[3];
    if (bound == 0 || bound > (1u << 24)) return 0;
    std::vector<Inst> insts;
    for (size_t i = 5; i < words.size();) {
        const uint32_t wc = words[i] >> 16, op = words[i] & 0xffff;
        if (wc == 0 || i + wc > words.size()) return 0;
        insts.push_back({op, (uint32_t)i, wc});
        i += wc;
    }
    const uint32_t* w = words.data();
    // (by id: a pointer type's storage class, a type's 32-bit values, a value's or variable's type)
    std::vector<uint32_t> pointer_class(bound, kNone), width(bound, 0), type_of(bound, 0);
    bool ends = false;   // (the module can demote or end an invocation)
    {
        bool inside = false;
        for (const Inst& x : insts) {
            const uint32_t* v = w + x.at;
            if (x.wc >= 2 && v[1] < bound) {
                if (x.op == 32 && x.wc >= 4) pointer_class[v[1]] = v[2];
                else if ((x.op == 21 || x.op == 22) && x.wc >= 3) width[v[1]] = std::max(1u, v[2] / 32);
                else if (x.op == 20) width[v[1]] = 1;
                else if (x.op == 23 && x.wc >= 4 && v[2] < bound) width[v[1]] = std::max(1u, width[v[2]]) * v[3];
            }
            if (x.op == 54) inside = true;
            else if (x.op == 56) inside = false;
            else if (((inside && has_result(x.op)) || x.op == 59) && x.wc >= 3 && v[2] < bound) type_of[v[2]] = v[1];
            if (x.op == 252 || x.op == 4416 || x.op == 5380) ends = true;
        }
    }
    const auto class_of = [&](uint32_t pointer) {   // (the storage class a pointer value points into)
        if (pointer >= bound) return kNone;
        const uint32_t t = type_of[pointer];
        return t < bound ? pointer_class[t] : kNone;
    };
    const auto width_of = [&](uint32_t type) { return type < bound && width[type] ? width[type] : 1u; };
    // (the functions: which only compute, which read by address, themselves or through a call)
    std::vector<Func> funcs;
    std::vector<uint32_t> func_of(bound, kNone);
    {
        Func* cur = nullptr;
        for (size_t n = 0; n < insts.size(); n++) {
            const Inst& x = insts[n];
            const uint32_t* v = w + x.at;
            if (x.op == 54) {
                if (cur || x.wc < 3 || v[2] >= bound) return 0;
                func_of[v[2]] = (uint32_t)funcs.size();
                funcs.emplace_back();
                cur = &funcs.back();
                cur->id = v[2], cur->first = n;
            } else if (x.op == 56) {
                if (!cur) return 0;
                cur->last = n;
                cur = nullptr;
            } else if (cur) {
                if (x.op == 57 && x.wc >= 4) {
                    cur->calls.push_back(v[3]);
                } else if (x.op == 61 && x.wc >= 4) {
                    const uint32_t cls = class_of(v[3]);
                    if (cls == kPhysical) cur->reads_physical = true;
                    else if (!read_only_class(cls) && cls != kFunctionClass) cur->reads_other = true;
                } else if (x.op == 62 && x.wc >= 3) {
                    if (class_of(v[1]) != kFunctionClass) cur->effects = true;
                } else if ((pure(x.op) && !(x.op == 12 && x.wc >= 5 && (v[4] == 35 || v[4] == 51))) || subgroup(x.op) || plain_control(x.op)) {
                    // (computes only; GLSL.std.450's Modf and Frexp write through a pointer)
                } else {
                    cur->effects = cur->reads_other = cur->reads_physical = true;
                }
            }
        }
        if (cur) return 0;
        for (bool changed = true; changed;) {
            changed = false;
            for (Func& f : funcs)
                for (const uint32_t c : f.calls) {
                    const uint32_t g = c < bound ? func_of[c] : kNone;
                    const bool e = g == kNone || funcs[g].effects, p = g == kNone || funcs[g].reads_physical, o = g == kNone || funcs[g].reads_other;
                    if ((e && !f.effects) || (p && !f.reads_physical) || (o && !f.reads_other)) changed = true;
                    f.effects |= e, f.reads_physical |= p, f.reads_other |= o;
                }
        }
    }
    const auto callee = [&](uint32_t id) -> const Func* {
        const uint32_t g = id < bound ? func_of[id] : kNone;
        return g == kNone ? nullptr : &funcs[g];
    };
    const auto movable = [&](const Inst& x) {
        const uint32_t* v = w + x.at;
        if (pure(x.op)) return x.wc >= 3 && !(x.op == 12 && x.wc >= 5 && (v[4] == 35 || v[4] == 51));
        if (subgroup(x.op)) return x.wc >= 3 && !ends;
        if (x.op == 61 && x.wc >= 4) {   // (OpLoad: of memory nothing writes, or by address)
            const uint32_t cls = class_of(v[3]);
            return read_only_class(cls) || cls == kPhysical;
        }
        if (x.op == 57 && x.wc >= 4) {   // (OpFunctionCall: of a function that only computes, reading nothing that can change but by address)
            const Func* f = callee(v[3]);
            return f && !f->effects && !f->reads_other;
        }
        return false;
    };
    /// (an instruction after which memory read by address may hold other values)
    const auto physical_writer = [&](const Inst& x) {
        const uint32_t* v = w + x.at;
        if (x.op == 62) {
            const uint32_t cls = x.wc >= 2 ? class_of(v[1]) : kNone;
            return cls == kPhysical || cls == kNone;
        }
        if (x.op == 63 || x.op == 64 || atomic(x.op) || x.op == 224 || x.op == 225) return true;
        if (x.op == 57) {
            const Func* f = x.wc >= 4 ? callee(v[3]) : nullptr;
            return !f || f->effects;
        }
        return false;
    };
    /// (a movable instruction that reads memory by address: it stays between the writers around it)
    const auto physical_reader = [&](const Inst& x) {
        const uint32_t* v = w + x.at;
        if (x.op == 61 && x.wc >= 4) return class_of(v[3]) == kPhysical;
        if (x.op == 57 && x.wc >= 4) {
            const Func* f = callee(v[3]);
            return f && f->reads_physical;
        }
        return false;
    };

    std::vector<Inst> out_insts = insts;
    LateResult res;
    // (by id, for the function at hand: its value's number; a label's block)
    std::vector<uint32_t> val_of(bound, kNone), block_of(bound, kNone);
    // (scratch of one function and one block, reused)
    std::vector<Block> blocks;
    std::vector<uint32_t> vwidth, vblock, vid, cross_of, cross_val, node_of, node_epoch;
    std::vector<uint64_t> ue, def, live_in, live_out;
    std::vector<std::vector<uint32_t>> deps, operands, users;
    std::vector<uint32_t> remaining, size, order, stack, work, near, seen, after, readers, todo, virtuals, last_pos, identity, values, uses, pos;
    std::vector<uint8_t> emitted, is_movable, is_writer;
    uint32_t block_epoch = 0;
    for (const Func& f : funcs) {
        // ---- the function's blocks and values ----
        blocks.clear(), vwidth.clear(), vblock.clear(), vid.clear();
        for (size_t n = f.first; n <= f.last; n++) {
            const Inst& x = insts[n];
            const uint32_t* v = w + x.at;
            if (x.op == 248 && x.wc >= 2 && v[1] < bound) {
                block_of[v[1]] = (uint32_t)blocks.size();
                blocks.push_back({(uint32_t)n, (uint32_t)n, v[1], {}});
            } else if (!blocks.empty() && x.op != 56) {
                blocks.back().last = (uint32_t)n;
            }
            if (has_result(x.op) && x.wc >= 3 && v[2] < bound) {
                val_of[v[2]] = (uint32_t)vwidth.size();
                vwidth.push_back(width_of(v[1]));
                vid.push_back(v[2]);
                vblock.push_back(blocks.empty() ? kNone : (uint32_t)blocks.size() - 1);
            }
        }
        const uint32_t n_blocks = (uint32_t)blocks.size(), n_vals = (uint32_t)vwidth.size();
        if (!n_blocks) continue;
        // (a word of this function's instruction as one of its values, or kNone; val_of keeps the numbers of earlier
        // functions, so the value's own id is checked)
        const auto value = [&](uint32_t id) -> uint32_t {
            if (id >= bound) return kNone;
            const uint32_t vi = val_of[id];
            return vi < n_vals && vid[vi] == id ? vi : kNone;
        };
        const auto block_at = [&](uint32_t id) -> uint32_t {
            if (id >= bound) return kNone;
            const uint32_t b = block_of[id];
            return b < n_blocks && blocks[b].label == id ? b : kNone;
        };
        // ---- which values are held from one block into another (or into a phi), and where: liveness ----
        cross_of.assign(n_vals, kNone), cross_val.clear();
        const auto mark = [&](uint32_t vi) {
            if (cross_of[vi] == kNone) cross_of[vi] = (uint32_t)cross_val.size(), cross_val.push_back(vi);
        };
        for (uint32_t b = 0; b < n_blocks; b++)
            for (uint32_t n = blocks[b].first; n <= blocks[b].last; n++) {
                const Inst& x = insts[n];
                const uint32_t* v = w + x.at;
                if (x.op == 245) {
                    for (uint32_t j = 3; j + 1 < x.wc; j += 2)
                        if (const uint32_t vi = value(v[j]); vi != kNone) mark(vi);
                    continue;
                }
                for (uint32_t j = has_result(x.op) ? 3 : 1; j < x.wc; j++) {
                    if (!maybe_id(x.op, j)) continue;
                    if (const uint32_t vi = value(v[j]); vi != kNone && vblock[vi] != b) mark(vi);
                }
            }
        const uint32_t n_cross = (uint32_t)cross_val.size(), W = (n_cross + 63) / 64;
        ue.assign((size_t)n_blocks * W, 0), def.assign((size_t)n_blocks * W, 0), live_in.assign((size_t)n_blocks * W, 0), live_out.assign((size_t)n_blocks * W, 0);
        const auto set_bit = [&](std::vector<uint64_t>& s, uint32_t b, uint32_t c) { s[(size_t)b * W + c / 64] |= 1ull << (c % 64); };
        const auto test = [&](const std::vector<uint64_t>& s, uint32_t b, uint32_t c) { return (s[(size_t)b * W + c / 64] >> (c % 64)) & 1; };
        for (uint32_t b = 0; b < n_blocks; b++) {
            for (uint32_t n = blocks[b].first; n <= blocks[b].last; n++) {
                const Inst& x = insts[n];
                const uint32_t* v = w + x.at;
                if (has_result(x.op) && x.wc >= 3)
                    if (const uint32_t vi = value(v[2]); vi != kNone && cross_of[vi] != kNone) set_bit(def, b, cross_of[vi]);
                if (x.op == 245) {   // (a phi's operand is held to the end of the block it comes from)
                    for (uint32_t j = 3; j + 1 < x.wc; j += 2) {
                        const uint32_t vi = value(v[j]), from = block_at(v[j + 1]);
                        if (vi != kNone && from != kNone) set_bit(live_out, from, cross_of[vi]);
                    }
                    continue;
                }
                for (uint32_t j = has_result(x.op) ? 3 : 1; j < x.wc; j++) {
                    if (!maybe_id(x.op, j)) continue;
                    if (const uint32_t vi = value(v[j]); vi != kNone && vblock[vi] != b) set_bit(ue, b, cross_of[vi]);
                }
            }
            const Inst& t = insts[blocks[b].last];
            for (uint32_t j = 1; j < t.wc; j++)
                if (const uint32_t s = block_at(w[t.at + j]); s != kNone && std::find(blocks[b].succ.begin(), blocks[b].succ.end(), s) == blocks[b].succ.end())
                    blocks[b].succ.push_back(s);
        }
        const std::vector<uint64_t> phi_out = live_out;
        for (bool changed = true; changed && W;) {
            changed = false;
            for (uint32_t b = n_blocks; b-- > 0;) {
                uint64_t* out = &live_out[(size_t)b * W];
                uint64_t* in = &live_in[(size_t)b * W];
                for (uint32_t k = 0; k < W; k++) {
                    uint64_t o = phi_out[(size_t)b * W + k];
                    for (const uint32_t s : blocks[b].succ) o |= live_in[(size_t)s * W + k];
                    const uint64_t i = ue[(size_t)b * W + k] | (o & ~def[(size_t)b * W + k]);
                    if (o != out[k] || i != in[k]) out[k] = o, in[k] = i, changed = true;
                }
            }
        }
        node_of.assign(n_vals, 0), node_epoch.assign(n_vals, 0);
        block_epoch = 0;
        // ---- block by block ----
        for (uint32_t b = 0; b < n_blocks; b++) {
            const Block& blk = blocks[b];
            const uint32_t nb = blk.last - blk.first + 1;
            if (nb < 3) continue;
            uint32_t held_in = 0;   // (the values held on entry)
            for (uint32_t c = 0; c < n_cross; c++)
                if (test(live_in, b, c)) held_in += vwidth[cross_val[c]];
            {   // (a block that cannot reach min_peak whatever its order is not looked at: what it holds on entry and all it makes)
                uint32_t most = held_in;
                for (uint32_t n = blk.first; n <= blk.last; n++)
                    if (has_result(insts[n].op) && insts[n].wc >= 3) most += width_of(w[insts[n].at + 1]);
                if (most < options.min_peak) {
                    res.peak_before = std::max(res.peak_before, held_in), res.peak_after = std::max(res.peak_after, held_in);
                    continue;
                }
            }
            block_epoch++;
            // (the values this block takes from before it and uses for the last time: they come first, as instructions
            // already emitted, so that what frees them is seen)
            virtuals.clear();
            for (uint32_t n = blk.first; n <= blk.last; n++) {
                const Inst& x = insts[n];
                if (x.op == 245) continue;
                const uint32_t* v = w + x.at;
                for (uint32_t j = has_result(x.op) ? 3 : 1; j < x.wc; j++) {
                    if (!maybe_id(x.op, j)) continue;
                    const uint32_t vi = value(v[j]);
                    if (vi == kNone || vblock[vi] == b || node_epoch[vi] == block_epoch) continue;
                    if (cross_of[vi] == kNone || test(live_out, b, cross_of[vi])) continue;
                    node_epoch[vi] = block_epoch, node_of[vi] = (uint32_t)virtuals.size();
                    virtuals.push_back(vi);
                }
            }
            const uint32_t nv = (uint32_t)virtuals.size(), N = nv + nb;
            const auto inst_of = [&](uint32_t node) -> const Inst& { return insts[blk.first + (node - nv)]; };
            is_movable.assign(N, 0), is_writer.assign(N, 0), emitted.assign(N, 0), remaining.assign(N, 0), size.assign(N, 1), seen.assign(N, 0);
            after.assign(N, kNone);
            if (deps.size() < N) deps.resize(N), operands.resize(N), users.resize(N);
            for (uint32_t n = 0; n < N; n++) deps[n].clear(), operands[n].clear(), users[n].clear();
            order.clear(), readers.clear();
            values.assign(N, 0);   // (a node's 32-bit values)
            for (uint32_t n = 0; n < nv; n++) values[n] = vwidth[virtuals[n]], emitted[n] = 1;
            uint32_t writer = kNone;
            for (uint32_t n = nv; n < N; n++) {
                const Inst& x = inst_of(n);
                const uint32_t* v = w + x.at;
                is_movable[n] = movable(x);
                if (!is_movable[n] && physical_writer(x)) is_writer[n] = 1, writer = n;
                else if (is_movable[n] && physical_reader(x)) after[n] = writer, readers.push_back(n);
                if (x.op != 245)
                    for (uint32_t j = has_result(x.op) ? 3 : 1; j < x.wc; j++) {   // (operands made earlier in this block or taken into it, each once)
                        if (!maybe_id(x.op, j)) continue;
                        const uint32_t vi = value(v[j]);
                        if (vi == kNone || node_epoch[vi] != block_epoch) continue;
                        const uint32_t k = node_of[vi];
                        if (std::find(operands[n].begin(), operands[n].end(), k) != operands[n].end()) continue;
                        operands[n].push_back(k), users[k].push_back(n), remaining[k]++;
                        if (is_movable[k]) deps[n].push_back(k);
                    }
                if (has_result(x.op) && x.wc >= 3)
                    if (const uint32_t vi = value(v[2]); vi != kNone) {
                        node_epoch[vi] = block_epoch, node_of[vi] = n;
                        values[n] = vwidth[vi];
                        if (cross_of[vi] != kNone && test(live_out, b, cross_of[vi])) remaining[n] += kImmortal;   // (held past the block: it does not die here)
                    }
                uint64_t s = 1;
                for (const uint32_t d : deps[n]) s += size[d];
                size[n] = (uint32_t)std::min<uint64_t>(s, 1u << 30);
            }
            // (the most values held at once with the block's instructions in `seq`: a value from its instruction to its
            // last user, or to the block's end when it is held past it)
            uses.assign(remaining.begin(), remaining.end());
            pos.assign(N, 0), last_pos.assign(N, 0);
            const auto peak_of = [&](const std::vector<uint32_t>& seq) {
                for (uint32_t p = 0; p < seq.size(); p++) pos[seq[p]] = p;
                for (uint32_t d = 0; d < N; d++) {
                    last_pos[d] = 0;
                    for (const uint32_t u : users[d]) last_pos[d] = std::max(last_pos[d], pos[u]);
                }
                uint32_t live = held_in, peak = held_in;
                for (uint32_t p = 0; p < seq.size(); p++) {
                    const uint32_t n = seq[p];
                    if (uses[n]) live += values[n];
                    for (const uint32_t d : operands[n])
                        if (uses[d] < kImmortal && last_pos[d] == p) live -= values[d];
                    peak = std::max(peak, live);
                }
                return peak;
            };
            identity.resize(nb);
            for (uint32_t n = 0; n < nb; n++) identity[n] = nv + n;
            const uint32_t peak_before = peak_of(identity);
            res.peak_before = std::max(res.peak_before, peak_before);
            if (peak_before < options.min_peak) {
                res.peak_after = std::max(res.peak_after, peak_before);
                continue;
            }
            res.blocks++;
            uint32_t epoch = 0;
            // (instruction k goes here; then every movable instruction that is ready and frees at least as many values as it
            // makes, its operands' last use, goes right after it, and so on. head: k is placed already (one of the block's
            // first instructions, or a value taken into the block): only what follows from it is looked for)
            const auto place = [&](uint32_t first, bool head) {
                work.assign(1, first);
                while (!work.empty()) {
                    const uint32_t k = work.back();
                    work.pop_back();
                    if (emitted[k] && !head) continue;
                    near.clear();
                    epoch++;
                    const auto add = [&](uint32_t c) {
                        if (seen[c] != epoch) seen[c] = epoch, near.push_back(c);
                    };
                    for (const uint32_t c : users[k]) add(c);
                    if (!head) {
                        emitted[k] = 1;
                        order.push_back(k);
                        for (const uint32_t d : operands[k])
                            if (--remaining[d] == 1)   // (its last user may now free it)
                                for (const uint32_t c : users[d]) add(c);
                    }
                    head = false;
                    for (const uint32_t c : near) {
                        if (emitted[c] || !is_movable[c]) continue;
                        if (after[c] != kNone && !emitted[after[c]]) continue;   // (a read by address: not before the write it follows)
                        bool ready = true;
                        uint32_t freed = 0;
                        for (const uint32_t d : operands[c]) {
                            if (!emitted[d]) {
                                ready = false;
                                break;
                            }
                            if (remaining[d] == 1) freed += values[d];
                        }
                        // (a component taken out of a vector is the vector's own register)
                        if (ready && (inst_of(c).op == 81 || values[c] <= freed)) work.push_back(c);
                    }
                }
            };
            const auto emit = [&](uint32_t first) {
                stack.assign(1, first);
                while (!stack.empty()) {
                    const uint32_t k = stack.back();
                    if (emitted[k]) {
                        stack.pop_back();
                        continue;
                    }
                    todo.clear();
                    for (const uint32_t d : deps[k])
                        if (!emitted[d]) todo.push_back(d);
                    if (!todo.empty()) {   // (the stack pops the last: the largest tree first, the earlier of two equal ones)
                        std::sort(todo.begin(), todo.end(), [&](uint32_t a, uint32_t c) { return size[a] != size[c] ? size[a] < size[c] : a > c; });
                        stack.insert(stack.end(), todo.begin(), todo.end());
                        continue;
                    }
                    stack.pop_back();
                    place(k, false);
                }
            };
            // (the block's end stays last: its terminator and the merge before it)
            uint32_t tail = N - 1;
            if (tail > nv && (inst_of(tail - 1).op == 246 || inst_of(tail - 1).op == 247)) tail--;
            uint32_t first = nv;   // (the label, the phis, the variables and their line marks stay first, together)
            for (; first < tail; first++) {
                const uint32_t op = inst_of(first).op;
                if (op != 248 && op != 245 && op != 59 && op != 8 && op != 317) break;
                emitted[first] = 1;
                order.push_back(first);
                for (const uint32_t d : operands[first]) remaining[d]--;
            }
            for (uint32_t n = 0; n < first; n++) place(n, true);
            size_t next_reader = 0;
            for (uint32_t n = first; n < tail; n++) {
                if (is_movable[n]) continue;
                if (is_writer[n])   // (the reads by address that came before this write stay before it)
                    for (; next_reader < readers.size() && readers[next_reader] < n; next_reader++) emit(readers[next_reader]);
                emit(n);
            }
            for (uint32_t n = first; n < tail; n++)
                if (!emitted[n]) emit(n);
            for (uint32_t n = tail; n < N; n++) emit(n);
            if (order.size() != nb) return 0;   // (not reached: every instruction is placed once)
            const uint32_t peak_after = peak_of(order);
            res.peak_after = std::max(res.peak_after, std::min(peak_after, peak_before));
            if (options.log) {
                char line[200];
                std::snprintf(line, sizeof line, "function %u block %u (label %u): %u instructions, %u values held on entry, at most %u -> %u%s", f.id, b,
                              blk.label, nb, held_in, peak_before, peak_after, peak_after < peak_before ? "" : " (left as it was)");
                options.log(line);
            }
            if (peak_after >= peak_before) continue;   // (kept only when it holds fewer values at once)
            res.blocks_changed++;
            for (uint32_t n = 0; n < nb; n++) {
                res.moved += order[n] != nv + n;
                out_insts[blk.first + n] = inst_of(order[n]);
            }
        }
    }
    if (res.moved) {
        std::vector<uint32_t> out;
        out.reserve(words.size());
        out.insert(out.end(), words.begin(), words.begin() + 5);
        for (const Inst& x : out_insts) out.insert(out.end(), words.begin() + x.at, words.begin() + x.at + x.wc);
        words.swap(out);
    }
    if (result) *result = res;
    return res.moved;
}

}  // namespace hsd
