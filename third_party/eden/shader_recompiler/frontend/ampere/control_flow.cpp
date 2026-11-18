// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.
// The builder's walk follows Eden's Maxwell front (frontend/maxwell/control_flow.cpp, yuzu Emulator Project, GPL-2.0+).

#include <algorithm>
#include <optional>
#include <ranges>

#include <fmt/format.h>

#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/control_flow.h"
#include "shader_recompiler/frontend/ampere/geometry.h"

namespace Shader::Ampere::Flow {
namespace {
struct Compare {
    bool operator()(const Block& lhs, Location rhs) const noexcept {
        return lhs.begin < rhs;
    }
    bool operator()(Location lhs, const Block& rhs) const noexcept {
        return lhs < rhs.begin;
    }
    bool operator()(const Block& lhs, const Block& rhs) const noexcept {
        return lhs.begin < rhs.begin;
    }
};

constexpr u32 OP_BRA = 0x947, OP_BRX = 0x949, OP_BRXU = 0x958, OP_EXIT = 0x94d, OP_KILL = 0x95b, OP_CALL = 0x944,
              OP_RET = 0x950, OP_BSSY = 0x945, OP_BSYNC = 0x941, OP_BREAK = 0x942, OP_WARPSYNC = 0x948;
/// The longest program (program_capture.h's MaximumInstructions)
constexpr s64 MAX_PROGRAM_BYTES{s64{16} << 20};

[[noreturn]] void Fail(Location pc, const Insn& insn, std::string_view reason) {
    throw NotImplementedException("Ampere {:03x} {} at {:#x}: {}", insn.Opcode(), OpcodeName(insn.Opcode()),
                                  ByteOffsetOf(pc), reason);
}

void Split(Block* old_block, Block* new_block, Location pc) {
    if (pc <= old_block->begin || pc >= old_block->end) {
        throw InvalidArgument("Invalid address to split={}", pc);
    }
    *new_block = Block{};
    new_block->begin = pc;
    new_block->end = old_block->end;
    new_block->end_class = old_block->end_class;
    new_block->cond = old_block->cond;
    new_block->stack = old_block->stack;
    new_block->branch_true = old_block->branch_true;
    new_block->branch_false = old_block->branch_false;
    new_block->function_call = old_block->function_call;
    new_block->return_block = old_block->return_block;
    new_block->branch_reg = old_block->branch_reg;
    new_block->branch_offset = old_block->branch_offset;
    new_block->indirect_branches = std::move(old_block->indirect_branches);

    const Location old_begin{old_block->begin};
    Stack old_stack{std::move(old_block->stack)};
    *old_block = Block{};
    old_block->begin = old_begin;
    old_block->end = pc;
    old_block->end_class = EndClass::Branch;
    old_block->cond = IR::Condition(true);
    old_block->stack = old_stack;
    old_block->branch_true = new_block;
    old_block->branch_false = nullptr;
}

IR::Condition GuardOf(const Insn& insn) {
    const u32 pred{insn.Pred()};
    if (insn.IsUniformDatapath() && pred != PT) {
        return IR::Condition{static_cast<IR::Pred>(static_cast<u32>(IR::Pred::UP0) + pred), insn.PredInv()};
    }
    return IR::Condition{static_cast<IR::Pred>(pred), insn.PredInv()};
}

/// BRA's and EXIT's second predicate (87-89, negated by 90)
IR::Condition SecondPredicateOf(const Insn& insn) {
    return IR::Condition{static_cast<IR::Pred>(insn.PredSrc(87)), insn.Bit(90)};
}

/// Instructions without an effect on the data: reconvergence, scoreboard waits, NOP
bool HasNoEffect(u32 opcode) {
    switch (opcode) {
    case OP_BSSY:
    case OP_BSYNC:
    case OP_BREAK:
    case OP_WARPSYNC:
    case 0x918: // NOP
    case 0x91a: // DEPBAR
        return true;
    default:
        return false;
    }
}

/// Whether insn may write GPR reg (conservative: an unknown instruction naming it at 16-23 does)
bool MayWrite(const Insn& insn, u32 reg) {
    const u32 op{insn.Opcode()};
    if ((op >= 0x940 && op <= 0x95f) || HasNoEffect(op)) {
        return false;
    }
    switch (op) {
    case 0x385: case 0x386: case 0x387: case 0x388: case 0x985: case 0x986: case 0x987: case 0x988: // stores
    case 0x322: case 0x927: case 0x98e: // AST, ISBEWR, RED
        return false;
    default:
        break;
    }
    const u32 alu{insn.AluOp()};
    if (op < 0xe00 && (alu == 0x00b || alu == 0x00c || alu == 0x01c || alu == 0x034 || alu == 0x08c)) {
        return false; // predicate results only
    }
    const u32 dst{insn.Dst()};
    u32 span{1};
    switch (op) {
    case 0x225: case 0x625: case 0x825: case 0x805:
        span = 2;
        break;
    case 0x381: case 0x981: case 0x983: case 0x984: case 0xb82: {
        const u32 size{insn.Field(73, 3)};
        span = size == 6 ? 4 : size == 5 ? 2 : 1;
        break;
    }
    default:
        if ((op & 0x1f0) == 0x160) { // texture fetches: up to four, and a second destination at 64
            const u32 second{insn.Reg64()};
            if (second != RZ && reg >= second && reg < second + 2) {
                return true;
            }
            span = 4;
        }
        break;
    }
    return dst != RZ && reg >= dst && reg < dst + span;
}

struct JumpTable {
    u32 bank;
    u32 offset;
    u32 entries;
};

/// The jump table a BRX's register comes from, as the compiler builds it: LDC Rt = c[bank][Ri + offset] with
/// Ri = Rx << 2 (SHF.L or IMAD.SHL) and Rx = IMNMX(Rv, N) bounding the index to N + 1 entries
std::optional<JumpTable> TrackJumpTable(Environment& env, Location begin, Location pc, u32 target_reg,
                                        std::string& seen) {
    constexpr int MAX_DISTANCE{96};
    const auto find_writer{[&](Location from, u32 reg) -> std::optional<std::pair<Location, Insn>> {
        Location at{from};
        for (int k = 0; k < MAX_DISTANCE && at > begin; ++k) {
            --at;
            const Insn insn{ReadInsn(env, at)};
            if (!insn.NeverExecutes() && MayWrite(insn, reg)) {
                seen += fmt::format(" {:#x}:{:03x}", ByteOffsetOf(at), insn.Opcode());
                return std::pair{at, insn};
            }
        }
        return std::nullopt;
    }};
    const auto load{find_writer(pc, target_reg)};
    if (!load || load->second.Opcode() != 0xb82 || !load->second.AlwaysExecutes() ||
        load->second.Field(73, 3) != 4 || load->second.Field(78, 2) != 0 || load->second.Bit(91)) {
        return std::nullopt;
    }
    const Insn& ldc{load->second};
    const auto scale{find_writer(load->first, ldc.Src0())};
    if (!scale || !scale->second.AlwaysExecutes()) {
        return std::nullopt;
    }
    const Insn& shl{scale->second};
    const bool shf{shl.Opcode() == 0x819 && shl.Reg64() == RZ && shl.Imm32() == 2 && !shl.Bit(76) && !shl.Bit(80) &&
                   shl.Field(73, 2) == 3};
    const bool imad{shl.Opcode() == 0x824 && shl.Reg64() == RZ && shl.Imm32() == 4 && !shl.Bit(74) && !shl.Bit(75)};
    if (!shf && !imad) {
        return std::nullopt;
    }
    const auto bound{find_writer(scale->first, shl.Src0())};
    if (!bound || !bound->second.AlwaysExecutes() || bound->second.Opcode() != 0x817 ||
        bound->second.PredSrc(87) != PT || bound->second.Bit(90)) {
        return std::nullopt;
    }
    const u32 limit{bound->second.Imm32()};
    if (limit >= 0x1000) {
        return std::nullopt;
    }
    return JumpTable{ldc.CbufBank(), ldc.CbufOffset(), limit + 1};
}

/// Whether insn may write uniform register ureg (conservative): the uniform datapath's results at 16-23, and the
/// instructions that move into URs from elsewhere
bool MayWriteUniform(const Insn& insn, u32 ureg) {
    const u32 op{insn.Opcode()};
    if (insn.NeverExecutes()) {
        return false;
    }
    const bool moves_in{op == 0x9c3 || op == 0x3c2 || op == 0x3c4}; // S2UR, R2UR, REDUX
    if (!moves_in && !insn.IsUniformDatapath()) {
        return false;
    }
    const u32 alu{insn.AluOp()};
    if (!moves_in && (alu == 0x08c || alu == 0x09c)) { // UISETP, UPLOP3: predicates only
        return false;
    }
    const bool wide{!moves_in && (alu == 0x0a5 || ((op == 0xab9 || op == 0xabb) && insn.Field(73, 3) == 5))};
    const u32 span{wide ? 2U : 1U}; // UIMAD.WIDE, ULDC.64
    const u32 dst{insn.Dst()};
    return dst != URZ && ureg >= dst && ureg < dst + span;
}

/// Whether insn may write uniform predicate up (conservative: any uniform datapath instruction naming it as one of the
/// two predicate destinations, 81-83 and 84-86)
bool MayWriteUniformPred(const Insn& insn, u32 up) {
    return !insn.NeverExecutes() && (insn.IsUniformDatapath() || insn.Opcode() == 0x886) &&
           (insn.PredDst(81) == up || insn.PredDst(84) == up);
}

/// The jump table a BRXU's uniform register comes from: ULDC URt = c[bank][URx + offset] (0xabb), URx = URs << 2
/// (USHF.L), URs = USEL(URi, N, UPp) after UISETP.LT.U32 UPp = URi < N, N an immediate (N + 1 entries) or a uniform
/// register (a bound from the game's data: the table is measured)
std::optional<JumpTable> TrackUniformJumpTable(Environment& env, Location begin, Location pc, u32 target_reg,
                                               std::string& seen) {
    constexpr int MAX_DISTANCE{96};
    const auto find{[&](Location from, auto&& writes) -> std::optional<std::pair<Location, Insn>> {
        Location at{from};
        for (int k = 0; k < MAX_DISTANCE && at > begin; ++k) {
            --at;
            const Insn insn{ReadInsn(env, at)};
            if (writes(insn)) {
                seen += fmt::format(" {:#x}:{:03x}", ByteOffsetOf(at), insn.Opcode());
                return std::pair{at, insn};
            }
        }
        return std::nullopt;
    }};
    const auto ureg_writer{[&](Location from, u32 ureg) {
        return find(from, [ureg](const Insn& insn) { return MayWriteUniform(insn, ureg); });
    }};
    const auto load{ureg_writer(pc, target_reg)};
    if (!load || load->second.Opcode() != 0xabb || !load->second.AlwaysExecutes() ||
        load->second.Field(73, 3) != 4 || !load->second.Bit(91)) {
        return std::nullopt;
    }
    const Insn& uldc{load->second};
    const auto scale{ureg_writer(load->first, uldc.Src0())};
    if (!scale || !scale->second.AlwaysExecutes()) {
        return std::nullopt;
    }
    const Insn& shl{scale->second};
    if (shl.Opcode() != 0x899 || shl.Reg64() != URZ || shl.Imm32() != 2 || shl.Bit(76) || shl.Bit(80) ||
        shl.Field(73, 2) != 3) {
        return std::nullopt;
    }
    const auto clamp{ureg_writer(scale->first, shl.Src0())};
    if (!clamp || !clamp->second.AlwaysExecutes()) {
        return std::nullopt;
    }
    const Insn& sel{clamp->second};
    const bool imm_bound{sel.Opcode() == 0x887};
    if ((!imm_bound && sel.Opcode() != 0x287) || sel.Bit(90) || sel.PredSrc(87) == PT) {
        return std::nullopt;
    }
    const u32 index{sel.Src0()};
    const u32 bound{imm_bound ? sel.Imm32() : sel.UReg32()};
    const u32 up{sel.PredSrc(87)};
    const auto compare{find(clamp->first, [up](const Insn& insn) { return MayWriteUniformPred(insn, up); })};
    if (!compare || !compare->second.AlwaysExecutes()) {
        return std::nullopt;
    }
    const Insn& setp{compare->second};
    if (setp.Opcode() != (imm_bound ? 0x88cU : 0x28cU) || setp.PredDst(81) != up || setp.PredDst(84) != PT ||
        setp.Src0() != index || (imm_bound ? setp.Imm32() : setp.UReg32()) != bound || setp.Bit(72) ||
        setp.Field(73, 3) != 0 || setp.Field(76, 3) != 1 || setp.PredSrc(87) != PT || setp.Bit(90)) {
        return std::nullopt;
    }
    for (Location k{compare->first + 1}; k < clamp->first; ++k) {
        if (MayWriteUniform(ReadInsn(env, k), index) || (!imm_bound && MayWriteUniform(ReadInsn(env, k), bound))) {
            return std::nullopt;
        }
    }
    if (imm_bound) {
        if (bound >= 0x1000) {
            return std::nullopt;
        }
        return JumpTable{uldc.CbufBank(), uldc.CbufOffset(), bound + 1};
    }
    return JumpTable{uldc.CbufBank(), uldc.CbufOffset(), 0};
}
} // Anonymous namespace

Insn ReadInsn(Environment& env, Location location) {
    const u128 raw{env.ReadInstruction128(env.StartAddress() + ByteOffsetOf(location))};
    return Insn{raw[0], raw[1]};
}

CFG::CFG(Environment& env_, ObjectPool<Block>& block_pool_, bool exits_to_dispatcher_)
    : Shader::Flow::CFG{exits_to_dispatcher_}, env{env_}, block_pool{block_pool_}, program_start{LocationOf(0)} {
    if (exits_to_dispatcher) {
        dispatch_block = block_pool.Create(Block{});
        dispatch_block->begin = {};
        dispatch_block->end = {};
        dispatch_block->end_class = EndClass::Exit;
        dispatch_block->cond = IR::Condition(true);
        dispatch_block->stack = {};
        dispatch_block->branch_true = nullptr;
        dispatch_block->branch_false = nullptr;
    }
    at_exit_prologue_end = AtExitPrologueEnd(env, program_start);
    functions.emplace_back(block_pool, program_start);
    function_links.push_back(RZ);
    for (FunctionId function_id = 0; function_id < functions.size(); ++function_id) {
        while (!functions[function_id].labels.empty()) {
            Function& function{functions[function_id]};
            Label label{function.labels.back()};
            function.labels.pop_back();
            AnalyzeLabel(function_id, label);
        }
    }
    for (const auto& [function_id, exit_block] : late_exits) {
        const auto last_block{functions[function_id].blocks.rbegin()};
        exit_block->begin = last_block->end + 1;
        exit_block->end = last_block->end + 1;
        functions[function_id].blocks.insert(*exit_block);
    }
    if (exits_to_dispatcher) {
        const auto last_block{functions[0].blocks.rbegin()};
        dispatch_block->begin = last_block->end + 1;
        dispatch_block->end = last_block->end + 1;
        functions[0].blocks.insert(*dispatch_block);
    }
}

void CFG::AnalyzeLabel(FunctionId function_id, Label& label) {
    if (InspectVisitedBlocks(function_id, label)) {
        return;
    }
    Function* const function{&functions[function_id]};
    Location pc{label.address};
    const auto next_it{function->blocks.upper_bound(pc, Compare{})};
    const bool is_last{next_it == function->blocks.end()};
    Block* const next{is_last ? nullptr : &*next_it};
    Block* const block{label.block};
    bool is_branch{false};
    while (!next || pc < next->begin) {
        is_branch = AnalyzeInst(block, function_id, pc) == AnalysisState::Branch;
        if (is_branch) {
            break;
        }
        ++pc;
    }
    if (!is_branch) {
        block->end = pc;
        block->cond = IR::Condition{true};
        block->branch_true = next;
        block->branch_false = nullptr;
    }
    functions[function_id].blocks.insert(*block);
}

bool CFG::InspectVisitedBlocks(FunctionId function_id, const Label& label) {
    const Location pc{label.address};
    Function& function{functions[function_id]};
    const auto it{std::ranges::find_if(function.blocks, [pc](auto& block) { return block.Contains(pc); })};
    if (it == function.blocks.end()) {
        return false;
    }
    Block* const visited_block{&*it};
    if (visited_block->begin == pc) {
        throw LogicError("Dangling block");
    }
    Block* const new_block{label.block};
    Split(visited_block, new_block, pc);
    function.blocks.insert(it, *new_block);
    return true;
}

CFG::AnalysisState CFG::AnalyzeInst(Block* block, FunctionId function_id, Location pc) {
    const Insn insn{ReadInsn(env, pc)};
    const u32 opcode{insn.Opcode()};
    if (insn.NeverExecutes() || HasNoEffect(opcode)) {
        return AnalysisState::Continue;
    }
    switch (opcode) {
    case OP_BRA:
        return AnalyzeBRA(block, function_id, pc, insn);
    case OP_BRX:
    case OP_BRXU:
        return AnalyzeBRX(block, function_id, pc, insn);
    case OP_EXIT:
        return AnalyzeEXIT(block, function_id, pc, insn);
    case OP_CALL:
        return AnalyzeCALL(block, function_id, pc, insn);
    case OP_RET:
        return AnalyzeRET(block, function_id, pc, insn);
    case OP_KILL:
        AnalyzeCondInst(block, function_id, pc, EndClass::Kill, GuardOf(insn));
        return AnalysisState::Branch;
    case 0x34e: // LEPC
    case 0x357: // BMOV.64 ATEXIT_PC
        if (at_exit_prologue_end && pc < *at_exit_prologue_end) {
            return AnalysisState::Continue;
        }
        Fail(pc, insn, "outside a geometry stage's at-exit prologue");
    case 0x957:
        Fail(pc, insn, "outside a geometry stage's at-exit handler");
    default:
        break;
    }
    if (opcode >= 0x940 && opcode <= 0x95f) {
        Fail(pc, insn, "control flow instruction");
    }
    if ((opcode & 0x1fe) == 0x154 || opcode == 0x356 || opcode == 0x355) { // BMOV
        Fail(pc, insn, "barrier register move");
    }
    if (insn.AlwaysExecutes()) {
        return AnalysisState::Continue;
    }
    AnalyzeCondInst(block, function_id, pc, EndClass::Branch, GuardOf(insn));
    return AnalysisState::Branch;
}

Block* CFG::AnalyzeCondInst(Block* block, FunctionId function_id, Location pc, EndClass insn_end_class,
                            IR::Condition cond) {
    if (block->begin != pc) {
        block->end = pc;
        block->cond = IR::Condition{true};
        block->branch_true = AddLabel(block, block->stack, pc, function_id);
        block->branch_false = nullptr;
        return nullptr;
    }
    Block* const conditional_block{block_pool.Create()};
    Block virtual_block{};
    virtual_block.begin = block->begin.Virtual();
    virtual_block.end = block->begin.Virtual();
    virtual_block.end_class = EndClass::Branch;
    virtual_block.stack = block->stack;
    virtual_block.cond = cond;
    virtual_block.branch_true = conditional_block;
    virtual_block.branch_false = nullptr;
    *conditional_block = std::move(*block);
    *block = std::move(virtual_block);
    conditional_block->end = pc + 1;
    conditional_block->end_class = insn_end_class;
    Block* const endif_block{AddLabel(conditional_block, block->stack, pc + 1, function_id)};
    block->branch_false = endif_block;
    if (insn_end_class == EndClass::Branch || insn_end_class == EndClass::Kill) {
        conditional_block->cond = IR::Condition{true};
        conditional_block->branch_true = endif_block;
        conditional_block->branch_false = nullptr;
    }
    functions[function_id].blocks.insert(*conditional_block);
    return conditional_block;
}

Location CFG::BranchTarget(Location pc, const Insn& insn) const {
    const s64 target{s64{ByteOffsetOf(pc)} + 16 + insn.BranchOffset()};
    if (target < 0 || target % 16 != 0 || target >= MAX_PROGRAM_BYTES) {
        Fail(pc, insn, fmt::format("branch target {:#x}", target));
    }
    return LocationOf(static_cast<u32>(target));
}

CFG::AnalysisState CFG::AnalyzeBRA(Block* block, FunctionId function_id, Location pc, const Insn& insn) {
    // Bits 32-33: .U (1) only says the lanes agree; .DIV and .CONV branch on the warp's divergence, and 85-86 (.INC,
    // .DEC) count reconvergence
    if (insn.Field(32, 2) > 1 || insn.Field(85, 2) != 0) {
        Fail(pc, insn, "branch divergence mode");
    }
    const IR::Condition second{SecondPredicateOf(insn)};
    if (second == IR::Condition{false}) {
        return AnalysisState::Continue;
    }
    const Location target{BranchTarget(pc, insn)};
    if (!insn.AlwaysExecutes() && second != IR::Condition{true}) {
        // @Pg BRA Pc: a conditional block for the guard that branches on Pc
        Block* const conditional{AnalyzeCondInst(block, function_id, pc, EndClass::Exit, GuardOf(insn))};
        if (!conditional) {
            return AnalysisState::Branch;
        }
        conditional->end = pc;
        conditional->end_class = EndClass::Branch;
        conditional->cond = second;
        conditional->branch_false = AddLabel(conditional, conditional->stack, pc + 1, function_id);
        conditional->branch_true = AddLabel(conditional, conditional->stack, target, function_id);
        return AnalysisState::Branch;
    }
    const IR::Condition cond{insn.AlwaysExecutes() ? second : GuardOf(insn)};
    block->cond = cond;
    if (cond != IR::Condition{true}) {
        block->branch_false = AddLabel(block, block->stack, pc + 1, function_id);
    }
    block->branch_true = AddLabel(block, block->stack, target, function_id);
    block->end = pc;
    return AnalysisState::Branch;
}

CFG::AnalysisState CFG::AnalyzeBRX(Block* block, FunctionId function_id, Location pc, const Insn& insn) {
    const bool uniform{insn.Opcode() == OP_BRXU};
    if (!insn.AlwaysExecutes() || insn.Field(32, 2) != 0 || insn.Field(84, 7) != 0x38 || insn.Bit(91) != uniform) {
        Fail(pc, insn, "computed branch mode");
    }
    const u32 reg{insn.Src0()};
    std::string seen;
    const Location entry{functions[function_id].entrypoint};
    const std::optional<JumpTable> table{uniform ? TrackUniformJumpTable(env, entry, pc, reg, seen)
                                                 : TrackJumpTable(env, entry, pc, reg, seen)};
    if (!table) {
        Fail(pc, insn, fmt::format("jump table not found (writers:{})", seen));
    }
    // The target is the register plus the next instruction's offset plus the encoded one
    const s64 base{s64{ByteOffsetOf(pc)} + 16 + insn.BranchOffset()};
    const auto target_of{[&](u32 i) {
        return base + static_cast<s32>(env.ReadCbufValue(table->bank, table->offset + i * 4));
    }};
    const auto plausible{[&](s64 target) {
        return target > s64{ByteOffsetOf(pc)} && target % 16 == 0 && target < MAX_PROGRAM_BYTES;
    }};
    std::vector<u32> targets;
    if (table->entries == 0) {
        // A bound from the game's data: the table runs while its entries are cases (after the BRXU, as every table
        // with a constant bound in the seeds)
        for (u32 i = 0; i < 256 && plausible(target_of(i)); ++i) {
            targets.push_back(static_cast<u32>(target_of(i)));
        }
        if (targets.empty()) {
            Fail(pc, insn, "data-bounded jump table without a case");
        }
    }
    for (u32 i = 0; i < table->entries; ++i) {
        const s64 target{target_of(i)};
        if (!plausible(target)) {
            Fail(pc, insn, fmt::format("jump table entry {} is {:#x}", i, target));
        }
        targets.push_back(static_cast<u32>(target));
    }
    std::ranges::sort(targets);
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    block->indirect_branches.reserve(targets.size());
    for (const u32 target : targets) {
        Block* const branch{AddLabel(block, block->stack, LocationOf(target), function_id)};
        block->indirect_branches.push_back({.block = branch, .address = target});
    }
    block->cond = IR::Condition{true};
    block->end = pc + 1;
    block->end_class = EndClass::IndirectBranch;
    block->branch_reg = uniform ? IR::UniformReg(reg) : static_cast<IR::Reg>(reg);
    block->branch_offset = static_cast<s32>(base);
    return AnalysisState::Branch;
}

CFG::AnalysisState CFG::AnalyzeEXIT(Block* block, FunctionId function_id, Location pc, const Insn& insn) {
    if (insn.Field(84, 2) != 0) {
        Fail(pc, insn, "conditional or special EXIT");
    }
    const IR::Condition second{SecondPredicateOf(insn)};
    if (second == IR::Condition{false}) {
        return AnalysisState::Continue;
    }
    if (exits_to_dispatcher && function_id != 0) {
        Fail(pc, insn, "EXIT in a called function of a program with one exit");
    }
    const bool guarded{!insn.AlwaysExecutes()};
    const bool second_guarded{second != IR::Condition{true}};
    if (!guarded && !second_guarded) {
        if (exits_to_dispatcher) {
            block->cond = IR::Condition{true};
            block->end = pc;
            block->end_class = EndClass::Branch;
            block->branch_true = dispatch_block;
            block->branch_false = nullptr;
            return AnalysisState::Branch;
        }
        block->end = pc + 1;
        block->end_class = EndClass::Exit;
        return AnalysisState::Branch;
    }
    if (exits_to_dispatcher) {
        // @Pg EXIT Pc: to the dispatcher when both hold
        Block* const conditional{guarded && second_guarded
                                     ? AnalyzeCondInst(block, function_id, pc, EndClass::Exit, GuardOf(insn))
                                     : block};
        if (!conditional) {
            return AnalysisState::Branch;
        }
        conditional->end = pc;
        conditional->end_class = EndClass::Branch;
        conditional->cond = second_guarded ? second : GuardOf(insn);
        conditional->branch_true = dispatch_block;
        conditional->branch_false = AddLabel(conditional, conditional->stack, pc + 1, function_id);
        return AnalysisState::Branch;
    }
    if (guarded && second_guarded) {
        // The guard's conditional block holds the EXIT (its fragment outputs) and branches on Pc to an exit
        Block* const conditional{AnalyzeCondInst(block, function_id, pc, EndClass::Exit, GuardOf(insn))};
        if (!conditional) {
            return AnalysisState::Branch;
        }
        Block* const exit_block{block_pool.Create()};
        *exit_block = Block{};
        exit_block->end_class = EndClass::Exit;
        exit_block->cond = IR::Condition{true};
        exit_block->stack = conditional->stack;
        late_exits.emplace_back(function_id, exit_block);
        conditional->end_class = EndClass::Branch;
        conditional->cond = second;
        conditional->branch_true = exit_block;
        conditional->branch_false = AddLabel(conditional, conditional->stack, pc + 1, function_id);
        return AnalysisState::Branch;
    }
    AnalyzeCondInst(block, function_id, pc, EndClass::Exit, guarded ? GuardOf(insn) : second);
    return AnalysisState::Branch;
}

CFG::AnalysisState CFG::AnalyzeCALL(Block* block, FunctionId function_id, Location pc, const Insn& insn) {
    // CALL.REL.NOINC (bit 86) pushes no return address: with the return PC moved into a link register just before,
    // the callee returns with RET.REL.NODEC Rlink (an inlined call); a guarded one is a jump
    if (!insn.Bit(86) || insn.Field(32, 2) != 0 || insn.Field(87, 4) != 7 || insn.Bit(91) || insn.Src0() != 0 ||
        insn.Dst() != 0) {
        Fail(pc, insn, "call mode");
    }
    const Location target{BranchTarget(pc, insn)};
    if (!insn.AlwaysExecutes()) {
        block->cond = GuardOf(insn);
        block->branch_false = AddLabel(block, block->stack, pc + 1, function_id);
        block->branch_true = AddLabel(block, block->stack, target, function_id);
        block->end = pc;
        return AnalysisState::Branch;
    }
    // The link: MOV Rlink, <the next instruction's offset> up to 48 instructions back, not written after it
    const u32 return_offset{ByteOffsetOf(pc) + 16};
    std::optional<u32> link;
    Location at{pc};
    for (int back = 0; back < 48 && at > functions[function_id].entrypoint; ++back) {
        --at;
        const Insn prior{ReadInsn(env, at)};
        if (prior.Opcode() == 0x802 && prior.AlwaysExecutes() && prior.Imm32() == return_offset &&
            prior.Field(72, 4) == 0xf && prior.Dst() != RZ) {
            link = prior.Dst();
            break;
        }
        const u32 prior_op{prior.Opcode()};
        if (prior_op >= 0x940 && prior_op <= 0x95f && !HasNoEffect(prior_op)) {
            break;
        }
    }
    if (!link) {
        Fail(pc, insn, "call without a return link");
    }
    for (Location k{at + 1}; k < pc; ++k) {
        if (MayWrite(ReadInsn(env, k), *link)) {
            Fail(pc, insn, "call link overwritten before the call");
        }
    }
    const auto it{std::ranges::find(functions, target, &Function::entrypoint)};
    const bool exists{it != functions.end()};
    const FunctionId call_id{exists ? static_cast<size_t>(std::distance(functions.begin(), it)) : functions.size()};
    if (!exists) {
        functions.emplace_back(block_pool, target);
        function_links.push_back(*link);
    } else if (function_links[call_id] != *link) {
        Fail(pc, insn, "function called with two return links");
    }
    block->end_class = EndClass::Call;
    block->function_call = call_id;
    block->return_block = AddLabel(block, block->stack, pc + 1, function_id);
    block->end = pc;
    return AnalysisState::Branch;
}

CFG::AnalysisState CFG::AnalyzeRET(Block* block, FunctionId function_id, Location pc, const Insn& insn) {
    // RET.REL.NODEC Rlink with a relative base of zero: to the address in the register, which must be the link
    // the function was called with (or a copy of it)
    const u32 link{function_links[function_id]};
    const s64 base{s64{ByteOffsetOf(pc)} + 16 + insn.BranchOffset()};
    if (!insn.AlwaysExecutes() || function_id == 0 || base != 0 || insn.Field(32, 2) != 0 ||
        insn.Field(84, 7) != 0x3c || insn.Bit(91)) {
        Fail(pc, insn, "return mode");
    }
    const u32 reg{insn.Src0()};
    const Location entry{functions[function_id].entrypoint};
    const auto copies_link{[&](const Insn& prior) {
        if (prior.Dst() != reg || !prior.AlwaysExecutes()) {
            return false;
        }
        if (prior.Opcode() == 0x202) { // MOV Rd, Rlink
            return prior.Reg32() == link && prior.Field(72, 4) == 0xf;
        }
        if (prior.Opcode() == 0x224) { // IMAD.MOV.U32 Rd, RZ, RZ, Rlink
            return prior.Src0() == RZ && prior.Reg32() == RZ && prior.Reg64() == link && !prior.Bit(74) &&
                   !prior.Bit(75);
        }
        return false;
    }};
    bool linked{reg == link};
    for (Location k{pc}; k > entry && !linked;) {
        --k;
        const Insn prior{ReadInsn(env, k)};
        if (copies_link(prior)) {
            linked = true;
        } else if (MayWrite(prior, reg)) {
            break;
        }
    }
    for (Location k{entry}; linked && reg == link && k < pc; ++k) {
        if (MayWrite(ReadInsn(env, k), link)) {
            linked = false;
        }
    }
    if (!linked) {
        Fail(pc, insn, fmt::format("return through R{}, not the call's link R{}", reg, link));
    }
    block->end_class = EndClass::Return;
    block->cond = IR::Condition{true};
    block->end = pc;
    return AnalysisState::Branch;
}

Block* CFG::AddLabel(Block* block, Stack stack, Location pc, FunctionId function_id) {
    Function& function{functions[function_id]};
    if (block->begin == pc) {
        return block;
    }
    if (const auto it{function.blocks.find(pc, Compare{})}; it != function.blocks.end()) {
        if (function.blocks.begin() != it) {
            const auto prev{std::prev(it)};
            if (it->begin.Virtual() == prev->begin) {
                return &*prev;
            }
        }
        return &*it;
    }
    const auto label_it{std::ranges::find(function.labels, pc, &Label::address)};
    if (label_it != function.labels.end()) {
        return label_it->block;
    }
    Block* const new_block{block_pool.Create()};
    new_block->begin = pc;
    new_block->end = pc;
    new_block->end_class = EndClass::Branch;
    new_block->cond = IR::Condition(true);
    new_block->stack = stack;
    new_block->branch_true = nullptr;
    new_block->branch_false = nullptr;
    function.labels.push_back(Label{
        .address{pc},
        .block = new_block,
        .stack{std::move(stack)},
    });
    return new_block;
}

} // namespace Shader::Ampere::Flow
