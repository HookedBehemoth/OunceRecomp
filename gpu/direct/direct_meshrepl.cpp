// Hand-written mesh stages in place of the translator's (direct_meshrepl.h).
#include "direct_meshrepl.h"

#include <cstdio>
#include <map>
#include <set>

#include "direct_internal.h"
#include "shaders/mesh/40d0c0768e874600_spv.h"

namespace hsd {
namespace {

// (one line a stage of gpu/direct/shaders/mesh/swap.txt; tests/test_direct_mesh_repl.py holds the two together)
#define MESH_REPL(code, task, words, n, ...) {0x##code##ull, task, words, (uint32_t)(sizeof(words) / sizeof(words[0])), {__VA_ARGS__}, n}
const MeshRepl kTable[] = {
    MESH_REPL(40d0c0768e874600, true, mesh_40d0c0768e874600_spv, 5, 0, 3, 4, 5, 6),
};
#undef MESH_REPL

/// What of a mesh stage's SPIR-V the rest of the program depends on
struct Interface {
    std::map<uint32_t, std::vector<uint32_t>> modes, mode_ids;   // (execution mode -> its operands)
    std::set<uint32_t> locations, builtins;            // (of output variables)
    std::set<uint64_t> uniforms;                       // (set << 32 | binding of uniform-class variables)
    bool ok = false;
};

Interface read_interface(const uint32_t* w, size_t n) {
    Interface f;
    if (n < 5 || w[0] != 0x07230203) return f;
    std::map<uint32_t, uint32_t> location, builtin, set, binding, storage, constant;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (wc == 0 || i + wc > n) return f;
        if (op == 16 && wc >= 3) {   // (OpExecutionMode)
            f.modes[w[i + 2]].assign(w + i + 3, w + i + wc);
        } else if (op == 331 && wc >= 3) {   // (OpExecutionModeId: glslang's LocalSizeId, kept as LocalSize once the constants are read)
            f.mode_ids[w[i + 2]].assign(w + i + 3, w + i + wc);
        } else if (op == 43 && wc == 4) {   // (OpConstant of one word)
            constant[w[i + 2]] = w[i + 3];
        } else if (op == 71 && wc >= 3) {   // (OpDecorate)
            const uint32_t id = w[i + 1], d = w[i + 2], v = wc > 3 ? w[i + 3] : 0;
            if (d == 30) location[id] = v;
            else if (d == 11) builtin[id] = v;
            else if (d == 34) set[id] = v;
            else if (d == 33) binding[id] = v;
        } else if (op == 59 && wc >= 4) {   // (OpVariable)
            storage[w[i + 2]] = w[i + 3];
        } else if (op == 54) {   // (OpFunction: the declarations are over)
            break;
        }
        i += wc;
    }
    for (const auto& [id, cls] : storage) {
        if (cls == 3) {   // (Output)
            if (const auto l = location.find(id); l != location.end()) f.locations.insert(l->second);
            if (const auto b = builtin.find(id); b != builtin.end()) f.builtins.insert(b->second);
        } else if (cls == 2 || cls == 12 || cls == 0) {   // (Uniform, StorageBuffer, UniformConstant)
            const auto b = binding.find(id);
            if (b != binding.end()) f.uniforms.insert((uint64_t)(set.count(id) ? set[id] : 0) << 32 | b->second);
        }
    }
    if (const auto l = f.mode_ids.find(38); l != f.mode_ids.end()) {   // (LocalSizeId as LocalSize)
        std::vector<uint32_t> v;
        for (const uint32_t id : l->second) v.push_back(constant.count(id) ? constant[id] : ~0u);
        f.modes[17] = v;
    }
    // (a block's built-ins are member decorations: the position is every mesh stage's; SubgroupSize modes are the device's)
    f.modes.erase(35), f.modes.erase(36), f.modes.erase(37);   // (subgroup sizes: not part of the interface)
    f.modes.erase(4461), f.modes.erase(4462), f.modes.erase(4463);   // (float controls: the stage's own arithmetic)
    f.ok = true;
    return f;
}

}  // namespace

const MeshRepl* mesh_repl_find(uint64_t code, bool task) {
    static const bool off = dbg("nomeshrepl");
    if (off) return nullptr;
    for (const MeshRepl& r : kTable)
        if (r.code == code && r.task == task) return &r;
    return nullptr;
}

bool mesh_repl_fits(const MeshRepl& r, const std::vector<uint32_t>& translated, const std::vector<uint32_t>& ubos, uint32_t first_binding,
                    std::string& why) {
    const Interface a = read_interface(translated.data(), translated.size()), b = read_interface(r.words, r.count);
    char t[160];
    if (!a.ok || !b.ok) return why = "a stage's SPIR-V is not readable", false;
    for (const auto& [mode, ops] : a.modes) {
        const auto o = b.modes.find(mode);
        if (o == b.modes.end() || o->second != ops)
            return std::snprintf(t, sizeof t, "execution mode %u differs (the translated stage's first operand %u)", mode, ops.empty() ? 0u : ops[0]), why = t, false;
    }
    for (const auto& [mode, ops] : b.modes)
        if (!a.modes.count(mode)) return std::snprintf(t, sizeof t, "execution mode %u only in the replacement", mode), why = t, false;
    if (a.locations != b.locations) {
        for (const uint32_t l : a.locations)
            if (!b.locations.count(l)) return std::snprintf(t, sizeof t, "output location %u only in the translated stage", l), why = t, false;
        for (const uint32_t l : b.locations)
            if (!a.locations.count(l)) return std::snprintf(t, sizeof t, "output location %u only in the replacement", l), why = t, false;
    }
    for (const uint32_t x : b.builtins)
        if (!a.builtins.count(x)) return std::snprintf(t, sizeof t, "output built-in %u only in the replacement", x), why = t, false;
    // (its bindings 0.. are written for the translated stage's uniform buffers in their order, from binding 0)
    if (first_binding != 0) return why = "the stage's bindings do not start at 0", false;
    if (ubos.size() != r.ubo_count) return std::snprintf(t, sizeof t, "%zu uniform buffers, the replacement has %u", ubos.size(), r.ubo_count), why = t, false;
    for (uint32_t i = 0; i < r.ubo_count; i++)
        if (ubos[i] != r.ubos[i]) return std::snprintf(t, sizeof t, "binding %u is c%u, the replacement's c%u", i, ubos[i], (unsigned)r.ubos[i]), why = t, false;
    for (const uint64_t u : b.uniforms)
        if (!a.uniforms.count(u) || u >= r.ubo_count)
            return std::snprintf(t, sizeof t, "uniform binding %u of set %u is not a uniform buffer of the translated stage", (unsigned)u, (unsigned)(u >> 32)), why = t, false;
    return true;
}

}  // namespace hsd
