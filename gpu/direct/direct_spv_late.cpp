// Late scheduling of a translated stage's SPIR-V (direct_spv_late.h): the translator's register-pressure pass. Stands alone:
// tools/shaders/spvlate.cpp builds it into a command, tests/test_direct_spv_late.py checks it, tools/shaders/late_gate.py
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
    uint32_t first = 0, last = 0;   // (instructions of the function's body, inclusive)
    uint32_t label = 0;
    std::vector<uint32_t> succ, pred;
};

struct Tally {
    uint32_t moved = 0, blocks = 0, changed = 0, peak_before = 0, peak_after = 0;
    bool bad = false;
};

struct Pass {
    const uint32_t* w = nullptr;
    uint32_t bound = 0;
    const LateOptions* options = nullptr;
    // (by id: a pointer type's storage class, a type's 32-bit values, a value's or variable's type, a function's number)
    std::vector<uint32_t> pointer_class, width, type_of, func_of;
    std::vector<Func> funcs;
    bool ends = false;   // (the module can demote or end an invocation)
    // (by id, for the function at hand: its value's number; a label's block)
    std::vector<uint32_t> val_of, block_of;
    // (the function at hand)
    std::vector<Block> blocks;
    std::vector<uint32_t> vwidth, vblock, vid, inst_block;
    std::vector<uint32_t> held_in;                 // (by block: the values held on entry, as 32-bit values)
    std::vector<std::vector<uint32_t>> out_list;   // (by block: the values held at its end)
    std::vector<uint32_t> out_mark;                // (by value: the block being looked at, when the value is held at its end)
    std::vector<uint32_t> use_at, use_list, fill_at, in_stamp, out_stamp, walk;
    // (scratch of one block, reused)
    std::vector<uint32_t> node_of, node_epoch;
    std::vector<std::vector<uint32_t>> deps, operands, users;
    std::vector<uint32_t> remaining, size, order, stack, work, near, seen, after, readers, todo, virtuals, last_pos, identity, values, uses, pos;
    std::vector<uint8_t> emitted, is_movable, is_writer;

    uint32_t class_of(uint32_t pointer) const {   // (the storage class a pointer value points into)
        if (pointer >= bound) return kNone;
        const uint32_t t = type_of[pointer];
        return t < bound ? pointer_class[t] : kNone;
    }
    uint32_t width_of(uint32_t type) const { return type < bound && width[type] ? width[type] : 1u; }
    const Func* callee(uint32_t id) const {
        const uint32_t g = id < bound ? func_of[id] : kNone;
        return g == kNone ? nullptr : &funcs[g];
    }
    /// (computes only: GLSL.std.450's Modf and Frexp write through a pointer)
    bool computes(const Inst& x) const {
        return pure(x.op) && x.wc >= 3 && !(x.op == 12 && x.wc >= 5 && (w[x.at + 4] == 35 || w[x.at + 4] == 51));
    }
    bool movable(const Inst& x) const {
        const uint32_t* v = w + x.at;
        if (pure(x.op)) return computes(x);
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
    }
    /// (an instruction after which memory read by address may hold other values)
    bool physical_writer(const Inst& x) const {
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
    }
    /// (a movable instruction that reads memory by address: it stays between the writers around it)
    bool physical_reader(const Inst& x) const {
        const uint32_t* v = w + x.at;
        if (x.op == 61 && x.wc >= 4) return class_of(v[3]) == kPhysical;
        if (x.op == 57 && x.wc >= 4) {
            const Func* f = callee(v[3]);
            return f && f->reads_physical;
        }
        return false;
    }
    /// (a word of the function's instruction as one of its values, or kNone; val_of keeps the numbers of other functions, so
    /// the value's own id is checked)
    uint32_t value(uint32_t id) const {
        if (id >= bound) return kNone;
        const uint32_t vi = val_of[id];
        return vi < vid.size() && vid[vi] == id ? vi : kNone;
    }
    uint32_t block_at(uint32_t id) const {
        if (id >= bound) return kNone;
        const uint32_t b = block_of[id];
        return b < blocks.size() && blocks[b].label == id ? b : kNone;
    }
    template <class F> void each_operand(const Inst& x, F&& f) const {   // (the function's values an instruction uses; not a phi's)
        const uint32_t* v = w + x.at;
        for (uint32_t j = has_result(x.op) ? 3 : 1; j < x.wc; j++) {
            if (!maybe_id(x.op, j)) continue;
            if (const uint32_t vi = value(v[j]); vi != kNone) f(vi);
        }
    }

    bool parse(const std::vector<Inst>& insts) {
        pointer_class.assign(bound, kNone), width.assign(bound, 0), type_of.assign(bound, 0), func_of.assign(bound, kNone);
        val_of.assign(bound, kNone), block_of.assign(bound, kNone);
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
        // (the functions: which only compute, which read by address, themselves or through a call)
        Func* cur = nullptr;
        for (size_t n = 0; n < insts.size(); n++) {
            const Inst& x = insts[n];
            const uint32_t* v = w + x.at;
            if (x.op == 54) {
                if (cur || x.wc < 3 || v[2] >= bound) return false;
                func_of[v[2]] = (uint32_t)funcs.size();
                funcs.emplace_back();
                cur = &funcs.back();
                cur->id = v[2], cur->first = n;
            } else if (x.op == 56) {
                if (!cur) return false;
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
                } else if (computes(x) || subgroup(x.op) || plain_control(x.op)) {
                } else {
                    cur->effects = cur->reads_other = cur->reads_physical = true;
                }
            }
        }
        if (cur) return false;
        for (bool changed = true; changed;) {
            changed = false;
            for (Func& f : funcs)
                for (const uint32_t c : f.calls) {
                    const Func* g = callee(c);
                    const bool e = !g || g->effects, p = !g || g->reads_physical, o = !g || g->reads_other;
                    if ((e && !f.effects) || (p && !f.reads_physical) || (o && !f.reads_other)) changed = true;
                    f.effects |= e, f.reads_physical |= p, f.reads_other |= o;
                }
        }
        return true;
    }

    /// The function's blocks, their edges, and its values
    void analyze(const std::vector<Inst>& body) {
        blocks.clear(), vwidth.clear(), vblock.clear(), vid.clear();
        inst_block.assign(body.size(), kNone);
        for (uint32_t n = 0; n < body.size(); n++) {
            const Inst& x = body[n];
            const uint32_t* v = w + x.at;
            if (x.op == 248 && x.wc >= 2 && v[1] < bound) {
                block_of[v[1]] = (uint32_t)blocks.size();
                blocks.push_back({n, n, v[1], {}, {}});
            } else if (!blocks.empty() && x.op != 56) {
                blocks.back().last = n;
            }
            if (!blocks.empty() && x.op != 56) inst_block[n] = (uint32_t)blocks.size() - 1;
            if (has_result(x.op) && x.wc >= 3 && v[2] < bound) {
                val_of[v[2]] = (uint32_t)vwidth.size();
                vwidth.push_back(width_of(v[1]));
                vid.push_back(v[2]);
                vblock.push_back(blocks.empty() ? kNone : (uint32_t)blocks.size() - 1);
            }
        }
        for (uint32_t b = 0; b < blocks.size(); b++) {
            const Inst& t = body[blocks[b].last];
            for (uint32_t j = 1; j < t.wc; j++)
                if (const uint32_t s = block_at(w[t.at + j]); s != kNone && std::find(blocks[b].succ.begin(), blocks[b].succ.end(), s) == blocks[b].succ.end())
                    blocks[b].succ.push_back(s), blocks[s].pred.push_back(b);
        }
    }

    /// Which values are held from one block into another (or into a phi), and where: for each value, the blocks from its
    /// uses back to the block that makes it. Kept as what each block holds on entry (a sum) and the values it holds at its
    /// end (a list): a function of 17,000 blocks and 60,000 such values would need a gigabyte as bit sets
    void liveness(const std::vector<Inst>& body) {
        const uint32_t n_blocks = (uint32_t)blocks.size(), n_vals = (uint32_t)vwidth.size();
        constexpr uint32_t kPhi = 1u << 31;
        // (each value's uses in blocks other than its own, and the blocks it goes into a phi from, in one list by value)
        use_at.assign(n_vals + 1, 0);
        const auto each_use = [&](auto&& f) {
            for (uint32_t b = 0; b < n_blocks; b++)
                for (uint32_t n = blocks[b].first; n <= blocks[b].last; n++) {
                    const Inst& x = body[n];
                    if (x.op == 245) {
                        for (uint32_t j = 3; j + 1 < x.wc; j += 2) {
                            const uint32_t vi = value(w[x.at + j]), from = block_at(w[x.at + j + 1]);
                            if (vi != kNone && from != kNone) f(vi, from | kPhi);
                        }
                        continue;
                    }
                    each_operand(x, [&](uint32_t vi) {
                        if (vblock[vi] != b) f(vi, b);
                    });
                }
        };
        each_use([&](uint32_t vi, uint32_t) { use_at[vi + 1]++; });
        for (uint32_t v = 0; v < n_vals; v++) use_at[v + 1] += use_at[v];
        use_list.resize(use_at[n_vals]);
        fill_at.assign(use_at.begin(), use_at.end() - 1);
        each_use([&](uint32_t vi, uint32_t where) { use_list[fill_at[vi]++] = where; });
        held_in.assign(n_blocks, 0);
        if (out_list.size() < n_blocks) out_list.resize(n_blocks);
        for (uint32_t b = 0; b < n_blocks; b++) out_list[b].clear();
        in_stamp.assign(n_blocks, kNone), out_stamp.assign(n_blocks, kNone);
        for (uint32_t vi = 0; vi < n_vals; vi++) {
            if (use_at[vi] == use_at[vi + 1]) continue;
            const uint32_t D = vblock[vi];
            walk.clear();
            const auto in = [&](uint32_t b) {
                if (b == D || in_stamp[b] == vi) return;
                in_stamp[b] = vi, held_in[b] += vwidth[vi];
                walk.push_back(b);
            };
            const auto out = [&](uint32_t b) {
                if (out_stamp[b] != vi) out_stamp[b] = vi, out_list[b].push_back(vi);
                in(b);
            };
            for (uint32_t k = use_at[vi]; k < use_at[vi + 1]; k++) {
                if (use_list[k] & kPhi) out(use_list[k] & ~kPhi);
                else in(use_list[k]);
            }
            while (!walk.empty()) {
                const uint32_t b = walk.back();
                walk.pop_back();
                for (const uint32_t p : blocks[b].pred) out(p);
            }
        }
        out_mark.assign(n_vals, kNone);
    }

    /// Each block's instructions in the late order, where that holds fewer values at once than the order it has
    Tally local(std::vector<Inst>& body, uint32_t fid) {
        Tally res;
        analyze(body);
        liveness(body);
        std::vector<Inst> out = body;
        const uint32_t n_blocks = (uint32_t)blocks.size();
        node_of.assign(vwidth.size(), 0), node_epoch.assign(vwidth.size(), 0);
        uint32_t block_epoch = 0;
        for (uint32_t b = 0; b < n_blocks; b++) {
            const Block& blk = blocks[b];
            const uint32_t nb = blk.last - blk.first + 1;
            const auto untouched = [&](uint32_t peak) { res.peak_before = std::max(res.peak_before, peak), res.peak_after = std::max(res.peak_after, peak); };
            if (nb < 3) {
                untouched(held_in[b]);
                continue;
            }
            {   // (a block that cannot pass the budget whatever its order is not looked at: what it holds on entry and all it makes)
                uint32_t most = held_in[b];
                for (uint32_t n = blk.first; n <= blk.last; n++)
                    if (has_result(body[n].op) && body[n].wc >= 3) most += width_of(w[body[n].at + 1]);
                if (most <= options->budget) {
                    untouched(held_in[b]);
                    continue;
                }
            }
            block_epoch++;
            for (const uint32_t vi : out_list[b]) out_mark[vi] = b;
            // (the values this block takes from before it and uses for the last time: they come first, as instructions
            // already emitted, so that what frees them is seen)
            virtuals.clear();
            for (uint32_t n = blk.first; n <= blk.last; n++) {
                if (body[n].op == 245) continue;
                each_operand(body[n], [&](uint32_t vi) {
                    if (vblock[vi] == b || node_epoch[vi] == block_epoch) return;
                    if (out_mark[vi] == b) return;
                    node_epoch[vi] = block_epoch, node_of[vi] = (uint32_t)virtuals.size();
                    virtuals.push_back(vi);
                });
            }
            const uint32_t nv = (uint32_t)virtuals.size(), N = nv + nb;
            const auto inst_of = [&](uint32_t node) -> const Inst& { return body[blk.first + (node - nv)]; };
            is_movable.assign(N, 0), is_writer.assign(N, 0), emitted.assign(N, 0), remaining.assign(N, 0), size.assign(N, 1), seen.assign(N, 0);
            after.assign(N, kNone), values.assign(N, 0);
            if (deps.size() < N) deps.resize(N), operands.resize(N), users.resize(N);
            for (uint32_t n = 0; n < N; n++) deps[n].clear(), operands[n].clear(), users[n].clear();
            order.clear(), readers.clear();
            for (uint32_t n = 0; n < nv; n++) values[n] = vwidth[virtuals[n]], emitted[n] = 1;
            uint32_t writer = kNone;
            for (uint32_t n = nv; n < N; n++) {
                const Inst& x = inst_of(n);
                is_movable[n] = movable(x);
                if (!is_movable[n] && physical_writer(x)) is_writer[n] = 1, writer = n;
                else if (is_movable[n] && physical_reader(x)) after[n] = writer, readers.push_back(n);
                if (x.op != 245)
                    each_operand(x, [&](uint32_t vi) {   // (operands made earlier in this block or taken into it, each once)
                        if (node_epoch[vi] != block_epoch) return;
                        const uint32_t k = node_of[vi];
                        if (std::find(operands[n].begin(), operands[n].end(), k) != operands[n].end()) return;
                        operands[n].push_back(k), users[k].push_back(n), remaining[k]++;
                        if (is_movable[k]) deps[n].push_back(k);
                    });
                if (has_result(x.op) && x.wc >= 3)
                    if (const uint32_t vi = value(w[x.at + 2]); vi != kNone) {
                        node_epoch[vi] = block_epoch, node_of[vi] = n;
                        values[n] = vwidth[vi];
                        if (out_mark[vi] == b) remaining[n] += kImmortal;   // (held past the block: it does not die here)
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
                uint32_t live = held_in[b], peak = held_in[b];
                for (uint32_t p = 0; p < seq.size(); p++) {
                    const uint32_t n = seq[p];
                    if (uses[n]) live += values[n];
                    for (const uint32_t d : operands[n])
                        if (uses[d] < kImmortal && last_pos[d] == p) live -= values[d];
                    peak = std::max(peak, live);
                }
                return peak;
            };
            // (a new order is taken only when it is one: every instruction once, the first and last ones in their places, the
            // instructions with effects in their order, each value made before it is used, a read by address between the
            // writes around it)
            const auto sound = [&](const std::vector<uint32_t>& seq, uint32_t first, uint32_t tail) {
                if (seq.size() != nb) return false;
                std::fill(pos.begin(), pos.end(), kNone);
                for (uint32_t p = 0; p < nb; p++) {
                    if (seq[p] < nv || seq[p] >= N || pos[seq[p]] != kNone) return false;
                    pos[seq[p]] = p;
                }
                for (uint32_t n = nv; n < first; n++)
                    if (pos[n] != n - nv) return false;
                for (uint32_t n = tail; n < N; n++)
                    if (pos[n] != n - nv) return false;
                uint32_t last_writer = kNone, last_fixed = kNone;   // (the last write by address, the last instruction with an effect)
                for (uint32_t n = nv; n < N; n++) {
                    for (const uint32_t d : operands[n])
                        if (d >= nv && pos[d] >= pos[n]) return false;
                    if (!is_movable[n]) {
                        if (last_fixed != kNone && pos[last_fixed] >= pos[n]) return false;
                        last_fixed = n;
                    }
                    if (is_writer[n]) {
                        if (last_writer != kNone && pos[last_writer] >= pos[n]) return false;
                        last_writer = n;
                    } else if (after[n] != kNone && pos[after[n]] >= pos[n]) {
                        return false;
                    }
                }
                uint32_t next_writer = kNone;
                for (uint32_t n = N; n-- > nv;) {
                    if (is_writer[n]) next_writer = n;
                    else if (is_movable[n] && next_writer != kNone && pos[n] >= pos[next_writer] && (after[n] != kNone || physical_reader(inst_of(n)))) return false;
                }
                return true;
            };
            identity.resize(nb);
            for (uint32_t n = 0; n < nb; n++) identity[n] = nv + n;
            const uint32_t peak_before = peak_of(identity);
            if (peak_before <= options->budget) {
                untouched(peak_before);
                continue;
            }
            res.blocks++;
            // (the block's end stays last: its terminator and the merge before it; the label, the phis, the variables and
            // their line marks stay first, together)
            uint32_t tail = N - 1;
            if (tail > nv && (inst_of(tail - 1).op == 246 || inst_of(tail - 1).op == 247)) tail--;
            uint32_t first = nv;
            for (; first < tail; first++) {
                const uint32_t op = inst_of(first).op;
                if (op != 248 && op != 245 && op != 59 && op != 8 && op != 317) break;
            }
            uint32_t epoch = 0, live = 0;
            uint32_t passed = N;   // (the instructions before this one may follow at once: all of them, or those held back)
            // (instruction k goes here; then every movable instruction before `passed` that is ready and frees at least as
            // many values as it makes, its operands' last use, goes right after it, and so on. head: k is placed already (one of
            // the block's first instructions, or a value taken into the block): only what follows from it is looked for)
            const auto place = [&](uint32_t start, bool head) {
                work.assign(1, start);
                while (!work.empty()) {
                    const uint32_t k = work.back();
                    work.pop_back();
                    if (emitted[k] && !head) continue;
                    near.clear();
                    epoch++;
                    const auto add = [&](uint32_t c) {
                        if (c < passed && seen[c] != epoch) seen[c] = epoch, near.push_back(c);
                    };
                    for (const uint32_t c : users[k]) add(c);
                    if (!head) {
                        emitted[k] = 1;
                        order.push_back(k);
                        if (uses[k]) live += values[k];
                        for (const uint32_t d : operands[k]) {
                            if (--remaining[d] == 1)   // (its last user may now free it)
                                for (const uint32_t c : users[d]) add(c);
                            if (remaining[d] == 0) live -= values[d];
                        }
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
            const auto emit = [&](uint32_t start) {
                stack.assign(1, start);
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
            /// (would instruction n, its operands placed, keep the block within `budget` values?)
            const auto fits = [&](uint32_t n) {
                uint32_t freed = 0;
                for (const uint32_t d : operands[n]) {
                    if (!emitted[d]) return false;
                    if (remaining[d] == 1) freed += values[d];
                }
                return live + (uses[n] ? values[n] : 0) <= options->budget + freed;
            };
            // (the block in a new order. held: the translator's order, an instruction that only computes held back while
            // the block is at `budget` values and placed as soon as there is room again (or when something needs it): the
            // loads stay ahead of their uses by what the budget allows. Not held: every such instruction at its first use)
            const auto schedule = [&](bool held) {
                std::fill(emitted.begin() + nv, emitted.end(), 0);
                std::copy(uses.begin(), uses.end(), remaining.begin());
                order.clear();
                live = held_in[b], passed = held ? first : N;
                for (uint32_t n = nv; n < first; n++) {
                    emitted[n] = 1;
                    order.push_back(n);
                    if (uses[n]) live += values[n];
                }
                for (uint32_t n = 0; n < first; n++) place(n, true);
                size_t next_reader = 0;
                uint32_t waiting = first;   // (held: the first instruction passed over that is not placed yet)
                for (uint32_t n = first; n < tail; n++) {
                    if (held) passed = n;
                    if (held)
                        for (; waiting < n; waiting++) {
                            if (emitted[waiting]) continue;
                            if (!fits(waiting)) break;
                            emit(waiting);
                        }
                    if (emitted[n]) continue;
                    if (is_movable[n]) {
                        if (held && fits(n)) emit(n);   // (in its place: its operands are there and there is room)
                        continue;
                    }
                    if (is_writer[n])   // (the reads by address that came before this write stay before it)
                        for (; next_reader < readers.size() && readers[next_reader] < n; next_reader++) emit(readers[next_reader]);
                    emit(n);
                }
                // (what is left is needed by no instruction with an effect: the values held past the block, each with what it
                // needs, as they came; then what nothing uses; then the block's end, which brings what only it uses)
                passed = N;
                for (uint32_t n = first; n < tail; n++)
                    if (!emitted[n] && uses[n] >= kImmortal) emit(n);
                for (uint32_t n = first; n < tail; n++)
                    if (!emitted[n] && !uses[n]) emit(n);
                for (uint32_t n = tail; n < N; n++)   // (what only the block's end uses, before its merge)
                    for (const uint32_t d : operands[n])
                        if (!emitted[d]) emit(d);
                for (uint32_t n = tail; n < N; n++) emit(n);
            };
            schedule(true);
            if (!sound(order, first, tail)) {   // (not reached)
                res.bad = true;
                return res;
            }
            uint32_t peak_after = peak_of(order);
            const char* how = "held to the budget";
            if (peak_after >= peak_before || peak_after > options->budget + options->budget / 4) {
                // (the budget is not kept that way: values held on entry, or operands that must be placed early)
                std::vector<uint32_t> kept = order;
                schedule(false);
                if (!sound(order, first, tail)) {
                    res.bad = true;
                    return res;
                }
                const uint32_t peak_late = peak_of(order);
                if (peak_late < peak_after) peak_after = peak_late, how = "at first use";
                else order.swap(kept);
            }
            res.peak_before = std::max(res.peak_before, peak_before), res.peak_after = std::max(res.peak_after, std::min(peak_after, peak_before));
            if (options->log) {
                char line[200];
                std::snprintf(line, sizeof line, "function %u block %u (label %u): %u instructions, %u values held on entry, at most %u -> %u (%s)", fid, b,
                              blk.label, nb, held_in[b], peak_before, peak_after, peak_after < peak_before ? how : "left as it was");
                options->log(line);
            }
            if (peak_after >= peak_before) continue;   // (kept only when it holds fewer values at once)
            res.changed++;
            for (uint32_t n = 0; n < nb; n++) {
                res.moved += order[n] != nv + n;
                out[blk.first + n] = inst_of(order[n]);
            }
        }
        body.swap(out);
        return res;
    }

    /// Instructions that only compute, moved down into the one other block that uses them (the larger change: asked for
    /// only where a function still holds LateOptions::sink_peak values after `local`). An instruction goes when every use
    /// of its value is in one block U other than its own (a phi's use is in the block the value comes from), U is not in a loop its own block is outside
    /// of (no way from U back to U that does not pass its block: it runs at most as often as before), and holding its
    /// operands down to U costs fewer values than not holding its result; a load by address only when no instruction that
    /// may write by address lies on any way from it to U. Subgroup operations and calls stay in their blocks. Returns how
    /// many went
    uint32_t sink(std::vector<Inst>& body) {
        analyze(body);
        const uint32_t n = (uint32_t)body.size(), n_blocks = (uint32_t)blocks.size(), n_vals = (uint32_t)vwidth.size();
        constexpr uint32_t kMulti = ~0u - 1;
        std::vector<uint32_t> def_inst(n_vals, kNone), result(n, kNone);
        std::vector<std::vector<uint32_t>> vusers(n_vals), ops(n);
        std::vector<std::vector<uint32_t>> vphi(n_vals);   // (the blocks a value goes into a phi from: a use at that block's end)
        std::vector<uint8_t> phi_used(n_vals, 0), eligible(n, 0), reader(n, 0);
        std::vector<uint32_t> last_writer(n_blocks, kNone);
        for (uint32_t i = 0; i < n; i++) {
            const Inst& x = body[i];
            const uint32_t b = inst_block[i];
            if (b == kNone) continue;
            if (has_result(x.op) && x.wc >= 3)
                if (const uint32_t vi = value(w[x.at + 2]); vi != kNone) def_inst[vi] = i, result[i] = vi;
            if (x.op == 245) {
                for (uint32_t j = 3; j + 1 < x.wc; j += 2)
                    if (const uint32_t vi = value(w[x.at + j]); vi != kNone) phi_used[vi] = 1, vphi[vi].push_back(block_at(w[x.at + j + 1]));
                continue;
            }
            each_operand(x, [&](uint32_t vi) {
                if (std::find(ops[i].begin(), ops[i].end(), vi) == ops[i].end()) ops[i].push_back(vi), vusers[vi].push_back(i);
            });
            if (physical_writer(x)) last_writer[b] = i;
            if (result[i] == kNone) continue;
            if (computes(x)) eligible[i] = 1;
            else if (x.op == 61 && x.wc >= 4) {
                const uint32_t cls = class_of(w[x.at + 3]);
                eligible[i] = read_only_class(cls) || cls == kPhysical;
                reader[i] = cls == kPhysical;
            }
        }
        std::vector<uint8_t> has_writer(n_blocks, 0);
        for (uint32_t b = 0; b < n_blocks; b++) has_writer[b] = last_writer[b] != kNone;
        // (a pair of blocks: 1 = U is in a loop D is outside of; 2 = a block that may write by address lies between them)
        std::vector<std::pair<uint64_t, uint8_t>> pairs;
        std::vector<uint8_t> from_d(n_blocks), to_u(n_blocks);
        std::vector<uint32_t> walk;
        const auto pair_of = [&](uint32_t D, uint32_t U) -> uint8_t {
            const uint64_t key = (uint64_t)D << 32 | U;
            for (const auto& p : pairs)
                if (p.first == key) return p.second;
            uint8_t flags = 0;
            std::fill(from_d.begin(), from_d.end(), 0), std::fill(to_u.begin(), to_u.end(), 0);
            walk.assign(blocks[D].succ.begin(), blocks[D].succ.end());
            while (!walk.empty()) {   // (the blocks reached from D without passing D again)
                const uint32_t b = walk.back();
                walk.pop_back();
                if (b == D || from_d[b]) continue;
                from_d[b] = 1;
                for (const uint32_t s : blocks[b].succ) walk.push_back(s);
            }
            walk.assign(blocks[U].pred.begin(), blocks[U].pred.end());
            while (!walk.empty()) {   // (the blocks that reach U without passing D)
                const uint32_t b = walk.back();
                walk.pop_back();
                if (b == D || to_u[b]) continue;
                to_u[b] = 1;
                for (const uint32_t s : blocks[b].pred) walk.push_back(s);
            }
            if (!from_d[U] || to_u[U]) flags |= 1;   // (U not reached from D, or reached again from itself without D: not there)
            for (uint32_t b = 0; b < n_blocks; b++)
                if (b != U && from_d[b] && to_u[b] && has_writer[b]) flags |= 2;
            pairs.emplace_back(key, flags);
            return flags;
        };
        std::vector<uint32_t> cur(inst_block);
        const auto target = [&](uint32_t i) -> uint32_t {   // (the one block all of a value's uses are in, or kMulti, or kNone)
            const uint32_t vi = result[i];
            uint32_t U = kNone;
            for (const uint32_t from : vphi[vi]) {
                if (from == kNone) return kMulti;
                if (U == kNone) U = from;
                else if (U != from) return kMulti;
            }
            for (const uint32_t u : vusers[vi]) {
                if (U == kNone) U = cur[u];
                else if (U != cur[u]) return kMulti;
            }
            return U;
        };
        const auto legal = [&](uint32_t i, uint32_t U) {
            const uint32_t D = inst_block[i];
            if (U == kNone || U == kMulti || U == D) return false;
            const uint8_t flags = pair_of(D, U);
            if (flags & 1) return false;
            if (reader[i] && ((flags & 2) || (last_writer[D] != kNone && last_writer[D] > i))) return false;
            return true;
        };
        // (first every instruction that may go, the users before what they use: a chain goes together)
        for (uint32_t i = n; i-- > 0;) {
            if (!eligible[i]) continue;
            const uint32_t U = target(i);
            if (legal(i, U)) cur[i] = U;
        }
        // (then those are taken back whose operands, held down to U, cost as much as the result did: an operand that stays
        // in the block and has no use elsewhere would die there; and with them what only they carried)
        const auto going = [&](uint32_t i) { return cur[i] != inst_block[i]; };
        const auto cost = [&](uint32_t i) {
            const uint32_t D = inst_block[i];
            uint32_t c = 0;
            for (const uint32_t op : ops[i]) {
                const uint32_t j = def_inst[op];
                if (j != kNone && going(j)) continue;   // (it goes along)
                if (phi_used[op]) continue;
                bool elsewhere = false, first = true;
                for (const uint32_t u : vusers[op]) {
                    if (u == i) continue;
                    if (cur[u] != D) {
                        if (inst_block[u] != D) elsewhere = true;   // (used in another block anyway)
                        else if (u < i) first = false;              // (another instruction that goes pays for it)
                    }
                }
                if (!elsewhere && first) c += vwidth[op];
            }
            return c;
        };
        std::vector<uint32_t> queue;
        std::vector<uint8_t> queued(n, 0);
        for (uint32_t i = 0; i < n; i++)
            if (going(i)) queue.push_back(i), queued[i] = 1;
        while (!queue.empty()) {
            const uint32_t i = queue.back();
            queue.pop_back();
            queued[i] = 0;
            if (!going(i)) continue;
            const uint32_t U = target(i);
            if (U == cur[i] && cost(i) < vwidth[result[i]]) continue;
            cur[i] = inst_block[i];   // (it stays: what it uses and what uses it are looked at again)
            const auto again = [&](uint32_t k) {
                if (going(k) && !queued[k]) queue.push_back(k), queued[k] = 1;
            };
            for (const uint32_t op : ops[i]) {
                if (def_inst[op] != kNone) again(def_inst[op]);
                for (const uint32_t u : vusers[op]) again(u);
            }
            for (const uint32_t u : vusers[result[i]]) again(u);
        }
        uint32_t sunk = 0;
        for (uint32_t i = 0; i < n; i++) sunk += going(i);
        if (!sunk) return 0;
        // (the body again: each block's first instructions, then what came down into it in the order it had, then its own)
        std::vector<std::vector<uint32_t>> incoming(n_blocks);
        for (uint32_t i = 0; i < n; i++)
            if (going(i)) incoming[cur[i]].push_back(i);
        std::vector<Inst> out;
        out.reserve(n);
        for (uint32_t i = 0; i < n && inst_block[i] == kNone; i++) out.push_back(body[i]);
        for (uint32_t b = 0; b < n_blocks; b++) {
            uint32_t i = blocks[b].first;
            for (; i <= blocks[b].last; i++) {
                const uint32_t op = body[i].op;
                if (op != 248 && op != 245 && op != 59 && op != 8 && op != 317) break;
                out.push_back(body[i]);
            }
            for (const uint32_t k : incoming[b]) out.push_back(body[k]);
            for (; i <= blocks[b].last; i++)
                if (!going(i)) out.push_back(body[i]);
        }
        for (uint32_t i = blocks.back().last + 1; i < n; i++) out.push_back(body[i]);
        if (out.size() != body.size()) return 0;   // (not reached)
        body.swap(out);
        return sunk;
    }
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
    Pass pass;
    pass.w = words.data(), pass.bound = bound, pass.options = &options;
    if (!pass.parse(insts)) return 0;
    LateResult res;
    for (const Func& f : pass.funcs) {
        const std::vector<Inst> body(insts.begin() + (long)f.first, insts.begin() + (long)f.last + 1);
        std::vector<Inst> a = body;
        Tally ta = pass.local(a, f.id);
        if (ta.bad) return 0;
        if (ta.peak_after >= ta.peak_before) {   // (a function is changed only when its worst block holds fewer values)
            a = body;
            ta.moved = ta.changed = 0, ta.peak_after = ta.peak_before;
        }
        uint32_t sunk = 0;
        if (options.sink_peak && ta.peak_after >= options.sink_peak) {
            // (still many values held: with the instructions that can go down into the block that uses them gone there)
            std::vector<Inst> b = body;
            const uint32_t went = pass.sink(b);
            if (went) {
                Tally tb = pass.local(b, f.id);
                if (tb.bad) return 0;
                if (options.log) {
                    char line[200];
                    std::snprintf(line, sizeof line, "function %u: %u instructions into the block that uses them: at most %u values held, %u without%s", f.id,
                                  went, tb.peak_after, ta.peak_after, tb.peak_after < ta.peak_after ? "" : " (not kept)");
                    options.log(line);
                }
                if (tb.peak_after < ta.peak_after) {
                    tb.peak_before = ta.peak_before;
                    tb.moved = 0;
                    for (size_t n = 0; n < body.size(); n++) tb.moved += b[n].at != body[n].at;
                    a.swap(b), ta = tb, sunk = went;
                }
            }
        }
        res.moved += ta.moved, res.blocks += ta.blocks, res.blocks_changed += ta.changed, res.sunk += sunk;
        res.peak_before = std::max(res.peak_before, ta.peak_before), res.peak_after = std::max(res.peak_after, ta.peak_after);
        std::copy(a.begin(), a.end(), insts.begin() + (long)f.first);
    }
    if (res.moved) {
        std::vector<uint32_t> out;
        out.reserve(words.size());
        out.insert(out.end(), words.begin(), words.begin() + 5);
        for (const Inst& x : insts) out.insert(out.end(), words.begin() + x.at, words.begin() + x.at + x.wc);
        words.swap(out);
    }
    if (result) *result = res;
    return res.moved;
}

}  // namespace hsd
