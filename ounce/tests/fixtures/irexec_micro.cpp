// Small IR programs, built with IR::IREmitter and the translator's passes as a front would leave them, through irexec
// (tools/shaders/irexec): each case's results checked against values worked out by hand or by a plain loop here. Prints
// one line a case ("ok <case>" or "FAIL <case>: ..."); the exit code is the number of failures.
#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/post_order.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"

#include "../../../tools/shaders/irexec/irexec.h"

using namespace Shader;
using Node = IR::AbstractSyntaxNode;

namespace {

int failures = 0;
void check(const std::string& name, bool ok, const std::string& why = "") {
    std::printf("%s %s%s%s\n", ok ? "ok" : "FAIL", name.c_str(), ok ? "" : ": ", ok ? "" : why.c_str());
    failures += !ok;
}

/// A trace state entry ("addr=value ...") as words by address
std::map<uint64_t, uint32_t> words(const irexec::Trace& t, const std::string& key) {
    std::map<uint64_t, uint32_t> out;
    const auto it = t.state.find(key);
    if (it == t.state.end()) return out;
    std::istringstream in(it->second);
    std::string item;
    while (in >> item) {
        const size_t eq = item.find('=');
        const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        const size_t colon = k.find(':');   // (frag: "lane:slot")
        const uint64_t key_v = colon == std::string::npos ? std::stoull(k, nullptr, 16) : std::stoull(k.substr(0, colon)) * 64 + std::stoull(k.substr(colon + 1));
        out[key_v] = (uint32_t)std::stoul(v, nullptr, 16);
    }
    return out;
}

struct Prog {
    ObjectPool<IR::Inst> insts;
    ObjectPool<IR::Block> blocks;
    IR::Program program;
    IR::Block* block() { return blocks.Create(insts); }
    /// The passes a front's program gets before the back end, on the syntax list given
    void finish(Stage stage, IR::AbstractSyntaxList list) {
        program.syntax_list = std::move(list);
        for (const Node& n : program.syntax_list)
            if (n.type == Node::Type::Block) program.blocks.push_back(n.data.block);
        program.post_order_blocks = IR::PostOrder(program.syntax_list.front());
        program.stage = stage;
        program.workgroup_size = {32, 1, 1};
        program.shared_memory_size = 4096;
        Optimization::SsaRewritePass(program);
        Optimization::IdentityRemovalPass(program);
        Optimization::DeadCodeEliminationPass(program);
    }
};
Node block_node(IR::Block* b) { return {.data{.block = b}, .type = Node::Type::Block}; }
Node return_node() { return {.type = Node::Type::Return}; }

irexec::Trace run(Prog& p, uint64_t max_steps = 1u << 16, const irexec::Options& options = {}) {
    const auto c = irexec::compile(p.program, options);
    irexec::Inputs in;
    in.seed = 1;
    in.max_steps = max_steps;
    return irexec::run(*c, in);
}

/// Straight-line ops, each lane's result of case i in shared word lane * N + i
void ops() {
    using Op = IR::IREmitter::SubgroupReduceOp;
    struct Case {
        const char* name;
        std::function<IR::U32(IR::IREmitter&, IR::U32)> make;
        std::function<uint32_t(uint32_t)> want;
    };
    const auto f = [](float x) { return std::bit_cast<uint32_t>(x); };
    const auto nan = [](IR::IREmitter& ir) { return ir.BitCast<IR::F32>(ir.Imm32(0x7fc00000u)); };
    const std::vector<Case> cases{
        {"ReduceIAdd", [](IR::IREmitter& ir, IR::U32 l) { return ir.SubgroupReduce(Op::IAdd, l); }, [](uint32_t) { return 496u; }},
        {"ReduceUMax", [](IR::IREmitter& ir, IR::U32 l) { return ir.SubgroupReduce(Op::UMax, ir.IMul(l, ir.Imm32(3u))); }, [](uint32_t) { return 93u; }},
        {"ReduceSMin", [](IR::IREmitter& ir, IR::U32 l) { return ir.SubgroupReduce(Op::SMin, ir.ISub(l, ir.Imm32(5u))); }, [](uint32_t) { return (uint32_t)-5; }},
        {"ReduceXor", [](IR::IREmitter& ir, IR::U32 l) { return ir.SubgroupReduce(Op::Xor, l); }, [](uint32_t) { return 0u; }},
        {"ReduceOr", [](IR::IREmitter& ir, IR::U32 l) { return ir.SubgroupReduce(Op::Or, ir.ShiftLeftLogical(ir.Imm32(1u), ir.BitwiseAnd(l, ir.Imm32(7u)))); }, [](uint32_t) { return 0xffu; }},
        {"ReadFirstLane", [](IR::IREmitter& ir, IR::U32 l) { return ir.SubgroupReadFirstLane(ir.IAdd(l, ir.Imm32(3u))); }, [](uint32_t) { return 3u; }},
        {"Ballot", [](IR::IREmitter& ir, IR::U32 l) { return ir.SubgroupBallot(ir.INotEqual(ir.BitwiseAnd(l, ir.Imm32(1u)), ir.Imm32(0u))); }, [](uint32_t) { return 0xaaaaaaaau; }},
        {"ShuffleIndex", [](IR::IREmitter& ir, IR::U32 l) { return ir.ShuffleIndex(ir.IMul(l, ir.Imm32(10u)), ir.BitwiseXor(l, ir.Imm32(1u)), ir.Imm32(31u), ir.Imm32(0u)); },
         [](uint32_t l) { return (l ^ 1) * 10; }},
        {"ShuffleButterfly", [](IR::IREmitter& ir, IR::U32 l) { return ir.ShuffleButterfly(l, ir.Imm32(16u), ir.Imm32(31u), ir.Imm32(0u)); }, [](uint32_t l) { return l ^ 16; }},
        {"ShuffleUp", [](IR::IREmitter& ir, IR::U32 l) { return ir.ShuffleUp(l, ir.Imm32(1u), ir.Imm32(0u), ir.Imm32(0u)); }, [](uint32_t l) { return l == 0 ? 0 : l - 1; }},
        {"ShuffleDownSegment", [](IR::IREmitter& ir, IR::U32 l) { return ir.ShuffleDown(l, ir.Imm32(1u), ir.Imm32(7u), ir.Imm32(0x18u)); },
         [](uint32_t l) { return (l & 7) == 7 ? l : l + 1; }},
        {"FPMaxNaN", [&](IR::IREmitter& ir, IR::U32) { return ir.BitCast<IR::U32>(IR::F32{ir.FPMax(nan(ir), ir.Imm32(1.0f))}); }, [&](uint32_t) { return f(1.0f); }},
        {"FPMinZeros", [](IR::IREmitter& ir, IR::U32) { return ir.BitCast<IR::U32>(IR::F32{ir.FPMin(ir.Imm32(0.0f), ir.Imm32(-0.0f))}); }, [](uint32_t) { return 0x80000000u; }},
        {"FPSaturateNaN", [&](IR::IREmitter& ir, IR::U32) { return ir.BitCast<IR::U32>(IR::F32{ir.FPSaturate(nan(ir))}); }, [](uint32_t) { return 0u; }},
        {"FPAddDenormFlushed", [](IR::IREmitter& ir, IR::U32) { return ir.BitCast<IR::U32>(IR::F32{ir.FPAdd(ir.BitCast<IR::F32>(ir.Imm32(1u)), ir.Imm32(0.0f))}); },
         [](uint32_t) { return 0u; }},
        {"FPMulDenormResult", [](IR::IREmitter& ir, IR::U32) { return ir.BitCast<IR::U32>(IR::F32{ir.FPMul(ir.Imm32(1e-30f), ir.Imm32(1e-10f))}); }, [](uint32_t) { return 0u; }},
        {"FPAddNaN", [&](IR::IREmitter& ir, IR::U32) { return ir.BitCast<IR::U32>(IR::F32{ir.FPAdd(ir.BitCast<IR::F32>(ir.Imm32(0xffc00123u)), ir.Imm32(1.0f))}); },
         [](uint32_t) { return 0x7fc00000u; }},
        {"F2INaN", [&](IR::IREmitter& ir, IR::U32) { return IR::U32{ir.ConvertFToI(32, true, nan(ir))}; }, [](uint32_t) { return 0u; }},
        {"F2IHigh", [](IR::IREmitter& ir, IR::U32) { return IR::U32{ir.ConvertFToI(32, true, ir.Imm32(3e9f))}; }, [](uint32_t) { return 0x7fffffffu; }},
        {"F2UNegative", [](IR::IREmitter& ir, IR::U32) { return IR::U32{ir.ConvertFToU(32, ir.Imm32(-1.0f))}; }, [](uint32_t) { return 0u; }},
        {"F32F16F32", [](IR::IREmitter& ir, IR::U32) { return ir.BitCast<IR::U32>(IR::F32{ir.FPConvert(32, ir.FPConvert(16, ir.Imm32(1.0f / 3.0f)))}); },
         [](uint32_t) { return 0x3eaaa000u; }},
        {"F16NaNIsZero", [&](IR::IREmitter& ir, IR::U32) { return ir.BitCast<IR::U32>(IR::F32{ir.FPConvert(32, ir.FPConvert(16, nan(ir)))}); }, [](uint32_t) { return 0u; }},
        {"BitFieldUExtract", [](IR::IREmitter& ir, IR::U32) { return ir.BitFieldExtract(ir.Imm32(0xabcdu), ir.Imm32(4u), ir.Imm32(8u), false); }, [](uint32_t) { return 0xbcu; }},
        {"BitFieldUExtract32", [](IR::IREmitter& ir, IR::U32) { return ir.BitFieldExtract(ir.Imm32(0x12345678u), ir.Imm32(4u), ir.Imm32(32u), false); },
         [](uint32_t) { return 0x12345678u; }},
        {"BitFieldSExtract", [](IR::IREmitter& ir, IR::U32) { return ir.BitFieldExtract(ir.Imm32(0xf0u), ir.Imm32(4u), ir.Imm32(4u), true); }, [](uint32_t) { return ~0u; }},
        {"FindUMsbZero", [](IR::IREmitter& ir, IR::U32) { return ir.FindUMsb(ir.Imm32(0u)); }, [](uint32_t) { return ~0u; }},
        {"FindUMsbLane", [](IR::IREmitter& ir, IR::U32 l) { return ir.FindUMsb(l); }, [](uint32_t l) { return l ? 31 - (uint32_t)std::countl_zero(l) : ~0u; }},
        {"ShiftMasked", [](IR::IREmitter& ir, IR::U32) { return IR::U32{ir.ShiftLeftLogical(ir.Imm32(1u), ir.Imm32(33u))}; }, [](uint32_t) { return 2u; }},
        {"IAddCarry", [](IR::IREmitter& ir, IR::U32 l) {
             const IR::U32 sum{ir.IAdd(ir.Imm32(0xffffffffu), l)};
             return IR::U32{ir.Select(ir.GetCarryFromOp(sum), ir.Imm32(100u), ir.Imm32(7u))};
         },
         [](uint32_t l) { return l ? 100u : 7u; }},
        {"LaneMasks", [](IR::IREmitter& ir, IR::U32) { return ir.BitwiseXor(ir.SubgroupLtMask(), ir.SubgroupGeMask()); }, [](uint32_t) { return ~0u; }},
    };
    Prog p;
    IR::Block* const b = p.block();
    IR::IREmitter ir{*b};
    const IR::U32 lane = ir.LaneId();
    const uint32_t n = (uint32_t)cases.size();
    for (uint32_t i = 0; i < n; i++)
        ir.WriteShared(32, ir.IMul(ir.IAdd(ir.IMul(lane, ir.Imm32(n)), ir.Imm32(i)), ir.Imm32(4u)), cases[i].make(ir, lane));
    p.finish(Stage::Compute, {block_node(b), return_node()});
    const auto t = run(p);
    const auto w = words(t, "mem.shared");
    for (uint32_t i = 0; i < n; i++) {
        std::string why;
        for (uint32_t l = 0; l < 32 && why.empty(); l++) {
            const auto it = w.find(l * n + i);
            const uint32_t want = cases[i].want(l);
            if (it == w.end()) why = "lane " + std::to_string(l) + " wrote nothing";
            else if (it->second != want) why = fmt::format("lane {}: {:08x}, want {:08x}", l, it->second, want);
        }
        check(cases[i].name, why.empty(), why);
    }
}

/// A divergent if: the first active lane inside it, and the lanes outside it write nothing
void divergent_if() {
    Prog p;
    IR::Block *const entry = p.block(), *const body = p.block(), *const merge = p.block();
    IR::IREmitter e{*entry};
    const IR::U1 cond = e.ConditionRef(e.IGreaterThanEqual(e.LaneId(), e.Imm32(5u), false));   // (as fronts keep a node's condition)
    entry->AddBranch(body), entry->AddBranch(merge), body->AddBranch(merge);
    IR::IREmitter bi{*body};
    bi.WriteShared(32, bi.IMul(bi.LaneId(), bi.Imm32(4u)), bi.SubgroupReadFirstLane(bi.IAdd(bi.LaneId(), bi.Imm32(3u))));
    IR::IREmitter mi{*merge};
    mi.WriteShared(32, mi.Imm32(1024u), mi.SubgroupReduce(IR::IREmitter::SubgroupReduceOp::IAdd, mi.Imm32(1u)));
    Node if_node{.type = Node::Type::If};
    if_node.data.if_node = {cond, body, merge};
    Node end_if{.type = Node::Type::EndIf};
    end_if.data.end_if.merge = merge;
    p.finish(Stage::Compute, {block_node(entry), if_node, block_node(body), end_if, block_node(merge), return_node()});
    const auto w = words(run(p), "mem.shared");
    bool ok = w.size() == 28 && w.at(256) == 32;
    for (uint32_t l = 0; l < 32 && ok; l++) ok = l < 5 ? !w.count(l) : w.at(l) == 8;
    check("DivergentIf", ok, "the lanes in the if read lane 5's value, all 32 meet at the merge");
}

/// A do-while loop of max(lane, 1) trips through registers (phis after the SSA rewrite), each trip adding the number of
/// lanes still looping; the lanes reconverge after it
void loop() {
    Prog p;
    IR::Block *const entry = p.block(), *const header = p.block(), *const merge = p.block();
    IR::IREmitter e{*entry};
    e.SetReg(IR::Reg::R0, e.Imm32(0u)), e.SetReg(IR::Reg::R1, e.Imm32(0u)), e.SetReg(IR::Reg::R2, e.Imm32(0u));
    entry->AddBranch(header), header->AddBranch(header), header->AddBranch(merge);
    IR::IREmitter h{*header};
    const IR::U32 i = h.GetReg(IR::Reg::R1);
    h.SetReg(IR::Reg::R0, h.IAdd(h.GetReg(IR::Reg::R0), i));
    h.SetReg(IR::Reg::R2, h.IAdd(h.GetReg(IR::Reg::R2), h.BitCount(h.SubgroupBallot(h.Imm1(true)))));
    const IR::U32 next = h.IAdd(i, h.Imm32(1u));
    h.SetReg(IR::Reg::R1, next);
    const IR::U1 again = h.ConditionRef(h.ILessThan(next, h.LaneId(), false));
    IR::IREmitter m{*merge};
    const IR::U32 lane = m.LaneId();
    m.WriteShared(32, m.IMul(lane, m.Imm32(8u)), m.GetReg(IR::Reg::R0));
    m.WriteShared(32, m.IAdd(m.IMul(lane, m.Imm32(8u)), m.Imm32(4u)), m.GetReg(IR::Reg::R2));
    m.WriteShared(32, m.Imm32(1024u), m.SubgroupReduce(IR::IREmitter::SubgroupReduceOp::IAdd, m.GetReg(IR::Reg::R0)));
    Node loop{.type = Node::Type::Loop};
    loop.data.loop = {header, header, merge};
    Node repeat{.type = Node::Type::Repeat};
    repeat.data.repeat = {again, header, merge};
    p.finish(Stage::Compute, {block_node(entry), loop, block_node(header), repeat, block_node(merge), return_node()});
    const auto t = run(p);
    const auto w = words(t, "mem.shared");
    // (by hand: lane l takes n(l) = max(l, 1) trips; at trip k the lanes still looping are those with n > k)
    const auto trips = [](uint32_t l) { return std::max(l, 1u); };
    std::string why;
    uint32_t total = 0;
    for (uint32_t l = 0; l < 32; l++) {
        uint32_t sum = 0, active = 0;
        for (uint32_t k = 0; k < trips(l); k++) {
            sum += k;
            for (uint32_t o = 0; o < 32; o++) active += trips(o) > k;
        }
        total += sum;
        if (!w.count(2 * l) || w.at(2 * l) != sum || w.at(2 * l + 1) != active)
            why = fmt::format("lane {}: sum {} active {}, want {} {}", l, w.count(2 * l) ? w.at(2 * l) : 0, w.count(2 * l + 1) ? w.at(2 * l + 1) : 0, sum, active);
    }
    if (why.empty() && w.at(256) != total) why = fmt::format("reduce after the loop {}, want {}", w.at(256), total);
    if (why.empty() && t.steps != 1 + 31 + 1) why = fmt::format("{} block entries, want 33", t.steps);
    check("LoopPhisReconverge", why.empty(), why);
}

/// A loop that never ends: the back end's safety counter ends it after shader_loop_safety_count trips; max_steps caps it
void loop_cap() {
    const auto make = [](Prog& p) {
        IR::Block *const entry = p.block(), *const header = p.block(), *const merge = p.block();
        entry->AddBranch(header), header->AddBranch(header), header->AddBranch(merge);
        IR::IREmitter e{*entry};
        e.SetReg(IR::Reg::R0, e.Imm32(0u));
        IR::IREmitter h{*header};
        h.SetReg(IR::Reg::R0, h.IAdd(h.GetReg(IR::Reg::R0), h.Imm32(1u)));
        IR::IREmitter m{*merge};
        m.WriteShared(32, m.IMul(m.LaneId(), m.Imm32(4u)), m.GetReg(IR::Reg::R0));
        Node loop{.type = Node::Type::Loop};
        loop.data.loop = {header, header, merge};
        Node repeat{.type = Node::Type::Repeat};
        repeat.data.repeat = {h.Imm1(true), header, merge};
        p.finish(Stage::Compute, {block_node(entry), loop, block_node(header), repeat, block_node(merge), return_node()});
    };
    Prog p;
    make(p);
    const auto w = words(run(p), "mem.shared");
    check("LoopSafetyCounter", w.size() == 32 && w.at(0) == 0x2001 && w.at(31) == 0x2001, w.empty() ? "no writes" : fmt::format("{:x} trips", w.begin()->second));
    Prog q;
    make(q);
    const auto t = run(q, 100);
    check("MaxStepsCap", t.capped && t.state.count("capped") && !t.state.count("mem.shared"), "a capped run stops before the merge");
}

/// Fragment: a derivative of the pixel position across the quad, and odd lanes demoted (their outputs dropped)
void fragment() {
    Prog p;
    IR::Block* const b = p.block();
    IR::IREmitter ir{*b};
    ir.SetFragColor(0, 0, ir.DPdxFine(ir.GetAttribute(IR::Attribute::PositionX)));
    ir.SetFragColor(0, 1, ir.DPdyCoarse(ir.GetAttribute(IR::Attribute::PositionY)));
    Node if_node{.type = Node::Type::If};
    IR::Block* const demote = p.block();
    IR::Block* const merge = p.block();
    b->AddBranch(demote), b->AddBranch(merge), demote->AddBranch(merge);
    const IR::U1 odd = ir.ConditionRef(ir.INotEqual(ir.BitwiseAnd(ir.LaneId(), ir.Imm32(1u)), ir.Imm32(0u)));
    IR::IREmitter d{*demote};
    d.DemoteToHelperInvocation();
    if_node.data.if_node = {odd, demote, merge};
    Node end_if{.type = Node::Type::EndIf};
    end_if.data.end_if.merge = merge;
    p.finish(Stage::Fragment, {block_node(b), if_node, block_node(demote), end_if, block_node(merge), return_node()});
    const auto t = run(p);
    const auto w = words(t, "frag");
    bool ok = t.state.count("discard") && t.state.at("discard") == "aaaaaaaa" && w.size() == 32;
    for (uint32_t l = 0; l < 32 && ok; l += 2) ok = w.at(l * 64) == 0x3f800000u && w.at(l * 64 + 1) == 0x3f800000u;
    check("FragmentDerivativesDemote", ok, "derivatives 1.0 in the even lanes, the odd ones discarded");
}

/// Shared atomics apply in lane order and return the old value; every one is in the trace
void atomics() {
    Prog p;
    IR::Block* const b = p.block();
    IR::IREmitter ir{*b};
    const IR::U32 old = ir.SharedAtomicIAdd(ir.Imm32(64u), ir.Imm32(1u));
    ir.WriteShared(32, ir.IMul(ir.LaneId(), ir.Imm32(4u)), ir.ISub(old, ir.SubgroupReadFirstLane(old)));
    p.finish(Stage::Compute, {block_node(b), return_node()});
    const auto t = run(p);
    const auto w = words(t, "mem.shared");
    bool ok = w.size() == 32;   // (word 16, the counter, is lane 16's own word: last written by its store)
    for (uint32_t l = 0; l < 32 && ok; l++) ok = w.at(l) == l;
    const auto a = t.state.find("atomic");
    ok = ok && a != t.state.end() && std::count(a->second.begin(), a->second.end(), ',') == 32;
    check("SharedAtomicOrder", ok, "each lane's old value is the first lane's plus its lane");
}

/// A bank alias reads one constant bank as another (the old front's root table at 17, the new one's at 24)
void bank_alias() {
    Prog p;
    IR::Block* const b = p.block();
    IR::IREmitter ir{*b};
    ir.WriteShared(32, ir.Imm32(0u), ir.GetCbuf(ir.Imm32(24u), ir.Imm32(0x40u)));
    ir.WriteShared(32, ir.Imm32(4u), ir.GetCbuf(ir.Imm32(17u), ir.Imm32(0x40u)));
    p.finish(Stage::Compute, {block_node(b), return_node()});
    const auto plain = words(run(p), "mem.shared");
    const auto aliased = words(run(p, 1u << 16, {.bank_alias = {{24, 17}}}), "mem.shared");
    check("BankAlias", plain.at(0) != plain.at(1) && aliased.at(0) == aliased.at(1) && aliased.at(1) == plain.at(1),
          "bank 24 read as 17 only with the alias");
}

}  // namespace

int main() {
    try {
        ops();
        divergent_if();
        loop();
        loop_cap();
        fragment();
        atomics();
        bank_alias();
    } catch (const std::exception& e) {
        std::printf("FAIL exception: %s\n", e.what());
        return 100;
    }
    return failures;
}
