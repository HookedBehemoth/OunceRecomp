// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's loads: LDC and ULDC (constant buffers), LDG (global), LDL (local), LDS (shared).

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
/// set_mem_type(73..76): U8, S8, U16, S16, B32, B64, B128
struct MemType {
    int bits;
    bool is_signed;
};

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

void CheckDestination(Translator& t, u32 dst, int bits) {
    if (bits > 32 && dst != RZ && dst % (bits / 32) != 0) {
        t.Fail("unaligned destination registers");
    }
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
    CheckDestination(t, dst, type.bits);
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
    // URd = c[bank][offset]
    t.Expect(64, 9, 0, "ULDC mode");
    t.Expect(76, 16, 0, "ULDC mode");
    const MemType type{MemTypeOf(t)};
    const u32 dst{t.insn.Dst()};
    if (type.bits == 64 && dst != URZ && dst % 2 != 0) {
        t.Fail("unaligned destination registers");
    }
    LoadCbuf(t, t.insn.CbufBank(), t.ir.Imm32(t.insn.CbufOffset()), type, [&](u32 i, const IR::U32& value) {
        if (dst != URZ) {
            t.SetUR(dst + i, value);
        }
    });
}

IR::U64 Pair(Translator& t, u32 reg, bool uniform) {
    const auto read{[&](u32 r) { return uniform ? t.UR(r) : t.R(r); }};
    const u32 zero{uniform ? URZ : RZ};
    if (reg != zero && reg % 2 != 0) {
        t.Fail("unaligned address pair");
    }
    return t.ir.PackUint2x32(t.ir.CompositeConstruct(read(reg), reg == zero ? t.ir.Imm32(0) : read(reg + 1)));
}

void LDG(Translator& t) {
    // 0x381: [Ra.64 + offset]; 0x981: [Ra (64-bit by 90) + URb.64 (32-37) + offset]; the offset 40-63, signed
    const bool uniform_form{t.insn.Opcode() == 0x981};
    const MemType type{MemTypeOf(t)};
    IR::U64 address;
    if (uniform_form) {
        t.Expect(91, 1, 1, "LDG mode");
        t.Expect(72, 1, 1, "LDG 32-bit uniform address");
        t.Expect(38, 2, 0, "LDG mode");
        t.Expect(64, 4, 0, "LDG predicate");
        t.Expect(76, 1, 0, "LDG memory descriptor");
        t.Expect(81, 3, PT, "LDG predicate output");
        const u32 ra{t.insn.Src0()};
        const bool wide{t.insn.Bit(90)};
        const IR::U64 base{Pair(t, t.insn.UReg32(), true)};
        if (ra == RZ) {
            address = base;
        } else {
            const IR::U64 reg{wide ? Pair(t, ra, false) : IR::U64{t.ir.UConvert(64, t.R(ra))}};
            address = t.ir.IAdd(base, reg);
        }
    } else {
        t.Expect(72, 1, 1, "LDG 32-bit address");
        t.Expect(32, 8, 0, "LDG mode");
        address = Pair(t, t.insn.Src0(), false);
    }
    const s64 offset{static_cast<s64>(t.insn.Bits(40, 24) << 40) >> 40};
    address = t.ir.IAdd(address, t.ir.Imm64(static_cast<u64>(offset)));
    const u32 dst{t.insn.Dst()};
    CheckDestination(t, dst, type.bits);
    const auto set_vector{[&](const IR::Value& vector, int count) {
        for (int i = 0; i < count; ++i) {
            if (dst != RZ) {
                t.SetR(dst + static_cast<u32>(i), IR::U32{t.ir.CompositeExtract(vector, static_cast<size_t>(i))});
            }
        }
    }};
    switch (type.bits) {
    case 8:
        t.SetR(dst, type.is_signed ? t.ir.LoadGlobalS8(address) : t.ir.LoadGlobalU8(address));
        break;
    case 16:
        t.SetR(dst, type.is_signed ? t.ir.LoadGlobalS16(address) : t.ir.LoadGlobalU16(address));
        break;
    case 32:
        t.SetR(dst, t.ir.LoadGlobal32(address));
        break;
    case 64:
        set_vector(t.ir.LoadGlobal64(address), 2);
        break;
    default:
        set_vector(t.ir.LoadGlobal128(address), 4);
        break;
    }
}

/// [Ra + offset] of LDL and LDS (24 bits at 40, signed with a register); the uniform part (32-37) must be URZ
IR::U32 LocalAddress(Translator& t) {
    t.Expect(91, 1, 0, "local or shared address with a uniform register");
    const u32 ra{t.insn.Src0()};
    if (ra == RZ) {
        return t.ir.Imm32(t.insn.Field(40, 24));
    }
    const s32 offset{static_cast<s32>(t.insn.Field(40, 24) << 8) >> 8};
    return t.ir.IAdd(t.R(ra), t.ir.Imm32(offset));
}

void LDL(Translator& t) {
    // only words: the 8- and 16-bit forms are not translated; out-of-range words read 0, as on Maxwell
    const MemType type{MemTypeOf(t)};
    if (type.bits < 32) {
        t.Fail("LDL size");
    }
    const u32 dst{t.insn.Dst()};
    CheckDestination(t, dst, type.bits);
    const IR::U32 offset{LocalAddress(t)};
    const IR::U32 size{t.ir.Imm32(t.env.LocalMemorySize())};
    for (int i = 0; i < type.bits / 32; ++i) {
        const IR::U32 byte_offset{i == 0 ? offset : IR::U32{t.ir.IAdd(offset, t.ir.Imm32(i * 4))}};
        const IR::U32 word_offset{t.ir.ShiftRightArithmetic(byte_offset, t.ir.Imm32(2))};
        const IR::U1 in_bounds{t.ir.ILessThan(byte_offset, size, false)};
        if (dst != RZ) {
            t.SetR(dst + static_cast<u32>(i),
                   IR::U32{t.ir.Select(in_bounds, t.ir.LoadLocal(word_offset), t.ir.Imm32(0))});
        }
    }
}

void LDS(Translator& t) {
    t.Expect(78, 2, 0, "LDS offset stride");
    t.Expect(87, 1, 0, "LDS mode");
    const MemType type{MemTypeOf(t)};
    const u32 dst{t.insn.Dst()};
    CheckDestination(t, dst, type.bits);
    const IR::Value value{t.ir.LoadShared(type.bits, type.is_signed, LocalAddress(t))};
    if (type.bits <= 32) {
        t.SetR(dst, IR::U32{value});
        return;
    }
    for (int i = 0; i < type.bits / 32; ++i) {
        if (dst != RZ) {
            t.SetR(dst + static_cast<u32>(i), IR::U32{t.ir.CompositeExtract(value, static_cast<size_t>(i))});
        }
    }
}
} // Anonymous namespace

void TranslateMemory(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0xb82:
        return LDC(t);
    case 0xab9:
        return ULDC(t);
    case 0x381:
    case 0x981:
        return LDG(t);
    case 0x983:
        return LDL(t);
    case 0x984:
        return LDS(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
