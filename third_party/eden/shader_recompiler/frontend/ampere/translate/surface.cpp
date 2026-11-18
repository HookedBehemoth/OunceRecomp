// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's surface instructions with a descriptor in a constant buffer (40-58, as TEX's):
// SULD.P and SUST.P (formatted, the channel mask at 72-75), SUATOM and SURED (32-bit integer atomics, the operation at
// 87-90, the type at 73-75). The coordinates are integers in Ra on (the array index last), the data in Rb, the
// dimension at 61-63 as NAK's ImageDim.

#include <bit>

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
struct ImageDim {
    TextureType type;
    u32 coordinates; ///< with the array index
    bool array;
};

ImageDim ImageDimOf(Translator& t) {
    switch (t.insn.Field(61, 3)) {
    case 0:
        return {TextureType::Color1D, 1, false};
    case 1:
        return {TextureType::Buffer, 1, false};
    case 2:
        return {TextureType::ColorArray1D, 2, true};
    case 3:
        return {TextureType::Color2D, 2, false};
    case 4:
        return {TextureType::ColorArray2D, 3, true};
    case 5:
        return {TextureType::Color3D, 3, false};
    default:
        t.Fail("surface dimension");
    }
}

IR::Value Coords(Translator& t, const ImageDim& dim) {
    const u32 ra{t.insn.Src0()};
    if (ra != RZ && ra + dim.coordinates > RZ) {
        t.Fail("surface coordinate registers");
    }
    const auto value{[&](u32 i) { return ra == RZ ? t.ir.Imm32(0) : t.R(ra + i); }};
    const auto layer{[&](u32 i) { return t.ir.BitFieldExtract(value(i), t.ir.Imm32(0), t.ir.Imm32(16)); }};
    switch (dim.coordinates) {
    case 1:
        return value(0);
    case 2:
        return t.ir.CompositeConstruct(value(0), dim.array ? layer(1) : value(1));
    default:
        return t.ir.CompositeConstruct(value(0), value(1), dim.array ? layer(2) : value(2));
    }
}

IR::U32 Handle(Translator& t) {
    return t.ir.GetCbuf(t.ir.Imm32(t.insn.CbufBank()), t.ir.Imm32(t.insn.Field(40, 14) * 4));
}

/// Fields every bound surface instruction shares: no .B or bindless (59-60, 91), weak or strong-GPU order (77-80)
void ExpectBound(Translator& t) {
    t.Expect(59, 2, 0, "surface .B");
    t.Expect(91, 1, 0, "surface bindless handle");
}

/// SULD.P: the enabled channels (1, 3 or 15) to Rd on, as 32-bit words
void SULD(Translator& t) {
    ExpectBound(t);
    t.Expect(76, 1, 0, "SULD bit 76");
    t.Expect(77, 4, 0, "SULD memory order");
    t.Expect(81, 3, PT, "SULD fault predicate");
    const u32 mask{t.insn.Field(72, 4)};
    if (mask != 1 && mask != 3 && mask != 15) {
        t.Fail("SULD channel mask");
    }
    const ImageDim dim{ImageDimOf(t)};
    IR::TextureInstInfo info{};
    info.type.Assign(dim.type);
    info.image_format.Assign(ImageFormat::Typeless);
    const IR::Value texel{t.ir.ImageRead(Handle(t), Coords(t, dim), info)};
    const u32 dst{t.insn.Dst()};
    const u32 count{static_cast<u32>(std::popcount(mask))};
    if (dst != RZ && dst + count > RZ) {
        t.Fail("SULD destination registers");
    }
    for (u32 i = 0; i < count && dst != RZ; ++i) {
        t.SetR(dst + i, IR::U32{t.ir.CompositeExtract(texel, i)});
    }
}

/// SUST.P with all four channels from Rb on
void SUST(Translator& t) {
    ExpectBound(t);
    t.Expect(76, 1, 0, "SUST bit 76");
    t.Expect(77, 4, 0, "SUST memory order");
    t.Expect(72, 4, 15, "SUST channel mask");
    const ImageDim dim{ImageDimOf(t)};
    const u32 rb{t.insn.Reg32()};
    if (rb != RZ && rb + 4 > RZ) {
        t.Fail("SUST data registers");
    }
    const auto value{[&](u32 i) { return rb == RZ ? t.ir.Imm32(0) : t.R(rb + i); }};
    IR::TextureInstInfo info{};
    info.type.Assign(dim.type);
    info.image_format.Assign(ImageFormat::Typeless);
    t.ir.ImageWrite(Handle(t), Coords(t, dim), t.ir.CompositeConstruct(value(0), value(1), value(2), value(3)),
                    info);
}

/// SUATOM (with the old value to Rd) and SURED: U32 or S32 (73-75), ADD MIN MAX INC DEC AND OR XOR EXCH (87-90),
/// strong at GPU scope (77-80 = 7), as the image atomics are
void Atomic(Translator& t, bool returns) {
    ExpectBound(t);
    t.Expect(72, 1, 0, "surface atomic .BA");
    t.Expect(76, 1, 0, "surface atomic bit 76");
    t.Expect(77, 4, 7, "surface atomic memory order");
    t.Expect(81, 3, returns ? PT : 0, "surface atomic fault predicate");
    const u32 type{t.insn.Field(73, 3)};
    if (type > 1) {
        t.Fail("surface atomic type");
    }
    const bool is_signed{type == 1};
    const ImageDim dim{ImageDimOf(t)};
    IR::TextureInstInfo info{};
    info.type.Assign(dim.type);
    info.image_format.Assign(ImageFormat::R32_UINT);
    const IR::U32 handle{Handle(t)};
    const IR::Value coords{Coords(t, dim)};
    const IR::U32 data{t.R(t.insn.Reg32())};
    IR::Value old;
    switch (t.insn.Field(87, 4)) {
    case 0:
        old = t.ir.ImageAtomicIAdd(handle, coords, data, info);
        break;
    case 1:
        old = t.ir.ImageAtomicIMin(handle, coords, data, is_signed, info);
        break;
    case 2:
        old = t.ir.ImageAtomicIMax(handle, coords, data, is_signed, info);
        break;
    case 3:
        old = t.ir.ImageAtomicInc(handle, coords, data, info);
        break;
    case 4:
        old = t.ir.ImageAtomicDec(handle, coords, data, info);
        break;
    case 5:
        old = t.ir.ImageAtomicAnd(handle, coords, data, info);
        break;
    case 6:
        old = t.ir.ImageAtomicOr(handle, coords, data, info);
        break;
    case 7:
        old = t.ir.ImageAtomicXor(handle, coords, data, info);
        break;
    case 8:
        old = t.ir.ImageAtomicExchange(handle, coords, data, info);
        break;
    default:
        t.Fail("surface atomic operation");
    }
    if (returns) {
        t.SetR(t.insn.Dst(), IR::U32{old});
    }
}
} // Anonymous namespace

void TranslateSurface(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0xb97:
        return SULD(t);
    case 0xb9b:
        return SUST(t);
    case 0x593:
        return Atomic(t, true);
    case 0x59f:
        return Atomic(t, false);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
