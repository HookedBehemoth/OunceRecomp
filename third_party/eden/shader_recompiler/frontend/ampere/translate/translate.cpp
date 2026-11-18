// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

#include <fmt/format.h>

#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/control_flow.h"
#include "shader_recompiler/frontend/ampere/translate/translator.h"
#include "shader_recompiler/program_header.h"

namespace Shader::Ampere {

void Translator::Fail(std::string_view reason) const {
    throw NotImplementedException("Ampere {:03x} {} at {:#x}: {}", insn.Opcode(), OpcodeName(insn.Opcode()),
                                  Flow::ByteOffsetOf(pc), reason);
}

void Translator::Expect(u32 start, u32 count, u64 value, std::string_view what) const {
    if (insn.Bits(start, count) != value) {
        Fail(fmt::format("{} (bits {}-{} = {:#x})", what, start, start + count - 1, insn.Bits(start, count)));
    }
}

IR::U32 Translator::R(u32 reg) {
    return reg == RZ ? ir.Imm32(0) : ir.GetReg(static_cast<IR::Reg>(reg));
}

void Translator::SetR(u32 reg, const IR::U32& value) {
    if (reg != RZ) {
        ir.SetReg(static_cast<IR::Reg>(reg), value);
    }
}

IR::F32 Translator::F(u32 reg) {
    return ir.BitCast<IR::F32>(R(reg));
}

void Translator::SetF(u32 reg, const IR::F32& value) {
    SetR(reg, ir.BitCast<IR::U32>(value));
}

IR::U32 Translator::UR(u32 reg) {
    return reg == URZ ? ir.Imm32(0) : ir.GetReg(IR::UniformReg(reg));
}

void Translator::SetUR(u32 reg, const IR::U32& value) {
    if (reg != URZ) {
        ir.SetReg(IR::UniformReg(reg), value);
    }
}

IR::U1 Translator::P(u32 pred, bool negated) {
    if (pred == PT) {
        return ir.Imm1(!negated);
    }
    return ir.GetPred(static_cast<IR::Pred>(pred), negated);
}

void Translator::SetP(u32 pred, const IR::U1& value) {
    if (pred != PT) {
        ir.SetPred(static_cast<IR::Pred>(pred), value);
    }
}

IR::U1 Translator::UP(u32 pred, bool negated) {
    if (pred == PT) {
        return ir.Imm1(!negated);
    }
    return ir.GetPred(static_cast<IR::Pred>(static_cast<u32>(IR::Pred::UP0) + pred), negated);
}

void Translator::SetUP(u32 pred, const IR::U1& value) {
    if (pred != PT) {
        ir.SetPred(static_cast<IR::Pred>(static_cast<u32>(IR::Pred::UP0) + pred), value);
    }
}

IR::U1 Translator::PredSrc(u32 start) {
    return P(insn.PredSrc(start), insn.Bit(start + 3));
}

IR::U1 Translator::UPredSrc(u32 start) {
    return UP(insn.PredSrc(start), insn.Bit(start + 3));
}

IR::U32 Translator::X(u32 reg) {
    if (!uniform) {
        return R(reg);
    }
    if (reg > URZ) {
        Fail(fmt::format("uniform register field {:#x}", reg));
    }
    return UR(reg);
}

void Translator::SetX(u32 reg, const IR::U32& value) {
    if (!uniform) {
        return SetR(reg, value);
    }
    if (reg > URZ) {
        Fail(fmt::format("uniform register field {:#x}", reg));
    }
    SetUR(reg, value);
}

bool Translator::XIsZero(u32 reg) const {
    return reg == (uniform ? URZ : RZ);
}

IR::U1 Translator::XPredSrc(u32 start) {
    return uniform ? UPredSrc(start) : PredSrc(start);
}

void Translator::SetXP(u32 pred, const IR::U1& value) {
    uniform ? SetUP(pred, value) : SetP(pred, value);
}

IR::U32 Translator::Cbuf() {
    if (insn.CbufBindless()) {
        Fail("bindless constant buffer operand");
    }
    const u32 offset{insn.CbufOffset()};
    if (offset % 4 != 0) {
        Fail(fmt::format("unaligned constant buffer operand c[{}][{:#x}]", insn.CbufBank(), offset));
    }
    return ir.GetCbuf(ir.Imm32(insn.CbufBank()), ir.Imm32(offset));
}

Operand Translator::Src0() {
    return {X(insn.Src0()), insn.Bit(73), insn.Bit(72)};
}

Operand Translator::Src1() {
    if (uniform) {
        // encode_ualu: URb at 32, an immediate or URc at 64
        switch (insn.Form()) {
        case AluForm::RegReg:
            return {X(insn.Reg32()), insn.Bit(62), insn.Bit(63)};
        case AluForm::RegImm:
            return {X(insn.Reg64()), insn.Bit(74), insn.Bit(75)};
        case AluForm::ImmReg:
            return {ir.Imm32(insn.Imm32()), false, false, true};
        default:
            Fail("uniform ALU source form");
        }
    }
    switch (insn.Form()) {
    case AluForm::RegReg:
        return {R(insn.Reg32()), insn.Bit(62), insn.Bit(63)};
    case AluForm::RegImm:
    case AluForm::RegCbuf:
    case AluForm::RegUReg:
        return {R(insn.Reg64()), insn.Bit(74), insn.Bit(75)};
    case AluForm::ImmReg:
        return {ir.Imm32(insn.Imm32()), false, false, true};
    case AluForm::CbufReg:
        return {Cbuf(), insn.Bit(62), insn.Bit(63)};
    case AluForm::URegReg:
        return {UR(insn.UReg32()), insn.Bit(62), insn.Bit(63)};
    }
    Fail("ALU source form");
}

Operand Translator::Src2() {
    if (uniform) {
        switch (insn.Form()) {
        case AluForm::RegReg:
        case AluForm::ImmReg:
            return {X(insn.Reg64()), insn.Bit(74), insn.Bit(75)};
        case AluForm::RegImm:
            return {ir.Imm32(insn.Imm32()), false, false, true};
        default:
            Fail("uniform ALU source form");
        }
    }
    switch (insn.Form()) {
    case AluForm::RegReg:
    case AluForm::ImmReg:
    case AluForm::CbufReg:
    case AluForm::URegReg:
        return {R(insn.Reg64()), insn.Bit(74), insn.Bit(75)};
    case AluForm::RegImm:
        return {ir.Imm32(insn.Imm32()), false, false, true};
    case AluForm::RegCbuf:
        return {Cbuf(), insn.Bit(62), insn.Bit(63)};
    case AluForm::RegUReg:
        return {UR(insn.UReg32()), insn.Bit(62), insn.Bit(63)};
    }
    Fail("ALU source form");
}

IR::F32 Translator::Float(const Operand& operand) {
    const IR::F32 value{ir.BitCast<IR::F32>(operand.value)};
    return operand.abs || operand.neg ? IR::F32{ir.FPAbsNeg(value, operand.abs, operand.neg)} : value;
}

void TranslateExit(Translator& t) {
    // A fragment stage's outputs are in the registers at EXIT, as its program header lays them out
    if (t.env.ShaderStage() != Stage::Fragment) {
        return;
    }
    const ProgramHeader sph{t.env.SPH()};
    u32 reg{0};
    for (u32 render_target = 0; render_target < 8; ++render_target) {
        if (!sph.ps.HasOutputComponents(render_target)) {
            continue;
        }
        const std::array<bool, 4> mask{sph.ps.EnabledOutputComponents(render_target)};
        for (u32 component = 0; component < 4; ++component, ++reg) {
            if (mask[component]) {
                t.ir.SetFragColor(render_target, component, t.F(reg));
            }
        }
    }
    if (sph.ps.omap.sample_mask != 0) {
        t.ir.SetSampleMask(t.R(reg));
    }
    if (sph.ps.omap.depth != 0) {
        t.ir.SetFragDepth(t.F(reg + 1));
    }
}

namespace {
void TranslateOne(Translator& t) {
    const u32 opcode{t.insn.Opcode()};
    switch (opcode) {
    // reconvergence, scoreboard waits and the branches the control flow graph took
    case 0x945: // BSSY
    case 0x941: // BSYNC
    case 0x942: // BREAK
    case 0x948: // WARPSYNC
    case 0x918: // NOP
    case 0x91a: // DEPBAR
    case 0x947: // BRA
    case 0x949: // BRX
    case 0x958: // BRXU
    case 0x944: // CALL
    case 0x950: // RET
    case 0x95b: // KILL (the block's end)
        return;
    case 0x94d:
        return TranslateExit(t);
    case 0xb82: case 0xab9: case 0xabb: case 0x381: case 0x981: case 0x386: case 0x986: case 0x983: case 0x387:
    case 0x987: case 0x984: case 0x388: case 0x988:
        return TranslateMemory(t);
    case 0x98e: case 0x3a8: case 0x9a8: case 0x3a9: case 0x38c: case 0x98c: case 0xf8c:
        return TranslateAtomic(t);
    case 0x919: case 0x9c3: case 0x805:
        return TranslateSpecial(t);
    case 0xb1d: case 0x992: case 0x9ab: case 0x98f:
        return TranslateBarrier(t);
    case 0x923: case 0x927:
        return TranslateIsbe(t);
    case 0x321: case 0x322: case 0x326: case 0x526: case 0x925:
        return TranslateAttribute(t);
    case 0x806: case 0x886: case 0x389: case 0x589: case 0x989: case 0xf89: case 0x3a1: case 0x3c4: case 0x822:
        return TranslateWarp(t);
    case 0x23c: case 0x83b: case 0x23a:
        return TranslateMatrix(t);
    default:
        break;
    }
    switch (opcode & 0x1ff) {
    case 0x020: case 0x021: case 0x023: case 0x009: case 0x00a: case 0x00b: case 0x008: case 0x108: case 0x107:
        return TranslateFloat(t);
    case 0x010: case 0x012: case 0x017: case 0x019: case 0x024: case 0x025: case 0x027: case 0x00c: case 0x007:
    case 0x011: case 0x01c: case 0x013: case 0x090:
    case 0x016: case 0x01a: case 0x100: case 0x101: case 0x109:
    case 0x087: case 0x08c: case 0x091: case 0x092: case 0x096: case 0x099: case 0x09a: case 0x09c: case 0x0a4:
    case 0x0a5: case 0x0bd: case 0x0be: case 0x0bf:
        return TranslateInteger(t);
    case 0x002: case 0x082: case 0x1c2:
        return TranslateMove(t);
    case 0x105: case 0x045: case 0x106: case 0x03e: case 0x043:
        return TranslateConversion(t);
    case 0x030: case 0x031: case 0x032: case 0x034: case 0x040:
        return TranslateHalf(t);
    case 0x003: case 0x004: case 0x083:
        return TranslatePredicate(t);
    default:
        break;
    }
    switch (opcode) {
    case 0xb60: case 0xf60: case 0x361: case 0xb66: case 0xb63: case 0xb69: case 0xb6c: case 0xb6f:
        return TranslateTexture(t);
    case 0xb97: case 0xb9b: case 0x593: case 0x59f:
        return TranslateSurface(t);
    default:
        break;
    }
    t.Fail("not translated");
}
} // Anonymous namespace

void Translate(Environment& env, IR::Block* block, Location begin, Location end) {
    if (begin == end) {
        return;
    }
    Translator t{env, *block};
    for (Location pc = begin; pc != end; ++pc) {
        t.pc = pc;
        t.insn = Flow::ReadInsn(env, pc);
        t.uniform = t.insn.IsUniformDatapath();
        if (t.insn.NeverExecutes()) {
            continue;
        }
        TranslateOne(t);
    }
}

} // namespace Shader::Ampere
