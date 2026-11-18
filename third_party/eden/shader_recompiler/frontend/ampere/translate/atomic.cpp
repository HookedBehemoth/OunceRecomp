// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's atomics: ATOMG and RED (global, with and without the old value), ATOMG.CAS,
// ATOMS (shared) and ATOMS.POPC.INC. INC and DEC are the hardware's wrapping ones (old >= b ? 0 : old + 1, and
// old == 0 || old > b ? b : old - 1), as the IR's atomic Inc and Dec.

#include "shader_recompiler/frontend/ampere/translate/memory.h"

namespace Shader::Ampere {
namespace {
/// set_atom_op: the operation, numbered as NAK's AtomOp
enum class AtomOp : u32 { Add, Min, Max, Inc, Dec, And, Or, Xor, Exch };

/// set_atom_type(73..76) before SM90
enum class AtomType : u32 { U32, S32, U64, F32, F16x2, S64, F64 };

AtomType AtomTypeOf(Translator& t) {
    const u32 type{t.insn.Field(73, 3)};
    if (type > 6) {
        t.Fail("atomic type");
    }
    return static_cast<AtomType>(type);
}

bool Is64(AtomType type) {
    return type == AtomType::U64 || type == AtomType::S64;
}

IR::Value GlobalOp(Translator& t, const IR::U64& address, u32 data, AtomOp op, AtomType type) {
    auto& ir{t.ir};
    switch (type) {
    case AtomType::U32:
    case AtomType::S32:
    case AtomType::U64:
    case AtomType::S64: {
        const bool is_signed{type == AtomType::S32 || type == AtomType::S64};
        const IR::U32U64 value{Is64(type) ? IR::U32U64{ir.PackUint2x32(RegVector(t, data, 2))}
                                          : IR::U32U64{t.R(data)}};
        switch (op) {
        case AtomOp::Add:
            return ir.GlobalAtomicIAdd(address, value);
        case AtomOp::Min:
            return ir.GlobalAtomicIMin(address, value, is_signed);
        case AtomOp::Max:
            return ir.GlobalAtomicIMax(address, value, is_signed);
        case AtomOp::Inc:
        case AtomOp::Dec:
            if (type != AtomType::U32) {
                t.Fail("atomic increment or decrement type");
            }
            return op == AtomOp::Inc ? ir.GlobalAtomicInc(address, IR::U32{value})
                                     : ir.GlobalAtomicDec(address, IR::U32{value});
        case AtomOp::And:
            return ir.GlobalAtomicAnd(address, value);
        case AtomOp::Or:
            return ir.GlobalAtomicOr(address, value);
        case AtomOp::Xor:
            return ir.GlobalAtomicXor(address, value);
        case AtomOp::Exch:
            return ir.GlobalAtomicExchange(address, value);
        }
        break;
    }
    case AtomType::F32:
        if (op == AtomOp::Add) {
            // the hardware's F32 atomic add flushes denormals and rounds to nearest even
            return ir.GlobalAtomicF32Add(address, t.F(data),
                                         {.no_contraction = false, .rounding = IR::FpRounding::RN,
                                          .fmz_mode = IR::FmzMode::FTZ});
        }
        break;
    case AtomType::F16x2: {
        const IR::FpControl control{.no_contraction = false, .rounding = IR::FpRounding::RN,
                                    .fmz_mode = IR::FmzMode::DontCare};
        const IR::Value value{ir.UnpackFloat2x16(t.R(data))};
        switch (op) {
        case AtomOp::Add:
            return ir.PackFloat2x16(ir.GlobalAtomicF16x2Add(address, value, control));
        case AtomOp::Min:
            return ir.PackFloat2x16(ir.GlobalAtomicF16x2Min(address, value, control));
        case AtomOp::Max:
            return ir.PackFloat2x16(ir.GlobalAtomicF16x2Max(address, value, control));
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    t.Fail("global atomic operation and type");
}

void SetResult(Translator& t, const IR::Value& result, AtomType type) {
    const u32 dst{t.insn.Dst()};
    if (Is64(type)) {
        SetRegVector(t, dst, t.ir.UnpackUint2x32(IR::U64{result}), 2);
    } else if (type == AtomType::F32) {
        t.SetF(dst, IR::F32{result});
    } else {
        t.SetR(dst, IR::U32{result});
    }
}

AtomOp GlobalAtomOp(Translator& t, u32 start, u32 count) {
    const u32 op{t.insn.Field(start, count)};
    if (op > static_cast<u32>(AtomOp::Exch)) {
        t.Fail("atomic operation");
    }
    return static_cast<AtomOp>(op);
}

void RED(Translator& t) {
    // [Ra (+ URc 64-69 when bit 91) + offset] op= Rb; the operation 87-89; no result
    if (t.insn.Bit(91)) {
        ExpectOnly(t, HighBits(64, 70) | HighBits(72, 73) | kAccessBits | HighBits(87, 92), "RED mode");
    } else {
        ExpectOnly(t, HighBits(72, 73) | kAccessBits | HighBits(87, 90), "RED mode");
    }
    t.Expect(16, 8, 0, "RED mode");
    const IR::U64 address{t.insn.Bit(91) ? GlobalAddress(t, 90, 64) : GlobalAddress(t, 72, -1)};
    GlobalOp(t, address, t.insn.Reg32(), GlobalAtomOp(t, 87, 3), AtomTypeOf(t));
}

void ATOMG(Translator& t) {
    // 0x3a8: Rd = [Ra (64-bit by 72) + offset] op= Rb; 0x9a8: [Ra (64-bit by 70) + URc (64-69) + offset]; 87-90 the
    // operation
    IR::U64 address;
    if (t.insn.Opcode() == 0x9a8) {
        ExpectOnly(t, HighBits(64, 71) | HighBits(72, 73) | kAccessBits | kPredicateOutput | HighBits(87, 92),
                   "ATOMG mode");
        t.Expect(91, 1, 1, "ATOMG mode");
        address = GlobalAddress(t, 70, 64);
    } else {
        ExpectOnly(t, HighBits(72, 73) | kAccessBits | kPredicateOutput | HighBits(87, 91), "ATOMG mode");
        address = GlobalAddress(t, 72, -1);
    }
    ExpectNoPredicateOutput(t);
    const AtomType type{AtomTypeOf(t)};
    SetResult(t, GlobalOp(t, address, t.insn.Reg32(), GlobalAtomOp(t, 87, 4), type), type);
}

void ATOMG_CAS(Translator& t) {
    // Rd = [Ra (64-bit by 72) + offset] == Rb ? Rc (64-71) : unchanged, the old value; 32 bits only
    ExpectOnly(t, HighBits(64, 73) | kAccessBits | kPredicateOutput, "ATOMG.CAS mode");
    ExpectNoPredicateOutput(t);
    if (AtomTypeOf(t) != AtomType::U32 && AtomTypeOf(t) != AtomType::S32) {
        t.Fail("ATOMG.CAS type");
    }
    const IR::U64 address{GlobalAddress(t, 72, -1)};
    t.SetR(t.insn.Dst(), t.ir.GlobalAtomicCompareExchange(address, t.R(t.insn.Reg32()), t.R(t.insn.Reg64())));
}

void ATOMS(Translator& t) {
    // 0x38c: Rd = [Ra.Xn + offset] op= Rb; 0x98c: + URc (64-69, bit 91); 87-90 the operation; U32 and S32, and
    // EXCH of 64 bits
    if (t.insn.Opcode() == 0x98c) {
        ExpectOnly(t, HighBits(64, 70) | HighBits(73, 76) | HighBits(78, 80) | HighBits(87, 92), "ATOMS mode");
        t.Expect(91, 1, 1, "ATOMS mode");
    } else {
        ExpectOnly(t, HighBits(73, 76) | HighBits(78, 80) | HighBits(87, 91), "ATOMS mode");
    }
    auto& ir{t.ir};
    const IR::U32 address{SharedAddress(t, 64)};
    const AtomType type{AtomTypeOf(t)};
    const AtomOp op{GlobalAtomOp(t, 87, 4)};
    const u32 data{t.insn.Reg32()};
    if (type == AtomType::U64 && op == AtomOp::Exch) {
        SetResult(t, ir.SharedAtomicExchange(address, ir.PackUint2x32(RegVector(t, data, 2))), type);
        return;
    }
    if (type != AtomType::U32 && type != AtomType::S32) {
        t.Fail("ATOMS type");
    }
    const bool is_signed{type == AtomType::S32};
    const IR::U32 value{t.R(data)};
    IR::U32 result;
    switch (op) {
    case AtomOp::Add:
        result = ir.SharedAtomicIAdd(address, value);
        break;
    case AtomOp::Min:
        result = ir.SharedAtomicIMin(address, value, is_signed);
        break;
    case AtomOp::Max:
        result = ir.SharedAtomicIMax(address, value, is_signed);
        break;
    case AtomOp::Inc:
        result = ir.SharedAtomicInc(address, value);
        break;
    case AtomOp::Dec:
        result = ir.SharedAtomicDec(address, value);
        break;
    case AtomOp::And:
        result = ir.SharedAtomicAnd(address, value);
        break;
    case AtomOp::Or:
        result = ir.SharedAtomicOr(address, value);
        break;
    case AtomOp::Xor:
        result = ir.SharedAtomicXor(address, value);
        break;
    case AtomOp::Exch:
        result = IR::U32{ir.SharedAtomicExchange(address, value)};
        break;
    }
    t.SetR(t.insn.Dst(), result);
}

void ATOMS_POPC_INC(Translator& t) {
    // [Ra.Xn (+ URc 64-69) + offset] += the number of active lanes, one increment per lane; no result (Rd RZ)
    ExpectOnly(t, HighBits(64, 70) | HighBits(78, 80) | HighBits(87, 92), "ATOMS.POPC.INC mode");
    t.Expect(87, 5, 0x1a, "ATOMS.POPC.INC mode");
    t.Expect(16, 8, RZ, "ATOMS.POPC.INC result");
    t.Expect(32, 8, 0, "ATOMS.POPC.INC mode");
    (void)t.ir.SharedAtomicIAdd(SharedAddress(t, 64), t.ir.Imm32(1));
}
} // Anonymous namespace

void TranslateAtomic(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0x98e:
        return RED(t);
    case 0x3a8:
    case 0x9a8:
        return ATOMG(t);
    case 0x3a9:
        return ATOMG_CAS(t);
    case 0x38c:
    case 0x98c:
        return ATOMS(t);
    case 0xf8c:
        return ATOMS_POPC_INC(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
