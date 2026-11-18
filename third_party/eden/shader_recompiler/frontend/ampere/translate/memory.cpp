// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's loads and stores: LDC and ULDC (constant buffers), LDG and STG (global), LDL and
// STL (local), LDS and STS (shared), and the memory operands memory.h declares.

#include <optional>

#include <fmt/format.h>

#include "shader_recompiler/frontend/ampere/translate/memory.h"

namespace Shader::Ampere {

MemType MemTypeOf(Translator& t) {
    switch (t.insn.Field(73, 3)) {
    case 0:
        return {8, false};
    case 1:
        return {8, true};
    case 2:
        return {16, false};
    case 3:
        return {16, true};
    case 4:
        return {32, false};
    case 5:
        return {64, false};
    case 6:
        return {128, false};
    default:
        t.Fail("memory type");
    }
}

void ExpectOnly(Translator& t, u32 allowed, std::string_view what) {
    const u32 rest{t.insn.Field(64, 28) & ~allowed};
    if (rest != 0) {
        t.Fail(fmt::format("{} (bits {:#x} from 64)", what, rest));
    }
}

void ExpectNoPredicateOutput(Translator& t) {
    t.Expect(81, 3, PT, "predicate output");
}

IR::U64 GlobalAddress(Translator& t, u32 reg_wide_bit, int ureg_start) {
    auto& ir{t.ir};
    const u32 ra{t.insn.Src0()};
    const bool reg_wide{t.insn.Bit(reg_wide_bit)};
    const bool has_ureg{ureg_start >= 0 && t.insn.Bit(91) && t.insn.Field(static_cast<u32>(ureg_start), 6) != URZ};
    const u32 ur{has_ureg ? t.insn.Field(static_cast<u32>(ureg_start), 6) : URZ};
    const bool ureg_wide{t.insn.Bit(72)};
    if (ra != RZ && reg_wide && ra % 2 != 0) {
        t.Fail("unaligned address register pair");
    }
    if (has_ureg && ureg_wide && ur % 2 != 0) {
        t.Fail("unaligned uniform address pair");
    }
    // the two words of each part (a 32-bit part is zero-extended)
    const IR::U32 zero{ir.Imm32(0)};
    const IR::U32 reg_lo{t.R(ra)};
    const IR::U32 reg_hi{ra != RZ && reg_wide ? t.R(ra + 1) : zero};
    IR::U32 lo{reg_lo};
    IR::U32 hi{reg_hi};
    if (has_ureg) {
        const IR::U32 ureg_lo{t.UR(ur)};
        const IR::U32 ureg_hi{ureg_wide ? t.UR(ur + 1) : zero};
        if (ra == RZ) {
            lo = ureg_lo;
            hi = ureg_hi;
        } else {
            // the 64-bit sum in words, so that the pointer's low word is an IAdd32 of its parts
            lo = ir.IAdd(ureg_lo, reg_lo);
            const IR::U32 carry{ir.Select(ir.ILessThan(lo, ureg_lo, false), ir.Imm32(1), zero)};
            hi = ir.IAdd(ir.IAdd(ureg_hi, reg_hi), carry);
        }
    }
    const bool absolute{ra == RZ && !has_ureg};
    const u64 offset{absolute ? t.insn.Bits(40, 24)
                              : static_cast<u64>(static_cast<s64>(t.insn.Bits(40, 24) << 40) >> 40)};
    return ir.IAdd(ir.PackUint2x32(ir.CompositeConstruct(lo, hi)), ir.Imm64(offset));
}

IR::U32 SharedAddress(Translator& t, int ureg_start) {
    auto& ir{t.ir};
    const u32 ra{t.insn.Src0()};
    const u32 stride{t.insn.Field(78, 2)};
    const bool has_ureg{ureg_start >= 0 && t.insn.Bit(91)};
    const u32 ur{has_ureg ? t.insn.Field(static_cast<u32>(ureg_start), 6) : URZ};
    if (has_ureg && t.insn.Field(static_cast<u32>(ureg_start) + 6, 2) != 0) {
        t.Fail("uniform address register");
    }
    std::optional<IR::U32> base;
    if (ra != RZ) {
        // .X4, .X8, .X16: the register is an index of that many bytes
        base = stride == 0 ? t.R(ra) : IR::U32{ir.ShiftLeftLogical(t.R(ra), ir.Imm32(stride + 1))};
    }
    if (ur != URZ) {
        base = base ? IR::U32{ir.IAdd(*base, t.UR(ur))} : t.UR(ur);
    }
    if (!base) {
        return ir.Imm32(t.insn.Field(40, 24));
    }
    const s32 offset{static_cast<s32>(t.insn.Field(40, 24) << 8) >> 8};
    return offset == 0 ? *base : IR::U32{ir.IAdd(*base, ir.Imm32(offset))};
}

IR::Value RegVector(Translator& t, u32 reg, int count) {
    if (count == 1) {
        return t.R(reg);
    }
    if (reg != RZ && reg % static_cast<u32>(count) != 0) {
        t.Fail("unaligned register vector");
    }
    const auto at{[&](u32 i) { return reg == RZ ? t.ir.Imm32(0) : t.R(reg + i); }};
    if (count == 2) {
        return t.ir.CompositeConstruct(at(0), at(1));
    }
    return t.ir.CompositeConstruct(at(0), at(1), at(2), at(3));
}

void SetRegVector(Translator& t, u32 reg, const IR::Value& value, int count) {
    if (count > 1 && reg != RZ && reg % static_cast<u32>(count) != 0) {
        t.Fail("unaligned destination registers");
    }
    if (reg == RZ) {
        return;
    }
    if (count == 1) {
        t.SetR(reg, IR::U32{value});
        return;
    }
    for (int i = 0; i < count; ++i) {
        t.SetR(reg + static_cast<u32>(i), IR::U32{t.ir.CompositeExtract(value, static_cast<size_t>(i))});
    }
}

namespace {
int Words(const MemType& type) {
    return type.bits <= 32 ? 1 : type.bits / 32;
}

/// A constant buffer load of the given size into one or two registers (written by set)
template <typename Set>
void LoadCbuf(Translator& t, u32 bank, const IR::U32& offset, MemType type, Set&& set) {
    const IR::U32 binding{t.ir.Imm32(bank)};
    if (type.bits > 64) {
        t.Fail("constant load size");
    }
    if (type.bits == 64) {
        const IR::Value vector{t.ir.GetCbuf(binding, offset, 64, false)};
        set(0, IR::U32{t.ir.CompositeExtract(vector, 0)});
        set(1, IR::U32{t.ir.CompositeExtract(vector, 1)});
        return;
    }
    set(0, IR::U32{t.ir.GetCbuf(binding, offset, static_cast<size_t>(type.bits), type.is_signed)});
}

void LDC(Translator& t) {
    // Rd = c[bank][Ra + offset]; 78-79 the mode (0 indexed; the segmented and linear ones are not translated)
    t.Expect(78, 2, 0, "LDC mode");
    t.Expect(91, 1, 0, "bindless LDC");
    t.Expect(64, 8, 0, "LDC mode");
    const MemType type{MemTypeOf(t)};
    const u32 dst{t.insn.Dst()};
    if (type.bits == 64 && dst != RZ && dst % 2 != 0) {
        t.Fail("unaligned destination registers");
    }
    const u32 offset{t.insn.CbufOffset()};
    const IR::U32 address{t.insn.Src0() == RZ ? t.ir.Imm32(offset)
                                              : IR::U32{t.ir.IAdd(t.R(t.insn.Src0()), t.ir.Imm32(offset))}};
    LoadCbuf(t, t.insn.CbufBank(), address, type, [&](u32 i, const IR::U32& value) {
        if (dst != RZ) {
            t.SetR(dst + i, value);
        }
    });
}

void ULDC(Translator& t) {
    // URd = c[bank][offset] (0xab9), or c[bank][URa + offset] (0xabb, URa at 24, bit 91 set)
    const bool indexed{t.insn.Opcode() == 0xabb};
    if (indexed) {
        ExpectOnly(t, HighBits(73, 76) | HighBits(91, 92), "ULDC mode");
        t.Expect(91, 1, 1, "ULDC mode");
        t.Expect(30, 2, 0, "ULDC index register");
    } else {
        t.Expect(64, 9, 0, "ULDC mode");
        t.Expect(76, 16, 0, "ULDC mode");
    }
    const MemType type{MemTypeOf(t)};
    const u32 dst{t.insn.Dst()};
    if (type.bits == 64 && dst != URZ && dst % 2 != 0) {
        t.Fail("unaligned destination registers");
    }
    const u32 offset{t.insn.CbufOffset()};
    const u32 index{indexed ? t.insn.Field(24, 6) : URZ};
    const IR::U32 address{index == URZ ? t.ir.Imm32(offset) : IR::U32{t.ir.IAdd(t.UR(index), t.ir.Imm32(offset))}};
    LoadCbuf(t, t.insn.CbufBank(), address, type, [&](u32 i, const IR::U32& value) {
        if (dst != URZ) {
            t.SetUR(dst + i, value);
        }
    });
}

void LDG(Translator& t) {
    // 0x381: [Ra (64-bit by 72) + offset]; 0x981: [Ra (64-bit by 90) + URb (32-37, 64-bit by 72) + offset], with a
    // predicate (64-67, reversed: 0 is PT) that is not translated
    IR::U64 address;
    if (t.insn.Opcode() == 0x981) {
        ExpectOnly(t, HighBits(72, 73) | kAccessBits | kPredicateOutput | HighBits(90, 92), "LDG mode");
        t.Expect(91, 1, 1, "LDG mode");
        t.Expect(38, 2, 0, "LDG uniform address register");
        address = GlobalAddress(t, 90, 32);
    } else {
        ExpectOnly(t, HighBits(72, 73) | kAccessBits | kPredicateOutput, "LDG mode");
        t.Expect(32, 8, 0, "LDG mode");
        address = GlobalAddress(t, 72, -1);
    }
    ExpectNoPredicateOutput(t);
    const MemType type{MemTypeOf(t)};
    const u32 dst{t.insn.Dst()};
    switch (type.bits) {
    case 8:
        SetRegVector(t, dst, type.is_signed ? t.ir.LoadGlobalS8(address) : t.ir.LoadGlobalU8(address), 1);
        break;
    case 16:
        SetRegVector(t, dst, type.is_signed ? t.ir.LoadGlobalS16(address) : t.ir.LoadGlobalU16(address), 1);
        break;
    case 32:
        SetRegVector(t, dst, t.ir.LoadGlobal32(address), 1);
        break;
    case 64:
        SetRegVector(t, dst, t.ir.LoadGlobal64(address), 2);
        break;
    default:
        SetRegVector(t, dst, t.ir.LoadGlobal128(address), 4);
        break;
    }
}

void STG(Translator& t) {
    // 0x386: [Ra (64-bit by 72) + offset]; 0x986: [Ra (64-bit by 90) + URc (64-69, 64-bit by 72) + offset]; the data
    // Rb at 32
    IR::U64 address;
    if (t.insn.Opcode() == 0x986) {
        ExpectOnly(t, HighBits(64, 70) | HighBits(72, 73) | kAccessBits | HighBits(90, 92), "STG mode");
        t.Expect(91, 1, 1, "STG mode");
        address = GlobalAddress(t, 90, 64);
    } else {
        ExpectOnly(t, HighBits(72, 73) | kAccessBits, "STG mode");
        address = GlobalAddress(t, 72, -1);
    }
    t.Expect(16, 8, 0, "STG mode");
    const MemType type{MemTypeOf(t)};
    const u32 data{t.insn.Reg32()};
    switch (type.bits) {
    case 8:
        type.is_signed ? t.ir.WriteGlobalS8(address, t.R(data)) : t.ir.WriteGlobalU8(address, t.R(data));
        break;
    case 16:
        type.is_signed ? t.ir.WriteGlobalS16(address, t.R(data)) : t.ir.WriteGlobalU16(address, t.R(data));
        break;
    case 32:
        t.ir.WriteGlobal32(address, t.R(data));
        break;
    case 64:
        t.ir.WriteGlobal64(address, RegVector(t, data, 2));
        break;
    default:
        t.ir.WriteGlobal128(address, RegVector(t, data, 4));
        break;
    }
}

/// [Ra + URb + offset] of LDL (URb at 32) and STL (0x987: URb at 64), in bytes; the offset is signed with a register
IR::U32 LocalAddress(Translator& t, int ureg_start) {
    const u32 ra{t.insn.Src0()};
    const u32 ur{ureg_start >= 0 && t.insn.Bit(91) ? t.insn.Field(static_cast<u32>(ureg_start), 6) : URZ};
    std::optional<IR::U32> base;
    if (ra != RZ) {
        base = t.R(ra);
    }
    if (ur != URZ) {
        base = base ? IR::U32{t.ir.IAdd(*base, t.UR(ur))} : t.UR(ur);
    }
    if (!base) {
        return t.ir.Imm32(t.insn.Field(40, 24));
    }
    const s32 offset{static_cast<s32>(t.insn.Field(40, 24) << 8) >> 8};
    return offset == 0 ? *base : IR::U32{t.ir.IAdd(*base, t.ir.Imm32(offset))};
}

/// The word of local memory at a byte offset; past the program's local memory it reads 0, as Maxwell's LDL does
IR::U32 LoadLocalWord(Translator& t, const IR::U32& offset) {
    const IR::U32 word_offset{t.ir.ShiftRightArithmetic(offset, t.ir.Imm32(2))};
    const IR::U1 in_bounds{t.ir.ILessThan(offset, t.ir.Imm32(t.env.LocalMemorySize()), false)};
    return IR::U32{t.ir.Select(in_bounds, t.ir.LoadLocal(word_offset), t.ir.Imm32(0))};
}

/// The bit position of a byte (8) or half-word (16) access within its word
IR::U32 SubWordBit(Translator& t, const IR::U32& offset, int bits) {
    return t.ir.BitwiseAnd(t.ir.ShiftLeftLogical(offset, t.ir.Imm32(3)), t.ir.Imm32(bits == 8 ? 24 : 16));
}

void LDL(Translator& t) {
    // URb at 32 when bit 91 is set
    ExpectOnly(t, HighBits(73, 76) | HighBits(84, 87) | HighBits(91, 92), "LDL mode");
    if (t.insn.Bit(91)) {
        t.Expect(38, 2, 0, "LDL uniform address register");
    } else {
        t.Expect(32, 8, 0, "LDL mode");
    }
    const MemType type{MemTypeOf(t)};
    const u32 dst{t.insn.Dst()};
    const IR::U32 offset{LocalAddress(t, 32)};
    if (type.bits < 32) {
        const IR::U32 word{LoadLocalWord(t, offset)};
        t.SetR(dst, t.ir.BitFieldExtract(word, SubWordBit(t, offset, type.bits), t.ir.Imm32(type.bits),
                                         type.is_signed));
        return;
    }
    const int words{Words(type)};
    if (dst != RZ && dst % static_cast<u32>(words) != 0) {
        t.Fail("unaligned destination registers");
    }
    for (int i = 0; i < words; ++i) {
        const IR::U32 byte_offset{i == 0 ? offset : IR::U32{t.ir.IAdd(offset, t.ir.Imm32(i * 4))}};
        if (dst != RZ) {
            t.SetR(dst + static_cast<u32>(i), LoadLocalWord(t, byte_offset));
        }
    }
}

void STL(Translator& t) {
    // 0x387: [Ra + offset]; 0x987: [Ra + URc (64-69) + offset]; the data Rb at 32
    if (t.insn.Opcode() == 0x987) {
        ExpectOnly(t, HighBits(64, 70) | HighBits(73, 76) | HighBits(84, 87) | HighBits(91, 92), "STL mode");
        t.Expect(91, 1, 1, "STL mode");
    } else {
        ExpectOnly(t, HighBits(73, 76) | HighBits(84, 87), "STL mode");
    }
    t.Expect(16, 8, 0, "STL mode");
    const MemType type{MemTypeOf(t)};
    const IR::U32 offset{LocalAddress(t, 64)};
    if (offset.IsImmediate() && offset.U32() >= t.env.LocalMemorySize()) {
        // past the program's local memory: dropped, as Maxwell's STL does
        return;
    }
    const u32 data{t.insn.Reg32()};
    const IR::U32 word_offset{t.ir.ShiftRightArithmetic(offset, t.ir.Imm32(2))};
    if (type.bits < 32) {
        const IR::U32 merged{t.ir.BitFieldInsert(t.ir.LoadLocal(word_offset), t.R(data), SubWordBit(t, offset, type.bits),
                                                 t.ir.Imm32(type.bits))};
        t.ir.WriteLocal(word_offset, merged);
        return;
    }
    const int words{Words(type)};
    if (data != RZ && data % static_cast<u32>(words) != 0) {
        t.Fail("unaligned data registers");
    }
    for (int i = 0; i < words; ++i) {
        const IR::U32 at{i == 0 ? word_offset : IR::U32{t.ir.IAdd(word_offset, t.ir.Imm32(i))}};
        t.ir.WriteLocal(at, data == RZ ? t.ir.Imm32(0) : t.R(data + static_cast<u32>(i)));
    }
}

void LDS(Translator& t) {
    // [Ra.Xn + URb (32, bit 91) + offset]
    ExpectOnly(t, HighBits(73, 76) | HighBits(78, 80) | HighBits(91, 92), "LDS mode");
    if (!t.insn.Bit(91)) {
        t.Expect(32, 8, 0, "LDS mode");
    }
    const MemType type{MemTypeOf(t)};
    const IR::Value value{t.ir.LoadShared(type.bits, type.is_signed, SharedAddress(t, 32))};
    SetRegVector(t, t.insn.Dst(), value, Words(type));
}

void STS(Translator& t) {
    // 0x388: [Ra.Xn + offset]; 0x988: [Ra.Xn + URc (64-69) + offset]; the data Rb at 32
    if (t.insn.Opcode() == 0x988) {
        ExpectOnly(t, HighBits(64, 70) | HighBits(73, 76) | HighBits(78, 80) | HighBits(91, 92), "STS mode");
        t.Expect(91, 1, 1, "STS mode");
    } else {
        ExpectOnly(t, HighBits(73, 76) | HighBits(78, 80), "STS mode");
    }
    t.Expect(16, 8, 0, "STS mode");
    const MemType type{MemTypeOf(t)};
    const IR::U32 address{SharedAddress(t, 64)};
    const int words{Words(type)};
    const IR::Value data{RegVector(t, t.insn.Reg32(), words)};
    t.ir.WriteShared(type.bits, address, data);
}
} // Anonymous namespace

void TranslateMemory(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0xb82:
        return LDC(t);
    case 0xab9:
    case 0xabb:
        return ULDC(t);
    case 0x381:
    case 0x981:
        return LDG(t);
    case 0x386:
    case 0x986:
        return STG(t);
    case 0x983:
        return LDL(t);
    case 0x387:
    case 0x987:
        return STL(t);
    case 0x984:
        return LDS(t);
    case 0x388:
    case 0x988:
        return STS(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
