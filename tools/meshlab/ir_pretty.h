// meshlab: a structured, expression-inlined print of an Eden IR program (analysis only; from M0's task/mesh tool)
#include <map>
#include <sstream>
#include <fmt/format.h>
#include "shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "shader_recompiler/ir_opt/passes.h"
namespace pretty {
using namespace Shader::IR;
struct P {
    bool flat = false;   // (every value its own line but the uniform buffer reads: for reading the stage line by line)
    std::map<const Inst*, int> ids; std::map<const Block*, int> bids; int next = 0;
    std::string out;
    int id(const Inst* i) { auto it = ids.find(i); if (it != ids.end()) return it->second; return ids[i] = next++; }
    int bid(const Block* b) { auto it = bids.find(b); if (it != bids.end()) return it->second; int n = (int)bids.size(); return bids[b] = n; }
    bool inl(const Inst* i) {
        if (i->GetOpcode() == Opcode::Phi || i->MayHaveSideEffects()) return false;
        if (flat) {
            const Opcode o = i->GetOpcode();
            return o == Opcode::GetCbufU32 || o == Opcode::GetCbufF32 || o == Opcode::GetCbufU32x2 || o == Opcode::LaneId;
        }
        return i->UseCount() <= 1;
    }
    std::string val(const Value& v0, int depth = 0) {
        Value v = v0.Resolve();
        if (v.IsEmpty()) return "<null>";
        if (!v.IsImmediate()) {
            const Inst* i = v.InstRecursive();
            if (i->GetOpcode() == Opcode::ConditionRef) return val(i->Arg(0), depth);
            if (inl(i)) return expr(i, depth + 1);   // (no depth limit: an instruction left as an id would have no line of its own)
            return fmt::format("v{}", id(i));
        }
        switch (v.Type()) {
        case Type::U1: return v.U1() ? "true" : "false";
        case Type::U32: { u32 x = v.U32(); return x < 4096 ? fmt::format("{}", x) : fmt::format("{:#x}", x); }
        case Type::U64: return fmt::format("{:#x}", v.U64());
        case Type::F32: return fmt::format("{}f", v.F32());
        case Type::Attribute: return fmt::format("{}", v.Attribute());
        default: return "imm?";
        }
    }
    std::string expr(const Inst* i, int depth = 0) {
        std::string s{NameOf(i->GetOpcode())};
        s += "(";
        if (i->GetOpcode() == Opcode::Phi) {
            for (size_t a = 0; a < i->NumArgs(); a++) s += fmt::format("{}{}@B{}", a ? ", " : "", val(i->Arg(a), 0), bid(i->PhiBlock(a)));
        } else {
            for (size_t a = 0; a < i->NumArgs(); a++) s += (a ? ", " : "") + val(i->Arg(a), depth);
        }
        return s + ")";
    }
    void block(const Block* b, int ind) {
        std::string pad(ind * 2, ' ');
        out += fmt::format("{}// B{}\n", pad, bid(b));
        for (const Inst& i : b->Instructions()) {
            if (i.GetOpcode() == Opcode::Identity || i.GetOpcode() == Opcode::ConditionRef) continue;
            if (inl(&i) && i.UseCount() == 1) continue;
            if (!i.MayHaveSideEffects() && i.UseCount() == 0 && i.GetOpcode() != Opcode::Phi) continue;
            if (i.HasUses()) out += fmt::format("{}v{} = {}\n", pad, id(&i), expr(&i));
            else out += fmt::format("{}{}\n", pad, expr(&i));
        }
    }
    void program(const Program& p) {
        int ind = 0;
        for (const auto& n : p.syntax_list) {
            using T = AbstractSyntaxNode::Type;
            switch (n.type) {
            case T::Block: block(n.data.block, ind); break;
            case T::If: out += fmt::format("{}if ({}) {{\n", std::string(ind * 2, ' '), val(n.data.if_node.cond)); ind++; break;
            case T::EndIf: ind--; out += fmt::format("{}}}\n", std::string(ind * 2, ' ')); break;
            case T::Loop: out += fmt::format("{}loop {{   // LOOP\n", std::string(ind * 2, ' ')); ind++; break;
            case T::Repeat: ind--; out += fmt::format("{}}} while ({});\n", std::string(ind * 2, ' '), val(n.data.repeat.cond)); break;
            case T::Break: out += fmt::format("{}if ({}) break;\n", std::string(ind * 2, ' '), val(n.data.break_node.cond)); break;
            case T::Return: out += fmt::format("{}return;\n", std::string(ind * 2, ' ')); break;
            case T::Unreachable: out += fmt::format("{}unreachable;\n", std::string(ind * 2, ' ')); break;
            }
        }
    }
};
inline std::string print(Shader::IR::Program& p, bool flat = false) {
    Shader::Optimization::IdentityRemovalPass(p);
    Shader::Optimization::DeadCodeEliminationPass(p);
    P pr; pr.flat = flat; pr.program(p); return pr.out;
}
}
