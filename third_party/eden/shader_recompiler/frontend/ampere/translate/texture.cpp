// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding reference: Mesa NAK sm70_encode.rs, nak_nir_lower_tex.c (MIT), Copyright 2022 Collabora, Ltd.

// (fork) The direct Ampere front's texture instructions: TEX (bound, uniform-register and bindless handles), TLD,
// TLD4, TMML, TXD and TXQ. A handle is the constant buffer word (GetCbuf), a uniform register or a GPR, which the
// texture pass tracks to its descriptor. The operands follow NAK's layout: the first list (the array index, then the
// coordinates) in Ra, the second (a bindless handle, the LOD or bias, offsets, the depth reference, the sample) in Rb;
// the scalar form (bit 60) packs both lists, at most four values, as halves of Ra and Rb.

#include <bit>

#include "shader_recompiler/frontend/ampere/translate/translator.h"

namespace Shader::Ampere {
namespace {
/// set_tex_dim(61..64)
struct Dimension {
    TextureType type;
    u32 coordinates; ///< without the array index
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
        return {TextureType::ColorArray1D, 1, true, false};
    case 5:
        return {TextureType::ColorArray2D, 2, true, false};
    case 7:
        return {TextureType::ColorArrayCube, 3, true, true};
    default:
        t.Fail("texture dimension");
    }
}

/// The operand values of a texture instruction: the first list's in Ra, the second's in Rb, or both as halves when
/// scalar
class Operands {
public:
    Operands(Translator& t_, u32 first_, u32 second_, bool scalar) : t{t_}, first{first_}, second{second_} {
        const u32 count{first + second};
        if (scalar && count > 4) {
            t.Fail("more than four operands in the scalar form");
        }
        in_a = scalar ? (count + 1) / 2 : first;
        ra = t.insn.Src0();
        rb = t.insn.Reg32();
        if ((ra != RZ && in_a != 0 && ra + in_a > RZ) || (rb != RZ && count > in_a && rb + (count - in_a) > RZ)) {
            t.Fail("texture operand registers");
        }
    }

    /// The i-th value of the two lists together
    IR::U32 operator[](u32 i) const {
        if (i < in_a) {
            return ra == RZ ? t.ir.Imm32(0) : t.R(ra + i);
        }
        return rb == RZ ? t.ir.Imm32(0) : t.R(rb + (i - in_a));
    }
    IR::F32 F(u32 i) const {
        return t.ir.BitCast<IR::F32>((*this)[i]);
    }

    u32 first;
    u32 second;

private:
    Translator& t;
    u32 in_a{};
    u32 ra{};
    u32 rb{};
};

/// The coordinates of a sample (floats; the array index an integer, its low 16 bits, as the last coordinate)
IR::Value SampleCoords(Translator& t, const Operands& op, const Dimension& dim) {
    const u32 base{dim.array ? 1U : 0U};
    if (!dim.array) {
        switch (dim.coordinates) {
        case 1:
            return op.F(0);
        case 2:
            return t.ir.CompositeConstruct(op.F(0), op.F(1));
        default:
            return t.ir.CompositeConstruct(op.F(0), op.F(1), op.F(2));
        }
    }
    const IR::F32 layer{t.ir.ConvertUToF(32, 16, op[0])};
    switch (dim.coordinates) {
    case 1:
        return t.ir.CompositeConstruct(op.F(base), layer);
    case 2:
        return t.ir.CompositeConstruct(op.F(base), op.F(base + 1), layer);
    default:
        return t.ir.CompositeConstruct(op.F(base), op.F(base + 1), op.F(base + 2), layer);
    }
}

/// The coordinates of a texel fetch (integers; the array index's low 16 bits last)
IR::Value FetchCoords(Translator& t, const Operands& op, const Dimension& dim) {
    const u32 base{dim.array ? 1U : 0U};
    if (!dim.array) {
        switch (dim.coordinates) {
        case 1:
            return op[0];
        case 2:
            return t.ir.CompositeConstruct(op[0], op[1]);
        default:
            return t.ir.CompositeConstruct(op[0], op[1], op[2]);
        }
    }
    const IR::U32 layer{t.ir.BitFieldExtract(op[0], t.ir.Imm32(0), t.ir.Imm32(16))};
    switch (dim.coordinates) {
    case 1:
        return t.ir.CompositeConstruct(op[base], layer);
    case 2:
        return t.ir.CompositeConstruct(op[base], op[base + 1], layer);
    default:
        return t.ir.CompositeConstruct(op[base], op[base + 1], op[base + 2], layer);
    }
}

/// Packed offsets: one signed field of `bits` per coordinate, every `stride` bits (TEX, TLD, TXD: 4 at 4; TLD4: 6 at 8)
IR::Value Offsets(Translator& t, const IR::U32& packed, u32 coordinates, u32 bits, u32 stride) {
    const auto component{[&](u32 i) {
        return t.ir.BitFieldExtract(packed, t.ir.Imm32(i * stride), t.ir.Imm32(bits), true);
    }};
    switch (coordinates) {
    case 1:
        return component(0);
    case 2:
        return t.ir.CompositeConstruct(component(0), component(1));
    default:
        return t.ir.CompositeConstruct(component(0), component(1), component(2));
    }
}

/// The handle: the constant buffer word (40-58), a uniform register (40-45 with bit 91) or, bindless (.B), none here
enum class HandleForm { Cbuf, Uniform, Register };

IR::U32 CbufHandle(Translator& t) {
    return t.ir.GetCbuf(t.ir.Imm32(t.insn.CbufBank()), t.ir.Imm32(t.insn.Field(40, 14) * 4));
}

IR::U32 UniformHandle(Translator& t) {
    t.Expect(46, 13, 0, "uniform texture handle field");
    return t.UR(t.insn.Field(40, 6));
}

/// Bits 79-80: F32 results (0) or F16 pairs (1)
bool HalfResults(Translator& t) {
    const u32 precision{t.insn.Field(79, 2)};
    if (precision > 1) {
        t.Fail("texture result precision");
    }
    return precision == 1;
}

/// Writes the enabled components (mask 72-75) of a result: the first two to Rd and Rd+1, the others to the second
/// destination (64-71) or, when that is RZ, on from Rd+2. F16 results go as pairs, (c0, c1) in Rd, (c2, c3) in the
/// second destination (or Rd+1), the low half first.
template <typename Component>
void WriteResults(Translator& t, u32 mask, bool half, Component&& component) {
    const u32 dst{t.insn.Dst()}, second{t.insn.Reg64()};
    const bool split{second != RZ};
    const auto target{[&](u32 n) -> u32 {
        if (!split || n < 2) {
            return dst == RZ ? RZ : dst + n;
        }
        return second + (n - 2);
    }};
    u32 written{0};
    std::array<IR::F32, 4> values;
    for (u32 element = 0; element < 4; ++element) {
        if (((mask >> element) & 1) != 0) {
            values[written++] = component(element);
        }
    }
    if (!half) {
        for (u32 n = 0; n < written; ++n) {
            t.SetF(target(n), values[n]);
        }
        return;
    }
    for (u32 pair = 0; pair * 2 < written; ++pair) {
        const IR::F32 high{pair * 2 + 1 < written ? values[pair * 2 + 1] : t.ir.Imm32(0.0f)};
        const u32 reg{pair == 0 ? (dst == RZ ? RZ : dst) : split ? second : (dst == RZ ? RZ : dst + 1)};
        t.SetR(reg, t.ir.PackHalf2x16(t.ir.CompositeConstruct(values[pair * 2], high)));
    }
}

void ExpectNoFault(Translator& t) {
    t.Expect(81, 3, PT, "texture fault predicate");
}

void TEX(Translator& t, HandleForm form) {
    t.Expect(59, 1, form == HandleForm::Register ? 1 : 0, "TEX .B");
    if (form == HandleForm::Uniform) {
        t.Expect(91, 1, 1, "TEX uniform handle");
    }
    ExpectNoFault(t);
    const u32 lod_mode{t.insn.Field(87, 3)};
    if (lod_mode > 3) {
        t.Fail("TEX LOD mode");
    }
    const u32 mask{t.insn.Field(72, 4)};
    const bool offsets{t.insn.Bit(76)};
    const bool depth_compare{t.insn.Bit(78)};
    const bool half{HalfResults(t)};
    if (mask == 0 || (depth_compare && mask != 1)) {
        t.Fail("TEX channel mask");
    }
    const Dimension dim{DimensionOf(t)};
    if (offsets && dim.cube) {
        t.Fail("offsets on a cube sample");
    }
    const bool has_lod{lod_mode == 2 || lod_mode == 3};
    const bool bindless{form == HandleForm::Register};
    const u32 first{dim.coordinates + (dim.array ? 1U : 0U)};
    const u32 second{(bindless ? 1U : 0U) + (has_lod ? 1U : 0U) + (offsets ? 1U : 0U) + (depth_compare ? 1U : 0U)};
    const Operands op{t, first, second, t.insn.Bit(60)};
    const IR::Value coords{SampleCoords(t, op, dim)};
    u32 next{first};
    const IR::U32 handle{form == HandleForm::Cbuf ? CbufHandle(t) : form == HandleForm::Uniform ? UniformHandle(t)
                                                                                                : op[next++]};
    // .LZ samples level 0, .LL the given level, .LB biases the implicit level; with no mode there is no bias
    IR::F32 lod;
    if (has_lod) {
        lod = op.F(next++);
    } else if (lod_mode == 1) {
        lod = t.ir.Imm32(0.0f);
    }
    IR::Value offset;
    if (offsets) {
        offset = Offsets(t, op[next++], dim.coordinates, 4, 4);
    }
    IR::F32 dref;
    if (depth_compare) {
        dref = op.F(next++);
    }
    IR::TextureInstInfo info{};
    info.type.Assign(dim.type);
    info.is_depth.Assign(depth_compare ? 1 : 0);
    info.has_bias.Assign(lod_mode == 2 ? 1 : 0);
    info.ndv_is_active.Assign(t.insn.Bit(77) ? 1 : 0);
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
    WriteResults(t, mask, half, [&](u32 element) {
        return depth_compare ? IR::F32{sample} : IR::F32{t.ir.CompositeExtract(sample, element)};
    });
}

/// TLD: texel fetch at level 0 (.LZ) or a given level (.LL), with offsets (76) or a sample index (78, .MS)
void TLD(Translator& t) {
    t.Expect(59, 1, 0, "TLD .B");
    t.Expect(77, 1, 0, "TLD .CL");
    ExpectNoFault(t);
    const u32 lod_mode{t.insn.Field(87, 3)};
    if (lod_mode != 1 && lod_mode != 3) {
        t.Fail("TLD LOD mode");
    }
    const u32 mask{t.insn.Field(72, 4)};
    if (mask == 0) {
        t.Fail("TLD channel mask");
    }
    const bool offsets{t.insn.Bit(76)};
    const bool multisample{t.insn.Bit(78)};
    const bool half{HalfResults(t)};
    const Dimension dim{DimensionOf(t)};
    if (dim.cube || (multisample && lod_mode != 1)) {
        t.Fail("TLD dimension or mode");
    }
    const bool has_lod{lod_mode == 3};
    const u32 first{dim.coordinates + (dim.array ? 1U : 0U)};
    const u32 second{(has_lod ? 1U : 0U) + (offsets ? 1U : 0U) + (multisample ? 1U : 0U)};
    const Operands op{t, first, second, t.insn.Bit(60)};
    const IR::Value coords{FetchCoords(t, op, dim)};
    u32 next{first};
    const IR::U32 lod{has_lod ? op[next++] : t.ir.Imm32(0U)};
    IR::Value offset;
    if (offsets) {
        offset = Offsets(t, op[next++], dim.coordinates, 4, 4);
    }
    IR::U32 sample_index;
    if (multisample) {
        sample_index = op[next++];
    }
    IR::TextureInstInfo info{};
    info.type.Assign(dim.type);
    const IR::Value texel{t.ir.ImageFetch(CbufHandle(t), coords, offset, lod, sample_index, info)};
    WriteResults(t, mask, half, [&](u32 element) { return IR::F32{t.ir.CompositeExtract(texel, element)}; });
}

/// TLD4: gather of one component (87-88), with one offset (76-77 = 1), four (2, PTP) or a depth reference (78)
void TLD4(Translator& t) {
    t.Expect(59, 1, 0, "TLD4 .B");
    t.Expect(89, 1, 0, "TLD4 bit 89");
    ExpectNoFault(t);
    const u32 mask{t.insn.Field(72, 4)};
    const u32 offset_mode{t.insn.Field(76, 2)};
    const bool depth_compare{t.insn.Bit(78)};
    const bool half{HalfResults(t)};
    if (mask == 0 || offset_mode == 3) {
        t.Fail("TLD4 channel mask or offset mode");
    }
    const Dimension dim{DimensionOf(t)};
    if (dim.coordinates != 2 && !dim.cube) {
        t.Fail("TLD4 dimension");
    }
    if (offset_mode != 0 && dim.cube) {
        t.Fail("offsets on a cube gather");
    }
    const u32 first{dim.coordinates + (dim.array ? 1U : 0U)};
    const u32 second{offset_mode + (depth_compare ? 1U : 0U)};
    const Operands op{t, first, second, t.insn.Bit(60)};
    const IR::Value coords{SampleCoords(t, op, dim)};
    u32 next{first};
    IR::Value offset, offset2;
    const auto ptp{[&](const IR::U32& packed) {
        const auto field{[&](u32 at) {
            return t.ir.BitFieldExtract(packed, t.ir.Imm32(at), t.ir.Imm32(6), true);
        }};
        return t.ir.CompositeConstruct(field(0), field(8), field(16), field(24));
    }};
    if (offset_mode == 1) {
        offset = Offsets(t, op[next++], dim.coordinates, 6, 8);
    } else if (offset_mode == 2) {
        offset = ptp(op[next++]);
        offset2 = ptp(op[next++]);
    }
    IR::F32 dref;
    if (depth_compare) {
        dref = op.F(next++);
    }
    IR::TextureInstInfo info{};
    info.type.Assign(dim.type);
    info.is_depth.Assign(depth_compare ? 1 : 0);
    info.gather_component.Assign(t.insn.Field(87, 2));
    const IR::U32 handle{CbufHandle(t)};
    const IR::Value texels{depth_compare ? t.ir.ImageGatherDref(handle, coords, offset, offset2, dref, info)
                                         : t.ir.ImageGather(handle, coords, offset, offset2, info)};
    WriteResults(t, mask, half, [&](u32 element) { return IR::F32{t.ir.CompositeExtract(texels, element)}; });
}

/// TMML: the LOD a sample would take, as 8.8 fixed point: channel 0 the computed LOD (signed), 1 the level accessed
void TMML(Translator& t) {
    t.Expect(59, 2, 0, "TMML .B or scalar form");
    t.Expect(78, 12, 0, "TMML mode");
    t.Expect(76, 1, 0, "TMML offsets");
    const u32 mask{t.insn.Field(72, 4)};
    if (mask == 0 || (mask & 0b1100) != 0) {
        t.Fail("TMML channel mask");
    }
    const Dimension dim{DimensionOf(t)};
    // the array index the hardware wants is in the first list (NAK passes 0); the query has no use for it
    const u32 first{dim.coordinates + (dim.array ? 1U : 0U)};
    const Operands op{t, first, 0, false};
    const u32 base{dim.array ? 1U : 0U};
    IR::Value coords;
    switch (dim.coordinates) {
    case 1:
        coords = op.F(base);
        break;
    case 2:
        coords = t.ir.CompositeConstruct(op.F(base), op.F(base + 1));
        break;
    default:
        coords = t.ir.CompositeConstruct(op.F(base), op.F(base + 1), op.F(base + 2));
        break;
    }
    IR::TextureInstInfo info{};
    info.type.Assign(dim.type);
    info.ndv_is_active.Assign(t.insn.Bit(77) ? 1 : 0);
    const IR::Value lod{t.ir.ImageQueryLod(CbufHandle(t), coords, info)};
    const u32 dst{t.insn.Dst()};
    if (t.insn.Reg64() != RZ) {
        t.Fail("TMML second destination");
    }
    u32 written{0};
    for (u32 element = 0; element < 2; ++element) {
        if (((mask >> element) & 1) == 0) {
            continue;
        }
        const IR::F32 value{t.ir.CompositeExtract(lod, element ^ 1)};
        const IR::F32 scaled{t.ir.FPMul(value, t.ir.Imm32(256.0f))};
        const IR::U32 fixed{element == 0 ? t.ir.ConvertFToS(32, scaled) : t.ir.ConvertFToU(32, scaled)};
        t.SetR(dst == RZ ? RZ : dst + written, t.ir.BitwiseAnd(fixed, t.ir.Imm32(0xffff)));
        ++written;
    }
}

/// TXD: a sample with explicit gradients: Ra the coordinates then the array index, Rb (dx, dy) per coordinate
void TXD(Translator& t) {
    t.Expect(59, 2, 0, "TXD .B or scalar form");
    t.Expect(76, 5, 0, "TXD offsets or mode");
    t.Expect(87, 3, 0, "TXD bits 87-89");
    ExpectNoFault(t);
    const u32 mask{t.insn.Field(72, 4)};
    if (mask == 0) {
        t.Fail("TXD channel mask");
    }
    const Dimension dim{DimensionOf(t)};
    if (dim.coordinates > 2 || dim.cube) {
        t.Fail("TXD dimension");
    }
    const u32 first{dim.coordinates + (dim.array ? 1U : 0U)};
    const Operands op{t, first, dim.coordinates * 2, false};
    IR::Value coords;
    if (dim.coordinates == 1) {
        coords = dim.array ? t.ir.CompositeConstruct(op.F(0), t.ir.ConvertUToF(32, 16, op[1])) : IR::Value{op.F(0)};
    } else {
        coords = dim.array ? t.ir.CompositeConstruct(op.F(0), op.F(1), t.ir.ConvertUToF(32, 16, op[2]))
                           : t.ir.CompositeConstruct(op.F(0), op.F(1));
    }
    const IR::Value derivatives{dim.coordinates == 1
                                    ? t.ir.CompositeConstruct(op.F(first), op.F(first + 1))
                                    : t.ir.CompositeConstruct(op.F(first), op.F(first + 1), op.F(first + 2),
                                                              op.F(first + 3))};
    IR::TextureInstInfo info{};
    info.type.Assign(dim.type);
    info.num_derivatives.Assign(dim.coordinates);
    IR::F32 lod_clamp;
    const IR::Value sample{t.ir.ImageGradient(CbufHandle(t), coords, derivatives, {}, lod_clamp, info)};
    WriteResults(t, mask, false, [&](u32 element) { return IR::F32{t.ir.CompositeExtract(sample, element)}; });
}

/// TXQ: the dimension query (62-63 = 0): width, height, depth or layers, levels of the level in Ra
void TXQ(Translator& t) {
    t.Expect(59, 5, 0, "TXQ query");
    t.Expect(76, 15, 0, "TXQ mode");
    const u32 mask{t.insn.Field(72, 4)};
    if (mask == 0) {
        t.Fail("TXQ channel mask");
    }
    const u32 ra{t.insn.Src0()};
    const IR::U32 level{t.R(ra)};
    const IR::Value query{t.ir.ImageQueryDimension(CbufHandle(t), level, t.ir.Imm1((mask & 8) == 0))};
    const u32 dst{t.insn.Dst()}, second{t.insn.Reg64()};
    u32 written{0};
    for (u32 element = 0; element < 4; ++element) {
        if (((mask >> element) & 1) == 0) {
            continue;
        }
        const u32 target{second == RZ || written < 2 ? (dst == RZ ? RZ : dst + written) : second + (written - 2)};
        t.SetR(target, IR::U32{t.ir.CompositeExtract(query, element)});
        ++written;
    }
}
} // Anonymous namespace

void TranslateTexture(Translator& t) {
    switch (t.insn.Opcode()) {
    case 0xb60:
        return TEX(t, HandleForm::Cbuf);
    case 0xf60:
        return TEX(t, HandleForm::Uniform);
    case 0x361:
        return TEX(t, HandleForm::Register);
    case 0xb66:
        return TLD(t);
    case 0xb63:
        return TLD4(t);
    case 0xb69:
        return TMML(t);
    case 0xb6c:
        return TXD(t);
    case 0xb6f:
        return TXQ(t);
    default:
        t.Fail("not translated");
    }
}

} // namespace Shader::Ampere
