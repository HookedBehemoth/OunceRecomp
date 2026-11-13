// Late scheduling of a translated stage's SPIR-V (direct_spv_late.h). Stands alone (tools/meshlab/spvlate.cpp builds it into
// a command, tests/test_direct_spv_late.py checks it); tools/meshlab/spv_sink.py is the same pass in Python, kept as the
// reference: the two give the same module word for word.
#include "direct_spv_late.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace hsd {
namespace {

struct Inst {
    uint32_t op, at, wc;
};

bool pure(uint32_t op) {
    // (OpExtInst, the access chains, composites and copies; conversions; arithmetic; logic and comparisons; shifts and bits)
    return op == 12 || op == 65 || op == 66 || op == 67 || (op >= 77 && op <= 83) || (op >= 109 && op <= 124) || (op >= 126 && op <= 152) ||
           (op >= 154 && op <= 191) || (op >= 194 && op <= 205);
}
bool atomic(uint32_t op) { return op >= 227 && op <= 242; }
bool writes(uint32_t op) { return op == 62 || op == 63 || op == 64; }   // (OpStore, OpCopyMemory, OpCopyMemorySized)
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
bool block_end(uint32_t op) { return op == 246 || op == 247 || (op >= 249 && op <= 255); }   // (merges and terminators)

constexpr uint32_t kPhysical = 5349, kFunctionClass = 7;

struct Func {
    size_t first = 0, last = 0;
    bool writes = false;
    std::vector<uint32_t> calls;
};

}  // namespace

uint32_t spv_schedule_late(std::vector<uint32_t>& words) {
    if (words.size() < 5 || words[0] != 0x07230203) return 0;
    std::vector<Inst> insts;
    for (size_t i = 5; i < words.size();) {
        const uint32_t wc = words[i] >> 16, op = words[i] & 0xffff;
        if (wc == 0 || i + wc > words.size()) return 0;
        insts.push_back({op, (uint32_t)i, wc});
        i += wc;
    }
    const uint32_t* w = words.data();
    std::unordered_map<uint32_t, uint32_t> pointer_class, type_of, width;
    const auto get = [](const std::unordered_map<uint32_t, uint32_t>& m, uint32_t k, uint32_t otherwise) {
        const auto it = m.find(k);
        return it == m.end() ? otherwise : it->second;
    };
    bool inside = false;
    for (const Inst& x : insts) {
        const uint32_t* v = w + x.at;
        if (x.op == 32 && x.wc >= 4) pointer_class[v[1]] = v[2];
        else if ((x.op == 21 || x.op == 22) && x.wc >= 3) width[v[1]] = std::max(1u, v[2] / 32);
        else if (x.op == 20 && x.wc >= 2) width[v[1]] = 1;
        else if (x.op == 23 && x.wc >= 4) width[v[1]] = get(width, v[2], 1) * v[3];
        if (x.op == 54) inside = true;
        else if (x.op == 56) inside = false;
        else if (((inside && has_result(x.op)) || x.op == 59) && x.wc >= 3) type_of[v[2]] = v[1];   // (a function's values, the module's variables)
    }
    const auto class_of = [&](uint32_t pointer, uint32_t otherwise) {   // (the storage class a pointer value points into)
        const auto t = type_of.find(pointer);
        return t == type_of.end() ? otherwise : get(pointer_class, t->second, otherwise);
    };
    // (the functions: which write memory that is not their own, themselves or through a call)
    std::unordered_map<uint32_t, Func> funcs;
    std::vector<uint32_t> func_order;
    {
        Func* cur = nullptr;
        for (size_t n = 0; n < insts.size(); n++) {
            const Inst& x = insts[n];
            const uint32_t* v = w + x.at;
            if (x.op == 54 && x.wc >= 3) {
                cur = &funcs[v[2]];
                cur->first = n;
                func_order.push_back(v[2]);
            } else if (x.op == 56) {
                if (cur) cur->last = n;
                cur = nullptr;
            } else if (cur) {
                if (writes(x.op) && x.wc >= 2 && class_of(v[1], kFunctionClass) != kFunctionClass) cur->writes = true;
                if (atomic(x.op) || x.op == 224 || x.op == 225 || x.op == 4428 || x.op == 4429 || x.op == 5294 || x.op == 5295) cur->writes = true;
                if (x.op == 57 && x.wc >= 4) cur->calls.push_back(v[3]);
            }
        }
        if (cur) return 0;
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& [id, f] : funcs) {
            if (f.writes) continue;
            for (const uint32_t c : f.calls) {
                const auto it = funcs.find(c);
                if (it == funcs.end() || it->second.writes) {
                    f.writes = changed = true;
                    break;
                }
            }
        }
    }
    // (does the stage write memory by address in code its entry points reach? then loads by address stay where they are)
    bool writes_physical = false;
    {
        std::unordered_set<uint32_t> reach;
        std::vector<uint32_t> todo;
        for (const Inst& x : insts)
            if (x.op == 15 && x.wc >= 3) todo.push_back(w[x.at + 2]);
        while (!todo.empty()) {
            const uint32_t f = todo.back();
            todo.pop_back();
            const auto it = funcs.find(f);
            if (it == funcs.end() || !reach.insert(f).second) continue;
            for (const uint32_t c : it->second.calls) todo.push_back(c);
            for (size_t n = it->second.first; n <= it->second.last; n++) {
                const Inst& x = insts[n];
                if ((writes(x.op) && x.wc >= 2 && class_of(w[x.at + 1], ~0u) == kPhysical) || atomic(x.op)) writes_physical = true;
            }
        }
    }
    const auto movable = [&](const Inst& x) {
        if (pure(x.op)) return x.wc >= 3;
        const uint32_t* v = w + x.at;
        if (x.op == 61 && x.wc >= 4) {   // (OpLoad: of memory nothing in the stage writes)
            const uint32_t cls = class_of(v[3], ~0u);
            return cls == 0 || cls == 1 || cls == 2 || cls == 9 || (cls == kPhysical && !writes_physical);
        }
        if (x.op == 57 && x.wc >= 4) {   // (OpFunctionCall: of a function that only reads)
            const auto it = funcs.find(v[3]);
            return it != funcs.end() && !it->second.writes && !writes_physical;
        }
        return false;
    };

    std::vector<Inst> out_insts = insts;
    uint32_t moved = 0;
    // (scratch of one block, reused)
    std::vector<Inst> block;
    std::vector<std::vector<uint32_t>> deps, operands, users;
    std::vector<uint32_t> remaining, size, order, stack, work, near, seen;
    std::vector<uint8_t> emitted, is_movable;
    std::unordered_map<uint32_t, uint32_t> where;
    for (const uint32_t fid : func_order) {
        const Func& f = funcs[fid];
        // (how many instructions of the function use each value, the one that makes it not counted)
        std::unordered_map<uint32_t, uint32_t> uses_total;
        {
            std::unordered_set<uint32_t> defined;
            for (size_t n = f.first; n <= f.last; n++)
                if (has_result(insts[n].op) && insts[n].wc >= 3) defined.insert(w[insts[n].at + 2]);
            for (size_t n = f.first; n <= f.last; n++) {
                const Inst& x = insts[n];
                const uint32_t* v = w + x.at;
                const uint32_t own = has_result(x.op) && x.wc >= 3 ? v[2] : 0;
                for (uint32_t j = 1; j < x.wc; j++) {
                    const uint32_t id = v[j];
                    if (id == own || !defined.count(id)) continue;
                    bool again = false;
                    for (uint32_t k = 1; k < j && !again; k++) again = v[k] == id;
                    if (!again) uses_total[id]++;
                }
            }
        }
        size_t out_at = f.first;
        const auto flush = [&] {
            const uint32_t n_block = (uint32_t)block.size();
            if (!n_block) return;
            where.clear();
            is_movable.assign(n_block, 0), emitted.assign(n_block, 0), remaining.assign(n_block, 0), size.assign(n_block, 1), seen.assign(n_block, 0);
            if (deps.size() < n_block) deps.resize(n_block), operands.resize(n_block), users.resize(n_block);
            for (uint32_t n = 0; n < n_block; n++) deps[n].clear(), operands[n].clear(), users[n].clear();
            order.clear();
            for (uint32_t n = 0; n < n_block; n++) {
                const Inst& x = block[n];
                is_movable[n] = movable(x);
                const uint32_t* v = w + x.at;
                for (uint32_t j = 1; j < x.wc; j++) {   // (operands made earlier in this block, each once, in the instruction's order)
                    const auto it = where.find(v[j]);
                    if (it == where.end()) continue;
                    const uint32_t k = it->second;
                    if (std::find(operands[n].begin(), operands[n].end(), k) != operands[n].end()) continue;
                    operands[n].push_back(k), users[k].push_back(n), remaining[k]++;
                    if (is_movable[k]) deps[n].push_back(k);
                }
                if (has_result(x.op) && x.wc >= 3) where[v[2]] = n;
            }
            for (uint32_t n = 0; n < n_block; n++) {
                const Inst& x = block[n];
                if (has_result(x.op) && x.wc >= 3) {
                    const auto it = uses_total.find(w[x.at + 2]);
                    if (it != uses_total.end() && it->second > remaining[n]) remaining[n] += 1u << 20;   // (used in another block: it does not die here)
                }
                uint64_t s = 1;
                for (const uint32_t d : deps[n]) s += size[d];
                size[n] = (uint32_t)std::min<uint64_t>(s, 1u << 30);
            }
            const auto values = [&](uint32_t n) {
                const Inst& x = block[n];
                return has_result(x.op) && x.wc >= 3 ? get(width, w[x.at + 1], 1) : 0u;
            };
            uint32_t epoch = 0;
            // (instruction k goes here; then every movable instruction that is ready and frees at least as many values as it
            // makes, its operands' last use, goes right after it, and so on)
            // (head: k is one of the block's first instructions, its label, phis and variables, placed already, nothing may go
            // between them: only what follows from it is looked for)
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
                        bool ready = true;
                        uint32_t freed = 0;
                        for (const uint32_t d : operands[c]) {
                            if (!emitted[d]) {
                                ready = false;
                                break;
                            }
                            if (remaining[d] == 1) freed += values(d);
                        }
                        // (a component taken out of a vector is the vector's own register)
                        if (ready && (block[c].op == 81 || values(c) <= freed)) work.push_back(c);
                    }
                }
            };
            const auto emit = [&](uint32_t first) {
                stack.assign(1, first);
                std::vector<uint32_t> todo;
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
                        std::sort(todo.begin(), todo.end(), [&](uint32_t a, uint32_t b) { return size[a] != size[b] ? size[a] < size[b] : a > b; });
                        stack.insert(stack.end(), todo.begin(), todo.end());
                        continue;
                    }
                    stack.pop_back();
                    place(k, false);
                }
            };
            uint32_t tail = n_block;
            while (tail > 0 && block_end(block[tail - 1].op)) tail--;
            uint32_t first = 0;   // (the label, the phis, the variables and their line marks stay first, together)
            for (; first < tail; first++) {
                const uint32_t op = block[first].op;
                if (op != 248 && op != 245 && op != 59 && op != 8 && op != 317) break;
                emitted[first] = 1;
                order.push_back(first);
                for (const uint32_t d : operands[first]) remaining[d]--;
            }
            for (uint32_t n = 0; n < first; n++) place(n, true);
            for (uint32_t n = first; n < tail; n++)
                if (!is_movable[n]) emit(n);
            for (uint32_t n = 0; n < tail; n++)
                if (!emitted[n]) emit(n);
            for (uint32_t n = tail; n < n_block; n++) emit(n);
            for (uint32_t n = 0; n < n_block; n++) {
                moved += order[n] != n;
                out_insts[out_at++] = block[order[n]];
            }
            block.clear();
        };
        for (size_t n = f.first; n <= f.last; n++) {
            if (insts[n].op == 248) flush();   // (OpLabel)
            block.push_back(insts[n]);
        }
        flush();
    }
    if (!moved) return 0;
    std::vector<uint32_t> res;
    res.reserve(words.size());
    res.insert(res.end(), words.begin(), words.begin() + 5);
    for (const Inst& x : out_insts) res.insert(res.end(), words.begin() + x.at, words.begin() + x.at + x.wc);
    words.swap(res);
    return moved;
}

}  // namespace hsd
