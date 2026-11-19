// censustool: every stage of the game's shader seed (tools/shadercensus/seed.py) through the direct back end's
// translator fork as the back end translates it (tools/shaders/offline.h), a static profile of each from the IR (inputs and
// outputs, texture samples by kind and by where they sit, memory, control flow and loop bounds) and, with --compile, RADV's
// compile of its SPIR-V on the back end's pipeline layout (vk_pool.cpp make_set) and the driver's statistics of it
// (VK_KHR_pipeline_executable_properties: ACO's instruction count, code size, registers, LDS, scratch, its latency and throughput
// estimates). The back end makes unlinked shader objects, which report no statistics; the tool makes a pipeline instead: a
// graphics stage with the partner stage of the first program that uses it (vertex + fragment, mesh + fragment; a geometry stage
// between its program's vertex and fragment stages), a task or compute stage alone. For the shader census (docs/direct/shader-costs.md); nothing of the game is in this file.
//
//   censustool <seed> <first> <count> [--compile] [--spv <dir>] [--only <index>...] [--front old|new]
//              [--late <kinds>] [--budget N] [--sink-peak N] [--objects] [--list]
// One JSON line a stage on stdout; "@@stage <index>" on stderr before each (a crash names its stage).
// --late <kinds>: the back end's register-pressure pass (renderer/vk_spv_late.h) on the stages of these kinds (a comma
// list of vertex, fragment, mesh, task, compute; "all"), as the game applies it after EmitSPIRV. Every line carries the pass's
// own count for the stage ("late": the most 32-bit values held before and after, instructions moved, the pass's time), whether
// or not it is applied. --objects: the stage also made as the shader object the game makes of it (unlinked; RADV reports no
// statistics of those: with RADV_DEBUG=shaders its compile is printed between "@@object <index> <key> <kind>" and "@@end" on
// stderr, which tools/shaders/late_gate.py reads). --list: a line a stage of the seed (index, kind), nothing translated.
// --front new (the default, the renderer's) or old (the lowering to Maxwell, for comparisons). With the new front a partner
// stage it cannot translate is left out, its error in "partner_error".
// --mma-software: matrix multiplies through the software function (emit_spirv_warp.cpp) instead of cooperative matrices,
// as the renderer chooses on a device without them.
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "../shaders/offline.h"
#include "../../renderer/vk_spv_late.cpp"
#include "../shaders/ir_pretty.h"
#include "shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "shader_recompiler/frontend/ir/modifiers.h"

namespace {
using namespace Shader::IR;

// ---- the seed ----
struct SeedStage {
    uint32_t stage = 0;
    const uint8_t* ctl = nullptr;
    const uint8_t* code = nullptr;
    size_t code_n = 0;
};
struct Seed {
    std::vector<uint8_t> blob;
    std::vector<SeedStage> stages;
    std::vector<std::vector<uint32_t>> programs;
};
Seed seed;
bool seed_read(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    seed.blob.resize((size_t)std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(seed.blob.data(), 1, seed.blob.size(), f) != seed.blob.size()) return false;
    std::fclose(f);
    if (seed.blob.size() < 16 || std::memcmp(seed.blob.data(), "HKSEED01", 8)) return false;
    uint32_t np, ns;
    std::memcpy(&np, &seed.blob[8], 4), std::memcpy(&ns, &seed.blob[12], 4);
    size_t at = 16;
    for (uint32_t i = 0; i < ns; i++) {
        uint32_t st, cn, bn;
        std::memcpy(&st, &seed.blob[at + 8], 4), std::memcpy(&cn, &seed.blob[at + 12], 4), std::memcpy(&bn, &seed.blob[at + 16], 4);
        SeedStage s;
        s.stage = st, s.ctl = &seed.blob[at + 20], s.code = s.ctl + cn, s.code_n = bn;
        seed.stages.push_back(s);
        at += 20 + cn + bn;
    }
    for (uint32_t i = 0; i < np; i++) {
        uint32_t n;
        std::memcpy(&n, &seed.blob[at], 4);
        std::vector<uint32_t> ids(n);
        std::memcpy(ids.data(), &seed.blob[at + 4], 4 * n);
        seed.programs.push_back(std::move(ids));
        at += 4 + 4 * n;
    }
    return true;
}

// ---- the IR's profile ----
enum Cls { C_CONST = 0, C_UNIFORM = 1, C_DATA = 2, C_INPUT = 3 };   // (what a condition depends on: worst wins)
const char* cls_name(int c) { return c == C_CONST ? "const" : c == C_UNIFORM ? "uniform" : c == C_DATA ? "data" : "input"; }

bool is_load_op(std::string_view n) {
    return n.starts_with("LoadGlobal") || n.starts_with("LoadStorage") || n.starts_with("LoadLocal") || n.starts_with("LoadShared") ||
           n.starts_with("Image") || n.find("Atomic") != std::string_view::npos || n == "IsbeRead";
}
bool is_input_op(std::string_view n) {
    return n.starts_with("GetAttribute") || n == "WorkgroupId" || n == "LocalInvocationId" || n == "InvocationId" || n == "LaneId" ||
           n == "SampleId" || n == "IsHelperInvocation" || n == "YDirection" || n == "InvocationInfo" || n.starts_with("Subgroup") ||
           n.starts_with("Shuffle") || n.starts_with("Vote") || n.starts_with("DPdx") || n.starts_with("DPdy");
}
/// What a value depends on: constants (immediates, the program's c1), uniforms (driver banks, game buffers), inputs
/// (attributes, thread ids), data (memory, textures)
int classify(const Value& v0, std::unordered_map<const Inst*, int>& memo, int depth = 0) {
    const Value v = v0.Resolve();
    if (v.IsEmpty() || v.IsImmediate()) return C_CONST;
    const Inst* i = v.InstRecursive();
    if (const auto it = memo.find(i); it != memo.end()) return it->second;
    if (depth > 400) return C_DATA;
    memo[i] = C_CONST;   // (a phi's cycle: what the rest of it gives)
    const std::string_view n = NameOf(i->GetOpcode());
    int c = C_CONST;
    if (n.starts_with("GetCbuf")) {
        const Value bank = i->Arg(0);
        c = bank.IsImmediate() && bank.U32() == 1 ? C_CONST : C_UNIFORM;
        c = std::max(c, classify(i->Arg(1), memo, depth + 1));
        if (!bank.IsImmediate()) c = std::max(c, classify(bank, memo, depth + 1));
    } else if (is_load_op(n)) {
        c = C_DATA;
    } else if (is_input_op(n)) {
        c = C_INPUT;
    } else {
        for (size_t a = 0; a < i->NumArgs(); a++) c = std::max(c, classify(i->Arg(a), memo, depth + 1));
    }
    memo[i] = c;
    return c;
}

/// A constant-bounded loop's trip count: its header's phis stepped by evaluating their back-edge values and the exit
/// conditions on 32-bit integers and booleans (the ops below); nullopt for anything else or past 4096 trips
struct Eval {
    const std::set<const Block*>& body;
    std::unordered_map<const Inst*, std::pair<Value, Value>> phis;   // (the loop's phis: value from outside, from the back edge)
    std::vector<std::unordered_map<const Inst*, uint64_t>> memo;      // (a trip's values, each computed when asked)
    bool ok = true;
    int trip = 0;
    uint64_t val(const Value& v0, int depth = 0) {
        const Value v = v0.Resolve();
        if (!ok || depth > 200) return ok = false, 0;
        if (v.IsImmediate()) {
            switch (v.Type()) {
            case Type::U1: return v.U1();
            case Type::U32: return v.U32();
            default: return ok = false, 0;
            }
        }
        const Inst* i = v.InstRecursive();
        if ((size_t)trip >= memo.size()) memo.resize((size_t)trip + 1);
        if (const auto it = memo[(size_t)trip].find(i); it != memo[(size_t)trip].end()) return it->second;
        uint64_t r;
        if (const auto ph = phis.find(i); ph != phis.end()) {   // (trip 0: the value from outside; then the back edge's, a trip before)
            if (trip == 0) r = val(ph->second.first, depth + 1);
            else {
                trip--;
                r = val(ph->second.second, depth + 1);
                trip++;
            }
        } else {
            r = op(i, depth);
        }
        if (ok) memo[(size_t)trip][i] = r;
        return r;
    }
    uint64_t op(const Inst* i, int depth) {
        const auto a = [&](size_t k) { return (uint32_t)val(i->Arg(k), depth + 1); };
        switch (i->GetOpcode()) {
        case Opcode::ConditionRef: case Opcode::Identity: return val(i->Arg(0), depth + 1);
        case Opcode::IAdd32: return (uint32_t)(a(0) + a(1));
        case Opcode::INeg32: return (uint32_t)(0u - a(0));
        case Opcode::UMin32: return std::min(a(0), a(1));
        case Opcode::UMax32: return std::max(a(0), a(1));
        case Opcode::SMin32: return (uint32_t)std::min((int32_t)a(0), (int32_t)a(1));
        case Opcode::SMax32: return (uint32_t)std::max((int32_t)a(0), (int32_t)a(1));
        case Opcode::BitwiseNot32: return ~a(0);
        case Opcode::BitFieldUExtract: {
            const uint32_t x = a(0), off = a(1) & 31, cnt = a(2);
            return cnt >= 32 ? x >> off : (x >> off) & ((1u << cnt) - 1);
        }
        case Opcode::ISub32: return (uint32_t)(a(0) - a(1));
        case Opcode::IMul32: return (uint32_t)(a(0) * a(1));
        case Opcode::ShiftLeftLogical32: return (uint32_t)(a(0) << (a(1) & 31));
        case Opcode::ShiftRightLogical32: return a(0) >> (a(1) & 31);
        case Opcode::ShiftRightArithmetic32: return (uint32_t)((int32_t)a(0) >> (a(1) & 31));
        case Opcode::BitwiseAnd32: return a(0) & a(1);
        case Opcode::BitwiseOr32: return a(0) | a(1);
        case Opcode::BitwiseXor32: return a(0) ^ a(1);
        case Opcode::SLessThan: return (int32_t)a(0) < (int32_t)a(1);
        case Opcode::ULessThan: return a(0) < a(1);
        case Opcode::IEqual: return a(0) == a(1);
        case Opcode::SLessThanEqual: return (int32_t)a(0) <= (int32_t)a(1);
        case Opcode::ULessThanEqual: return a(0) <= a(1);
        case Opcode::SGreaterThan: return (int32_t)a(0) > (int32_t)a(1);
        case Opcode::UGreaterThan: return a(0) > a(1);
        case Opcode::INotEqual: return a(0) != a(1);
        case Opcode::SGreaterThanEqual: return (int32_t)a(0) >= (int32_t)a(1);
        case Opcode::UGreaterThanEqual: return a(0) >= a(1);
        case Opcode::LogicalOr: return val(i->Arg(0), depth + 1) || val(i->Arg(1), depth + 1);
        case Opcode::LogicalAnd: return val(i->Arg(0), depth + 1) && val(i->Arg(1), depth + 1);
        case Opcode::LogicalXor: return val(i->Arg(0), depth + 1) != val(i->Arg(1), depth + 1);
        case Opcode::LogicalNot: return !val(i->Arg(0), depth + 1);
        case Opcode::SelectU1: case Opcode::SelectU32: return val(i->Arg(0), depth + 1) ? val(i->Arg(1), depth + 1) : val(i->Arg(2), depth + 1);
        default: return ok = false, 0;
        }
    }
};

struct LoopInfo {
    int cls = C_CONST;
    int64_t trips = -1;   // (-1: not known)
    int depth = 1;
};

struct Profile {
    std::map<std::string, int> ops;
    int insts = 0;
    // samples: by kind, by dimension, in loops/branches
    std::map<std::string, int> tex_kind, tex_dim;
    int samples = 0, samples_in_branch = 0, samples_in_loop = 0, samples_in_unknown_loop = 0;
    double samples_weighted = 0;   // (constant-bounded loops by their trips)
    std::set<uint32_t> tex_desc_used;
    int cbuf_loads = 0, cbuf_loads_c1 = 0, cbuf_loads_driver = 0, cbuf_loads_game = 0, cbuf_loads_indexed = 0;
    int global_loads = 0, global_stores = 0, atomics = 0, shared_loads = 0, shared_stores = 0, local_ops = 0, barriers = 0;
    int branches = 0, branch_cls[4]{}, discards = 0, returns = 0;
    std::vector<LoopInfo> loops;
    // translation overhead markers
    int subgroup_ops = 0, cc_ops = 0, isbe_ops = 0, select_u1 = 0, fp_ops = 0, int_ops = 0, transcendental = 0, conversions = 0;
    double insts_weighted = 0;
};

Profile profile_of(Program& p) {
    Profile r;
    std::unordered_map<const Inst*, int> memo;
    // (the structure: each block's if and loop nesting; each loop's blocks and exit conditions)
    struct Open { bool loop; int index; };
    std::vector<Open> stack;
    std::unordered_map<const Block*, std::pair<int, int>> where;   // (block -> if depth, innermost loop index or -1)
    std::vector<std::set<const Block*>> loop_blocks;
    std::vector<std::vector<std::pair<Value, bool>>> loop_exits;   // (cond, exits when true)
    std::vector<const Block*> loop_header, loop_pre;
    std::vector<int> loop_parent;
    const Block* last_block = nullptr;
    for (const auto& n : p.syntax_list) {
        using T = AbstractSyntaxNode::Type;
        int ifd = 0, lp = -1;
        for (const Open& o : stack) o.loop ? (void)(lp = o.index) : (void)ifd++;
        switch (n.type) {
        case T::Block:
            last_block = n.data.block;
            where[n.data.block] = {ifd, lp};
            for (const Open& o : stack)
                if (o.loop) loop_blocks[o.index].insert(n.data.block);
            break;
        case T::If:
            r.branches++;
            r.branch_cls[classify(n.data.if_node.cond, memo)]++;
            stack.push_back({false, 0});
            break;
        case T::EndIf:
            if (!stack.empty()) stack.pop_back();
            break;
        case T::Loop:
            loop_parent.push_back(lp);
            loop_blocks.emplace_back(), loop_exits.emplace_back(), loop_header.push_back(n.data.loop.body), loop_pre.push_back(last_block);
            stack.push_back({true, (int)loop_blocks.size() - 1});
            break;
        case T::Repeat:
            if (lp >= 0) loop_exits[lp].push_back({n.data.repeat.cond, false});
            while (!stack.empty() && !stack.back().loop) stack.pop_back();
            if (!stack.empty()) stack.pop_back();
            break;
        case T::Break:
            if (lp >= 0) loop_exits[lp].push_back({n.data.break_node.cond, true});
            break;
        case T::Return: r.returns++; break;
        case T::Unreachable: break;
        }
    }
    // (each loop: what its exits depend on, and its trips when they are constants)
    for (size_t k = 0; k < loop_blocks.size(); k++) {
        LoopInfo li;
        for (int q = loop_parent[k]; q >= 0; q = loop_parent[q]) li.depth++;
        for (const auto& [c, _] : loop_exits[k]) li.cls = std::max(li.cls, classify(c, memo));
        if (li.cls == C_CONST && !loop_exits[k].empty()) {
            Eval e{loop_blocks[k]};
            // (the loop's phis: in its first block, or in the block the structurizer put before it; an argument from inside the
            // loop and one from outside)
            for (const Block* hb : {loop_header[k], loop_pre[k]}) {
                if (!hb) continue;
                for (const Inst& i : hb->Instructions()) {
                    if (i.GetOpcode() != Opcode::Phi) continue;
                    int in = -1, out = -1;
                    for (size_t a = 0; a < i.NumArgs(); a++) (loop_blocks[k].count(i.PhiBlock(a)) ? in : out) = (int)a;
                    if (in >= 0 && out >= 0) e.phis[&i] = {i.Arg((size_t)out), i.Arg((size_t)in)};
                }
            }
            int64_t trips = 0;
            for (e.trip = 0; e.ok && e.trip < 1024; e.trip++) {   // (trip t runs the body once more: its exits taken with this trip's values)
                trips++;
                bool leave = false;
                for (const auto& [c, when] : loop_exits[k])
                    if ((e.val(c) != 0) == when) {
                        leave = true;
                        break;
                    }
                if (leave) break;
            }
            const auto& phis = e.phis;
            if (e.ok && e.trip >= 1024) li.trips = -2;   // (constant, and past 1024 trips)
            if (e.trip >= 1024) e.ok = false;
            if (e.ok && trips < 4096) li.trips = trips;
            if (std::getenv("CENSUS_DEBUG"))
                std::fprintf(stderr, "loop %zu: %zu phis, %zu exits, ok %d, trips %lld, header %p pre %p\n", k, phis.size(), loop_exits[k].size(),
                             (int)e.ok, (long long)trips, (const void*)loop_header[k], (const void*)loop_pre[k]);
        }
        r.loops.push_back(li);
    }
    auto weight_of = [&](int lp, bool& unknown) {
        double w = 1;
        for (int q = lp; q >= 0; q = loop_parent[q]) {
            if (r.loops[q].trips >= 0) w *= (double)r.loops[q].trips;
            else unknown = true;
        }
        return w;
    };
    for (const Block* b : p.blocks) {
        const auto wi = where.find(b);
        const int ifd = wi == where.end() ? 0 : wi->second.first, lp = wi == where.end() ? -1 : wi->second.second;
        bool unknown = false;
        const double w = weight_of(lp, unknown);
        for (const Inst& i : b->Instructions()) {
            const Opcode op = i.GetOpcode();
            const std::string_view n = NameOf(op);
            if (op == Opcode::Identity || op == Opcode::Void || op == Opcode::ConditionRef || op == Opcode::Phi || n == "Reference" ||
                n == "PhiMove" || n == "Prologue" || n == "Epilogue")
                continue;
            if (!i.MayHaveSideEffects() && !i.HasUses()) continue;
            r.insts++;
            r.insts_weighted += w;
            r.ops[std::string(n)]++;
            if (n.starts_with("Image") && n != "ImageQueryDimensions" && n != "ImageQueryLod" && n != "ImageRead" && n != "ImageWrite" &&
                n.find("Atomic") == std::string_view::npos) {
                r.samples++;
                if (ifd) r.samples_in_branch++;
                if (lp >= 0) r.samples_in_loop++;
                if (unknown) r.samples_in_unknown_loop++;
                r.samples_weighted += w;
                const TextureInstInfo ti{i.Flags<TextureInstInfo>()};
                r.tex_desc_used.insert(ti.descriptor_index);
                std::string k = n == "ImageSampleImplicitLod" ? "implicit" : n == "ImageSampleExplicitLod" ? "lod"
                                : n == "ImageSampleDrefImplicitLod" ? "shadow" : n == "ImageSampleDrefExplicitLod" ? "shadow_lod"
                                : n == "ImageGather" ? "gather" : n == "ImageGatherDref" ? "gather_shadow" : n == "ImageFetch" ? "fetch"
                                : n == "ImageGradient" ? "grad" : std::string(n);
                if (ti.has_bias) k += "+bias";
                r.tex_kind[k]++;
                static const char* dims[] = {"1d", "1d_array", "2d", "2d_array", "3d", "cube", "cube_array", "buffer"};
                const uint32_t t = (uint32_t)ti.type.Value();
                r.tex_dim[t < 8 ? dims[t] : "rect"]++;
            }
            if (n.starts_with("GetCbuf")) {
                r.cbuf_loads++;
                const Value bank = i.Arg(0);
                if (!i.Arg(1).IsImmediate()) r.cbuf_loads_indexed++;
                if (!bank.IsImmediate() || bank.U32() >= 3) r.cbuf_loads_game++;
                else if (bank.U32() == 1) r.cbuf_loads_c1++;
                else r.cbuf_loads_driver++;
            }
            if (n.starts_with("LoadGlobal") || n.starts_with("LoadStorage")) r.global_loads++;
            if (n.starts_with("WriteGlobal") || n.starts_with("WriteStorage")) r.global_stores++;
            if (n.find("Atomic") != std::string_view::npos) r.atomics++;
            if (n.starts_with("LoadShared")) r.shared_loads++;
            if (n.starts_with("WriteShared")) r.shared_stores++;
            if (n == "LoadLocal" || n == "WriteLocal") r.local_ops++;
            if (n == "Barrier") r.barriers++;
            if (n == "DemoteToHelperInvocation") r.discards++;
            if (n.starts_with("Subgroup") || n.starts_with("Shuffle") || n.starts_with("Vote") || n == "FindUMsb32" || n == "LaneId") r.subgroup_ops++;
            if (n.starts_with("Get") && n.ends_with("FromOp")) r.cc_ops++;
            if (n == "SelectU1") r.select_u1++;
            if (n.starts_with("Isbe")) r.isbe_ops++;
            if (n.starts_with("FP")) r.fp_ops++;
            if (n.starts_with("I") || n.starts_with("Shift") || n.starts_with("Bitwise") || n.starts_with("BitField") || n.starts_with("S") ||
                n.starts_with("U"))
                r.int_ops++;
            if (n.starts_with("FPExp2") || n.starts_with("FPLog2") || n.starts_with("FPRecip") || n.starts_with("FPSqrt") || n.starts_with("FPSin") ||
                n.starts_with("FPCos"))
                r.transcendental++;
            if (n.starts_with("Convert")) r.conversions++;
        }
    }
    return r;
}

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}
template <class M> std::string json_map(const M& m) {
    std::string o = "{";
    for (const auto& [k, v] : m) o += (o.size() > 1 ? "," : "") + json_str(k) + ":" + std::to_string(v);
    return o + "}";
}

// ---- the device: shader objects as the back end makes them ----
struct Dev {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    PFN_vkGetPipelineExecutablePropertiesKHR exec_props = nullptr;
    PFN_vkGetPipelineExecutableStatisticsKHR exec_stats = nullptr;
    VkShaderStageFlags sg32 = 0;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, 256};
} gdev;
bool dev_init() {
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "censustool", ai.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &ai;
    if (vkCreateInstance(&ii, nullptr, &gdev.inst) != VK_SUCCESS) return false;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(gdev.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(gdev.inst, &n, pds.data());
    for (VkPhysicalDevice p : pds) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || !gdev.phys) gdev.phys = p;
    }
    if (!gdev.phys) return false;
    VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceShaderObjectFeaturesEXT so{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesKHR rb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR};
    VkPhysicalDeviceMeshShaderFeaturesEXT ms{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
    pe.pNext = &wl;
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13, f13.pNext = &f14, f14.pNext = &so, so.pNext = &rb, rb.pNext = &ms, ms.pNext = &af;
    af.pNext = &pe;
    vkGetPhysicalDeviceFeatures2(gdev.phys, &f);
    f.features.robustBufferAccess = VK_FALSE, f14.pipelineRobustness = VK_FALSE;
    rb.robustBufferAccess2 = rb.robustImageAccess2 = VK_FALSE;
    ms.taskShader = VK_FALSE, ms.multiviewMeshShader = ms.primitiveFragmentShadingRateMeshShader = ms.meshShaderQueries = VK_FALSE;
    const char* exts[] = {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME,
                          VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME,
                          VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME};
    vkGetPhysicalDeviceQueueFamilyProperties(gdev.phys, &n, nullptr);
    const float prio = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = 0, q.queueCount = 1, q.pQueuePriorities = &prio;
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f, ci.queueCreateInfoCount = 1, ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = (uint32_t)(sizeof exts / sizeof *exts), ci.ppEnabledExtensionNames = exts;
    if (vkCreateDevice(gdev.phys, &ci, nullptr, &gdev.dev) != VK_SUCCESS) return false;
    gdev.exec_props = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(gdev.dev, "vkGetPipelineExecutablePropertiesKHR");
    gdev.exec_stats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(gdev.dev, "vkGetPipelineExecutableStatisticsKHR");
    VkPhysicalDeviceSubgroupSizeControlProperties sc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sc;
    vkGetPhysicalDeviceProperties2(gdev.phys, &p2);
    gdev.sg32 = sc.minSubgroupSize <= 32 && sc.maxSubgroupSize >= 32 ? sc.requiredSubgroupSizeStages : 0;
    {   // (vk_pool.cpp make_set: the pools' set's layout, the pipeline layout every shader object shares)
        using namespace Shader::Backend::SPIRV;
        VkDescriptorSetLayoutBinding b[kPoolBindings];
        VkDescriptorBindingFlags bf[kPoolBindings];
        for (uint32_t i = 0; i < kPoolBindings; i++) {
            const VkDescriptorType type = i < kPoolStorageBinding ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                        : i < kPoolSamplerBinding ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            b[i] = {i, type, type == VK_DESCRIPTOR_TYPE_SAMPLER ? kPoolSamplers : kPoolSlots, VK_SHADER_STAGE_ALL, nullptr};
            bf[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bfi.bindingCount = kPoolBindings, bfi.pBindingFlags = bf;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.pNext = &bfi, li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, li.bindingCount = kPoolBindings, li.pBindings = b;
        if (vkCreateDescriptorSetLayout(gdev.dev, &li, nullptr, &gdev.set_layout) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1, pli.pSetLayouts = &gdev.set_layout, pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &gdev.push;
        if (vkCreatePipelineLayout(gdev.dev, &pli, nullptr, &gdev.layout) != VK_SUCCESS) return false;
    }
    return true;
}
struct Made {   // (a stage translated: what a pipeline needs of it)
    VkShaderStageFlagBits vk = VK_SHADER_STAGE_VERTEX_BIT;
    std::vector<uint32_t> spv;
    std::unique_ptr<offline::Translation> t;
    offline::Code code;
    Shader::InputTopology input{};   // (a geometry stage's)
};
std::string json_str(const std::string& s);
/// The pipeline of a stage (and its partner) and the driver's statistics of every executable, as JSON
std::string compile(const Made& a, const Made* b, int& result, const Made* c = nullptr) {
    struct StageIn {
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sz{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    };
    std::vector<const Made*> made{&a};
    if (b) made.push_back(b);
    if (c) made.push_back(c);
    std::vector<StageIn> in(made.size());
    std::vector<VkPipelineShaderStageCreateInfo> st(made.size());
    for (size_t k = 0; k < made.size(); k++) {
        StageIn& x = in[k];
        x.mc.codeSize = made[k]->spv.size() * 4, x.mc.pCode = made[k]->spv.data();
        x.sz.requiredSubgroupSize = 32;
        if (gdev.sg32 & made[k]->vk) x.sz.pNext = &x.mc;
        st[k] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st[k].pNext = (gdev.sg32 & made[k]->vk) ? (const void*)&x.sz : (const void*)&x.mc;
        st[k].stage = made[k]->vk, st[k].pName = "main";
    }
    VkPipelineCreateFlags2CreateInfo fl{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO};
    fl.flags = VK_PIPELINE_CREATE_2_CAPTURE_STATISTICS_BIT_KHR;
    VkPipeline pl = VK_NULL_HANDLE;
    if (a.vk == VK_SHADER_STAGE_COMPUTE_BIT) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.pNext = &fl, ci.stage = st[0], ci.layout = gdev.layout;
        result = (int)vkCreateComputePipelines(gdev.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pl);
    } else {
        const Made* fs = nullptr;
        bool vertex = false, lines = false;
        for (const Made* m : made) {
            if (m->vk == VK_SHADER_STAGE_FRAGMENT_BIT) fs = m;
            if (m->vk == VK_SHADER_STAGE_VERTEX_BIT) vertex = true;
            if (m->vk == VK_SHADER_STAGE_GEOMETRY_BIT) lines = m->input == Shader::InputTopology::Lines;
        }
        std::vector<VkVertexInputAttributeDescription> attrs;
        VkVertexInputBindingDescription bind{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
        for (const Made* m : made)
            if (m->vk == VK_SHADER_STAGE_VERTEX_BIT)
                for (uint32_t i = 0; i < 32; i++)
                    if (m->t->program.info.loads.Generic(i)) attrs.push_back({i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = attrs.empty() ? 0 : 1, vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size(), vi.pVertexAttributeDescriptions = attrs.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE, ds.depthWriteEnable = VK_TRUE, ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkFormat colors[8];
        VkPipelineColorBlendAttachmentState att[8]{};
        uint32_t nc = 0;
        for (int i = 0; i < 8; i++) {
            const bool w = fs && fs->t->program.info.stores_frag_color[i];
            colors[i] = w ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_UNDEFINED;
            att[i].colorWriteMask = w ? 0xf : 0;
            if (w) nc = (uint32_t)i + 1;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = nc, cb.pAttachments = att;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = nc, ri.pColorAttachmentFormats = colors, ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        fl.pNext = &ri;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &fl;
        ci.stageCount = (uint32_t)st.size(), ci.pStages = st.data();
        ci.pVertexInputState = vertex ? &vi : nullptr, ci.pInputAssemblyState = vertex ? &ia : nullptr;
        ci.pViewportState = &vp, ci.pRasterizationState = &rs, ci.pMultisampleState = &ms, ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb, ci.pDynamicState = &dy, ci.layout = gdev.layout;
        result = (int)vkCreateGraphicsPipelines(gdev.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pl);
    }
    if (result != VK_SUCCESS || !pl) return "[]";
    std::string out = "[";
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = pl;
    uint32_t ne = 0;
    gdev.exec_props(gdev.dev, &pi, &ne, nullptr);
    std::vector<VkPipelineExecutablePropertiesKHR> ep(ne, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    gdev.exec_props(gdev.dev, &pi, &ne, ep.data());
    for (uint32_t e = 0; e < ne; e++) {
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = pl, ei.executableIndex = e;
        uint32_t ns = 0;
        gdev.exec_stats(gdev.dev, &ei, &ns, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> sv(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        gdev.exec_stats(gdev.dev, &ei, &ns, sv.data());
        out += std::string(e ? "," : "") + "{\"name\":" + json_str(ep[e].name) + ",\"stages\":" + std::to_string(ep[e].stages) +
               ",\"subgroup\":" + std::to_string(ep[e].subgroupSize);
        for (const auto& x : sv) {
            std::string v;
            switch (x.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: v = std::to_string(x.value.b32); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: v = std::to_string(x.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: v = std::to_string(x.value.u64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: v = std::to_string(x.value.f64); break;
            default: v = "0";
            }
            out += "," + json_str(x.name) + ":" + v;
        }
        out += "}";
    }
    vkDestroyPipeline(gdev.dev, pl, nullptr);
    return out + "]";
}

int spirv_instructions(const std::vector<uint32_t>& spv) {
    int n = 0;
    for (size_t at = 5; at < spv.size();) {
        const uint32_t wc = spv[at] >> 16;
        if (!wc) break;
        const uint32_t op = spv[at] & 0xffff;
        if (op != 8 /*Line*/ && op != 317 /*NoLine*/ && op != 5 /*Name*/ && op != 6 /*MemberName*/ && op != 71 /*Decorate*/ && op != 72) n++;
        at += wc;
    }
    return n;
}

// ---- one stage, as the back end translates it in the first program that uses it ----
struct Partners {
    int vs = -1, fs = -1, task = -1, mesh = -1, gs = -1;
    int program = -1;
};
std::vector<Partners> partners;
std::vector<std::vector<uint32_t>> programs_of;
void partners_make() {
    partners.assign(seed.stages.size(), {});
    programs_of.assign(seed.stages.size(), {});
    for (size_t pi = 0; pi < seed.programs.size(); pi++) {
        Partners p;
        for (uint32_t id : seed.programs[pi]) {
            const uint32_t st = seed.stages[id].stage;
            if (st == 0) p.vs = (int)id;
            else if (st == 1) p.fs = (int)id;
            else if (st == 6) p.task = (int)id;
            else if (st == 7) p.mesh = (int)id;
            else if (st == 2) p.gs = (int)id;
        }
        p.program = (int)pi;
        for (uint32_t id : seed.programs[pi]) {
            programs_of[id].push_back((uint32_t)pi);
            if (partners[id].program < 0) partners[id] = p;
        }
    }
}
offline::Code code_of(int id) {
    const SeedStage& s = seed.stages[(size_t)id];
    return offline::load(s.ctl, s.code, s.code_n);
}

// (each kind of stage translated as the back end translates it: vk_shader.cpp translate_variant,
// translate_mesh_variant, translate_task_mesh_variant; vk_compute.cpp translate_variant)
void make_vs(Made& m, int id, bool before_gs = false) {
    m.code = code_of(id);
    offline::Env env(m.code, Shader::Stage::VertexB);
    m.t = offline::translate(env);
    Shader::RuntimeInfo rt{};
    if (!before_gs && !m.t->program.info.stores[Attribute::PointSize]) rt.fixed_state_point_size = 1.0f;
    rt.vertex_formats_pushed = true;   // (the attributes' formats: the draw's push data)
    rt.previous_stage_stores.mask.set();
    rt.convert_depth_mode = !before_gs;   // (in the last stage before the rasterizer)
    Shader::Backend::Bindings b{};
    m.spv = Shader::Backend::SPIRV::EmitSPIRV(offline::profile(), rt, m.t->program, b);
    m.vk = VK_SHADER_STAGE_VERTEX_BIT;
}
void make_mesh(Made& m, int id, int task) {
    m.code = code_of(id);
    offline::Code tc;
    if (task >= 0) tc = code_of(task);
    offline::Env env(m.code, Shader::Stage::Mesh, offline::regs_of(m.code, task >= 0 ? &tc : nullptr));
    m.t = offline::translate(env);
    Shader::RuntimeInfo rt{};
    rt.previous_stage_stores.mask.set();
    rt.convert_depth_mode = true;
    rt.mesh_without_task = true;
    rt.task_buffer = task >= 0;
    Shader::Backend::Bindings b{};
    m.spv = Shader::Backend::SPIRV::EmitSPIRV(offline::profile(), rt, m.t->program, b);
    m.vk = VK_SHADER_STAGE_MESH_BIT_EXT;
}
/// A geometry stage after its program's vertex stage (vs), which it passes on when it is a passthrough stage. Input:
/// triangles for a passthrough stage, else lines (the seeds' other geometry stages read two input vertices; the draw's
/// topology decides in the back end)
void make_gs(Made& m, int id, const Made& vs) {
    m.code = code_of(id);
    offline::Env env(m.code, Shader::Stage::Geometry);
    env.pass_through(vs.t->program.info.stores);
    m.t = offline::translate(env);
    Shader::RuntimeInfo rt{};
    rt.previous_stage_stores = vs.t->program.info.stores;
    m.input = env.SPH().common0.geometry_passthrough ? Shader::InputTopology::Triangles : Shader::InputTopology::Lines;
    rt.input_topology = m.input;
    rt.convert_depth_mode = true;
    Shader::Backend::Bindings b{};
    m.spv = Shader::Backend::SPIRV::EmitSPIRV(offline::profile(), rt, m.t->program, b);
    m.vk = VK_SHADER_STAGE_GEOMETRY_BIT;
}
bool no_ir_guard = false;       // (--no-ir-guard: the translator-fix prototype: GuardFragmentOutputs left out, frag_nan_guard kept)
void make_fs(Made& m, int id, const Made* prev) {
    m.code = code_of(id);
    offline::Env env(m.code, Shader::Stage::Fragment);
    m.t = offline::translate(env);
    Shader::RuntimeInfo rt{};
    const bool after_mesh = prev && prev->vk == VK_SHADER_STAGE_MESH_BIT_EXT;
    rt.fragment_inputs_pushed = !after_mesh;   // (after a vertex or geometry stage: what it writes is the draw's push data)
    if (after_mesh) rt.previous_stage_stores = prev->t->program.info.stores;
    else rt.previous_stage_stores.mask.set();
    if (after_mesh) {
        for (const u32 line : prev->t->program.mesh.primitive_lines)
            if (IsGeneric(static_cast<Attribute>(line))) rt.per_primitive_generics.set(GenericAttributeIndex(static_cast<Attribute>(line)));
        if (prev->t->program.mesh.topology == 0)
            for (size_t i = 0; i < NUM_GENERICS; i++)
                if (m.t->program.info.loads.Generic(i) && !prev->t->program.info.stores.Generic(i)) rt.point_coord_generics.set(i);
    }
    for (int i = 0; i < 8; i++) rt.color_output_types[i] = Shader::AttributeType::Float;   // (every logged pass's targets: float)
    if (!no_ir_guard) Shader::Optimization::GuardFragmentOutputs(m.t->program, 0xff);
    Shader::Backend::Bindings b{};
    m.spv = Shader::Backend::SPIRV::EmitSPIRV(offline::profile(), rt, m.t->program, b);
    m.vk = VK_SHADER_STAGE_FRAGMENT_BIT;
}
void make_task(Made& m, int id, int mesh) {
    m.code = code_of(id);
    const offline::Code mc = mesh >= 0 ? code_of(mesh) : offline::Code{};
    offline::Env env(m.code, Shader::Stage::Task, offline::regs_of(mc, &m.code));
    m.t = offline::translate(env);
    Shader::RuntimeInfo rt{};
    rt.previous_stage_stores.mask.set();
    rt.mesh_fallback = true;
    rt.task_buffer = true;
    Shader::Backend::Bindings b{};
    m.spv = Shader::Backend::SPIRV::EmitSPIRV(offline::profile(), rt, m.t->program, b);
    m.vk = VK_SHADER_STAGE_COMPUTE_BIT;
}
void make_cs(Made& m, int id) {
    m.code = code_of(id);
    offline::Env env(m.code, Shader::Stage::Compute);
    m.t = offline::translate(env);
    Shader::RuntimeInfo rt{};
    Shader::Backend::Bindings b{};
    m.spv = Shader::Backend::SPIRV::EmitSPIRV(offline::profile(), rt, m.t->program, b);
    m.vk = VK_SHADER_STAGE_COMPUTE_BIT;
}

uint32_t late_kinds = 0;   // (--late: VkShaderStageFlagBits the pass is applied to; a task stage is the compute bit with task = true)
bool late_task = false, late_compute = false;
hsd::LateOptions late_options;
bool make_objects = false;   // (--objects)
bool spv_only = false;       // (--spv-only: --spv writes the SPIR-V alone, not the IR's prints)
/// The pass's count of a stage's SPIR-V, and the pass applied when its kind is asked for
std::string late(Made& m, bool task) {
    const bool apply = m.vk == VK_SHADER_STAGE_COMPUTE_BIT ? (task ? late_task : late_compute) : (late_kinds & m.vk) != 0;
    std::vector<uint32_t> copy = m.spv;
    hsd::LateResult r;
    const auto t0 = std::chrono::steady_clock::now();
    hsd::spv_schedule_late(copy, &r, late_options);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (apply) m.spv.swap(copy);
    char b[200];
    std::snprintf(b, sizeof b, ",\"late\":{\"applied\":%d,\"moved\":%u,\"sunk\":%u,\"blocks\":%u,\"changed\":%u,\"before\":%u,\"after\":%u,\"ms\":%.3f}", apply ? 1 : 0,
                  r.moved, r.sunk, r.blocks, r.blocks_changed, r.peak_before, r.peak_after, ms);
    return b;
}
/// The stage as the shader object the game makes of it (vk_shader.cpp finish)
int make_object(const Made& m) {
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sz{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    sz.requiredSubgroupSize = 32;
    VkShaderCreateInfoEXT ci{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    ci.setLayoutCount = 1, ci.pSetLayouts = &gdev.set_layout, ci.pushConstantRangeCount = 1, ci.pPushConstantRanges = &gdev.push;
    if (m.vk == VK_SHADER_STAGE_MESH_BIT_EXT) ci.flags |= VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
    ci.stage = m.vk;
    ci.nextStage = m.vk == VK_SHADER_STAGE_MESH_BIT_EXT || m.vk == VK_SHADER_STAGE_VERTEX_BIT || m.vk == VK_SHADER_STAGE_GEOMETRY_BIT
                       ? VK_SHADER_STAGE_FRAGMENT_BIT : 0;
    ci.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    ci.codeSize = m.spv.size() * 4, ci.pCode = m.spv.data(), ci.pName = "main";
    if (gdev.sg32 & m.vk) ci.pNext = &sz;
    VkShaderEXT o = VK_NULL_HANDLE;
    static const auto create = (PFN_vkCreateShadersEXT)vkGetDeviceProcAddr(gdev.dev, "vkCreateShadersEXT");
    static const auto destroy = (PFN_vkDestroyShaderEXT)vkGetDeviceProcAddr(gdev.dev, "vkDestroyShaderEXT");
    if (!create || !destroy) return -1000;
    const VkResult r = create(gdev.dev, 1, &ci, nullptr, &o);
    if (o) destroy(gdev.dev, o, nullptr);
    return (int)r;
}

const char* repl_spv = nullptr;   // (--repl <spv>: the stage's SPIR-V replaced by this one before the compile, its interface the translator's)

void one(int id, bool do_compile, const char* spv_dir) {
    const SeedStage& s = seed.stages[(size_t)id];
    const Partners& pa = partners[(size_t)id];
    std::fflush(stdout);
    std::fprintf(stderr, "@@stage %d\n", id);
    std::fflush(stderr);
    std::string out = "{\"index\":" + std::to_string(id) + ",\"nvn_stage\":" + std::to_string(s.stage);
    offline::Code c;
    try {
        c = code_of(id);
    } catch (const std::exception& e) {
        std::printf("%s,\"error\":%s}\n", out.c_str(), json_str(std::string("load: ") + e.what()).c_str());
        return;
    }
    char key[20];
    std::snprintf(key, sizeof key, "%016" PRIx64, c.key);
    out += ",\"key\":\"" + std::string(key) + "\",\"native\":" + std::to_string(c.native) + ",\"programs\":" + std::to_string(programs_of[(size_t)id].size());
    {   // (partner keys: the program the stage was translated in)
        auto pk = [&](int pid) {
            if (pid < 0) return std::string("null");
            const SeedStage& q = seed.stages[(size_t)pid];
            char b[24];
            std::snprintf(b, sizeof b, "\"%016" PRIx64 "\"", offline::fnv(q.code, q.code_n, offline::fnv(q.ctl, offline::kControl)));
            return std::string(b);
        };
        out += ",\"vs\":" + pk(pa.vs) + ",\"fs\":" + pk(pa.fs) + ",\"task\":" + pk(pa.task) + ",\"mesh\":" + pk(pa.mesh);
    }
    if (c.header_only) {
        std::printf("%s,\"header_only\":true}\n", out.c_str());
        return;
    }
    try {
        Made m, partner, vertex;
        bool has_partner = false;
        std::string extra;
        // (--front new: a partner the new front does not translate yet is left out, the stage translated alone)
        const auto partnered = [&](auto&& make) {
            if (!offline::new_front) return make(), true;
            try {
                make();
                return true;
            } catch (const std::exception& e) {
                partner = Made{};
                extra += ",\"partner_error\":" + json_str(e.what());
                return false;
            }
        };
        if (s.stage == 0) {
            make_vs(m, id);
            if (pa.fs >= 0) has_partner = partnered([&] { make_fs(partner, pa.fs, &m); });
        } else if (s.stage == 1) {
            if (pa.vs >= 0) has_partner = partnered([&] { make_vs(partner, pa.vs); });
            else if (pa.mesh >= 0) has_partner = partnered([&] { make_mesh(partner, pa.mesh, pa.task); });
            make_fs(m, id, has_partner ? &partner : nullptr);
        } else if (s.stage == 7) {
            make_mesh(m, id, pa.task);
            if (pa.fs >= 0) has_partner = partnered([&] { make_fs(partner, pa.fs, &m); });
            const auto& mp = m.t->program.mesh;
            extra += ",\"max_vertices\":" + std::to_string(mp.max_vertices) + ",\"max_primitives\":" + std::to_string(mp.max_primitives) +
                     ",\"payload_words\":" + std::to_string(mp.payload_words) + ",\"vertex_lines\":" + std::to_string(mp.vertex_lines.size()) +
                     ",\"primitive_lines\":" + std::to_string(mp.primitive_lines.size()) + ",\"topology\":" + std::to_string((int)mp.topology);
        } else if (s.stage == 2) {
            if (pa.vs < 0) throw Shader::RuntimeError("geometry stage without a vertex stage");
            make_vs(vertex, pa.vs, true);
            make_gs(m, id, vertex);
            if (pa.fs >= 0) has_partner = partnered([&] { make_fs(partner, pa.fs, &m); });
            extra += std::string(",\"passthrough\":") + (m.t->program.is_geometry_passthrough ? "true" : "false") +
                     ",\"output_vertices\":" + std::to_string(m.t->program.output_vertices) + ",\"output_topology\":" +
                     std::to_string((int)m.t->program.output_topology);
        } else if (s.stage == 6) {
            make_task(m, id, pa.mesh);
            extra += ",\"payload_words\":" + std::to_string(m.t->program.mesh.payload_words);
        } else if (s.stage == 5) {
            make_cs(m, id);
        } else {
            std::printf("%s,\"error\":\"stage kind not handled\"}\n", out.c_str());
            return;
        }
        if (repl_spv) {
            std::vector<uint32_t> r;
            if (FILE* f = std::fopen(repl_spv, "rb")) {
                std::fseek(f, 0, SEEK_END);
                r.resize((size_t)std::ftell(f) / 4);
                std::fseek(f, 0, SEEK_SET);
                if (std::fread(r.data(), 4, r.size(), f) != r.size()) r.clear();
                std::fclose(f);
            }
            if (r.empty()) throw Shader::RuntimeError("cannot read {}", repl_spv);
            m.spv = std::move(r);
            extra += ",\"replaced\":true";
        }
        extra += late(m, s.stage == 6);
        if (has_partner) late(partner, false);
        const std::vector<uint32_t>& spv = m.spv;
        Program& p = m.t->program;
        const Shader::Info& in = p.info;
        // (inputs and outputs)
        int in_lines = 0, in_comp = 0, out_lines = 0, out_comp = 0;
        for (size_t g = 0; g < NUM_GENERICS; g++) {
            if (in.loads.Generic(g)) in_lines++;
            if (in.stores.Generic(g)) out_lines++;
            for (int k = 0; k < 4; k++) in_comp += in.loads.Generic(g, k), out_comp += in.stores.Generic(g, k);
        }
        int rts = 0, rt_comp = 0;
        for (int i = 0; i < 8; i++) rts += in.stores_frag_color[i], rt_comp += in.stores_frag_color[i] ? in.frag_color_components[i] : 0;
        const Profile r = profile_of(p);
        out += ",\"workgroup\":[" + std::to_string(p.workgroup_size[0]) + "," + std::to_string(p.workgroup_size[1]) + "," +
               std::to_string(p.workgroup_size[2]) + "],\"shared\":" + std::to_string(p.shared_memory_size) + ",\"local_mem\":" +
               std::to_string(p.local_memory_size);
        out += ",\"in_lines\":" + std::to_string(in_lines) + ",\"in_comp\":" + std::to_string(in_comp) + ",\"out_lines\":" +
               std::to_string(out_lines) + ",\"out_comp\":" + std::to_string(out_comp) + ",\"rts\":" + std::to_string(rts) + ",\"rt_comp\":" +
               std::to_string(rt_comp) + ",\"frag_depth\":" + std::to_string((int)in.stores_frag_depth) + ",\"sample_mask\":" +
               std::to_string((int)in.stores_sample_mask);
        out += ",\"textures\":" + std::to_string(in.texture_descriptors.size()) + ",\"images\":" + std::to_string(in.image_descriptors.size()) +
               ",\"tex_used\":" + std::to_string(r.tex_desc_used.size()) + ",\"ubos\":" + std::to_string(in.constant_buffer_descriptors.size()) +
               ",\"ssbos\":" + std::to_string(in.storage_buffers_descriptors.size());
        out += ",\"ir\":" + std::to_string(r.insts) + ",\"ir_weighted\":" + std::to_string(r.insts_weighted) + ",\"spirv\":" +
               std::to_string(spirv_instructions(spv)) + ",\"spirv_words\":" + std::to_string(spv.size());
        out += ",\"samples\":" + std::to_string(r.samples) + ",\"samples_branch\":" + std::to_string(r.samples_in_branch) + ",\"samples_loop\":" +
               std::to_string(r.samples_in_loop) + ",\"samples_unknown_loop\":" + std::to_string(r.samples_in_unknown_loop) +
               ",\"samples_weighted\":" + std::to_string(r.samples_weighted) + ",\"tex_kind\":" + json_map(r.tex_kind) + ",\"tex_dim\":" +
               json_map(r.tex_dim);
        out += ",\"cbuf\":" + std::to_string(r.cbuf_loads) + ",\"cbuf_c1\":" + std::to_string(r.cbuf_loads_c1) + ",\"cbuf_driver\":" +
               std::to_string(r.cbuf_loads_driver) + ",\"cbuf_game\":" + std::to_string(r.cbuf_loads_game) + ",\"cbuf_indexed\":" +
               std::to_string(r.cbuf_loads_indexed) + ",\"gload\":" + std::to_string(r.global_loads) + ",\"gstore\":" +
               std::to_string(r.global_stores) + ",\"atomics\":" + std::to_string(r.atomics) + ",\"sload\":" + std::to_string(r.shared_loads) +
               ",\"sstore\":" + std::to_string(r.shared_stores) + ",\"local_ops\":" + std::to_string(r.local_ops) + ",\"barriers\":" +
               std::to_string(r.barriers);
        out += ",\"branches\":" + std::to_string(r.branches) + ",\"branch_cls\":[" + std::to_string(r.branch_cls[0]) + "," +
               std::to_string(r.branch_cls[1]) + "," + std::to_string(r.branch_cls[2]) + "," + std::to_string(r.branch_cls[3]) +
               "],\"discards\":" + std::to_string(r.discards) + ",\"returns\":" + std::to_string(r.returns) + ",\"loops\":[";
        for (size_t k = 0; k < r.loops.size(); k++)
            out += std::string(k ? "," : "") + "{\"cls\":\"" + cls_name(r.loops[k].cls) + "\",\"trips\":" + std::to_string(r.loops[k].trips) +
                   ",\"depth\":" + std::to_string(r.loops[k].depth) + "}";
        out += "],\"subgroup_ops\":" + std::to_string(r.subgroup_ops) + ",\"cc_ops\":" + std::to_string(r.cc_ops) + ",\"select_u1\":" +
               std::to_string(r.select_u1) + ",\"isbe_ops\":" + std::to_string(r.isbe_ops) + ",\"fp_ops\":" + std::to_string(r.fp_ops) +
               ",\"int_ops\":" + std::to_string(r.int_ops) + ",\"transcendental\":" + std::to_string(r.transcendental) + ",\"conversions\":" +
               std::to_string(r.conversions) + ",\"ops\":" + json_map(r.ops) + extra;
        if (spv_dir) {   // (the stage's SPIR-V and its IR printed: structured and flat, for reading it)
            const std::string path = std::string(spv_dir) + "/" + key;
            if (FILE* f = std::fopen((path + ".spv").c_str(), "wb")) std::fwrite(spv.data(), 4, spv.size(), f), std::fclose(f);
            if (!spv_only)
                if (FILE* f = std::fopen((path + ".ir.c").c_str(), "w")) std::fputs(pretty::print(p).c_str(), f), std::fclose(f);
            if (!spv_only)
                if (FILE* f = std::fopen((path + ".flat.c").c_str(), "w")) std::fputs(pretty::print(p, true).c_str(), f), std::fclose(f);
        }
        if (do_compile) {
            int res = 0;
            const std::string st = s.stage == 2 ? compile(vertex, &m, res, has_partner ? &partner : nullptr)
                                                : compile(m, has_partner ? &partner : nullptr, res);
            out += ",\"compiled\":" + std::to_string(res) + ",\"radv\":" + st;
        }
        if (make_objects) {
            const char* kind = s.stage == 0 ? "vertex" : s.stage == 1 ? "fragment" : s.stage == 2 ? "geometry" : s.stage == 7 ? "mesh" : s.stage == 6 ? "task" : "compute";
            std::fflush(stdout);
            std::fprintf(stderr, "@@object %d %s %s\n", id, key, kind);
            std::fflush(stderr);
            const int res = make_object(m);
            std::fprintf(stderr, "@@end\n");
            std::fflush(stderr);
            out += ",\"object\":" + std::to_string(res);
        }
        std::printf("%s}\n", out.c_str());
        std::fflush(stdout);
    } catch (const std::exception& e) {
        std::printf("%s,\"error\":%s}\n", out.c_str(), json_str(e.what()).c_str());
        std::fflush(stdout);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) return std::fprintf(stderr, "censustool <seed> <first> <count> [--compile] [--spv <dir>] [--only <index>...]\n"), 2;
    if (!seed_read(argv[1])) return std::fprintf(stderr, "cannot read the seed %s\n", argv[1]), 1;
    partners_make();
    int first = std::atoi(argv[2]), count = std::atoi(argv[3]);
    bool do_compile = false;
    const char* spv_dir = nullptr;
    std::vector<int> only;
    for (int i = 4; i < argc; i++) {
        if (!std::strcmp(argv[i], "--compile")) do_compile = true;
        else if (!std::strcmp(argv[i], "--spv") && i + 1 < argc) spv_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--front") && i + 1 < argc) offline::new_front = !std::strcmp(argv[++i], "new");
        else if (!std::strcmp(argv[i], "--repl") && i + 1 < argc) repl_spv = argv[++i];
        else if (!std::strcmp(argv[i], "--no-ir-guard")) no_ir_guard = true;
        else if (!std::strcmp(argv[i], "--mma-software")) Shader::Backend::SPIRV::mma_software = true;
        else if (!std::strcmp(argv[i], "--explicit-shared")) offline::explicit_shared = true;
        else if (!std::strcmp(argv[i], "--objects")) make_objects = true;
        else if (!std::strcmp(argv[i], "--spv-only")) spv_only = true;
        else if (!std::strcmp(argv[i], "--budget") && i + 1 < argc) late_options.budget = (uint32_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--sink-peak") && i + 1 < argc) late_options.sink_peak = (uint32_t)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--list")) {
            for (size_t k = 0; k < seed.stages.size(); k++) std::printf("%zu %u\n", k, seed.stages[k].stage);
            return 0;
        } else if (!std::strcmp(argv[i], "--late") && i + 1 < argc) {
            const std::string kinds = std::string(",") + argv[++i] + ",";
            const bool all = kinds.find(",all,") != std::string::npos;
            if (all || kinds.find(",vertex,") != std::string::npos) late_kinds |= VK_SHADER_STAGE_VERTEX_BIT;
            if (all || kinds.find(",fragment,") != std::string::npos) late_kinds |= VK_SHADER_STAGE_FRAGMENT_BIT;
            if (all || kinds.find(",mesh,") != std::string::npos) late_kinds |= VK_SHADER_STAGE_MESH_BIT_EXT;
            late_task = all || kinds.find(",task,") != std::string::npos;
            late_compute = all || kinds.find(",compute,") != std::string::npos;
        }
        else if (!std::strcmp(argv[i], "--only")) {
            while (i + 1 < argc && argv[i + 1][0] != '-') only.push_back(std::atoi(argv[++i]));
        }
    }
    if (first < 0 && count < 0) {   // (the seed's shape)
        std::printf("{\"stages\":%zu,\"programs\":%zu}\n", seed.stages.size(), seed.programs.size());
        return 0;
    }
    if ((do_compile || make_objects) && !dev_init()) return std::fprintf(stderr, "no device\n"), 1;
    if (only.empty())
        for (int i = first; i < first + count && i < (int)seed.stages.size(); i++) only.push_back(i);
    for (int id : only) one(id, do_compile, spv_dir);
    return 0;
}
