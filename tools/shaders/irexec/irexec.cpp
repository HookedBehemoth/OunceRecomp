// The interpreter (irexec.h). The program is compiled once into slots (one per value: 4 components of 32 lanes, an
// immediate is a slot filled before the run) and blocks in the order of the abstract syntax list, each with its phis, its
// ops and the terminator the SPIR-V back end gives it (emit_spirv.cpp Traverse). A run steps the subgroup: of the blocks
// the live lanes wait at, the earliest in the list runs, for every lane waiting there. In a structured program that is
// where SPIR-V's merges reconverge (a merge follows its construct, a loop's lanes run before the lanes that left it), so
// the subgroup ops see the lanes the back end's maximal reconvergence gives them.
//
// Choices where the SPIR-V leaves one (the same for any two programs compared):
// - one subgroup: lanes are invocations 0-31 of the workgroup (fewer if it is smaller); barriers are no-ops; fragment
//   lanes are 8 quads (lane bit 0 x, bit 1 y), vertex lanes are vertices; workgroup and draw ids are small hashes
// - inputs: constant banks (bank 1 the program's own when given), attributes, storage, global, shared and local memory
//   and texels are hashes of the input seed and where they are read, shaped as floats in [-2, 2), [0, 1), small
//   integers or raw bits; memory that was written reads back what was written; constant reads past 64 KiB (past c1's
//   end for bank 1) are 0
// - several lanes writing one address in one op: the highest lane's value stays; atomics apply in lane order
// - a shuffle or a derivative reading a lane that is not active reads 0 (counted in Trace::undefined_reads); Undef* is 0
// - demoted lanes run on as helpers (active for subgroup ops and derivatives); their stores and outputs are dropped
// - Repeat carries the back end's loop safety counter (Settings::shader_loop_safety_count); Inputs::max_steps caps a run
// - the state hashed: outputs and frag outputs per lane at the end, memory written (storage, global, shared, images,
//   the stage buffer), atomics per address (sorted by lane and value), discards, emitted vertices, unreachable lanes
// Float semantics, as RADV/ACO with the modes EmitSPIRV sets: fp32 denormals flushed unless the program asks for
// DenormPreserve (only a program asking DenormFlushToZero alone flushes fp16/fp64); flushing is done on the inputs and
// result of arithmetic, comparisons and min/max, not on moves, negation, absolute value, selects or bit casts;
// FPMin/FPMax/clamps/saturate are NMin/NMax (the operand that is not NaN; -0 below +0); NaN propagates; conversions to
// integers saturate with NaN to 0 (16-bit ones through 32 bits, then truncated); ConvertF16F32 gives 0 for NaN (the
// fork's select); bit field ops and shifts as NIR lowers them (shift counts masked; a count over 31 passes the value or
// insert through).
// Deviations from RADV, accepted (both translations of a stage see the same ones):
// - every NaN an arithmetic op returns is 0x7fc00000; RADV keeps a payload
// - FPRecip, FPRecipSqrt, FPSqrt, FPSin, FPCos, FPExp2 and FPLog2 are libm's correctly rounded (or nearly) results;
//   ACO uses the hardware approximations (about 1 ulp; sin/cos scale by 1/(2 pi) first)
// - FPMul then FPAdd without NoContraction stay two roundings; NIR may fuse them into an fma
// - the IR's FpControl rounding and fmz modes are ignored (the back end ignores them too)
// - divisions by zero: UDiv32 gives 0xffffffff, SDiv32 -1 (or 1 for a negative dividend)
// - texel results are hashes of the descriptor and every operand (coordinates, lod, bias, offsets, dref), not of
//   derivatives; a sample's implicit lod is not derived from neighbouring lanes
// - one subgroup: shared memory between subgroups, and barriers, are not modelled
#include "irexec.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

#include <fmt/format.h>

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/ir/opcodes.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "common/settings.h"

namespace irexec {
namespace {
using namespace Shader;
using IR::Opcode;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using s64 = int64_t;
using f16 = _Float16;
constexpr u32 NONE = ~0u;
constexpr int LANES = 32;

u64 mix(u64 x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
u64 h(u64 a, u64 b) { return mix(a ^ mix(b)); }
u64 h(u64 a, u64 b, u64 c) { return h(h(a, b), c); }
u64 h(u64 a, u64 b, u64 c, u64 d) { return h(h(a, b, c), d); }
u64 fnv(const void* p, size_t n, u64 x = 0xcbf29ce484222325ull) {
    for (size_t i = 0; i < n; i++) x = (x ^ ((const u8*)p)[i]) * 0x100000001b3ull;
    return x;
}

/// An input word shaped as a shader likely uses it: a float in [-2, 2), a float in [0, 1), a small integer, raw bits
u32 shaped(u64 x) {
    const u32 kind = (u32)(x & 7), v = (u32)(x >> 8);
    float f;
    switch (kind) {
    case 0: case 1: case 2: case 3: f = (float)(s32)(v & 0xffff) / 16384.0f - 2.0f; return std::bit_cast<u32>(f);
    case 4: f = (float)(v & 0xfff) / 4096.0f; return std::bit_cast<u32>(f);
    case 5: case 6: return v & 15;
    default: return (u32)(x >> 32);
    }
}

u32 fbits(float f) { return std::bit_cast<u32>(f); }
float bitsf(u32 b) { return std::bit_cast<float>(b); }
u64 dbits(double d) { return std::bit_cast<u64>(d); }
double bitsd(u64 b) { return std::bit_cast<double>(b); }
u16 hbits(f16 x) { return std::bit_cast<u16>(x); }
f16 bitsh(u16 b) { return std::bit_cast<f16>(b); }

int comps_of(IR::Type t) {
    switch (t) {
    case IR::Type::U32x2: case IR::Type::F16x2: case IR::Type::F32x2: case IR::Type::F64x2: return 2;
    case IR::Type::U32x3: case IR::Type::F16x3: case IR::Type::F32x3: case IR::Type::F64x3: return 3;
    case IR::Type::U32x4: case IR::Type::F16x4: case IR::Type::F32x4: case IR::Type::F64x4: return 4;
    default: return 1;
    }
}

struct Arg {
    u32 slot = NONE;   // (NONE: an empty operand)
    u8 comps = 1;
    bool imm = false;
};
struct Op {
    Opcode code{};
    u32 dst = NONE;
    u32 args = 0;
    u8 nargs = 0;
    u32 flags = 0;
    u32 zero = NONE, sign = NONE, carry = NONE, overflow = NONE, sparse = NONE, in_bounds = NONE;
    u64 key = 0;   // (texture and storage ops: the descriptor's key)
    bool integer = false;
};
struct Phi {
    u32 dst;
    std::vector<std::pair<u32, u32>> in;   // (predecessor block, slot)
};
enum class Term { Jump, Cond, Repeat, Return, Unreachable };
struct Blk {
    std::vector<Phi> phis;
    u32 op_begin = 0, op_end = 0;
    Term term = Term::Return;
    u32 cond = NONE, t = NONE, f = NONE, repeat = NONE;
};

}  // namespace

class Compiled {
public:
    const IR::Program* program = nullptr;
    std::vector<Op> ops;
    std::vector<Arg> args;
    std::vector<Blk> blocks;
    std::vector<std::pair<u32, u64>> consts;   // (slot, raw bits for every lane)
    u32 slots = 0, repeats = 0;
    bool ftz32 = true, ftz16 = false, ftz64 = false;
    u32 lanes = 32;
    std::array<u32, 3> wg{1, 1, 1};
};

namespace {

bool supported(Opcode o);

u64 descriptor_key(const IR::Program& p, Opcode code, const IR::TextureInstInfo info, bool& integer) {
    const bool image = code == Opcode::ImageRead || code == Opcode::ImageWrite || (code >= Opcode::ImageAtomicIAdd32 && code <= Opcode::ImageAtomicExchange32);
    const bool buffer = info.type == TextureType::Buffer;
    const u32 i = info.descriptor_index;
    const auto bad = [&] { return Error(fmt::format("{}: no descriptor {}", IR::NameOf(code), i)); };
    if (image && buffer) {
        if (i >= p.info.image_buffer_descriptors.size()) throw bad();
        const auto& d = p.info.image_buffer_descriptors[i];
        integer = d.is_integer;
        return h(1, d.cbuf_index, d.cbuf_offset, d.handle_program);
    }
    if (image) {
        if (i >= p.info.image_descriptors.size()) throw bad();
        const auto& d = p.info.image_descriptors[i];
        integer = d.is_integer;
        return h(2, d.cbuf_index, d.cbuf_offset, d.handle_program);
    }
    if (buffer) {
        if (i >= p.info.texture_buffer_descriptors.size()) throw bad();
        const auto& d = p.info.texture_buffer_descriptors[i];
        integer = info.is_integer;
        return h(h(3, d.cbuf_index, d.cbuf_offset, d.shift_left), d.secondary_cbuf_index, d.secondary_cbuf_offset, d.handle_program);
    }
    if (i >= p.info.texture_descriptors.size()) throw bad();
    const auto& d = p.info.texture_descriptors[i];
    integer = d.is_integer || info.is_integer;
    return h(h(4, d.cbuf_index, d.cbuf_offset, d.shift_left), h(d.secondary_cbuf_index, d.secondary_cbuf_offset, d.secondary_shift_left),
             d.handle_program, d.has_secondary);
}

bool is_storage_op(Opcode o) {
    return (o >= Opcode::LoadStorageU8 && o <= Opcode::WriteStorage128) || (o >= Opcode::StorageAtomicIAdd32 && o <= Opcode::StorageAtomicMaxF32x2);
}
bool is_texture_op(Opcode o) {
    return (o >= Opcode::ImageSampleImplicitLod && o <= Opcode::ImageWrite) || (o >= Opcode::ImageAtomicIAdd32 && o <= Opcode::ImageAtomicExchange32);
}

}  // namespace

std::shared_ptr<Compiled> compile(const IR::Program& program) {
    auto c = std::make_shared<Compiled>();
    c->program = &program;
    const Info& info = program.info;
    c->ftz32 = !(info.uses_fp32_denorms_preserve && !info.uses_fp32_denorms_flush);
    c->ftz16 = info.uses_fp16_denorms_flush && !info.uses_fp16_denorms_preserve;
    c->ftz64 = c->ftz16;
    if (program.stage == Stage::Compute || program.stage == Stage::Task || program.stage == Stage::Mesh) {
        c->wg = program.workgroup_size;
        const u64 n = (u64)std::max(1u, c->wg[0]) * std::max(1u, c->wg[1]) * std::max(1u, c->wg[2]);
        c->lanes = (u32)std::min<u64>(32, n);
    }
    std::unordered_map<const IR::Inst*, u32> slot_of;
    std::unordered_map<const IR::Block*, u32> block_of;
    std::unordered_map<u64, u32> const_slot;   // (by type and bits)
    // (blocks in the syntax list's order; their indices order the scheduler)
    for (const auto& node : program.syntax_list)
        if (node.type == IR::AbstractSyntaxNode::Type::Block) {
            if (block_of.count(node.data.block)) throw Error("a block twice in the syntax list");
            block_of.emplace(node.data.block, (u32)block_of.size());
        }
    c->blocks.resize(block_of.size());
    for (const auto& [b, id] : block_of)
        for (const IR::Inst& inst : b->Instructions())
            if ((inst.Type() != IR::Type::Void && inst.GetOpcode() != Opcode::Identity) || inst.GetOpcode() == Opcode::Phi)
                slot_of.emplace(&inst, c->slots++);
    const auto arg_of = [&](const IR::Value& v0) -> Arg {
        Arg a;
        if (v0.IsEmpty()) return a;
        const IR::Value v = v0.Resolve();
        a.comps = (u8)comps_of(v.Type());
        if (!v.IsImmediate()) {
            const auto it = slot_of.find(v.InstRecursive());
            if (it == slot_of.end()) throw Error(fmt::format("an operand from outside the program's blocks ({})", IR::NameOf(v.InstRecursive()->GetOpcode())));
            a.slot = it->second;
            return a;
        }
        u64 bits;
        switch (v.Type()) {
        case IR::Type::U1: bits = v.U1(); break;
        case IR::Type::U8: bits = v.U8(); break;
        case IR::Type::U16: bits = v.U16(); break;
        case IR::Type::U32: bits = v.U32(); break;
        case IR::Type::F32: bits = fbits(v.F32()); break;
        case IR::Type::U64: bits = v.U64(); break;
        case IR::Type::F64: bits = dbits(v.F64()); break;
        case IR::Type::Attribute: bits = (u64)v.Attribute(); break;
        case IR::Type::Patch: bits = (u64)v.Patch(); break;
        case IR::Type::Reg: bits = (u64)v.Reg(); break;
        case IR::Type::Pred: bits = (u64)v.Pred(); break;
        default: throw Error(fmt::format("an immediate of type {}", IR::NameOf(v.Type())));
        }
        const u64 k = h((u64)v.Type(), bits);
        auto [it, fresh] = const_slot.emplace(k, 0);
        if (fresh) {
            it->second = c->slots++;
            c->consts.emplace_back(it->second, bits);
        }
        a.slot = it->second;
        a.imm = true;
        return a;
    };
    for (const auto& [b, id] : block_of) {
        Blk& blk = c->blocks[id];
        blk.op_begin = (u32)c->ops.size();
        for (const IR::Inst& cinst : b->Instructions()) {
            IR::Inst& inst = const_cast<IR::Inst&>(cinst);
            const Opcode code = inst.GetOpcode();
            if (code == Opcode::Phi) {
                Phi phi{slot_of.at(&inst), {}};
                for (size_t i = 0; i < inst.NumArgs(); i++) {
                    const auto pb = block_of.find(inst.PhiBlock(i));
                    if (pb == block_of.end()) throw Error("a phi operand from a block outside the syntax list");
                    phi.in.emplace_back(pb->second, arg_of(inst.Arg(i)).slot);
                }
                blk.phis.push_back(std::move(phi));
                continue;
            }
            if (!supported(code)) throw Error(fmt::format("opcode {} not supported", IR::NameOf(code)));
            if (code == Opcode::Identity || code == Opcode::Void || code == Opcode::Prologue || code == Opcode::Epilogue ||
                (code >= Opcode::GetZeroFromOp && code <= Opcode::GetInBoundsFromOp))
                continue;   // (operands resolve through identities; pseudo-ops are set by the op they belong to)
            Op op;
            op.code = code;
            op.flags = inst.Flags<u32>();
            const auto s = slot_of.find(&inst);
            if (s != slot_of.end()) op.dst = s->second;
            op.args = (u32)c->args.size();
            op.nargs = (u8)inst.NumArgs();
            for (size_t i = 0; i < inst.NumArgs(); i++) c->args.push_back(arg_of(inst.Arg(i)));
            const auto pseudo = [&](Opcode p) {
                IR::Inst* const q = inst.GetAssociatedPseudoOperation(p);
                return q ? slot_of.at(q) : NONE;
            };
            if (inst.HasAssociatedPseudoOperation()) {   // (the pseudo-ops share storage: ask only for the kind the op has)
                if (is_texture_op(code)) {
                    op.sparse = pseudo(Opcode::GetSparseFromOp);
                } else if (code >= Opcode::ShuffleIndex && code <= Opcode::ShuffleButterfly) {
                    op.in_bounds = pseudo(Opcode::GetInBoundsFromOp);
                } else {
                    op.zero = pseudo(Opcode::GetZeroFromOp), op.sign = pseudo(Opcode::GetSignFromOp);
                    op.carry = pseudo(Opcode::GetCarryFromOp), op.overflow = pseudo(Opcode::GetOverflowFromOp);
                }
            }
            if (is_texture_op(code)) op.key = descriptor_key(program, code, inst.Flags<IR::TextureInstInfo>(), op.integer);
            if (is_storage_op(code)) {
                const IR::Value bind = inst.Arg(0).Resolve();
                if (!bind.IsImmediate()) throw Error(fmt::format("{} with a dynamic binding", IR::NameOf(code)));
                const u32 bi = bind.U32();
                if (bi >= info.storage_buffers_descriptors.size()) throw Error(fmt::format("{}: no storage buffer {}", IR::NameOf(code), bi));
                const auto& d = info.storage_buffers_descriptors[bi];
                op.key = h('B', d.cbuf_index, d.cbuf_offset);
            }
            if (code == Opcode::IsbeWriteMapU32) {
                const IR::Value a = inst.Arg(0).Resolve();
                if (!a.IsImmediate() || a.U32() != 3) throw Error("IsbeWriteMapU32 at an address other than 3");
            }
            c->ops.push_back(op);
        }
        blk.op_end = (u32)c->ops.size();
    }
    // (terminators: the node after each block, as Traverse branches)
    const auto& list = program.syntax_list;
    for (size_t k = 0; k < list.size(); k++) {
        if (list[k].type != IR::AbstractSyntaxNode::Type::Block) continue;
        Blk& blk = c->blocks[block_of.at(list[k].data.block)];
        const auto id = [&](IR::Block* b) {
            const auto it = block_of.find(b);
            if (it == block_of.end()) throw Error("a branch to a block outside the syntax list");
            return it->second;
        };
        if (k + 1 >= list.size()) {
            blk.term = Term::Return;
            continue;
        }
        const auto& n = list[k + 1];
        using T = IR::AbstractSyntaxNode::Type;
        switch (n.type) {
        case T::Block: blk.term = Term::Jump, blk.t = id(n.data.block); break;
        case T::If: blk.term = Term::Cond, blk.cond = arg_of(n.data.if_node.cond).slot, blk.t = id(n.data.if_node.body), blk.f = id(n.data.if_node.merge); break;
        case T::Loop: blk.term = Term::Jump, blk.t = id(n.data.loop.body); break;
        case T::Break: blk.term = Term::Cond, blk.cond = arg_of(n.data.break_node.cond).slot, blk.t = id(n.data.break_node.merge), blk.f = id(n.data.break_node.skip); break;
        case T::EndIf: blk.term = Term::Jump, blk.t = id(n.data.end_if.merge); break;
        case T::Repeat:
            blk.term = Settings::values.disable_shader_loop_safety_checks ? Term::Cond : Term::Repeat;
            blk.cond = arg_of(n.data.repeat.cond).slot, blk.t = id(n.data.repeat.loop_header), blk.f = id(n.data.repeat.merge);
            blk.repeat = c->repeats++;
            break;
        case T::Return: blk.term = Term::Return; break;
        case T::Unreachable: blk.term = Term::Unreachable; break;
        }
    }
    if (c->blocks.empty()) throw Error("no blocks");
    return c;
}

namespace {

struct MemKey {
    u64 base, addr;
    bool operator<(const MemKey& o) const { return base != o.base ? base < o.base : addr < o.addr; }
    bool operator==(const MemKey& o) const { return base == o.base && addr == o.addr; }
};
struct MemKeyHash {
    size_t operator()(const MemKey& k) const { return (size_t)h(k.base, k.addr); }
};

/// The spaces memory is keyed by: storage buffers by their descriptor (Op::key), the rest by these
constexpr u64 kGlobal = 0x47, kShared = 0x53, kIsbe = 0x49, kIsbeMap = 0x4d, kIsbePayload = 0x50;
u64 local_base(int lane) { return h(0x4c, (u64)lane); }

class Machine {
public:
    Machine(const Compiled& c_, const Inputs& in_) : c(c_), in(in_), p(*c_.program) {
        v.assign((size_t)c.slots * 4 * LANES, 0);
        for (const auto& [slot, bits] : c.consts)
            for (int l = 0; l < LANES; l++) v[(size_t)slot * 128 + l] = bits;
        repeat_counters.assign((size_t)c.repeats * LANES, Settings::values.shader_loop_safety_count);
        seed = mix(in.seed ^ 0x1e5e7ull);
    }

    Trace run() {
        u32 start = in.lanes >= 32 ? ~0u : (1u << in.lanes) - 1;
        if (c.lanes < 32) start &= (1u << c.lanes) - 1;
        std::array<u32, LANES> next{}, pred{};
        for (int l = 0; l < LANES; l++) next[l] = (start >> l & 1) ? 0 : NONE, pred[l] = NONE;
        live = start;
        while (true) {
            u32 b = NONE;
            for (int l = 0; l < LANES; l++)
                if (next[l] < b) b = next[l];
            if (b == NONE) break;
            if (++trace.steps > in.max_steps) {
                trace.capped = true;
                break;
            }
            u32 mask = 0;
            for (int l = 0; l < LANES; l++)
                if (next[l] == b) mask |= 1u << l;
            const Blk& blk = c.blocks[b];
            // (phis read together: each from the block the lane came from)
            if (!blk.phis.empty()) {
                std::vector<u64> tmp(blk.phis.size() * 4 * LANES);
                for (size_t i = 0; i < blk.phis.size(); i++)
                    for (u32 m = mask; m; m &= m - 1) {
                        const int l = std::countr_zero(m);
                        u32 src = NONE;
                        for (const auto& [pb, s] : blk.phis[i].in)
                            if (pb == pred[l]) src = s;
                        if (src == NONE) throw Error(fmt::format("a phi without an operand for its predecessor (block {} from {})", b, pred[l]));
                        for (int k = 0; k < 4; k++) tmp[(i * 4 + k) * LANES + l] = R(src, k, l);
                    }
                for (size_t i = 0; i < blk.phis.size(); i++)
                    for (u32 m = mask; m; m &= m - 1) {
                        const int l = std::countr_zero(m);
                        for (int k = 0; k < 4; k++) R(blk.phis[i].dst, k, l) = tmp[(i * 4 + k) * LANES + l];
                    }
            }
            for (u32 i = blk.op_begin; i < blk.op_end; i++) {
                exec(c.ops[i], mask);
                trace.insts += (u64)std::popcount(mask);
            }
            for (u32 m = mask; m; m &= m - 1) {
                const int l = std::countr_zero(m);
                pred[l] = b;
                switch (blk.term) {
                case Term::Jump: next[l] = blk.t; break;
                case Term::Cond: next[l] = R(blk.cond, 0, l) ? blk.t : blk.f; break;
                case Term::Repeat: {
                    s32& counter = *(s32*)&repeat_counters[(size_t)blk.repeat * LANES + l];
                    counter--;
                    next[l] = R(blk.cond, 0, l) && counter >= 0 ? blk.t : blk.f;
                    break;
                }
                case Term::Return: next[l] = NONE; break;
                case Term::Unreachable:
                    next[l] = NONE;
                    unreachable |= 1u << l;
                    event(fmt::format("unreachable lane {}", l));
                    break;
                }
                if (next[l] == NONE) live &= ~(1u << l);
            }
        }
        finish();
        return std::move(trace);
    }

private:
    const Compiled& c;
    const Inputs& in;
    const IR::Program& p;
    std::vector<u64> v;
    std::vector<u32> repeat_counters;
    u64 seed;
    u32 live = 0, demoted = 0, unreachable = 0;
    Trace trace;
    std::unordered_map<MemKey, u32, MemKeyHash> mem;   // (words written)
    std::map<std::pair<int, u64>, u32> outputs;        // (lane, attribute or patch)
    std::map<std::pair<int, u32>, u32> frag;           // (lane, target * 4 + component; 32 depth, 33 sample mask)
    std::map<MemKey, std::vector<std::string>> atomics;
    std::vector<std::string> emits;

    u64& R(u32 slot, int k, int l) { return v[(size_t)slot * 128 + (size_t)k * LANES + (size_t)l]; }
    const Arg& A(const Op& op, int i) const { return c.args[op.args + (u32)i]; }
    u64 a(const Op& op, int i, int l, int k = 0) {
        const Arg& x = A(op, i);
        if (x.slot == NONE) throw Error(fmt::format("{}: operand {} is empty", IR::NameOf(op.code), i));
        return R(x.slot, k, l);
    }
    u32 a32(const Op& op, int i, int l, int k = 0) { return (u32)a(op, i, l, k); }
    bool present(const Op& op, int i) const { return i < op.nargs && A(op, i).slot != NONE; }
    void event(std::string s) {
        if (in.log) trace.log.push_back(std::move(s));
    }

    // ---- floats ----
    float in32(u64 b) const {
        float f = bitsf((u32)b);
        if (c.ftz32 && std::fpclassify(f) == FP_SUBNORMAL) f = std::copysign(0.0f, f);
        return f;
    }
    u64 out32(float f) const {
        if (std::isnan(f)) return 0x7fc00000u;
        if (c.ftz32 && std::fpclassify(f) == FP_SUBNORMAL) f = std::copysign(0.0f, f);
        return fbits(f);
    }
    float in16(u64 b) const {
        const f16 x = bitsh((u16)b);
        float f = (float)x;
        if (c.ftz16 && (b & 0x7c00) == 0 && (b & 0x3ff) != 0) f = std::copysign(0.0f, f);
        return f;
    }
    u64 out16(float f) const {   // (f exact or rounded once to half)
        if (std::isnan(f)) return 0x7e00u;
        u16 b = hbits((f16)f);
        if (c.ftz16 && (b & 0x7c00) == 0 && (b & 0x3ff) != 0) b &= 0x8000;
        return b;
    }
    u64 out16d(double d) const {   // (rounded once from an exact double)
        if (std::isnan(d)) return 0x7e00u;
        u16 b = hbits((f16)d);
        if (c.ftz16 && (b & 0x7c00) == 0 && (b & 0x3ff) != 0) b &= 0x8000;
        return b;
    }
    double in64(u64 b) const {
        double d = bitsd(b);
        if (c.ftz64 && std::fpclassify(d) == FP_SUBNORMAL) d = std::copysign(0.0, d);
        return d;
    }
    u64 out64(double d) const {
        if (std::isnan(d)) return 0x7ff8000000000000ull;
        if (c.ftz64 && std::fpclassify(d) == FP_SUBNORMAL) d = std::copysign(0.0, d);
        return dbits(d);
    }
    template <typename F>
    static F nmin(F x, F y) {
        if (std::isnan(x)) return y;
        if (std::isnan(y)) return x;
        if (x == y) return std::signbit(x) ? x : y;
        return x < y ? x : y;
    }
    template <typename F>
    static F nmax(F x, F y) {
        if (std::isnan(x)) return y;
        if (std::isnan(y)) return x;
        if (x == y) return std::signbit(x) ? y : x;
        return x > y ? x : y;
    }
    static s32 f2i(double d) {
        if (std::isnan(d)) return 0;
        if (d <= -2147483648.0) return std::numeric_limits<s32>::min();
        if (d >= 2147483647.0) return std::numeric_limits<s32>::max();
        return (s32)d;
    }
    static u32 f2u(double d) {
        if (std::isnan(d) || d <= 0) return 0;
        if (d >= 4294967295.0) return ~0u;
        return (u32)d;
    }
    static s64 f2i64(double d) {
        if (std::isnan(d)) return 0;
        if (d <= -9223372036854775808.0) return std::numeric_limits<s64>::min();
        if (d >= 9223372036854775807.0) return std::numeric_limits<s64>::max();
        return (s64)d;
    }
    static u64 f2u64(double d) {
        if (std::isnan(d) || d <= 0) return 0;
        if (d >= 18446744073709551615.0) return ~0ull;
        return (u64)d;
    }

    // ---- inputs ----
    u32 synth(u64 a0, u64 a1, u64 a2 = 0, u64 a3 = 0) const { return shaped(h(h(seed, a0, a1), a2, a3)); }
    u32 cbuf_word(u32 bank, u32 offset) const {   // (offset aligned to 4)
        if (bank == 1 && in.c1) {
            u32 w = 0;
            if ((size_t)offset + 4 <= in.c1->size()) std::memcpy(&w, in.c1->data() + offset, 4);
            return w;
        }
        if (offset >= 0x10000) return 0;
        return synth('C', bank, offset);
    }
    u32 cbuf_bytes(u32 bank, u32 offset, u32 size) const {   // (size 1, 2 or 4 at an offset aligned to it)
        const u32 w = cbuf_word(bank, offset & ~3u);
        if (size == 4) return w;
        return (w >> (8 * (offset & 3))) & (size == 1 ? 0xffu : 0xffffu);
    }
    u32 word(u64 base, u64 waddr) const {
        if (const auto it = mem.find({base, waddr}); it != mem.end()) return it->second;
        return synth('M', base, waddr);
    }
    u32 load(u64 base, u64 addr, u32 size) const {   // (bytes of one word: size 1, 2 or 4, addr aligned to it)
        const u32 w = word(base, addr >> 2);
        if (size == 4) return w;
        return (w >> (8 * (addr & 3))) & (size == 1 ? 0xffu : 0xffffu);
    }
    void store(u64 base, u64 addr, u32 size, u32 value, int l, const char* what) {
        u32 w = size == 4 ? value : word(base, addr >> 2);
        if (size != 4) {
            const u32 shift = 8 * (addr & 3), m = (size == 1 ? 0xffu : 0xffffu) << shift;
            w = (w & ~m) | ((value << shift) & m);
        }
        mem[{base, addr >> 2}] = w;
        event(fmt::format("{} lane {} [{:x}:{:x}] = {:08x}", what, l, base, addr, value));
    }
    bool writes(int l) const { return !(demoted >> l & 1); }

    u32 attribute(u64 attr, u32 vertex, int l) const {
        using At = IR::Attribute;
        const At at = (At)attr;
        const Stage st = p.stage;
        if (at == At::FrontFace) return (seed >> 7 & 1) ? ~0u : 0u;
        if (at == At::InstanceId) return (u32)(seed % 5);
        if (at == At::VertexId) return (u32)l + (u32)(seed % 7) * 32;
        if (at == At::BaseInstance || at == At::BaseVertex || at == At::DrawID) return (u32)(h(seed, attr) % 16);
        if (st == Stage::Fragment && (at == At::PositionX || at == At::PositionY)) {
            const u32 quad = (u32)l >> 2, x = (quad % 4) * 2 + ((u32)l & 1), y = (quad / 4) * 2 + ((u32)l >> 1 & 1);
            const u32 ox = (u32)(seed % 64) * 8, oy = (u32)(seed / 64 % 64) * 4;
            return fbits((float)((at == At::PositionX ? x + ox : y + oy)) + 0.5f);
        }
        return synth('A', attr, vertex, (u64)l);
    }
    u32 lane_index(int l, int k) const {   // (LocalInvocationId's components of lane l)
        const u32 x = std::max(1u, c.wg[0]), y = std::max(1u, c.wg[1]);
        return k == 0 ? (u32)l % x : k == 1 ? (u32)l / x % y : (u32)l / (x * y);
    }

    // ---- textures ----
    u64 texel_hash(const Op& op, int l, int first, u32 which) const {
        u64 x = h(seed, op.key, (u64)op.code, which);
        for (int i = first; i < op.nargs; i++) {
            const Arg& g = c.args[op.args + (u32)i];
            if (g.slot == NONE) {
                x = h(x, 0xe);
                continue;
            }
            for (int k = 0; k < g.comps; k++) x = h(x, v[(size_t)g.slot * 128 + (size_t)k * LANES + (size_t)l]);
        }
        return x;
    }
    u64 coord_hash(const Op& op, int l) const {   // (an image's texel address: its handle and coordinates)
        u64 x = h(op.key, 0x7e);
        for (int i = 0; i < 2; i++) {
            const Arg& g = c.args[op.args + (u32)i];
            if (g.slot == NONE) continue;
            for (int k = 0; k < g.comps; k++) x = h(x, v[(size_t)g.slot * 128 + (size_t)k * LANES + (size_t)l]);
        }
        return x;
    }
    void texel(const Op& op, int l, int ncomp, bool dref) {
        const u64 x = texel_hash(op, l, 0, 0);
        for (int k = 0; k < ncomp; k++) {
            const u64 y = h(x, (u64)k);
            if (dref) R(op.dst, k, l) = fbits((float)(y % 3) * 0.5f);
            else if (op.integer) R(op.dst, k, l) = (u32)(y % 256);
            else R(op.dst, k, l) = fbits((float)(y % 257) / 256.0f);
        }
        if (op.sparse != NONE) R(op.sparse, 0, l) = 1;
    }

    // ---- subgroup ----
    u64 from(u32 slot, int src, u32 mask, int k = 0) {
        if (src < 0 || src >= LANES || !(mask >> src & 1)) {
            trace.undefined_reads++;
            return 0;
        }
        return R(slot, k, src);
    }

    // ---- atomics ----
    template <typename F>
    void atomic32(const Op& op, u32 mask, u64 base, const std::function<u64(int)>& addr_of, int value_arg, F f) {
        for (u32 m = mask; m; m &= m - 1) {
            const int l = std::countr_zero(m);
            const u64 addr = addr_of(l);
            const u32 old = load(base, addr & ~3ull, 4), val = a32(op, value_arg, l);
            const u32 next = f(old, val, l);
            if (writes(l)) {
                mem[{base, addr >> 2}] = next;
                atomics[{base, addr >> 2}].push_back(fmt::format("{}:{:08x}", IR::NameOf(op.code), val));
                event(fmt::format("atomic {} lane {} [{:x}:{:x}] {:08x} -> {:08x}", IR::NameOf(op.code), l, base, addr, old, next));
            }
            R(op.dst, 0, l) = old;
        }
    }
    template <typename F>
    void atomic64(const Op& op, u32 mask, u64 base, const std::function<u64(int)>& addr_of, int value_arg, bool pair, F f) {
        for (u32 m = mask; m; m &= m - 1) {
            const int l = std::countr_zero(m);
            const u64 addr = addr_of(l) & ~7ull;
            const u64 old = load(base, addr, 4) | (u64)load(base, addr + 4, 4) << 32;
            const u64 val = pair ? (a(op, value_arg, l, 0) & 0xffffffffu) | a(op, value_arg, l, 1) << 32 : a(op, value_arg, l);
            const u64 next = f(old, val);
            if (writes(l)) {
                mem[{base, addr >> 2}] = (u32)next, mem[{base, (addr >> 2) + 1}] = (u32)(next >> 32);
                atomics[{base, addr >> 2}].push_back(fmt::format("{}:{:016x}", IR::NameOf(op.code), val));
                event(fmt::format("atomic {} lane {} [{:x}:{:x}] {:016x} -> {:016x}", IR::NameOf(op.code), l, base, addr, old, next));
            }
            if (pair) R(op.dst, 0, l) = (u32)old, R(op.dst, 1, l) = old >> 32;
            else R(op.dst, 0, l) = old;
        }
    }
    static u32 atomic_op(Opcode o, u32 old, u32 val) {
        switch (o) {
#define K(name) case Opcode::Shared##name: case Opcode::Global##name: case Opcode::Storage##name: case Opcode::Image##name:
        K(AtomicIAdd32) return old + val;
        K(AtomicSMin32) return (u32)std::min((s32)old, (s32)val);
        K(AtomicUMin32) return std::min(old, val);
        K(AtomicSMax32) return (u32)std::max((s32)old, (s32)val);
        K(AtomicUMax32) return std::max(old, val);
        K(AtomicInc32) return old >= val ? 0 : old + 1;
        K(AtomicDec32) return (old == 0 || old > val) ? val : old - 1;
        K(AtomicAnd32) return old & val;
        K(AtomicOr32) return old | val;
        K(AtomicXor32) return old ^ val;
        K(AtomicExchange32) return val;
#undef K
        default: throw Error(fmt::format("atomic {}", IR::NameOf(o)));
        }
    }
    static u64 atomic_op64(Opcode o, u64 old, u64 val) {
        switch (o) {
        case Opcode::GlobalAtomicIAdd64: case Opcode::StorageAtomicIAdd64: case Opcode::GlobalAtomicIAdd32x2: case Opcode::StorageAtomicIAdd32x2: return old + val;
        case Opcode::GlobalAtomicSMin64: case Opcode::StorageAtomicSMin64: case Opcode::GlobalAtomicSMin32x2: case Opcode::StorageAtomicSMin32x2: return (u64)std::min((s64)old, (s64)val);
        case Opcode::GlobalAtomicUMin64: case Opcode::StorageAtomicUMin64: case Opcode::GlobalAtomicUMin32x2: case Opcode::StorageAtomicUMin32x2: return std::min(old, val);
        case Opcode::GlobalAtomicSMax64: case Opcode::StorageAtomicSMax64: case Opcode::GlobalAtomicSMax32x2: case Opcode::StorageAtomicSMax32x2: return (u64)std::max((s64)old, (s64)val);
        case Opcode::GlobalAtomicUMax64: case Opcode::StorageAtomicUMax64: case Opcode::GlobalAtomicUMax32x2: case Opcode::StorageAtomicUMax32x2: return std::max(old, val);
        case Opcode::GlobalAtomicAnd64: case Opcode::StorageAtomicAnd64: case Opcode::GlobalAtomicAnd32x2: case Opcode::StorageAtomicAnd32x2: return old & val;
        case Opcode::GlobalAtomicOr64: case Opcode::StorageAtomicOr64: case Opcode::GlobalAtomicOr32x2: case Opcode::StorageAtomicOr32x2: return old | val;
        case Opcode::GlobalAtomicXor64: case Opcode::StorageAtomicXor64: case Opcode::GlobalAtomicXor32x2: case Opcode::StorageAtomicXor32x2: return old ^ val;
        case Opcode::GlobalAtomicExchange64: case Opcode::StorageAtomicExchange64: case Opcode::GlobalAtomicExchange32x2: case Opcode::StorageAtomicExchange32x2:
        case Opcode::SharedAtomicExchange64: case Opcode::SharedAtomicExchange32x2: return val;
        default: throw Error(fmt::format("atomic {}", IR::NameOf(o)));
        }
    }
    u32 f16x2_op(Opcode o, u32 old, u32 val) const {
        u32 r = 0;
        for (int k = 0; k < 2; k++) {
            const float x = in16(old >> (16 * k) & 0xffff), y = in16(val >> (16 * k) & 0xffff);
            const bool add = o == Opcode::GlobalAtomicAddF16x2 || o == Opcode::StorageAtomicAddF16x2;
            const bool mn = o == Opcode::GlobalAtomicMinF16x2 || o == Opcode::StorageAtomicMinF16x2;
            const u64 b = add ? out16(x + y) : out16(mn ? nmin(x, y) : nmax(x, y));
            r |= (u32)b << (16 * k);
        }
        return r;
    }

    void bit_flags(const Op& op, int l, u32 r) {
        if (op.zero != NONE) R(op.zero, 0, l) = r == 0;
        if (op.sign != NONE) R(op.sign, 0, l) = (s32)r < 0;
    }

    static u32 ubfe(u32 base, u32 offset, u32 count) {
        if (count > 31) return base;   // (NIR: bcsel(ult(31, bits), value, ubfe))
        offset &= 31;
        if (count == 0) return 0;
        if (offset + count < 32) return (base << (32 - count - offset)) >> (32 - count);
        return base >> offset;
    }
    static u32 sbfe(u32 base, u32 offset, u32 count) {
        if ((s32)count > 31 || (s32)count < 0) return base;   // (NIR: bcsel(ilt(31, bits), value, ibfe))
        offset &= 31;
        if (count == 0) return 0;
        if (offset + count < 32) return (u32)((s32)(base << (32 - count - offset)) >> (32 - count));
        return (u32)((s32)base >> offset);
    }
    static u32 bfi(u32 base, u32 insert, u32 offset, u32 count) {
        if ((s32)count > 31 || (s32)count < 0) return insert;   // (NIR: bcsel(ilt(31, bits), insert, bfi(bfm(...))))
        const u32 mask = ((count & 31) == 0 ? 0u : ((1u << (count & 31)) - 1)) << (offset & 31);
        return (base & ~mask) | ((insert << (offset & 31)) & mask);
    }

    void mma(const Op& op, u32 mask);
    void exec(const Op& op, u32 mask);
    void finish();
};

#define EACH for (u32 m_ = mask, l = 0; m_ && ((l = (u32)std::countr_zero(m_)), true); m_ &= m_ - 1)

void Machine::exec(const Op& op, u32 mask) {
    const Opcode o = op.code;
    const auto f32_1 = [&](auto fn) { EACH R(op.dst, 0, l) = out32(fn(in32(a(op, 0, l)))); };
    const auto f32_2 = [&](auto fn) { EACH R(op.dst, 0, l) = out32(fn(in32(a(op, 0, l)), in32(a(op, 1, l)))); };
    const auto f32_cmp = [&](auto fn) { EACH R(op.dst, 0, l) = fn(in32(a(op, 0, l)), in32(a(op, 1, l))) ? 1 : 0; };
    const auto f16_1 = [&](auto fn) { EACH R(op.dst, 0, l) = out16(fn(in16(a(op, 0, l)))); };
    const auto f16_2 = [&](auto fn) { EACH R(op.dst, 0, l) = out16(fn(in16(a(op, 0, l)), in16(a(op, 1, l)))); };
    const auto f16_cmp = [&](auto fn) { EACH R(op.dst, 0, l) = fn(in16(a(op, 0, l)), in16(a(op, 1, l))) ? 1 : 0; };
    const auto f64_1 = [&](auto fn) { EACH R(op.dst, 0, l) = out64(fn(in64(a(op, 0, l)))); };
    const auto f64_2 = [&](auto fn) { EACH R(op.dst, 0, l) = out64(fn(in64(a(op, 0, l)), in64(a(op, 1, l)))); };
    const auto f64_cmp = [&](auto fn) { EACH R(op.dst, 0, l) = fn(in64(a(op, 0, l)), in64(a(op, 1, l))) ? 1 : 0; };
    const auto u32_1 = [&](auto fn) { EACH R(op.dst, 0, l) = (u32)fn(a32(op, 0, l)); };
    const auto u32_2 = [&](auto fn) { EACH R(op.dst, 0, l) = (u32)fn(a32(op, 0, l), a32(op, 1, l)); };
    const auto u64_2 = [&](auto fn) { EACH R(op.dst, 0, l) = (u64)fn(a(op, 0, l), a(op, 1, l)); };
    const auto cmp = [&](auto fn) { EACH R(op.dst, 0, l) = fn(a32(op, 0, l), a32(op, 1, l)) ? 1 : 0; };
    const auto copy = [&](int arg, int n) { EACH for (int k = 0; k < n; k++) R(op.dst, k, l) = a(op, arg, l, k); };
    const auto construct = [&](int n) { EACH for (int k = 0; k < n; k++) R(op.dst, k, l) = a(op, k, l); };
    const auto extract = [&](int n) {
        EACH {
            const u32 i = a32(op, 1, l);
            if (i >= (u32)n) throw Error(fmt::format("{} index {}", IR::NameOf(o), i));
            R(op.dst, 0, l) = a(op, 0, l, (int)i);
        }
    };
    const auto insert = [&](int n) {
        EACH {
            const u32 i = a32(op, 2, l);
            if (i >= (u32)n) throw Error(fmt::format("{} index {}", IR::NameOf(o), i));
            for (int k = 0; k < n; k++) R(op.dst, k, l) = a(op, 0, l, k);
            R(op.dst, (int)i, l) = a(op, 1, l);
        }
    };
    const auto sext = [](u32 x, int bits) { return (u32)((s32)(x << (32 - bits)) >> (32 - bits)); };
    const auto cbuf = [&](u32 size, bool sign) {
        EACH {
            const u32 bank = a32(op, 0, l), off = a32(op, 1, l) & ~(size - 1);
            const u32 x = cbuf_bytes(bank, off, size);
            R(op.dst, 0, l) = sign ? sext(x, (int)size * 8) : x;
        }
    };
    const auto mem_load = [&](u64 base, auto addr_of, u32 size, bool sign, int words) {
        EACH {
            const u64 addr = addr_of(l);
            if (words) {
                for (int k = 0; k < words; k++) R(op.dst, k, l) = load(base, (addr & ~3ull) + 4 * (u64)k, 4);
            } else {
                const u32 x = load(base, addr & ~(u64)(size - 1), size);
                R(op.dst, 0, l) = sign ? sext(x, (int)size * 8) : x;
            }
        }
    };
    const auto mem_store = [&](u64 base, auto addr_of, int value_arg, u32 size, int words, const char* what) {
        EACH {
            if (!writes((int)l)) continue;
            const u64 addr = addr_of(l);
            if (words) {
                for (int k = 0; k < words; k++) store(base, (addr & ~3ull) + 4 * (u64)k, 4, a32(op, value_arg, l, k), (int)l, what);
            } else {
                store(base, addr & ~(u64)(size - 1), size, a32(op, value_arg, l), (int)l, what);
            }
        }
    };
    const auto global_addr = [&](u32 l) { return a(op, 0, l); };
    const auto storage_addr = [&](u32 l) { return (u64)a32(op, 1, l); };
    const auto shared_addr = [&](u32 l) { return (u64)a32(op, 0, l); };

    switch (o) {
    // ---- control and special ----
    case Opcode::DemoteToHelperInvocation:
        EACH {
            if (!(demoted >> l & 1)) event(fmt::format("demote lane {}", l));
            demoted |= 1u << l;
        }
        return;
    case Opcode::EmitVertex: case Opcode::EndPrimitive:
        EACH {
            std::string s = fmt::format("{} lane {} stream {}", o == Opcode::EmitVertex ? "emit" : "end", l, a32(op, 0, l));
            if (o == Opcode::EmitVertex)
                for (const auto& [k, x] : outputs)
                    if (k.first == (int)l) s += fmt::format(" {}={:08x}", k.second, x);
            event(s);
            if (writes((int)l)) emits.push_back(std::move(s));
        }
        return;
    case Opcode::Barrier: case Opcode::WorkgroupMemoryBarrier: case Opcode::SubgroupBarrier: case Opcode::DeviceMemoryBarrier: case Opcode::Join:
        return;
    case Opcode::ConditionRef: copy(0, 1); return;

    // ---- context ----
    case Opcode::GetCbufU8: cbuf(1, false); return;
    case Opcode::GetCbufS8: cbuf(1, true); return;
    case Opcode::GetCbufU16: cbuf(2, false); return;
    case Opcode::GetCbufS16: cbuf(2, true); return;
    case Opcode::GetCbufU32: case Opcode::GetCbufF32: cbuf(4, false); return;
    case Opcode::GetCbufU32x2:
        EACH {
            const u32 bank = a32(op, 0, l), off = a32(op, 1, l) & ~7u;
            R(op.dst, 0, l) = cbuf_word(bank, off), R(op.dst, 1, l) = cbuf_word(bank, off + 4);
        }
        return;
    case Opcode::GetAttribute: case Opcode::GetAttributeU32: EACH R(op.dst, 0, l) = attribute(a(op, 0, l), a32(op, 1, l), (int)l); return;
    case Opcode::GetAttributeIndexed: EACH R(op.dst, 0, l) = attribute(a32(op, 0, l) / 4, a32(op, 1, l), (int)l); return;
    case Opcode::GetPatch: EACH R(op.dst, 0, l) = synth('P', a(op, 0, l), (u64)l); return;
    case Opcode::SetAttribute: case Opcode::SetAttributeIndexed: case Opcode::SetPatch:
        EACH {
            u64 at = o == Opcode::SetAttributeIndexed ? a32(op, 0, l) / 4 : a(op, 0, l);
            if (o == Opcode::SetPatch) at |= 1ull << 40;
            const u32 vtx = o == Opcode::SetPatch ? 0 : a32(op, 2, l);
            at |= (u64)vtx << 48;
            event(fmt::format("out lane {} {} = {:08x}", l, at, a32(op, 1, l)));
            if (writes((int)l)) outputs[{(int)l, at}] = a32(op, 1, l);
        }
        return;
    case Opcode::SetFragColor:
        EACH {
            const u32 t = a32(op, 0, l), k = a32(op, 1, l);
            event(fmt::format("frag lane {} rt{}.{} = {:08x}", l, t, k, a32(op, 2, l)));
            frag[{(int)l, t * 4 + k}] = a32(op, 2, l);
        }
        return;
    case Opcode::SetFragDepth: EACH frag[{(int)l, 32}] = a32(op, 0, l); return;
    case Opcode::SetSampleMask: EACH frag[{(int)l, 33}] = a32(op, 0, l); return;
    case Opcode::IsbeRead:
        EACH {
            const u32 addr = a32(op, 0, l);
            if (p.stage == Stage::Task) R(op.dst, 0, l) = (u32)(seed % 9);
            else if (addr == 0) R(op.dst, 0, l) = (u32)(seed % 4);
            else R(op.dst, 0, l) = load(kIsbePayload, addr & ~3u, 4);
        }
        return;
    case Opcode::IsbeWriteAttribute: mem_store(p.stage == Stage::Task ? kIsbePayload : kIsbe, shared_addr, 1, 4, 0, "isbe"); return;
    case Opcode::IsbeWriteMapU8:
        EACH if (writes((int)l)) store(kIsbeMap, a32(op, 0, l), 1, a32(op, 1, l) & 0xff, (int)l, "isbe map");
        return;
    case Opcode::IsbeWriteMapU32:
        EACH if (writes((int)l)) store(kIsbeMap, 0x10000, 4, a32(op, 1, l), (int)l, "primitive count");
        return;
    case Opcode::WorkgroupId: EACH R(op.dst, 0, l) = (u32)(seed % 3), R(op.dst, 1, l) = (u32)(seed / 3 % 2), R(op.dst, 2, l) = 0; return;
    case Opcode::NumWorkgroups: EACH R(op.dst, 0, l) = 3, R(op.dst, 1, l) = 2, R(op.dst, 2, l) = 1; return;
    case Opcode::LocalInvocationId: EACH for (int k = 0; k < 3; k++) R(op.dst, k, l) = lane_index((int)l, k); return;
    case Opcode::InvocationId: case Opcode::SampleId: EACH R(op.dst, 0, l) = 0; return;
    case Opcode::InvocationInfo: EACH R(op.dst, 0, l) = synth('V', 0, 0); return;
    case Opcode::IsHelperInvocation: EACH R(op.dst, 0, l) = demoted >> l & 1; return;
    case Opcode::SR_WScaleFactorXY: case Opcode::SR_WScaleFactorZ: EACH R(op.dst, 0, l) = synth('W', (u64)o, 0); return;
    case Opcode::YDirection: case Opcode::ResolutionDownFactor: EACH R(op.dst, 0, l) = fbits(1.0f); return;
    case Opcode::RenderArea: {
        const float w = 1280.0f + (float)(seed % 4) * 64.0f, ht = 720.0f;
        EACH R(op.dst, 0, l) = fbits(w), R(op.dst, 1, l) = fbits(ht), R(op.dst, 2, l) = fbits(1.0f / w), R(op.dst, 3, l) = fbits(1.0f / ht);
        return;
    }
    case Opcode::IsTextureScaled: case Opcode::IsImageScaled: EACH R(op.dst, 0, l) = 0; return;
    case Opcode::UndefU1: case Opcode::UndefU8: case Opcode::UndefU16: case Opcode::UndefU32: case Opcode::UndefU64: EACH R(op.dst, 0, l) = 0; return;

    // ---- memory ----
    case Opcode::LoadGlobalU8: mem_load(kGlobal, global_addr, 1, false, 0); return;
    case Opcode::LoadGlobalS8: mem_load(kGlobal, global_addr, 1, true, 0); return;
    case Opcode::LoadGlobalU16: mem_load(kGlobal, global_addr, 2, false, 0); return;
    case Opcode::LoadGlobalS16: mem_load(kGlobal, global_addr, 2, true, 0); return;
    case Opcode::LoadGlobal32: mem_load(kGlobal, global_addr, 4, false, 1); return;
    case Opcode::LoadGlobal64: mem_load(kGlobal, global_addr, 4, false, 2); return;
    case Opcode::LoadGlobal128: mem_load(kGlobal, global_addr, 4, false, 4); return;
    case Opcode::WriteGlobalU8: case Opcode::WriteGlobalS8: mem_store(kGlobal, global_addr, 1, 1, 0, "global"); return;
    case Opcode::WriteGlobalU16: case Opcode::WriteGlobalS16: mem_store(kGlobal, global_addr, 1, 2, 0, "global"); return;
    case Opcode::WriteGlobal32: mem_store(kGlobal, global_addr, 1, 4, 1, "global"); return;
    case Opcode::WriteGlobal64: mem_store(kGlobal, global_addr, 1, 4, 2, "global"); return;
    case Opcode::WriteGlobal128: mem_store(kGlobal, global_addr, 1, 4, 4, "global"); return;
    case Opcode::LoadStorageU8: mem_load(op.key, storage_addr, 1, false, 0); return;
    case Opcode::LoadStorageS8: mem_load(op.key, storage_addr, 1, true, 0); return;
    case Opcode::LoadStorageU16: mem_load(op.key, storage_addr, 2, false, 0); return;
    case Opcode::LoadStorageS16: mem_load(op.key, storage_addr, 2, true, 0); return;
    case Opcode::LoadStorage32: mem_load(op.key, storage_addr, 4, false, 1); return;
    case Opcode::LoadStorage64: mem_load(op.key, storage_addr, 4, false, 2); return;
    case Opcode::LoadStorage128: mem_load(op.key, storage_addr, 4, false, 4); return;
    case Opcode::WriteStorageU8: case Opcode::WriteStorageS8: mem_store(op.key, storage_addr, 2, 1, 0, "storage"); return;
    case Opcode::WriteStorageU16: case Opcode::WriteStorageS16: mem_store(op.key, storage_addr, 2, 2, 0, "storage"); return;
    case Opcode::WriteStorage32: mem_store(op.key, storage_addr, 2, 4, 1, "storage"); return;
    case Opcode::WriteStorage64: mem_store(op.key, storage_addr, 2, 4, 2, "storage"); return;
    case Opcode::WriteStorage128: mem_store(op.key, storage_addr, 2, 4, 4, "storage"); return;
    case Opcode::LoadLocal:
        EACH {
            const u32 w = a32(op, 0, l);
            if ((u64)w * 4 >= p.local_memory_size) trace.undefined_reads++;
            R(op.dst, 0, l) = (u64)w * 4 < p.local_memory_size ? load(local_base((int)l), (u64)w * 4, 4) : 0;
        }
        return;
    case Opcode::WriteLocal:
        EACH {
            const u32 w = a32(op, 0, l);
            if ((u64)w * 4 < p.local_memory_size) mem[{local_base((int)l), w}] = a32(op, 1, l);
        }
        return;
    case Opcode::LoadSharedU8: mem_load(kShared, shared_addr, 1, false, 0); return;
    case Opcode::LoadSharedS8: mem_load(kShared, shared_addr, 1, true, 0); return;
    case Opcode::LoadSharedU16: mem_load(kShared, shared_addr, 2, false, 0); return;
    case Opcode::LoadSharedS16: mem_load(kShared, shared_addr, 2, true, 0); return;
    case Opcode::LoadSharedU32: mem_load(kShared, shared_addr, 4, false, 1); return;
    case Opcode::LoadSharedU64: mem_load(kShared, shared_addr, 4, false, 2); return;
    case Opcode::LoadSharedU128: mem_load(kShared, shared_addr, 4, false, 4); return;
    case Opcode::WriteSharedU8: mem_store(kShared, shared_addr, 1, 1, 0, "shared"); return;
    case Opcode::WriteSharedU16: mem_store(kShared, shared_addr, 1, 2, 0, "shared"); return;
    case Opcode::WriteSharedU32: mem_store(kShared, shared_addr, 1, 4, 1, "shared"); return;
    case Opcode::WriteSharedU64: mem_store(kShared, shared_addr, 1, 4, 2, "shared"); return;
    case Opcode::WriteSharedU128: mem_store(kShared, shared_addr, 1, 4, 4, "shared"); return;

    // ---- vectors, selects, casts ----
    case Opcode::CompositeConstructU32x2: case Opcode::CompositeConstructF16x2: case Opcode::CompositeConstructF32x2: case Opcode::CompositeConstructF64x2: construct(2); return;
    case Opcode::CompositeConstructU32x3: case Opcode::CompositeConstructF16x3: case Opcode::CompositeConstructF32x3: case Opcode::CompositeConstructF64x3: construct(3); return;
    case Opcode::CompositeConstructU32x4: case Opcode::CompositeConstructF16x4: case Opcode::CompositeConstructF32x4: case Opcode::CompositeConstructF64x4: construct(4); return;
    case Opcode::CompositeExtractU32x2: case Opcode::CompositeExtractF16x2: case Opcode::CompositeExtractF32x2: case Opcode::CompositeExtractF64x2: extract(2); return;
    case Opcode::CompositeExtractU32x3: case Opcode::CompositeExtractF16x3: case Opcode::CompositeExtractF32x3: case Opcode::CompositeExtractF64x3: extract(3); return;
    case Opcode::CompositeExtractU32x4: case Opcode::CompositeExtractF16x4: case Opcode::CompositeExtractF32x4: case Opcode::CompositeExtractF64x4: extract(4); return;
    case Opcode::CompositeInsertU32x2: case Opcode::CompositeInsertF16x2: case Opcode::CompositeInsertF32x2: case Opcode::CompositeInsertF64x2: insert(2); return;
    case Opcode::CompositeInsertU32x3: case Opcode::CompositeInsertF16x3: case Opcode::CompositeInsertF32x3: case Opcode::CompositeInsertF64x3: insert(3); return;
    case Opcode::CompositeInsertU32x4: case Opcode::CompositeInsertF16x4: case Opcode::CompositeInsertF32x4: case Opcode::CompositeInsertF64x4: insert(4); return;
    case Opcode::SelectU1: case Opcode::SelectU8: case Opcode::SelectU16: case Opcode::SelectU32: case Opcode::SelectU64: case Opcode::SelectF16:
    case Opcode::SelectF32: case Opcode::SelectF64:
        EACH R(op.dst, 0, l) = a(op, 0, l) ? a(op, 1, l) : a(op, 2, l);
        return;
    case Opcode::BitCastU16F16: case Opcode::BitCastU32F32: case Opcode::BitCastU64F64: case Opcode::BitCastF16U16: case Opcode::BitCastF32U32:
    case Opcode::BitCastF64U64:
        copy(0, 1);
        return;
    case Opcode::PackUint2x32: case Opcode::PackDouble2x32: EACH R(op.dst, 0, l) = (a(op, 0, l, 0) & 0xffffffffu) | a(op, 0, l, 1) << 32; return;
    case Opcode::UnpackUint2x32: case Opcode::UnpackDouble2x32: EACH R(op.dst, 0, l) = (u32)a(op, 0, l), R(op.dst, 1, l) = a(op, 0, l) >> 32; return;
    case Opcode::PackFloat2x16: EACH R(op.dst, 0, l) = (a(op, 0, l, 0) & 0xffff) | (a(op, 0, l, 1) & 0xffff) << 16; return;
    case Opcode::UnpackFloat2x16: EACH R(op.dst, 0, l) = a32(op, 0, l) & 0xffff, R(op.dst, 1, l) = a32(op, 0, l) >> 16; return;
    case Opcode::PackHalf2x16:
        EACH {
            u32 r = 0;
            for (int k = 0; k < 2; k++) {
                const float f = bitsf(a32(op, 0, l, k));
                r |= (u32)(std::isnan(f) ? 0x7e00u : hbits((f16)f)) << (16 * k);
            }
            R(op.dst, 0, l) = r;
        }
        return;
    case Opcode::UnpackHalf2x16:
        EACH for (int k = 0; k < 2; k++) R(op.dst, k, l) = fbits((float)bitsh((u16)(a32(op, 0, l) >> (16 * k))));
        return;

    // ---- floats ----
    case Opcode::FPAbs16: EACH R(op.dst, 0, l) = a(op, 0, l) & 0x7fff; return;
    case Opcode::FPAbs32: EACH R(op.dst, 0, l) = a(op, 0, l) & 0x7fffffffu; return;
    case Opcode::FPAbs64: EACH R(op.dst, 0, l) = a(op, 0, l) & 0x7fffffffffffffffull; return;
    case Opcode::FPNeg16: EACH R(op.dst, 0, l) = (a(op, 0, l) ^ 0x8000) & 0xffff; return;
    case Opcode::FPNeg32: EACH R(op.dst, 0, l) = (a(op, 0, l) ^ 0x80000000u) & 0xffffffffu; return;
    case Opcode::FPNeg64: EACH R(op.dst, 0, l) = a(op, 0, l) ^ 0x8000000000000000ull; return;
    case Opcode::FPAdd16: f16_2([](float x, float y) { return x + y; }); return;
    case Opcode::FPAdd32: f32_2([](float x, float y) { return x + y; }); return;
    case Opcode::FPAdd64: f64_2([](double x, double y) { return x + y; }); return;
    case Opcode::FPMul16: f16_2([](float x, float y) { return x * y; }); return;
    case Opcode::FPMul32: f32_2([](float x, float y) { return x * y; }); return;
    case Opcode::FPMul64: f64_2([](double x, double y) { return x * y; }); return;
    case Opcode::FPFma16: EACH R(op.dst, 0, l) = out16d((double)in16(a(op, 0, l)) * (double)in16(a(op, 1, l)) + (double)in16(a(op, 2, l))); return;
    case Opcode::FPFma32: EACH R(op.dst, 0, l) = out32(std::fma(in32(a(op, 0, l)), in32(a(op, 1, l)), in32(a(op, 2, l)))); return;
    case Opcode::FPFma64: EACH R(op.dst, 0, l) = out64(std::fma(in64(a(op, 0, l)), in64(a(op, 1, l)), in64(a(op, 2, l)))); return;
    case Opcode::FPMin32: f32_2([](float x, float y) { return nmin(x, y); }); return;
    case Opcode::FPMax32: f32_2([](float x, float y) { return nmax(x, y); }); return;
    case Opcode::FPMin64: f64_2([](double x, double y) { return nmin(x, y); }); return;
    case Opcode::FPMax64: f64_2([](double x, double y) { return nmax(x, y); }); return;
    case Opcode::FPRecip32: f32_1([](float x) { return 1.0f / x; }); return;
    case Opcode::FPRecip64: f64_1([](double x) { return 1.0 / x; }); return;
    case Opcode::FPRecipSqrt32: f32_1([](float x) { return (float)(1.0 / std::sqrt((double)x)); }); return;
    case Opcode::FPRecipSqrt64: f64_1([](double x) { return 1.0 / std::sqrt(x); }); return;
    case Opcode::FPSqrt: f32_1([](float x) { return std::sqrt(x); }); return;
    case Opcode::FPSin: f32_1([](float x) { return (float)std::sin((double)x); }); return;
    case Opcode::FPCos: f32_1([](float x) { return (float)std::cos((double)x); }); return;
    case Opcode::FPExp2: f32_1([](float x) { return (float)std::exp2((double)x); }); return;
    case Opcode::FPLog2: f32_1([](float x) { return (float)std::log2((double)x); }); return;
    case Opcode::FPSaturate16: f16_1([](float x) { return nmin(nmax(x, 0.0f), 1.0f); }); return;
    case Opcode::FPSaturate32: f32_1([](float x) { return nmin(nmax(x, 0.0f), 1.0f); }); return;
    case Opcode::FPSaturate64: f64_1([](double x) { return nmin(nmax(x, 0.0), 1.0); }); return;
    case Opcode::FPClamp16: EACH R(op.dst, 0, l) = out16(nmin(nmax(in16(a(op, 0, l)), in16(a(op, 1, l))), in16(a(op, 2, l)))); return;
    case Opcode::FPClamp32: EACH R(op.dst, 0, l) = out32(nmin(nmax(in32(a(op, 0, l)), in32(a(op, 1, l))), in32(a(op, 2, l)))); return;
    case Opcode::FPClamp64: EACH R(op.dst, 0, l) = out64(nmin(nmax(in64(a(op, 0, l)), in64(a(op, 1, l))), in64(a(op, 2, l)))); return;
    case Opcode::FPRoundEven16: f16_1([](float x) { return std::nearbyint(x); }); return;
    case Opcode::FPRoundEven32: f32_1([](float x) { return std::nearbyint(x); }); return;
    case Opcode::FPRoundEven64: f64_1([](double x) { return std::nearbyint(x); }); return;
    case Opcode::FPFloor16: f16_1([](float x) { return std::floor(x); }); return;
    case Opcode::FPFloor32: f32_1([](float x) { return std::floor(x); }); return;
    case Opcode::FPFloor64: f64_1([](double x) { return std::floor(x); }); return;
    case Opcode::FPCeil16: f16_1([](float x) { return std::ceil(x); }); return;
    case Opcode::FPCeil32: f32_1([](float x) { return std::ceil(x); }); return;
    case Opcode::FPCeil64: f64_1([](double x) { return std::ceil(x); }); return;
    case Opcode::FPTrunc16: f16_1([](float x) { return std::trunc(x); }); return;
    case Opcode::FPTrunc32: f32_1([](float x) { return std::trunc(x); }); return;
    case Opcode::FPTrunc64: f64_1([](double x) { return std::trunc(x); }); return;
#define FCMP(name, expr)                                                                   \
    case Opcode::name##16: f16_cmp([](auto x, auto y) { return expr; }); return;          \
    case Opcode::name##32: f32_cmp([](auto x, auto y) { return expr; }); return;          \
    case Opcode::name##64: f64_cmp([](auto x, auto y) { return expr; }); return;
        FCMP(FPOrdEqual, x == y)
        FCMP(FPUnordEqual, x == y || std::isunordered(x, y))
        FCMP(FPOrdNotEqual, x != y && !std::isunordered(x, y))
        FCMP(FPUnordNotEqual, x != y)
        FCMP(FPOrdLessThan, x < y)
        FCMP(FPUnordLessThan, !(x >= y))
        FCMP(FPOrdGreaterThan, x > y)
        FCMP(FPUnordGreaterThan, !(x <= y))
        FCMP(FPOrdLessThanEqual, x <= y)
        FCMP(FPUnordLessThanEqual, !(x > y))
        FCMP(FPOrdGreaterThanEqual, x >= y)
        FCMP(FPUnordGreaterThanEqual, !(x < y))
#undef FCMP
    case Opcode::FPIsNan16: EACH R(op.dst, 0, l) = std::isnan(in16(a(op, 0, l))); return;
    case Opcode::FPIsNan32: EACH R(op.dst, 0, l) = std::isnan(bitsf(a32(op, 0, l))); return;
    case Opcode::FPIsNan64: EACH R(op.dst, 0, l) = std::isnan(bitsd(a(op, 0, l))); return;

    // ---- integers ----
    case Opcode::IAdd32:
        EACH {
            const u32 x = a32(op, 0, l), y = a32(op, 1, l), r = x + y;
            R(op.dst, 0, l) = r;
            bit_flags(op, (int)l, r);
            if (op.carry != NONE) R(op.carry, 0, l) = r < x;
            if (op.overflow != NONE) R(op.overflow, 0, l) = (s32)x >= 0 ? (s32)y > (s32)(0x7fffffffu - x) : (s32)y < (s32)(0x7fffffffu - x);
        }
        return;
    case Opcode::IAdd64: u64_2([](u64 x, u64 y) { return x + y; }); return;
    case Opcode::ISub32: u32_2([](u32 x, u32 y) { return x - y; }); return;
    case Opcode::ISub64: u64_2([](u64 x, u64 y) { return x - y; }); return;
    case Opcode::IMul32: u32_2([](u32 x, u32 y) { return x * y; }); return;
    case Opcode::SDiv32: u32_2([](u32 x, u32 y) -> u32 {
            if (y == 0) return (s32)x < 0 ? 1u : ~0u;
            if (x == 0x80000000u && y == ~0u) return x;
            return (u32)((s32)x / (s32)y);
        });
        return;
    case Opcode::UDiv32: u32_2([](u32 x, u32 y) { return y ? x / y : ~0u; }); return;
    case Opcode::INeg32: u32_1([](u32 x) { return 0u - x; }); return;
    case Opcode::INeg64: EACH R(op.dst, 0, l) = 0ull - a(op, 0, l); return;
    case Opcode::IAbs32: u32_1([](u32 x) { return (s32)x < 0 ? 0u - x : x; }); return;
    case Opcode::IAbs64: EACH R(op.dst, 0, l) = (s64)a(op, 0, l) < 0 ? 0ull - a(op, 0, l) : a(op, 0, l); return;
    case Opcode::ShiftLeftLogical32: u32_2([](u32 x, u32 s) { return x << (s & 31); }); return;
    case Opcode::ShiftLeftLogical64: EACH R(op.dst, 0, l) = a(op, 0, l) << (a32(op, 1, l) & 63); return;
    case Opcode::ShiftRightLogical32: u32_2([](u32 x, u32 s) { return x >> (s & 31); }); return;
    case Opcode::ShiftRightLogical64: EACH R(op.dst, 0, l) = a(op, 0, l) >> (a32(op, 1, l) & 63); return;
    case Opcode::ShiftRightArithmetic32: u32_2([](u32 x, u32 s) { return (u32)((s32)x >> (s & 31)); }); return;
    case Opcode::ShiftRightArithmetic64: EACH R(op.dst, 0, l) = (u64)((s64)a(op, 0, l) >> (a32(op, 1, l) & 63)); return;
    case Opcode::BitwiseAnd32: EACH { const u32 r = a32(op, 0, l) & a32(op, 1, l); R(op.dst, 0, l) = r; bit_flags(op, (int)l, r); } return;
    case Opcode::BitwiseOr32: EACH { const u32 r = a32(op, 0, l) | a32(op, 1, l); R(op.dst, 0, l) = r; bit_flags(op, (int)l, r); } return;
    case Opcode::BitwiseXor32: EACH { const u32 r = a32(op, 0, l) ^ a32(op, 1, l); R(op.dst, 0, l) = r; bit_flags(op, (int)l, r); } return;
    case Opcode::BitFieldInsert: EACH R(op.dst, 0, l) = bfi(a32(op, 0, l), a32(op, 1, l), a32(op, 2, l), a32(op, 3, l)); return;
    case Opcode::BitFieldSExtract: EACH { const u32 r = sbfe(a32(op, 0, l), a32(op, 1, l), a32(op, 2, l)); R(op.dst, 0, l) = r; bit_flags(op, (int)l, r); } return;
    case Opcode::BitFieldUExtract: EACH { const u32 r = ubfe(a32(op, 0, l), a32(op, 1, l), a32(op, 2, l)); R(op.dst, 0, l) = r; bit_flags(op, (int)l, r); } return;
    case Opcode::BitReverse32: u32_1([](u32 x) {
            u32 r = 0;
            for (int i = 0; i < 32; i++) r |= (x >> i & 1) << (31 - i);
            return r;
        });
        return;
    case Opcode::BitCount32: u32_1([](u32 x) { return (u32)std::popcount(x); }); return;
    case Opcode::BitwiseNot32: u32_1([](u32 x) { return ~x; }); return;
    case Opcode::FindSMsb32: u32_1([](u32 x) { const u32 y = (s32)x < 0 ? ~x : x; return y ? 31u - (u32)std::countl_zero(y) : ~0u; }); return;
    case Opcode::FindUMsb32: u32_1([](u32 x) { return x ? 31u - (u32)std::countl_zero(x) : ~0u; }); return;
    case Opcode::SMin32: u32_2([](u32 x, u32 y) { return (u32)std::min((s32)x, (s32)y); }); return;
    case Opcode::UMin32: u32_2([](u32 x, u32 y) { return std::min(x, y); }); return;
    case Opcode::SMax32: u32_2([](u32 x, u32 y) { return (u32)std::max((s32)x, (s32)y); }); return;
    case Opcode::UMax32: u32_2([](u32 x, u32 y) { return std::max(x, y); }); return;
    case Opcode::SClamp32: EACH { const u32 r = (u32)std::min(std::max((s32)a32(op, 0, l), (s32)a32(op, 1, l)), (s32)a32(op, 2, l)); R(op.dst, 0, l) = r; bit_flags(op, (int)l, r); } return;
    case Opcode::UClamp32: EACH { const u32 r = std::min(std::max(a32(op, 0, l), a32(op, 1, l)), a32(op, 2, l)); R(op.dst, 0, l) = r; bit_flags(op, (int)l, r); } return;
    case Opcode::SLessThan: cmp([](u32 x, u32 y) { return (s32)x < (s32)y; }); return;
    case Opcode::ULessThan: cmp([](u32 x, u32 y) { return x < y; }); return;
    case Opcode::IEqual: cmp([](u32 x, u32 y) { return x == y; }); return;
    case Opcode::SLessThanEqual: cmp([](u32 x, u32 y) { return (s32)x <= (s32)y; }); return;
    case Opcode::ULessThanEqual: cmp([](u32 x, u32 y) { return x <= y; }); return;
    case Opcode::SGreaterThan: cmp([](u32 x, u32 y) { return (s32)x > (s32)y; }); return;
    case Opcode::UGreaterThan: cmp([](u32 x, u32 y) { return x > y; }); return;
    case Opcode::INotEqual: cmp([](u32 x, u32 y) { return x != y; }); return;
    case Opcode::SGreaterThanEqual: cmp([](u32 x, u32 y) { return (s32)x >= (s32)y; }); return;
    case Opcode::UGreaterThanEqual: cmp([](u32 x, u32 y) { return x >= y; }); return;
    case Opcode::LogicalOr: EACH R(op.dst, 0, l) = (a(op, 0, l) | a(op, 1, l)) & 1; return;
    case Opcode::LogicalAnd: EACH R(op.dst, 0, l) = (a(op, 0, l) & a(op, 1, l)) & 1; return;
    case Opcode::LogicalXor: EACH R(op.dst, 0, l) = (a(op, 0, l) ^ a(op, 1, l)) & 1; return;
    case Opcode::LogicalNot: EACH R(op.dst, 0, l) = !a(op, 0, l); return;

    // ---- conversions ----
    case Opcode::ConvertS16F16: EACH R(op.dst, 0, l) = sext((u32)f2i(in16(a(op, 0, l))) & 0xffff, 16); return;
    case Opcode::ConvertS16F32: EACH R(op.dst, 0, l) = sext((u32)f2i(in32(a(op, 0, l))) & 0xffff, 16); return;
    case Opcode::ConvertS16F64: EACH R(op.dst, 0, l) = sext((u32)f2i(in64(a(op, 0, l))) & 0xffff, 16); return;
    case Opcode::ConvertS32F16: EACH R(op.dst, 0, l) = (u32)f2i(in16(a(op, 0, l))); return;
    case Opcode::ConvertS32F32: EACH R(op.dst, 0, l) = (u32)f2i(in32(a(op, 0, l))); return;
    case Opcode::ConvertS32F64: EACH R(op.dst, 0, l) = (u32)f2i(in64(a(op, 0, l))); return;
    case Opcode::ConvertS64F16: EACH R(op.dst, 0, l) = (u64)f2i64(in16(a(op, 0, l))); return;
    case Opcode::ConvertS64F32: EACH R(op.dst, 0, l) = (u64)f2i64(in32(a(op, 0, l))); return;
    case Opcode::ConvertS64F64: EACH R(op.dst, 0, l) = (u64)f2i64(in64(a(op, 0, l))); return;
    case Opcode::ConvertU16F16: EACH R(op.dst, 0, l) = f2u(in16(a(op, 0, l))) & 0xffff; return;
    case Opcode::ConvertU16F32: EACH R(op.dst, 0, l) = f2u(in32(a(op, 0, l))) & 0xffff; return;
    case Opcode::ConvertU16F64: EACH R(op.dst, 0, l) = f2u(in64(a(op, 0, l))) & 0xffff; return;
    case Opcode::ConvertU32F16: EACH R(op.dst, 0, l) = f2u(in16(a(op, 0, l))); return;
    case Opcode::ConvertU32F32: EACH R(op.dst, 0, l) = f2u(in32(a(op, 0, l))); return;
    case Opcode::ConvertU32F64: EACH R(op.dst, 0, l) = f2u(in64(a(op, 0, l))); return;
    case Opcode::ConvertU64F16: EACH R(op.dst, 0, l) = f2u64(in16(a(op, 0, l))); return;
    case Opcode::ConvertU64F32: EACH R(op.dst, 0, l) = f2u64(in32(a(op, 0, l))); return;
    case Opcode::ConvertU64F64: EACH R(op.dst, 0, l) = f2u64(in64(a(op, 0, l))); return;
    case Opcode::ConvertU64U32: EACH R(op.dst, 0, l) = a(op, 0, l) & 0xffffffffu; return;
    case Opcode::ConvertU32U64: EACH R(op.dst, 0, l) = a(op, 0, l) & 0xffffffffu; return;
    case Opcode::ConvertF16F32:
        EACH {
            const u64 r = out16(in32(a(op, 0, l)));
            R(op.dst, 0, l) = (r & 0x7fff) > 0x7c00 ? 0 : r;   // (the fork's select: a NaN result is 0)
        }
        return;
    case Opcode::ConvertF32F16: EACH R(op.dst, 0, l) = out32(in16(a(op, 0, l))); return;
    case Opcode::ConvertF32F64: EACH R(op.dst, 0, l) = out32((float)in64(a(op, 0, l))); return;
    case Opcode::ConvertF64F32: EACH R(op.dst, 0, l) = out64((double)in32(a(op, 0, l))); return;
    case Opcode::ConvertF16S8: EACH R(op.dst, 0, l) = out16d((double)(s32)sext(a32(op, 0, l) & 0xff, 8)); return;
    case Opcode::ConvertF16S16: EACH R(op.dst, 0, l) = out16d((double)(s32)sext(a32(op, 0, l) & 0xffff, 16)); return;
    case Opcode::ConvertF16S32: EACH R(op.dst, 0, l) = out16d((double)(s32)a32(op, 0, l)); return;
    case Opcode::ConvertF16S64: EACH R(op.dst, 0, l) = out16((float)(s64)a(op, 0, l)); return;
    case Opcode::ConvertF16U8: EACH R(op.dst, 0, l) = out16d((double)(a32(op, 0, l) & 0xff)); return;
    case Opcode::ConvertF16U16: EACH R(op.dst, 0, l) = out16d((double)(a32(op, 0, l) & 0xffff)); return;
    case Opcode::ConvertF16U32: EACH R(op.dst, 0, l) = out16d((double)a32(op, 0, l)); return;
    case Opcode::ConvertF16U64: EACH R(op.dst, 0, l) = out16((float)a(op, 0, l)); return;
    case Opcode::ConvertF32S8: EACH R(op.dst, 0, l) = out32((float)(s32)sext(a32(op, 0, l) & 0xff, 8)); return;
    case Opcode::ConvertF32S16: EACH R(op.dst, 0, l) = out32((float)(s32)sext(a32(op, 0, l) & 0xffff, 16)); return;
    case Opcode::ConvertF32S32: EACH R(op.dst, 0, l) = out32((float)(s32)a32(op, 0, l)); return;
    case Opcode::ConvertF32S64: EACH R(op.dst, 0, l) = out32((float)(s64)a(op, 0, l)); return;
    case Opcode::ConvertF32U8: EACH R(op.dst, 0, l) = out32((float)(a32(op, 0, l) & 0xff)); return;
    case Opcode::ConvertF32U16: EACH R(op.dst, 0, l) = out32((float)(a32(op, 0, l) & 0xffff)); return;
    case Opcode::ConvertF32U32: EACH R(op.dst, 0, l) = out32((float)a32(op, 0, l)); return;
    case Opcode::ConvertF32U64: EACH R(op.dst, 0, l) = out32((float)a(op, 0, l)); return;
    case Opcode::ConvertF64S8: EACH R(op.dst, 0, l) = out64((double)(s32)sext(a32(op, 0, l) & 0xff, 8)); return;
    case Opcode::ConvertF64S16: EACH R(op.dst, 0, l) = out64((double)(s32)sext(a32(op, 0, l) & 0xffff, 16)); return;
    case Opcode::ConvertF64S32: EACH R(op.dst, 0, l) = out64((double)(s32)a32(op, 0, l)); return;
    case Opcode::ConvertF64S64: EACH R(op.dst, 0, l) = out64((double)(s64)a(op, 0, l)); return;
    case Opcode::ConvertF64U8: EACH R(op.dst, 0, l) = out64((double)(a32(op, 0, l) & 0xff)); return;
    case Opcode::ConvertF64U16: EACH R(op.dst, 0, l) = out64((double)(a32(op, 0, l) & 0xffff)); return;
    case Opcode::ConvertF64U32: EACH R(op.dst, 0, l) = out64((double)a32(op, 0, l)); return;
    case Opcode::ConvertF64U64: EACH R(op.dst, 0, l) = out64((double)a(op, 0, l)); return;
    case Opcode::ConvertU16U32: EACH R(op.dst, 0, l) = a32(op, 0, l) & 0xffff; return;
    case Opcode::ConvertU32U16: EACH R(op.dst, 0, l) = a32(op, 0, l) & 0xffff; return;
    case Opcode::ConvertU8U32: EACH R(op.dst, 0, l) = a32(op, 0, l) & 0xff; return;
    case Opcode::ConvertU32U8: EACH R(op.dst, 0, l) = a32(op, 0, l) & 0xff; return;
    case Opcode::ConvertS32S8: EACH R(op.dst, 0, l) = sext(a32(op, 0, l) & 0xff, 8); return;
    case Opcode::ConvertS32S16: EACH R(op.dst, 0, l) = sext(a32(op, 0, l) & 0xffff, 16); return;

    // ---- textures ----
    case Opcode::ImageSampleImplicitLod: case Opcode::ImageSampleExplicitLod: case Opcode::ImageGather: case Opcode::ImageFetch: case Opcode::ImageGradient:
        EACH texel(op, (int)l, 4, false);
        return;
    case Opcode::ImageGatherDref: EACH texel(op, (int)l, 4, true); return;
    case Opcode::ImageSampleDrefImplicitLod: case Opcode::ImageSampleDrefExplicitLod: EACH texel(op, (int)l, 1, true); return;
    case Opcode::ImageQueryDimensions:
        EACH {
            const u64 k = h(seed, op.key, 0xd1), lod = a32(op, 1, l);
            const u32 w = 16u << (k % 6), ht = 16u << ((k >> 3) % 6), d = 1 + (u32)(k >> 6 & 3), levels = 1 + (u32)(k >> 9 & 7);
            R(op.dst, 0, l) = std::max(1u, lod < 32 ? w >> lod : 0), R(op.dst, 1, l) = std::max(1u, lod < 32 ? ht >> lod : 0);
            R(op.dst, 2, l) = d, R(op.dst, 3, l) = a(op, 2, l) ? 0 : levels;
        }
        return;
    case Opcode::ImageQueryLod:
        EACH {
            const u64 x = texel_hash(op, (int)l, 1, 0);
            R(op.dst, 0, l) = R(op.dst, 1, l) = fbits((float)(x % 64) / 8.0f), R(op.dst, 2, l) = R(op.dst, 3, l) = 0;
        }
        return;
    case Opcode::ImageRead:
        EACH {
            const u64 at = coord_hash(op, (int)l);
            for (int k = 0; k < 4; k++) R(op.dst, k, l) = load(h('I', op.key), (at * 4 + (u64)k) * 4, 4);
            if (op.sparse != NONE) R(op.sparse, 0, l) = 1;
        }
        return;
    case Opcode::ImageWrite:
        EACH {
            if (!writes((int)l)) continue;
            const u64 at = coord_hash(op, (int)l);
            for (int k = 0; k < 4; k++) store(h('I', op.key), (at * 4 + (u64)k) * 4, 4, a32(op, 2, l, k), (int)l, "image");
        }
        return;

    // ---- warp ----
    case Opcode::LaneId: EACH R(op.dst, 0, l) = l; return;
    case Opcode::VoteAll: case Opcode::VoteAny: case Opcode::VoteEqual: {
        u32 ballot = 0;
        EACH if (a(op, 0, l)) ballot |= 1u << l;
        const bool r = o == Opcode::VoteAll ? ballot == mask : o == Opcode::VoteAny ? ballot != 0 : (ballot == 0 || ballot == mask);
        EACH R(op.dst, 0, l) = r;
        return;
    }
    case Opcode::SubgroupBallot: {
        u32 ballot = 0;
        EACH if (a(op, 0, l)) ballot |= 1u << l;
        EACH R(op.dst, 0, l) = ballot;
        return;
    }
    case Opcode::SubgroupEqMask: EACH R(op.dst, 0, l) = 1u << l; return;
    case Opcode::SubgroupLtMask: EACH R(op.dst, 0, l) = (1u << l) - 1; return;
    case Opcode::SubgroupLeMask: EACH R(op.dst, 0, l) = (u32)((2ull << l) - 1); return;
    case Opcode::SubgroupGtMask: EACH R(op.dst, 0, l) = ~(u32)((2ull << l) - 1); return;
    case Opcode::SubgroupGeMask: EACH R(op.dst, 0, l) = ~((1u << l) - 1); return;
    case Opcode::ShuffleIndex: case Opcode::ShuffleUp: case Opcode::ShuffleDown: case Opcode::ShuffleButterfly: {
        const u32 vs = A(op, 0).slot;
        std::array<u64, LANES> out{};
        std::array<bool, LANES> ok{};
        EACH {
            const u32 index = a32(op, 1, l), clamp = a32(op, 2, l), seg = a32(op, 3, l);
            const u32 min_id = l & seg, max_id = min_id | (clamp & ~seg);
            s32 src;
            bool in_range;
            if (o == Opcode::ShuffleIndex) {
                src = (s32)(((index & 31) & ~seg) | min_id);
                in_range = src <= (s32)max_id;
            } else if (o == Opcode::ShuffleUp) {
                src = (s32)l - (s32)(index & 31);
                in_range = src >= (s32)max_id;
            } else if (o == Opcode::ShuffleDown) {
                src = (s32)l + (s32)(index & 31);
                in_range = src <= (s32)max_id;
            } else {
                src = (s32)(l ^ (index & 31));
                in_range = src <= (s32)max_id;
            }
            ok[l] = in_range;
            out[l] = in_range ? from(vs, src, mask) : R(vs, 0, l);
        }
        EACH {
            R(op.dst, 0, l) = out[l];
            if (op.in_bounds != NONE) R(op.in_bounds, 0, l) = ok[l];
        }
        return;
    }
    case Opcode::SubgroupReadFirstLane: {
        const u64 x = mask ? R(A(op, 0).slot, 0, std::countr_zero(mask)) : 0;
        EACH R(op.dst, 0, l) = x & 0xffffffffu;
        return;
    }
    case Opcode::SubgroupReduceIAdd: case Opcode::SubgroupReduceUMin: case Opcode::SubgroupReduceUMax: case Opcode::SubgroupReduceSMin:
    case Opcode::SubgroupReduceSMax: case Opcode::SubgroupReduceAnd: case Opcode::SubgroupReduceOr: case Opcode::SubgroupReduceXor: {
        bool first = true;
        u32 r = 0;
        EACH {
            const u32 x = a32(op, 0, l);
            if (first) r = x, first = false;
            else if (o == Opcode::SubgroupReduceIAdd) r += x;
            else if (o == Opcode::SubgroupReduceUMin) r = std::min(r, x);
            else if (o == Opcode::SubgroupReduceUMax) r = std::max(r, x);
            else if (o == Opcode::SubgroupReduceSMin) r = (u32)std::min((s32)r, (s32)x);
            else if (o == Opcode::SubgroupReduceSMax) r = (u32)std::max((s32)r, (s32)x);
            else if (o == Opcode::SubgroupReduceAnd) r &= x;
            else if (o == Opcode::SubgroupReduceOr) r |= x;
            else r ^= x;
        }
        EACH R(op.dst, 0, l) = r;
        return;
    }
    case Opcode::QuadBroadcast: case Opcode::QuadSwap: {
        std::array<u64, LANES> out{};
        EACH {
            const u32 x = a32(op, 1, l);
            const int src = o == Opcode::QuadBroadcast ? (int)((l & ~3u) | (x & 3)) : (int)(l ^ (x + 1));
            out[l] = from(A(op, 0).slot, src, mask);
        }
        EACH R(op.dst, 0, l) = out[l];
        return;
    }
    case Opcode::FSwizzleAdd: {
        static constexpr float lut_a[4]{-1.0f, 1.0f, -1.0f, 0.0f}, lut_b[4]{-1.0f, -1.0f, 1.0f, -1.0f};
        EACH {
            const u32 sel = (a32(op, 2, l) >> ((l & 3) * 2)) & 3;
            const float x = bitsf((u32)out32(in32(a(op, 0, l)) * lut_a[sel])), y = bitsf((u32)out32(in32(a(op, 1, l)) * lut_b[sel]));
            R(op.dst, 0, l) = out32(in32(fbits(x)) + in32(fbits(y)));
        }
        return;
    }
    case Opcode::DPdxFine: case Opcode::DPdyFine: case Opcode::DPdxCoarse: case Opcode::DPdyCoarse: {
        const u32 vs = A(op, 0).slot;
        std::array<u64, LANES> out{};
        EACH {
            const int quad = (int)(l & ~3u), x = (int)(l & 1), y = (int)(l >> 1 & 1);
            const bool dx = o == Opcode::DPdxFine || o == Opcode::DPdxCoarse, fine = o == Opcode::DPdxFine || o == Opcode::DPdyFine;
            const int row = fine ? y : 0, col = fine ? x : 0;
            const int p0 = dx ? quad + row * 2 : quad + col, p1 = dx ? p0 + 1 : p0 + 2;
            out[l] = out32(in32(from(vs, p1, mask)) - in32(from(vs, p0, mask)));
        }
        EACH R(op.dst, 0, l) = out[l];
        return;
    }
    case Opcode::MatrixMulAddF16: mma(op, mask); return;

    default: break;
    }

    // ---- atomics ----
    const auto image_addr = [&](int l) { return coord_hash(op, l) * 4; };
    if (o >= Opcode::SharedAtomicIAdd32 && o <= Opcode::SharedAtomicXor32 || o == Opcode::SharedAtomicExchange32) {
        atomic32(op, mask, kShared, [&](int l) { return (u64)a32(op, 0, l); }, 1, [&](u32 x, u32 y, int) { return atomic_op(o, x, y); });
        return;
    }
    if (o == Opcode::SharedAtomicExchange64 || o == Opcode::SharedAtomicExchange32x2) {
        atomic64(op, mask, kShared, [&](int l) { return (u64)a32(op, 0, l); }, 1, o == Opcode::SharedAtomicExchange32x2,
                 [&](u64 x, u64 y) { return atomic_op64(o, x, y); });
        return;
    }
    if (o >= Opcode::GlobalAtomicIAdd32 && o <= Opcode::GlobalAtomicExchange32) {
        atomic32(op, mask, kGlobal, [&](int l) { return a(op, 0, l); }, 1, [&](u32 x, u32 y, int) { return atomic_op(o, x, y); });
        return;
    }
    if (o == Opcode::GlobalAtomicCompareExchange32 || o == Opcode::StorageAtomicCompareExchange32) {
        const bool g = o == Opcode::GlobalAtomicCompareExchange32;
        const int cmp_arg = g ? 1 : 2;
        atomic32(op, mask, g ? kGlobal : op.key, [&](int l) { return g ? a(op, 0, l) : (u64)a32(op, 1, l); }, cmp_arg + 1,
                 [&](u32 x, u32 y, int l) { return x == a32(op, cmp_arg, (u32)l) ? y : x; });
        return;
    }
    if ((o >= Opcode::GlobalAtomicIAdd64 && o <= Opcode::GlobalAtomicExchange32x2)) {
        const bool pair = o >= Opcode::GlobalAtomicIAdd32x2;
        atomic64(op, mask, kGlobal, [&](int l) { return a(op, 0, l); }, 1, pair, [&](u64 x, u64 y) { return atomic_op64(o, x, y); });
        return;
    }
    if (o >= Opcode::StorageAtomicIAdd32 && o <= Opcode::StorageAtomicExchange32) {
        atomic32(op, mask, op.key, [&](int l) { return (u64)a32(op, 1, l); }, 2, [&](u32 x, u32 y, int) { return atomic_op(o, x, y); });
        return;
    }
    if (o >= Opcode::StorageAtomicIAdd64 && o <= Opcode::StorageAtomicExchange32x2) {
        const bool pair = o >= Opcode::StorageAtomicIAdd32x2;
        atomic64(op, mask, op.key, [&](int l) { return (u64)a32(op, 1, l); }, 2, pair, [&](u64 x, u64 y) { return atomic_op64(o, x, y); });
        return;
    }
    if (o == Opcode::GlobalAtomicAddF32 || o == Opcode::StorageAtomicAddF32) {
        const bool g = o == Opcode::GlobalAtomicAddF32;
        atomic32(op, mask, g ? kGlobal : op.key, [&](int l) { return g ? a(op, 0, l) : (u64)a32(op, 1, l); }, g ? 1 : 2,
                 [&](u32 x, u32 y, int) { return (u32)out32(in32(x) + in32(y)); });
        return;
    }
    if (o == Opcode::GlobalAtomicAddF16x2 || o == Opcode::GlobalAtomicMinF16x2 || o == Opcode::GlobalAtomicMaxF16x2 || o == Opcode::StorageAtomicAddF16x2 ||
        o == Opcode::StorageAtomicMinF16x2 || o == Opcode::StorageAtomicMaxF16x2) {
        const bool g = o == Opcode::GlobalAtomicAddF16x2 || o == Opcode::GlobalAtomicMinF16x2 || o == Opcode::GlobalAtomicMaxF16x2;
        const int va = g ? 1 : 2;
        for (u32 m = mask; m; m &= m - 1) {   // (the value is an F16x2 vector: packed here)
            const int l = std::countr_zero(m);
            const u64 base = g ? kGlobal : op.key, addr = (g ? a(op, 0, l) : (u64)a32(op, 1, l)) & ~3ull;
            const u32 old = load(base, addr, 4), val = (u32)(a(op, va, l, 0) & 0xffff) | (u32)(a(op, va, l, 1) & 0xffff) << 16;
            const u32 next = f16x2_op(o, old, val);
            if (writes(l)) {
                mem[{base, addr >> 2}] = next;
                atomics[{base, addr >> 2}].push_back(fmt::format("{}:{:08x}", IR::NameOf(o), val));
            }
            R(op.dst, 0, l) = old;
        }
        return;
    }
    if (o >= Opcode::ImageAtomicIAdd32 && o <= Opcode::ImageAtomicExchange32) {
        atomic32(op, mask, h('I', op.key), image_addr, 2, [&](u32 x, u32 y, int) { return atomic_op(o, x, y); });
        return;
    }
    throw Error(fmt::format("opcode {} not supported", IR::NameOf(o)));
}

/// MatrixMulAddF16 as the back end's software function (emit_spirv_warp.cpp DefineSoftwareMatrixFunction): the 32 lanes'
/// fragments through shuffles, FMA in f32, rounded to f16 after each 8 columns of K
void Machine::mma(const Op& op, u32 mask) {
    const u32 sa = A(op, 0).slot, sb = A(op, 1).slot, sc = A(op, 2).slot;
    const auto half = [](u32 w, int k) { return (float)bitsh((u16)(w >> (16 * k))); };
    const auto round_half = [](float f) { return std::isnan(f) ? f : (float)(f16)f; };
    std::array<std::array<u64, 2>, LANES> out{};
    EACH {
        float acc[4];
        for (int row = 0; row < 2; row++)
            for (int col = 0; col < 2; col++) acc[row * 2 + col] = half((u32)R(sc, row, l), col);
        const int row_base = (int)(l & 28), col_base = (int)((l & 3) << 3);
        for (int pair = 0; pair < 8; pair++) {
            const bool upper = pair >= 4;
            const int component = pair & 3, a_lane = row_base + component, b_lane = col_base + component;
            float bs[2][2];
            for (int col = 0; col < 2; col++) {
                const u32 bw = (u32)from(sb, b_lane + col * 4, mask, upper ? 1 : 0);
                bs[col][0] = half(bw, 0), bs[col][1] = half(bw, 1);
            }
            for (int row = 0; row < 2; row++) {
                const u32 aw = (u32)from(sa, a_lane, mask, upper ? row + 2 : row);
                for (int col = 0; col < 2; col++) {
                    float x = acc[row * 2 + col];
                    for (int k = 0; k < 2; k++) x = std::fma(half(aw, k), bs[col][k], x);
                    acc[row * 2 + col] = x;
                }
            }
            if (component == 3)
                for (float& x : acc) x = round_half(x);
        }
        for (int row = 0; row < 2; row++) {
            u32 w = 0;
            for (int col = 0; col < 2; col++) {
                const float f = acc[row * 2 + col];
                w |= (u32)(std::isnan(f) ? 0x7e00u : hbits((f16)f)) << (16 * col);
            }
            out[l][row] = w;
        }
    }
    EACH R(op.dst, 0, l) = out[l][0], R(op.dst, 1, l) = out[l][1];
}

void Machine::finish() {
    auto& st = trace.state;
    std::string s;
    for (const auto& [k, x] : outputs) s += fmt::format("{}:{:x}={:08x} ", k.first, k.second, x);
    if (!s.empty()) st["out"] = std::move(s);
    s.clear();
    for (const auto& [k, x] : frag)
        if (writes(k.first)) s += fmt::format("{}:{}={:08x} ", k.first, k.second, x);
    if (!s.empty()) st["frag"] = std::move(s);
    std::map<u64, std::string> spaces;
    std::vector<std::pair<MemKey, u32>> words(mem.begin(), mem.end());
    std::sort(words.begin(), words.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    for (const auto& [k, x] : words) {
        bool local = false;
        for (int l = 0; l < LANES && !local; l++) local = k.base == local_base(l);
        if (local) continue;   // (private scratch: not a side effect)
        spaces[k.base] += fmt::format("{:x}={:08x} ", k.addr, x);
    }
    for (const auto& [base, text] : spaces) {
        const char* name = base == kGlobal ? "mem.global" : base == kShared ? "mem.shared" : base == kIsbe ? "isbe.attr" : base == kIsbeMap ? "isbe.map" :
                           base == kIsbePayload ? "isbe.payload" : nullptr;
        st[name ? std::string(name) : fmt::format("mem.{:016x}", base)] = text;
    }
    s.clear();
    for (auto& [k, list] : atomics) {
        std::sort(list.begin(), list.end());
        s += fmt::format("{:x}:{:x}=", k.base, k.addr);
        for (const auto& e : list) s += e + ",";
        s += " ";
    }
    if (!s.empty()) st["atomic"] = std::move(s);
    if (demoted) st["discard"] = fmt::format("{:08x}", demoted);
    if (unreachable) st["unreachable"] = fmt::format("{:08x}", unreachable);
    if (trace.capped) st["capped"] = "1";
    s.clear();
    for (const auto& e : emits) s += e + "; ";
    if (!s.empty()) st["emit"] = std::move(s);
    u64 x = 0xcbf29ce484222325ull;
    for (const auto& [k, val] : st) {
        x = fnv(k.data(), k.size() + 1, x);
        x = fnv(val.data(), val.size() + 1, x);
    }
    trace.hash = x;
}

// (the ops the interpreter knows; anything else, the pre-pass register and flag ops and the bound and bindless image ops
// among them, is an error at compile)
const std::vector<Opcode>& known() {
    static const std::vector<Opcode> list = [] {
        std::vector<Opcode> l;
        const auto add = [&](Opcode first, Opcode last) {
            for (u32 i = (u32)first; i <= (u32)last; i++) l.push_back((Opcode)i);
        };
        add(Opcode::Phi, Opcode::Void);
        l.push_back(Opcode::ConditionRef);
        add(Opcode::Prologue, Opcode::DeviceMemoryBarrier);
        add(Opcode::GetCbufU8, Opcode::SetFragDepth);
        add(Opcode::WorkgroupId, Opcode::RenderArea);
        add(Opcode::UndefU1, Opcode::UnpackDouble2x32);
        add(Opcode::GetZeroFromOp, Opcode::GetInBoundsFromOp);
        add(Opcode::FPAbs16, Opcode::StorageAtomicExchange32x2);
        for (Opcode o : {Opcode::GlobalAtomicAddF32, Opcode::GlobalAtomicAddF16x2, Opcode::GlobalAtomicMinF16x2, Opcode::GlobalAtomicMaxF16x2,
                         Opcode::StorageAtomicAddF32, Opcode::StorageAtomicAddF16x2, Opcode::StorageAtomicMinF16x2, Opcode::StorageAtomicMaxF16x2})
            l.push_back(o);
        add(Opcode::LogicalOr, Opcode::ConvertS32S16);
        add(Opcode::ImageSampleImplicitLod, Opcode::IsImageScaled);
        add(Opcode::ImageAtomicIAdd32, Opcode::DPdyCoarse);
        std::erase_if(l, [](Opcode o) {
            return o == Opcode::GetRegister || o == Opcode::SetRegister || o == Opcode::GetPred || o == Opcode::SetPred || o == Opcode::GetGotoVariable ||
                   o == Opcode::SetGotoVariable || o == Opcode::GetIndirectBranchVariable || o == Opcode::SetIndirectBranchVariable ||
                   (o >= Opcode::GetZFlag && o <= Opcode::SetOFlag) || o == Opcode::Reference || o == Opcode::PhiMove ||
                   (o >= Opcode::GlobalAtomicAddF32 && o <= Opcode::GlobalAtomicMaxF32x2 && o != Opcode::GlobalAtomicAddF32 &&
                    o != Opcode::GlobalAtomicAddF16x2 && o != Opcode::GlobalAtomicMinF16x2 && o != Opcode::GlobalAtomicMaxF16x2) ||
                   (o >= Opcode::StorageAtomicAddF32 && o <= Opcode::StorageAtomicMaxF32x2 && o != Opcode::StorageAtomicAddF32 &&
                    o != Opcode::StorageAtomicAddF16x2 && o != Opcode::StorageAtomicMinF16x2 && o != Opcode::StorageAtomicMaxF16x2);
        });
        std::sort(l.begin(), l.end());
        l.erase(std::unique(l.begin(), l.end()), l.end());
        return l;
    }();
    return list;
}
bool supported(Opcode o) { return std::binary_search(known().begin(), known().end(), o); }

}  // namespace

Trace run(const Compiled& c, const Inputs& in) { return Machine(c, in).run(); }

std::vector<std::string> supported_opcodes() {
    std::vector<std::string> out;
    for (Opcode o : known()) out.emplace_back(IR::NameOf(o));
    return out;
}

}  // namespace irexec
