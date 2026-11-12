// meshlab: the output dump (spv_dump.h).
#include "spv_dump.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>

namespace meshlab {
namespace {

enum : uint32_t {
    OpExtension = 10, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20,
    OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32,
    OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66,
    OpDecorate = 71, OpMemberDecorate = 72, OpCompositeExtract = 81, OpUConvert = 113, OpConvertUToPtr = 120, OpBitcast = 124,
    OpIAdd = 128, OpIMul = 132, OpSelect = 169, OpSetMeshOutputsEXT = 5295, OpExecutionModeId = 331,
};
enum : uint32_t { DecBuiltIn = 11, DecLocation = 30, DecPerPrimitive = 5271 };
enum : uint32_t { SCInput = 1, SCOutput = 3, SCPhysicalStorageBuffer = 5349 };
enum : uint32_t { BIPosition = 0, BIPointSize = 1, BIClip = 3, BICull = 4, BIPrimitiveId = 7, BILayer = 9, BIViewport = 10, BIWorkgroupId = 26,
                  BIPointIndices = 5294, BILineIndices = 5295, BITriangleIndices = 5296, BICullPrimitive = 5299 };
enum : uint32_t { CapInt64 = 11, CapPSB = 5347 };
enum : uint32_t { EmOutputVertices = 26, EmOutputPrimitives = 5270, EmLocalSize = 17 };

struct Inst {
    uint32_t op;
    std::vector<uint32_t> w;   // (operands, without word 0)
};

std::string builtin_name(uint32_t b) {
    switch (b) {
    case BIPosition: return "pos";
    case BIPointSize: return "psize";
    case BIClip: return "clip";
    case BICull: return "culld";
    case BIPrimitiveId: return "prim_id";
    case BILayer: return "layer";
    case BIViewport: return "viewport";
    case BIPointIndices: case BILineIndices: case BITriangleIndices: return "idx";
    case BICullPrimitive: return "cull";
    default: return "b" + std::to_string(b);
    }
}

struct Module {
    uint32_t header[5];
    std::vector<Inst> insts;
    std::map<uint32_t, const Inst*> def;   // (result id -> its instruction: types, constants, variables, access chains)
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> dec;              // (id -> (decoration, first operand))
    std::map<std::pair<uint32_t, uint32_t>, std::vector<std::pair<uint32_t, uint32_t>>> mdec;   // ((id, member) -> ...)

    bool parse(const std::vector<uint32_t>& in, std::string& err) {
        if (in.size() < 5 || in[0] != 0x07230203) return err = "not SPIR-V", false;
        std::memcpy(header, in.data(), sizeof header);
        for (size_t at = 5; at < in.size();) {
            const uint32_t n = in[at] >> 16, op = in[at] & 0xffff;
            if (!n || at + n > in.size()) return err = "bad instruction", false;
            insts.push_back({op, std::vector<uint32_t>(in.begin() + at + 1, in.begin() + at + n)});
            at += n;
        }
        for (const Inst& i : insts) {
            switch (i.op) {
            case OpTypeVoid: case OpTypeBool: case OpTypeInt: case OpTypeFloat: case OpTypeVector: case OpTypeArray: case OpTypeRuntimeArray:
            case OpTypeStruct: case OpTypePointer:
                def[i.w[0]] = &i;
                break;
            case OpConstant: case OpVariable: case OpAccessChain: case OpInBoundsAccessChain:
                def[i.w[1]] = &i;
                break;
            case OpDecorate:
                dec[i.w[0]].push_back({i.w[1], i.w.size() > 2 ? i.w[2] : 0});
                break;
            case OpMemberDecorate:
                mdec[{i.w[0], i.w[1]}].push_back({i.w[2], i.w.size() > 3 ? i.w[3] : 0});
                break;
            default: break;
            }
        }
        return true;
    }
    const Inst* d(uint32_t id) const {
        const auto it = def.find(id);
        return it == def.end() ? nullptr : it->second;
    }
    bool has_dec(uint32_t id, uint32_t what, uint32_t* val = nullptr) const {
        const auto it = dec.find(id);
        if (it == dec.end()) return false;
        for (const auto& [k, v] : it->second)
            if (k == what) return val ? (*val = v, true) : true;
        return false;
    }
    bool has_mdec(uint32_t id, uint32_t m, uint32_t what, uint32_t* val = nullptr) const {
        const auto it = mdec.find({id, m});
        if (it == mdec.end()) return false;
        for (const auto& [k, v] : it->second)
            if (k == what) return val ? (*val = v, true) : true;
        return false;
    }
    uint32_t constant(uint32_t id, bool& ok) const {
        const Inst* c = d(id);
        ok = c && c->op == OpConstant;
        return ok ? c->w[2] : 0;
    }
    /// One word per scalar
    uint32_t flat(uint32_t t) const {
        const Inst* i = d(t);
        if (!i) return 0;
        switch (i->op) {
        case OpTypeBool: return 1;
        case OpTypeInt: case OpTypeFloat: return i->w[1] == 64 ? 2 : 1;
        case OpTypeVector: return i->w[2] * flat(i->w[1]);
        case OpTypeArray: {
            bool ok;
            const uint32_t n = constant(i->w[2], ok);
            return ok ? n * flat(i->w[1]) : 0;
        }
        case OpTypeStruct: {
            uint32_t s = 0;
            for (size_t m = 1; m < i->w.size(); m++) s += flat(i->w[m]);
            return s;
        }
        default: return 0;
        }
    }
    bool is_float(uint32_t t) const {
        const Inst* i = d(t);
        while (i && i->op == OpTypeVector) i = d(i->w[1]);
        return i && i->op == OpTypeFloat;
    }
};

struct Builder {
    Module& m;
    uint32_t bound;
    std::vector<Inst> types;   // (new declarations, before the first function)
    std::vector<Inst> decs;    // (new decorations)
    std::vector<uint32_t> new_interface;
    bool need_int64 = false;
    uint32_t find_type(uint32_t op, const std::vector<uint32_t>& ops) {
        for (const Inst& i : m.insts)
            if (i.op == op && std::equal(ops.begin(), ops.end(), i.w.begin() + 1) && i.w.size() == ops.size() + 1) return i.w[0];
        for (const Inst& i : types)
            if (i.op == op && std::equal(ops.begin(), ops.end(), i.w.begin() + 1) && i.w.size() == ops.size() + 1) return i.w[0];
        const uint32_t id = bound++;
        std::vector<uint32_t> w{id};
        w.insert(w.end(), ops.begin(), ops.end());
        types.push_back({op, w});
        return id;
    }
    std::map<std::pair<uint32_t, uint64_t>, uint32_t> consts;
    uint32_t c32(uint32_t v) {
        const uint32_t t = find_type(OpTypeInt, {32, 0});
        if (const auto it = consts.find({t, v}); it != consts.end()) return it->second;
        const uint32_t id = bound++;
        types.push_back({OpConstant, {t, id, v}});
        return consts[{t, v}] = id;
    }
    uint32_t c64(uint64_t v) {
        need_int64 = true;
        const uint32_t t = find_type(OpTypeInt, {64, 0});
        if (const auto it = consts.find({t, v}); it != consts.end()) return it->second;
        const uint32_t id = bound++;
        types.push_back({OpConstant, {t, id, (uint32_t)v, (uint32_t)(v >> 32)}});
        return consts[{t, v}] = id;
    }
};

struct Leaf {
    std::vector<uint32_t> path;   // (CompositeExtract indices into the stored value)
    uint32_t type;                // (the scalar's type)
    uint32_t word;                // (its word within the stored value)
};
void leaves(const Module& m, uint32_t t, std::vector<uint32_t>& path, uint32_t& word, std::vector<Leaf>& out) {
    const Inst* i = m.d(t);
    if (!i) return;
    if (i->op == OpTypeVector || i->op == OpTypeArray || i->op == OpTypeStruct) {
        uint32_t n = 0;
        if (i->op == OpTypeVector) n = i->w[2];
        if (i->op == OpTypeArray) {
            bool ok;
            n = m.constant(i->w[2], ok);
        }
        if (i->op == OpTypeStruct) n = (uint32_t)i->w.size() - 1;
        for (uint32_t k = 0; k < n; k++) {
            path.push_back(k);
            leaves(m, i->op == OpTypeStruct ? i->w[1 + k] : i->w[1], path, word, out);
            path.pop_back();
        }
        return;
    }
    out.push_back({path, t, word});
    word += m.flat(t);
}

bool make_layout(const Module& m, OutLayout& L, std::string& err) {
    L = {};
    uint32_t at = 4;
    for (const Inst& i : m.insts) {
        if (i.op == OpExecutionMode && i.w.size() >= 3) {
            if (i.w[1] == EmOutputVertices) L.max_vertices = i.w[2];
            if (i.w[1] == EmOutputPrimitives) L.max_primitives = i.w[2];
            if (i.w[1] == EmLocalSize) L.local_size = i.w[2] * i.w[3] * i.w[4];
        }
        if (i.op != OpVariable || i.w[2] != SCOutput) continue;
        const Inst* pt = m.d(i.w[0]);
        const Inst* arr = pt ? m.d(pt->w[2]) : nullptr;
        if (!arr || arr->op != OpTypeArray) return err = "an output that is not an array", false;
        bool ok;
        OutVar v{};
        v.id = i.w[1];
        v.elements = m.constant(arr->w[2], ok);
        v.elem_words = m.flat(arr->w[1]);
        v.offset = at;
        uint32_t b = 0, loc = 0;
        v.per_primitive = m.has_dec(v.id, DecPerPrimitive) ||
                          (m.has_dec(v.id, DecBuiltIn, &b) && (b == BIPointIndices || b == BILineIndices || b == BITriangleIndices));
        const Inst* e = m.d(arr->w[1]);
        if (e && e->op == OpTypeStruct) {
            uint32_t word = 0;
            bool prim_block = false;
            for (size_t k = 1; k < e->w.size(); k++) {
                OutField f{};
                const uint32_t mt = e->w[k];
                if (m.has_mdec(e->w[0], (uint32_t)k - 1, DecBuiltIn, &b)) f.name = builtin_name(b);
                else f.name = "m" + std::to_string(k - 1);
                f.per_primitive = m.has_mdec(e->w[0], (uint32_t)k - 1, DecPerPrimitive) || b == BIPrimitiveId || b == BILayer || b == BIViewport || b == BICullPrimitive;
                prim_block |= f.per_primitive;
                f.word = word, f.words = m.flat(mt), f.is_float = m.is_float(mt);
                word += f.words;
                v.fields.push_back(f);
            }
            v.per_primitive |= prim_block;
        } else {
            OutField f{};
            if (m.has_dec(v.id, DecBuiltIn, &b)) f.name = builtin_name(b);
            else if (m.has_dec(v.id, DecLocation, &loc)) f.name = "loc" + std::to_string(loc);
            else f.name = "v" + std::to_string(v.id);
            f.per_primitive = v.per_primitive;
            f.word = 0, f.words = v.elem_words, f.is_float = m.is_float(arr->w[1]);
            v.fields.push_back(f);
        }
        at += v.elements * v.elem_words;
        L.vars.push_back(v);
    }
    L.record_words = (at + 63) & ~63u;
    return true;
}

}  // namespace

bool output_layout(const std::vector<uint32_t>& in, OutLayout& layout, std::string& err) {
    Module m;
    return m.parse(in, err) && make_layout(m, layout, err);
}

std::vector<uint32_t> add_output_dump(const std::vector<uint32_t>& in, uint64_t base, OutLayout& L, std::string& err) {
    Module m;
    if (!m.parse(in, err) || !make_layout(m, L, err)) return {};
    Builder b{m, m.header[3]};
    bool psb = false;
    for (const Inst& i : m.insts) psb |= i.op == OpCapability && i.w[0] == CapPSB;
    if (!psb) return err = "the module has no PhysicalStorageBufferAddresses", std::vector<uint32_t>{};
    const uint32_t tu32 = b.find_type(OpTypeInt, {32, 0}), tu64 = b.find_type(OpTypeInt, {64, 0});
    b.need_int64 = true;
    const uint32_t tv3 = b.find_type(OpTypeVector, {tu32, 3});
    const uint32_t tpsb = b.find_type(OpTypePointer, {SCPhysicalStorageBuffer, tu32});
    // (the workgroup id: the module's variable, or one of our own added to the interface)
    uint32_t wgvar = 0;
    for (const Inst& i : m.insts) {
        uint32_t bi;
        if (i.op == OpVariable && m.has_dec(i.w[1], DecBuiltIn, &bi) && bi == BIWorkgroupId) wgvar = i.w[1];
    }
    uint32_t wg_type = tv3;
    if (wgvar) {
        const Inst* pt = m.d(m.d(wgvar)->w[0]);
        wg_type = pt->w[2];
    } else {
        const uint32_t tp = b.find_type(OpTypePointer, {SCInput, tv3});
        wgvar = b.bound++;
        b.types.push_back({OpVariable, {tp, wgvar, SCInput}});
        b.decs.push_back({OpDecorate, {wgvar, DecBuiltIn, BIWorkgroupId}});
        b.new_interface.push_back(wgvar);
    }
    const uint32_t cbase = b.c64(base), cstride = b.c32(L.record_words), c4 = b.c32(4), c0 = b.c32(0), c1 = b.c32(1);
    std::map<uint32_t, const OutVar*> outvar;
    for (const OutVar& v : L.vars) outvar[v.id] = &v;

    // (an access chain's root Output variable and its indices, through chains of chains)
    std::function<bool(uint32_t, uint32_t&, std::vector<uint32_t>&)> root = [&](uint32_t id, uint32_t& var, std::vector<uint32_t>& idx) {
        const Inst* i = m.d(id);
        if (!i) return false;
        if (i->op == OpVariable) return outvar.count(id) ? (var = id, true) : false;
        if (i->op != OpAccessChain && i->op != OpInBoundsAccessChain) return false;
        if (!root(i->w[2], var, idx)) return false;
        idx.insert(idx.end(), i->w.begin() + 3, i->w.end());
        return true;
    };
    std::vector<Inst> out;
    out.reserve(m.insts.size() * 2);
    auto emit = [&](uint32_t op, std::vector<uint32_t> w) {
        out.push_back({op, std::move(w)});
        return out.back().w.size() > 1 ? out.back().w[1] : 0;
    };
    auto add = [&](uint32_t a, uint32_t c) { const uint32_t r = b.bound++; emit(OpIAdd, {tu32, r, a, c}); return r; };
    auto mul = [&](uint32_t a, uint32_t c) { const uint32_t r = b.bound++; emit(OpIMul, {tu32, r, a, c}); return r; };
    // (word w of this workgroup's record: its address, stored)
    auto store_word = [&](uint32_t word_id, uint32_t value_u32) {
        const uint32_t wv = b.bound++, wx = b.bound++;
        emit(OpLoad, {wg_type, wv, wgvar});
        emit(OpCompositeExtract, {tu32, wx, wv, 0});
        const uint32_t rec = mul(wx, cstride), w = add(rec, word_id), bytes = mul(w, c4);
        const uint32_t b64 = b.bound++, a64 = b.bound++, p = b.bound++;
        emit(OpUConvert, {tu64, b64, bytes});
        emit(OpIAdd, {tu64, a64, cbase, b64});
        emit(OpConvertUToPtr, {tpsb, p, a64});
        emit(OpStore, {p, value_u32, 0x2, 4});
    };
    int sites = 0;
    size_t decl_at = 0;
    size_t first_function = 0;
    for (size_t k = 0; k < m.insts.size(); k++)
        if (m.insts[k].op == OpFunction) { first_function = k; break; }
    size_t last_annotation = 0;
    for (size_t k = 0; k < m.insts.size(); k++)
        if (m.insts[k].op == OpDecorate || m.insts[k].op == OpMemberDecorate || m.insts[k].op == 332 /* OpDecorateId */ || m.insts[k].op == 5632)
            last_annotation = k;
    bool has_int64 = false;
    for (const Inst& i : m.insts) has_int64 |= i.op == OpCapability && i.w[0] == CapInt64;
    for (size_t k = 0; k < m.insts.size(); k++) {
        const Inst& i = m.insts[k];
        if (k == first_function) decl_at = out.size();   // (the new declarations go here, after the module's own, at the end)
        if (i.op == OpEntryPoint) {
            Inst e = i;
            for (uint32_t v : b.new_interface) e.w.push_back(v);
            out.push_back(e);
            continue;
        }
        if (i.op == OpCapability && k + 1 < m.insts.size() && m.insts[k + 1].op != OpCapability && !has_int64) {
            out.push_back(i);
            out.push_back({OpCapability, {CapInt64}});
            continue;
        }
        out.push_back(i);
        if (k == last_annotation)
            for (const Inst& d : b.decs) out.push_back(d);
        if (k < first_function) continue;
        if (i.op == OpSetMeshOutputsEXT) {
            store_word(c0, i.w[0]);
            store_word(c1, i.w[1]);
            sites++;
            continue;
        }
        if (i.op != OpStore) continue;
        uint32_t var = 0;
        std::vector<uint32_t> idx;
        if (!root(i.w[0], var, idx)) continue;
        const OutVar& v = *outvar[var];
        // (the stored value's type: the pointer's pointee)
        const Inst* ptr = m.d(i.w[0]);
        const uint32_t ptype = ptr->op == OpVariable ? ptr->w[0] : ptr->w[0];
        const uint32_t vtype = m.d(ptype)->w[2];
        // (the word offset of the chain within the variable: static part and dynamic terms)
        uint32_t t = m.d(m.d(m.d(var)->w[0])->w[2]) ? m.d(m.d(var)->w[0])->w[2] : 0;
        uint32_t off = b.c32(v.offset);
        for (uint32_t ix : idx) {
            const Inst* ti = m.d(t);
            if (!ti) break;
            if (ti->op == OpTypeStruct) {
                bool ok;
                const uint32_t mi = m.constant(ix, ok);
                uint32_t s = 0;
                for (uint32_t q = 0; q < mi; q++) s += m.flat(ti->w[1 + q]);
                off = add(off, b.c32(s));
                t = ti->w[1 + mi];
            } else {   // (array or vector: index times the element's words)
                const uint32_t et = ti->w[1];
                off = add(off, mul(ix, b.c32(m.flat(et))));
                t = et;
            }
        }
        std::vector<Leaf> ls;
        std::vector<uint32_t> path;
        uint32_t word = 0;
        leaves(m, vtype, path, word, ls);
        for (const Leaf& l : ls) {
            uint32_t val = i.w[1];
            if (!l.path.empty()) {
                const uint32_t r = b.bound++;
                std::vector<uint32_t> w{l.type, r, i.w[1]};
                w.insert(w.end(), l.path.begin(), l.path.end());
                emit(OpCompositeExtract, w);
                val = r;
            }
            const Inst* st = m.d(l.type);
            uint32_t u = val;
            if (st->op == OpTypeBool) {
                u = b.bound++;
                emit(OpSelect, {tu32, u, val, c1, c0});
            } else if (!(st->op == OpTypeInt && st->w[1] == 32 && st->w[2] == 0)) {
                u = b.bound++;
                emit(OpBitcast, {tu32, u, val});
            }
            store_word(l.word ? add(off, b.c32(l.word)) : off, u);
        }
        sites++;
    }
    if (!sites) return err = "no output store found", std::vector<uint32_t>{};
    out.insert(out.begin() + (ptrdiff_t)decl_at, b.types.begin(), b.types.end());
    std::vector<uint32_t> o(m.header, m.header + 5);
    o[3] = b.bound;
    for (const Inst& i : out) {
        o.push_back((uint32_t)(i.w.size() + 1) << 16 | i.op);
        o.insert(o.end(), i.w.begin(), i.w.end());
    }
    return o;
}

}  // namespace meshlab
