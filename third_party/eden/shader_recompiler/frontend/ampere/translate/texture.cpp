// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's texture sampling: TEX with a descriptor in a constant buffer. The handle is that
// buffer word (GetCbuf), which the texture pass tracks to its descriptor.

#include <bit>

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
/// set_tex_dim(61..64)
struct Dimension {
    TextureType type;
    u32 coordinates; ///< the values of the coordinates, the array index first
    bool array;
    bool cube;
};

Dimension DimensionOf(Translator& t) {
    switch (t.insn.Field(61, 3)) {
    case 0:
        return {TextureType::Color1D, 1, false, false};
    case 1:
        return {TextureType::Color2D, 2, false, false};
    case 2:
        return {TextureType::Color3D, 3, false, false};
    case 3:
        return {TextureType::ColorCube, 3, false, true};
    case 4:
        return {TextureType::ColorArray1D, 2, true, false};
    case 5:
        return {TextureType::ColorArray2D, 3, true, false};
    case 7:
        return {TextureType::ColorArrayCube, 4, true, true};
    default:
        t.Fail("texture dimension");
    }
}

void TEX(Translator& t) {
    // Rd (and the second destination at 64 with the split form, 60), the channel mask 72-75, Ra and Rb the operands:
    // the coordinates (the array index first), then the LOD or bias, the packed offsets (76) and the depth reference
    // (78), up to four values as halves of Ra and Rb, five or more as four in Ra and the rest in Rb
    t.Expect(59, 1, 0, "TEX mode");
    t.Expect(77, 1, 0, "TEX.NDV");
    t.Expect(79, 1, 0, "TEX F16 results");
    t.Expect(81, 3, PT, "TEX fault predicate");
    const u32 lod_mode{t.insn.Field(87, 3)};
    if (lod_mode > 3) {
        t.Fail("TEX LOD mode");
    }
    const u32 mask{t.insn.Field(72, 4)};
    const bool offsets{t.insn.Bit(76)};
    const bool depth_compare{t.insn.Bit(78)};
    if (mask == 0 || (depth_compare && mask != 1)) {
        t.Fail("TEX channel mask");
    }
    const Dimension dim{DimensionOf(t)};
    if (offsets && dim.cube) {
        t.Fail("offsets on a cube sample");
    }
    const bool has_lod{lod_mode == 2 || lod_mode == 3};
    const u32 count{dim.coordinates + (has_lod ? 1 : 0) + (offsets ? 1 : 0) + (depth_compare ? 1 : 0)};
    const u32 in_a{count <= 4 ? (count + 1) / 2 : 4};
    const u32 ra{t.insn.Src0()}, rb{t.insn.Reg32()};
    const auto value{[&](u32 i) -> IR::U32 {
        if (i < in_a) {
            return ra == RZ ? t.ir.Imm32(0) : t.R(ra + i);
        }
        return rb == RZ ? t.ir.Imm32(0) : t.R(rb + (i - in_a));
    }};
    const auto fvalue{[&](u32 i) { return t.ir.BitCast<IR::F32>(value(i)); }};
    if ((ra != RZ && ra + in_a > RZ) || (rb != RZ && count > in_a && rb + (count - in_a) > RZ)) {
        t.Fail("texture operand registers");
    }
    IR::Value coords;
    const u32 first{dim.array ? 1U : 0U};
    const auto array_index{[&] { return t.ir.ConvertUToF(32, 16, value(0)); }};
    switch (dim.coordinates - first) {
    case 1:
        coords = dim.array ? t.ir.CompositeConstruct(fvalue(1), array_index()) : IR::Value{fvalue(0)};
        break;
    case 2:
        coords = dim.array ? t.ir.CompositeConstruct(fvalue(1), fvalue(2), array_index())
                           : t.ir.CompositeConstruct(fvalue(0), fvalue(1));
        break;
    default:
        coords = dim.array ? t.ir.CompositeConstruct(fvalue(1), fvalue(2), fvalue(3), array_index())
                           : t.ir.CompositeConstruct(fvalue(0), fvalue(1), fvalue(2));
        break;
    }
    u32 next{dim.coordinates};
    const IR::F32 lod{has_lod ? fvalue(next++) : t.ir.Imm32(0.0f)};
    IR::Value offset;
    if (offsets) {
        const IR::U32 packed{value(next++)};
        const auto component{[&](u32 at) {
            return t.ir.BitFieldExtract(packed, t.ir.Imm32(at), t.ir.Imm32(4), true);
        }};
        switch (dim.coordinates - first) {
        case 1:
            offset = component(0);
            break;
        case 2:
            offset = t.ir.CompositeConstruct(component(0), component(4));
            break;
        default:
            offset = t.ir.CompositeConstruct(component(0), component(4), component(8));
            break;
        }
    }
    IR::F32 dref;
    if (depth_compare) {
        dref = fvalue(next++);
    }
    const IR::U32 handle{t.ir.GetCbuf(t.ir.Imm32(t.insn.CbufBank()), t.ir.Imm32(t.insn.Field(40, 14) * 4))};
    IR::TextureInstInfo info{};
    info.type.Assign(dim.type);
    info.is_depth.Assign(depth_compare ? 1 : 0);
    info.has_bias.Assign(lod_mode == 2 ? 1 : 0);
    const bool explicit_lod{lod_mode == 1 || lod_mode == 3};
    IR::F32 lod_clamp;
    IR::Value sample;
    if (!depth_compare) {
        sample = explicit_lod ? t.ir.ImageSampleExplicitLod(handle, coords, lod, offset, info)
                              : t.ir.ImageSampleImplicitLod(handle, coords, lod, offset, lod_clamp, info);
    } else {
        sample = explicit_lod ? t.ir.ImageSampleDrefExplicitLod(handle, coords, dref, lod, offset, info)
                              : t.ir.ImageSampleDrefImplicitLod(handle, coords, dref, lod, offset, lod_clamp, info);
    }
    const u32 dst{t.insn.Dst()}, second{t.insn.Reg64()};
    const bool split{second != RZ};
    u32 written{0};
    for (u32 element = 0; element < 4; ++element) {
        if (((mask >> element) & 1) == 0) {
            continue;
        }
        const IR::F32 result{depth_compare ? IR::F32{sample}
                                           : IR::F32{t.ir.CompositeExtract(sample, element)}};
        const u32 target{!split || written < 2 ? (dst == RZ ? RZ : dst + written) : second + (written - 2)};
        t.SetF(target, result);
        ++written;
    }
}
} // Anonymous namespace

void TranslateTexture(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0xb60:
        return TEX(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
