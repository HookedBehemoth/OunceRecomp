// SPDX-FileCopyrightText: Copyright 2026 Hookshot contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// (hookshot, native-vk, RX_VK_DESC_HEAP_BDA=1 with layer 0) Storage buffers read through device addresses. Layer 0 shows guest
// memory to the GPU through 4 GB window buffers, one every 2 GB of guest space (gpu/vk/layer0_windows.h), so guest address g
// is at window[g >> 31] + (g & 0x7fffffff) whatever pool it is in. A stage's storage buffer is then its guest address, read
// from the constant buffer word Eden's storage buffer pass found for it, translated through a table of the windows' device
// addresses (gpu/vk/vk_bda.cpp; its address pushed at WINDOW_TABLE_PUSH_OFFSET), plus the offset the pass computed, which
// is relative to the address rounded down to min_ssbo_alignment (StorageOffset). Windows not made yet, and addresses from
// 1 TB on (entry kNullEntry), read the null window: a sparse buffer with nothing bound, where loads read 0 and stores and
// atomics are dropped (residencyNonResidentStrict; work/proto/bda/bda_poc.c). No descriptor, no binding, no CPU lookup.
//
// With RX_VK_BDA_CHECK=1 each access is also checked against its buffer's size (the constant buffer's size word, as the
// classic binding's length: vk_draw.cpp build_descriptors): out of range it goes to scratch words (a load reads 0, a store
// or an atomic is lost) and bda_flag records it in the check words after the table, which the renderer reads every present.
//
// Kept apart from EmitContext so that tests/test_bda_emit.py can build these on a bare Sirit::Module and run spirv-val.
#include <cstdint>
#include <map>
#include <sirit/sirit.h>

namespace Shader::Backend::SPIRV::Bda {

using Sirit::Id;

constexpr std::uint32_t kWindows = 512;                 // (the table's window entries: 1 TB of guest space)
constexpr std::uint32_t kNullEntry = kWindows;          // (the null window, for addresses from 1 TB on)
constexpr std::uint32_t kCheckOffset = 8 * 520;         // (bytes: the check words, after the table)
constexpr std::uint32_t kCheckCount = 0;                // (u32 words there: out-of-range accesses,
constexpr std::uint32_t kCheckFirst = 1;                //  the first program's id (compare-exchange from 0),
constexpr std::uint32_t kCheckLast = 2;                 //  the last one's,
constexpr std::uint32_t kCheckLastOffset = 3;           //  its byte offset
constexpr std::uint32_t kCheckLastBound = 4;            //  and its buffer's bound)
constexpr std::uint32_t kScratchOffset = kCheckOffset + 64;   // (64 bytes the clamped accesses go to)
constexpr std::uint32_t kTableBytes = kScratchOffset + 64;

/// Why a stage's storage buffers stay on descriptors (Info::bda_why, BdaStage in spirv_emit_context.cpp): one bit for each
/// condition the stage meets, so that a log names all of them. kWideAllowed: the ones RX_VK_DESC_HEAP_BDA=2 translates
enum Why : std::uint32_t {
    kWhyMeshFallback = 1u << 0,    // (the compute fallback's task and mesh stages, and its generated vertex stage)
    kWhyRenderArea = 1u << 1,      // (the render area's push block; =2: the window table's words in that block)
    kWhyRenderAreaScale = 1u << 2, // (and the rescaling words, which share its offsets)
    kWhyGlobalOff = 1u << 3,       // (global memory with RX_VK_BDA_GLOBAL=0)
    kWhyNativeBounds = 1u << 4,    // (Info::native_global_bounds: the candidates' search; =2: the accesses translated instead)
    kWhyMeshFirst = 1u << 5,       // (the task stage under RX_VK_MESH_FIRST: its own block)
    kWhyCasLoop = 1u << 6,         // (inc/dec, f32/f16x2/f32x2 atomics: CAS loops; =2: the loops on a device address)
    kWhyNoU32View = 1u << 7,       // (no constant buffer read as u32 words; =2: the view declared)
    kWhyTable = 1u << 8,           // (a storage table, count > 1: the global tables, c0+0x880; =2: no base, the accesses translated)
    kWhyMissLog = 1u << 9,         // (RX_VK_MISS_LOG's buffer at c0+0xff0)
    kWhyUnaligned = 1u << 10,      // (an address word not on a u32 boundary)
    kWhyUndeclared = 1u << 11,     // (its address in a constant buffer the stage does not declare)
    kWhyCaps = 1u << 12,           // (the device lacks int64/8/16-bit storage or descriptor aliasing)
};
constexpr std::uint32_t kWhyBits = 13;
constexpr std::uint32_t kWideAllowed = kWhyRenderArea | kWhyNativeBounds | kWhyCasLoop | kWhyNoU32View | kWhyTable;
inline const char* WhyName(std::uint32_t bit) {
    static const char* const names[kWhyBits] = {"mesh fallback", "render area",     "render area with rescaling",
                                                "global off",    "native bounds",   "mesh first",
                                                "CAS loop",      "no u32 cbuf view", "table (count > 1)",
                                                "miss log",      "unaligned word",  "cbuf not declared",
                                                "device caps"};
    return bit < kWhyBits ? names[bit] : "?";
}

/// The basic types the helpers use, as the module defines them
struct Types {
    Id u1, u32, u64, void_t;
};

/// Pointer types to a value of one type at a device address: a Block struct of the value (as glslang makes buffer
/// references; NVIDIA's compiler crashed on pointers to bare scalars, spirv_emit_context.cpp psb_load), and its member
struct PtrTypes {
    Id block{}, member{};
};

/// Made once a type (Sirit merges equal declarations: a second Block decoration of the same struct would be a duplicate)
class PtrCache {
public:
    PtrTypes Get(Sirit::Module& m, Id type) {
        auto it{cache.find(type.value)};
        if (it == cache.end()) {
            const Id block{m.TypeStruct(type)};
            m.Decorate(block, spv::Decoration::Block);
            m.MemberDecorate(block, 0, spv::Decoration::Offset, 0U);
            it = cache.emplace(type.value, PtrTypes{m.TypePointer(spv::StorageClass::PhysicalStorageBuffer, block),
                                                    m.TypePointer(spv::StorageClass::PhysicalStorageBuffer, type)})
                     .first;
        }
        return it->second;
    }

private:
    std::map<std::uint32_t, PtrTypes> cache;
};

/// The pointer to the `type` at device address `address`
inline Id PointerAt(Sirit::Module& m, const Types& t, PtrTypes p, Id address) {
    return m.OpAccessChain(p.member, m.OpConvertUToPtr(p.block, address), m.Constant(t.u32, 0U));
}

/// What a draw pushes (WINDOW_TABLE_PUSH_OFFSET): the window table's address, and the draw's one virtual range [lo, hi)
/// (a virtual pool's addresses, from 1 TB, which no window holds) with the distance from it to its memory's device address
/// (vk_draw.cpp bda_virtual; lo = hi = 0: none)
constexpr std::uint32_t kPushedWords = 4;
struct Pushed {
    Id table{}, lo{}, hi{}, delta{};
};

/// Guest address `guest` (u64) as a device address: the window table's entry plus the offset in the window, rounded down to
/// `alignment` (Eden's storage offsets are from the address so rounded: StorageOffset); in the draw's virtual range, the
/// address plus its distance; from 1 TB otherwise the null window
inline Id Translate(Sirit::Module& m, const Types& t, PtrCache& ptrs, const Pushed& p, Id guest, std::uint64_t alignment) {
    const Id k{m.OpShiftRightLogical(t.u64, guest, m.Constant(t.u32, 31U))};
    const Id in_table{m.OpULessThan(t.u1, k, m.Constant(t.u64, std::uint64_t{kWindows}))};
    const Id entry_index{m.OpSelect(t.u64, in_table, k, m.Constant(t.u64, std::uint64_t{kNullEntry}))};
    const Id slot{m.OpIAdd(t.u64, p.table, m.OpShiftLeftLogical(t.u64, entry_index, m.Constant(t.u32, 3U)))};
    const Id window{m.OpLoad(t.u64, PointerAt(m, t, ptrs.Get(m, t.u64), slot), spv::MemoryAccessMask::Aligned, 8U)};
    const std::uint64_t mask{0x7fffffffULL & ~(alignment - 1)};
    const Id in_window{m.OpIAdd(t.u64, window, m.OpBitwiseAnd(t.u64, guest, m.Constant(t.u64, mask)))};
    if (!Sirit::ValidId(p.delta)) {
        return in_window;
    }
    const Id aligned{m.OpBitwiseAnd(t.u64, guest, m.Constant(t.u64, ~(alignment - 1)))};
    const Id in_virtual{m.OpLogicalAnd(t.u1, m.OpUGreaterThanEqual(t.u1, guest, p.lo), m.OpULessThan(t.u1, guest, p.hi))};
    return m.OpSelect(t.u64, in_virtual, m.OpIAdd(t.u64, aligned, p.delta), in_window);
}

/// A storage buffer's device address from its guest address (as Translate, rounded down to min_ssbo_alignment)
inline Id Base(Sirit::Module& m, const Types& t, PtrCache& ptrs, const Pushed& p, Id guest, std::uint64_t alignment) {
    return Translate(m, t, ptrs, p, guest, alignment);
}

/// (RX_VK_BDA_CHECK) a buffer's bound in bytes from its aligned base, as the classic binding's length: its size word plus
/// the address's distance from the aligned base, rounded up to 4, at least 16
inline Id Bound(Sirit::Module& m, const Types& t, Id size_word, Id guest, std::uint64_t alignment) {
    const Id misalign{m.OpBitwiseAnd(t.u32, m.OpUConvert(t.u32, guest), m.Constant(t.u32, std::uint32_t(alignment - 1)))};
    const Id n{m.OpBitwiseAnd(t.u32, m.OpIAdd(t.u32, m.OpIAdd(t.u32, size_word, misalign), m.Constant(t.u32, 3U)),
                              m.Constant(t.u32, ~3U))};
    return m.OpSelect(t.u32, m.OpULessThan(t.u1, n, m.Constant(t.u32, 16U)), m.Constant(t.u32, 16U), n);
}

/// (RX_VK_BDA_CHECK) whether an access of `size` bytes at byte offset `offset` (u32) lies within `bound`
inline Id Inside(Sirit::Module& m, const Types& t, Id offset, std::uint32_t size, Id bound) {
    const Id sz{m.Constant(t.u32, size)};
    return m.OpLogicalAnd(t.u1, m.OpUGreaterThanEqual(t.u1, bound, sz),
                          m.OpULessThanEqual(t.u1, offset, m.OpISub(t.u32, bound, sz)));
}

/// (RX_VK_BDA_CHECK) void bda_flag(bool out, u64 table, u32 offset, u32 bound): an out-of-range access recorded in the check
/// words (count, first and last program id, the last one's offset and bound). A function of its own, so that the branch
/// does not split the caller's block (Eden's phis name the IR blocks' labels as their predecessors). Defined before main
inline Id DefineFlag(Sirit::Module& m, const Types& t, PtrCache& ptrs, std::uint32_t id) {
    const Id fn_type{m.TypeFunction(t.void_t, t.u1, t.u64, t.u32, t.u32)};
    const Id fn{m.OpFunction(t.void_t, spv::FunctionControlMask::DontInline, fn_type)};
    const Id out{m.OpFunctionParameter(t.u1)};
    const Id table{m.OpFunctionParameter(t.u64)};
    const Id offset{m.OpFunctionParameter(t.u32)};
    const Id bound{m.OpFunctionParameter(t.u32)};
    m.AddLabel();
    const Id then_label{m.OpLabel()};
    const Id end_label{m.OpLabel()};
    m.OpSelectionMerge(end_label, spv::SelectionControlMask::MaskNone);
    m.OpBranchConditional(out, then_label, end_label);
    m.AddLabel(then_label);
    const PtrTypes p{ptrs.Get(m, t.u32)};
    const auto word{[&](std::uint32_t w) {
        return PointerAt(m, t, p, m.OpIAdd(t.u64, table, m.Constant(t.u64, std::uint64_t{kCheckOffset + 4 * w})));
    }};
    const Id scope{m.Constant(t.u32, static_cast<std::uint32_t>(spv::Scope::Device))};
    const Id relaxed{m.Constant(t.u32, 0U)};
    const Id id_value{m.Constant(t.u32, id)};
    m.OpAtomicIAdd(t.u32, word(kCheckCount), scope, relaxed, m.Constant(t.u32, 1U));
    m.OpAtomicCompareExchange(t.u32, word(kCheckFirst), scope, relaxed, relaxed, id_value, m.Constant(t.u32, 0U));
    m.OpAtomicExchange(t.u32, word(kCheckLast), scope, relaxed, id_value);
    m.OpStore(word(kCheckLastOffset), offset, spv::MemoryAccessMask::Aligned, 4U);
    m.OpStore(word(kCheckLastBound), bound, spv::MemoryAccessMask::Aligned, 4U);
    m.OpBranch(end_label);
    m.AddLabel(end_label);
    m.OpReturn();
    m.OpFunctionEnd();
    return fn;
}

/// One access: its pointer, and in check mode whether it was in range (else Id{}) and the alignment its loads and stores
/// state (PhysicalStorageBuffer accesses must)
struct Access {
    Id pointer{};
    Id inside{};
    std::uint32_t align{};
    Id address{};   // (the u64 the pointer was made of: the CAS loops take it, DefineCasLoop)
};

/// The access of a `type` value of `size` bytes at byte `offset` (u32) of the buffer whose device address is `base`.
/// Check mode (bound valid): out of range, the scratch words instead, and bda_flag called
inline Access Make(Sirit::Module& m, const Types& t, PtrCache& ptrs, Id type, std::uint32_t size, std::uint32_t align, Id base,
                   Id offset, Id table = {}, Id bound = {}, Id flag = {}) {
    const PtrTypes p{ptrs.Get(m, type)};
    Id address{m.OpIAdd(t.u64, base, m.OpUConvert(t.u64, offset))};
    Access a{};
    a.align = align;
    if (Sirit::ValidId(bound)) {
        a.inside = Inside(m, t, offset, size, bound);
        const Id scratch{m.OpIAdd(t.u64, table, m.Constant(t.u64, std::uint64_t{kScratchOffset}))};
        address = m.OpSelect(t.u64, a.inside, address, scratch);
        m.OpFunctionCall(t.void_t, flag, m.OpLogicalNot(t.u1, a.inside), table, offset, bound);
    }
    a.pointer = PointerAt(m, t, p, address);
    a.address = address;
    return a;
}

/// A load through an access (0 when it was out of range)
inline Id Load(Sirit::Module& m, const Access& a, Id type) {
    const Id v{m.OpLoad(type, a.pointer, spv::MemoryAccessMask::Aligned, a.align)};
    return Sirit::ValidId(a.inside) ? m.OpSelect(type, a.inside, v, m.ConstantNull(type)) : v;
}

/// A store through an access (to the scratch words when it was out of range)
inline void Store(Sirit::Module& m, const Access& a, Id value) {
    m.OpStore(a.pointer, value, spv::MemoryAccessMask::Aligned, a.align);
}

/// An atomic's or another read's result through an access: 0 when it was out of range
inline Id Result(Sirit::Module& m, const Access& a, Id type, Id value) {
    return Sirit::ValidId(a.inside) ? m.OpSelect(type, a.inside, value, m.ConstantNull(type)) : value;
}

/// (RX_VK_DESC_HEAP_BDA=2) value_type f(u64 address, value_type b): Eden's storage CAS loop (spirv_emit_context.cpp CasLoop) on
/// the u32 word at a device address: the word read, op(word as value_type, b) compare-exchanged in until it holds or `max_tries`
/// attempts were made, the old value returned. `op` is value_type op(value_type, value_type) (CasFunction). The word holds a
/// u32, an f32 or f16x2 (bitcast), or an f32x2 packed as two halves (`packed_half`, needs GLSL.std.450). Defined before main
inline Id DefineCasLoop(Sirit::Module& m, const Types& t, PtrCache& ptrs, Id value_type, Id op, bool packed_half,
                        std::uint32_t max_tries) {
    const bool is_u32{value_type.value == t.u32.value};
    const Id zero{m.Constant(t.u32, 0U)};
    const Id scope{m.Constant(t.u32, static_cast<std::uint32_t>(spv::Scope::Device))};
    const Id fn{m.OpFunction(value_type, spv::FunctionControlMask::MaskNone, m.TypeFunction(value_type, t.u64, value_type))};
    const Id address{m.OpFunctionParameter(t.u64)};
    const Id op_b{m.OpFunctionParameter(value_type)};
    m.AddLabel();
    const Id fn_u32{m.TypePointer(spv::StorageClass::Function, t.u32)};
    // (the function's variables before any other instruction: the expected word, the attempts)
    const Id expected_var{m.AddLocalVariable(fn_u32, spv::StorageClass::Function)};
    const Id tries_var{m.AddLocalVariable(fn_u32, spv::StorageClass::Function)};
    m.OpStore(tries_var, zero);
    const Id word_pointer{PointerAt(m, t, ptrs.Get(m, t.u32), address)};
    m.OpStore(expected_var, m.OpLoad(t.u32, word_pointer, spv::MemoryAccessMask::Aligned, 4U));
    const Id loop_header{m.OpLabel()};
    const Id continue_block{m.OpLabel()};
    const Id merge_block{m.OpLabel()};
    m.OpBranch(loop_header);
    m.AddLabel(loop_header);
    m.OpLoopMerge(merge_block, continue_block, spv::LoopControlMask::MaskNone);
    m.OpBranch(continue_block);
    m.AddLabel(continue_block);
    const auto from_word{[&](Id word) {
        if (packed_half) {
            return m.OpUnpackHalf2x16(value_type, word);
        }
        return is_u32 ? word : m.OpBitcast(value_type, word);
    }};
    const Id expected{m.OpLoad(t.u32, expected_var)};
    const Id new_value{m.OpFunctionCall(value_type, op, from_word(expected), op_b)};
    const Id new_word{packed_half ? m.OpPackHalf2x16(t.u32, new_value) : is_u32 ? new_value : m.OpBitcast(t.u32, new_value)};
    const Id result{m.OpAtomicCompareExchange(t.u32, word_pointer, scope, zero, zero, new_word, expected)};
    m.OpStore(expected_var, result);
    const Id success{m.OpIEqual(t.u1, result, expected)};
    const Id tries{m.OpIAdd(t.u32, m.OpLoad(t.u32, tries_var), m.Constant(t.u32, 1U))};
    m.OpStore(tries_var, tries);
    const Id done{m.OpLogicalOr(t.u1, success, m.OpUGreaterThanEqual(t.u1, tries, m.Constant(t.u32, max_tries)))};
    m.OpBranchConditional(done, merge_block, loop_header);
    m.AddLabel(merge_block);
    m.OpReturnValue(from_word(result));
    m.OpFunctionEnd();
    return fn;
}

} // namespace Shader::Backend::SPIRV::Bda
