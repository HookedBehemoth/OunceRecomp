"""Storage buffers through device addresses (RX_VK_DESC_HEAP_BDA, backend/spirv/bda_emit.h) make valid SPIR-V.

bda_emit.h's helpers are built on a bare Sirit module, as EmitContext uses them: a compute shader whose entry block
translates two guest addresses through the window table (Base, Bound), then loads 8-, 16-, 32-, 64- and 128-bit values,
stores them, and runs 32- and 64-bit atomics and a compare-exchange through those pointers (Make, Load, Store, Result).
Once plain, once with RX_VK_BDA_CHECK's clamp and its bda_flag function (DefineFlag); a Private local-memory struct of
one u32 sits beside the Block structs (Sirit merges equal declarations: the same struct must not get Block twice). Each
module must pass spirv-val for Vulkan 1.3. The translator's use of them is checked in the sources: the stages are picked
before any push block is made, the window table's member sits at WINDOW_TABLE_PUSH_OFFSET, the entry block makes the bases,
the storage loads, stores and atomics take the address path, and the CPU side skips read-only buffers of such stages.
Nothing runs on a GPU (work/proto/bda/bda_poc.c is the GPU's side).

    python -I -B tests/test_bda_emit.py --headers <SPIRV-Headers/include> [--cxx g++] [--spirv-val spirv-val]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPIRV = ROOT / "gpu" / "vk" / "eden" / "shader_recompiler" / "backend" / "spirv"

FIXTURE = r"""
#include <cstdint>
#include <cstdio>
#include <vector>
#include "bda_emit.h"
using namespace Shader::Backend::SPIRV;
using Sirit::Id;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const bool check = argv[2][0] == '1';
    Sirit::Module m{0x00010600};
    m.AddCapability(spv::Capability::Shader);
    m.AddCapability(spv::Capability::Int64);
    m.AddCapability(spv::Capability::Int8);
    m.AddCapability(spv::Capability::Int16);
    m.AddCapability(spv::Capability::Int64Atomics);
    m.AddCapability(spv::Capability::StorageBuffer8BitAccess);
    m.AddCapability(spv::Capability::StorageBuffer16BitAccess);
    m.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
    m.AddExtension("SPV_KHR_physical_storage_buffer");
    m.AddExtension("SPV_KHR_8bit_storage");
    m.AddExtension("SPV_KHR_16bit_storage");
    m.SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64, spv::MemoryModel::GLSL450);
    const Id void_t{m.TypeVoid()}, u1{m.TypeBool()}, u32{m.TypeInt(32, false)}, u64{m.TypeInt(64, false)};
    const Id u8{m.TypeInt(8, false)}, u16{m.TypeInt(16, false)};
    const Id u32x2{m.TypeVector(u32, 2)}, u32x4{m.TypeVector(u32, 4)};
    const Bda::Types t{u1, u32, u64, void_t};
    Bda::PtrCache ptrs;
    // (the push block: the window table's address and the draw's virtual range (Bda::Pushed), then two guest addresses with their
    // size words, as a constant buffer gives them)
    const Id pc_struct{m.TypeStruct(std::vector<Id>{u64, u64, u64, u64, u64, u64, u32, u32})};
    m.Decorate(pc_struct, spv::Decoration::Block);
    const std::uint32_t offs[] = {0, 8, 16, 24, 32, 40, 48, 52};
    for (std::uint32_t i = 0; i < 8; i++) m.MemberDecorate(pc_struct, i, spv::Decoration::Offset, offs[i]);
    const Id pc{m.AddGlobalVariable(m.TypePointer(spv::StorageClass::PushConstant, pc_struct), spv::StorageClass::PushConstant)};
    // (Eden's local memory: a Private array of a struct of one u32, as DefineLocalMemory makes it)
    const Id local_struct{m.TypeStruct(u32)};
    const Id local{m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Private, m.TypeArray(local_struct, m.Constant(u32, 4U))),
                                       spv::StorageClass::Private)};
    const Id flag{check ? Bda::DefineFlag(m, t, ptrs, 0x1234abcdU) : Id{}};
    // (RX_VK_DESC_HEAP_BDA=2: the CAS-loop atomics on a device address, as DefineStorageBuffers makes them: a u32 increment-like
    // loop, an f32 add (bitcast word) and an f32x2 add packed as two halves (GLSL.std.450))
    const Id f32{m.TypeFloat(32)}, f32x2{m.TypeVector(f32, 2)};
    const auto op_fn{[&](Id type, bool fp) {
        const Id fn{m.OpFunction(type, spv::FunctionControlMask::MaskNone, m.TypeFunction(type, type, type))};
        const Id a{m.OpFunctionParameter(type)}, b{m.OpFunctionParameter(type)};
        m.AddLabel();
        m.OpReturnValue(fp ? m.OpFAdd(type, a, b) : m.OpIAdd(type, a, b));
        m.OpFunctionEnd();
        return fn;
    }};
    const Id cas_u32{Bda::DefineCasLoop(m, t, ptrs, u32, op_fn(u32, false), false, 1024U)};
    const Id cas_f32{Bda::DefineCasLoop(m, t, ptrs, f32, op_fn(f32, true), false, 1024U)};
    const Id cas_f32x2{Bda::DefineCasLoop(m, t, ptrs, f32x2, op_fn(f32x2, true), true, 1024U)};
    const Id main{m.OpFunction(void_t, spv::FunctionControlMask::MaskNone, m.TypeFunction(void_t))};
    m.AddLabel();
    const auto member{[&](Id type, std::uint32_t i) {
        return m.OpLoad(type, m.OpAccessChain(m.TypePointer(spv::StorageClass::PushConstant, type), pc, m.Constant(u32, i)));
    }};
    const Bda::Pushed pushed{member(u64, 0), member(u64, 1), member(u64, 2), member(u64, 3)};
    const Id table{pushed.table};
    Id base[2], bound[2];
    for (std::uint32_t i = 0; i < 2; i++) {
        const Id guest{member(u64, 4 + i)};
        base[i] = Bda::Base(m, t, ptrs, pushed, guest, 4);
        bound[i] = check ? Bda::Bound(m, t, member(u32, 6 + i), guest, 4) : Id{};
    }
    const Id offset{m.OpBitwiseAnd(u32, m.OpUConvert(u32, member(u64, 4)), m.Constant(u32, 0xff0U))};
    // (a global access: the address translated as is, without a virtual range)
    const Id global{Bda::Translate(m, t, ptrs, Bda::Pushed{table}, m.OpIAdd(u64, member(u64, 5), m.Constant(u64, std::uint64_t{64})), 1)};
    const Id gword{m.OpLoad(u32, Bda::PointerAt(m, t, ptrs.Get(m, u32), global), spv::MemoryAccessMask::Aligned, 4U)};
    const auto access{[&](int b, Id type, std::uint32_t size) {
        const std::uint32_t align{size < 4 ? size : 4U};
        return check ? Bda::Make(m, t, ptrs, type, size, align, base[b], offset, table, bound[b], flag)
                     : Bda::Make(m, t, ptrs, type, size, align, base[b], offset);
    }};
    const Id v8{Bda::Load(m, access(0, u8, 1), u8)};
    const Id v16{Bda::Load(m, access(0, u16, 2), u16)};
    const Id v32{Bda::Load(m, access(0, u32, 4), u32)};
    const Id v64{Bda::Load(m, access(1, u32x2, 8), u32x2)};
    const Id v128{Bda::Load(m, access(1, u32x4, 16), u32x4)};
    Bda::Store(m, access(1, u8, 1), v8);
    Bda::Store(m, access(1, u16, 2), v16);
    Bda::Store(m, access(1, u32x2, 8), v64);
    Bda::Store(m, access(0, u32x4, 16), v128);
    const Id scope{m.Constant(u32, static_cast<std::uint32_t>(spv::Scope::Device))}, relaxed{m.Constant(u32, 0U)};
    const Bda::Access a32{access(0, u32, 4)};
    const Id old32{Bda::Result(m, a32, u32, m.OpAtomicIAdd(u32, a32.pointer, scope, relaxed, m.OpIAdd(u32, v32, gword)))};
    const Bda::Access a64{access(1, u64, 8)};
    const Id old64{Bda::Result(m, a64, u64, m.OpAtomicUMax(u64, a64.pointer, scope, relaxed, m.OpUConvert(u64, old32)))};
    const Bda::Access cas{access(0, u32, 4)};
    const Id swapped{Bda::Result(m, cas, u32, m.OpAtomicCompareExchange(u32, cas.pointer, scope, relaxed, relaxed,
                                                                         m.OpUConvert(u32, old64), v32))};
    m.OpStore(m.OpAccessChain(m.TypePointer(spv::StorageClass::Private, u32), local, m.Constant(u32, 0U), m.Constant(u32, 0U)),
              swapped);
    const Bda::Access ci{access(0, u32, 4)};
    const Id inc{Bda::Result(m, ci, u32, m.OpFunctionCall(u32, cas_u32, ci.address, swapped))};
    const Bda::Access cf{access(1, u32, 4)};
    const Id addf{Bda::Result(m, cf, f32, m.OpFunctionCall(f32, cas_f32, cf.address, m.OpBitcast(f32, inc)))};
    const Bda::Access ch{access(0, u32, 4)};
    const Id addh{Bda::Result(m, ch, f32x2, m.OpFunctionCall(f32x2, cas_f32x2, ch.address, m.OpCompositeConstruct(f32x2, addf, addf)))};
    m.OpStore(m.OpAccessChain(m.TypePointer(spv::StorageClass::Private, u32), local, m.Constant(u32, 1U), m.Constant(u32, 0U)),
              m.OpPackHalf2x16(u32, addh));
    m.OpReturn();
    m.OpFunctionEnd();
    m.AddEntryPoint(spv::ExecutionModel::GLCompute, main, "main", pc, local);
    m.AddExecutionMode(main, spv::ExecutionMode::LocalSize, 64U, 1U, 1U);
    const std::vector<std::uint32_t> code{m.Assemble()};
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 3;
    std::fwrite(code.data(), 4, code.size(), f);
    std::fclose(f);
    std::printf("module %s: %zu words\n", check ? "checked" : "plain", code.size());
    return 0;
}
"""


def need(text, what, why):
    if what not in text:
        raise SystemExit("FAIL: %s (%r not found)" % (why, what))


def sources():
    """The translator's and the renderer's use of bda_emit.h"""
    ctx = (SPIRV / "spirv_emit_context.cpp").read_text(encoding="utf-8")
    ctor = ctx[ctx.index("EmitContext::EmitContext("):]
    pick = ctor.index("BdaStage(profile, runtime_info, stage, program.info)")
    if pick > ctor.index("DefineRescalingInputPushConstant();"):
        raise SystemExit("FAIL: the stages are picked after a push block can be made (it would lack the table's member)")
    need(ctx, "MemberDecorate(push_constant_struct, window_table_member_index + i, spv::Decoration::Offset,\n"
         "                           WINDOW_TABLE_PUSH_OFFSET + 8 * i)", "the window table's members are not at WINDOW_TABLE_PUSH_OFFSET")
    rule = ctx[ctx.index("u32 BdaStage("):ctx.index("} // Anonymous namespace", ctx.index("u32 BdaStage("))]
    for cond in ("info.native_global_bounds", "info.uses_render_area", "d.count != 1", "0xff0U", "uses_global_increment",
                 "runtime_info.mesh_fallback"):
        need(rule, cond, "BdaStage no longer refuses what still needs descriptors")
    push = ctx[ctx.index("void EmitContext::DefineRescalingInputPushConstant()"):]
    push = push[:push.index("rescaling_push_constants = AddGlobalVariable")]
    if push.index("if (ssbo_bda) {") < push.index("mesh_fallback_indirect_input_member_index = member_index++;"):
        raise SystemExit("FAIL: the table's members come before the mesh fallback's (push block offsets must ascend)")
    body = ctx[ctx.index("const auto define_body{"):]
    if body.index("if (ssbo_bda) {") > body.index("const size_t num_buffers{"):
        raise SystemExit("FAIL: a by-address stage's global accesses search the bound buffers (they have no variables)")
    emit = (SPIRV / "emit_spirv.cpp").read_text(encoding="utf-8")
    need(emit, "ctx.EmitBdaPrologue();", "Traverse does not make the bases in the entry block")
    mem = (SPIRV / "emit_spirv_memory.cpp").read_text(encoding="utf-8")
    need(mem, "Bda::Load(ctx, ctx.BdaAccess(", "storage loads do not take the address path")
    need(mem, "Bda::Store(ctx, ctx.BdaAccess(", "storage stores do not take the address path")
    atomic = (SPIRV / "emit_spirv_atomic.cpp").read_text(encoding="utf-8")
    if atomic.count("if (ctx.ssbo_bda") < 6:
        raise SystemExit("FAIL: storage atomics lost their address paths")
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    need(draw, "if (info.ssbo_bda && !d.is_written) {", "build_descriptors does not skip a by-address stage's read-only buffers")
    need(draw, "constexpr uint32_t kPushSize = Shader::Backend::SPIRV::PUSH_CONSTANTS_END;", "the push range misses the table")
    need(draw, "const uint64_t words[4] = {table, bda_virt.lo, bda_virt.hi, bda_virt.delta};", "the table's address is not pushed")
    need(draw, "Shader::Backend::SPIRV::WINDOW_TABLE_PUSH_OFFSET, words, sizeof words", "the table's address is not pushed")
    need(draw, "p.uses_push |= st.info.ssbo_bda", "a by-address stage's push constants are not pushed")
    pools = (ROOT / "gpu" / "vk" / "vk_pools.cpp").read_text(encoding="utf-8")
    make = pools[pools.index("Window* window_make("):pools.index("struct One {")]
    if make.index("bda_window_made(k, w.address)") < make.index("w.address = vkGetBufferDeviceAddress"):
        raise SystemExit("FAIL: a window's table entry is written before its address is known")
    shader = (ROOT / "gpu" / "vk" / "vk_shader.cpp").read_text(encoding="utf-8")
    need(shader, '"RX_VK_DESC_HEAP_BDA", "RX_VK_BDA_"', "the switches are not in the program cache's stamp")
    wide(ctx, draw, shader)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def wide(ctx, draw, shader):
    """RX_VK_DESC_HEAP_BDA=2 (Profile::ssbo_bda_wide): the stages =1 leaves on descriptors for tables, the render area, CAS
    loops, native_global_bounds or a missing u32 view, by address; =1 picks the same stages as before"""
    emit_h = (SPIRV / "bda_emit.h").read_text(encoding="utf-8")
    allowed = emit_h[emit_h.index("constexpr std::uint32_t kWideAllowed ="):]
    allowed = allowed[:allowed.index(";")]
    for bit in ("kWhyRenderArea ", "kWhyNativeBounds", "kWhyCasLoop", "kWhyNoU32View", "kWhyTable"):
        need(allowed, bit.strip(), "kWideAllowed lacks a condition =2 translates")
    for bit in ("kWhyMeshFallback", "kWhyRenderAreaScale", "kWhyGlobalOff", "kWhyMeshFirst", "kWhyMissLog", "kWhyUnaligned",
                "kWhyUndeclared", "kWhyCaps"):
        if bit in allowed:
            raise SystemExit("FAIL: kWideAllowed takes %s, which has no by-address form" % bit)
    taken = block(ctx, "bool BdaTaken(")
    need(taken, "profile.ssbo_bda_wide ? Bda::kWideAllowed : 0U", "=1 no longer refuses every condition (the default path changed)")
    ctor = ctx[ctx.index("EmitContext::EmitContext("):]
    need(ctor[:ctor.index("DefineMesh(program);")], "program.info.bda_why = why;", "the refused stages do not keep their reasons")
    prologue = block(ctx, "void EmitContext::EmitBdaPrologue()")
    if prologue.index("if (d.count != 1)") > prologue.index("Bda::Base("):
        raise SystemExit("FAIL: a table gets a base (its words past the table's end are not buffers)")
    access = block(ctx, "Bda::Access EmitContext::BdaAccess(")
    need(access, "!Sirit::ValidId(bda_base[index])", "a storage access may index a table's missing base")
    sb = block(ctx, "void EmitContext::DefineStorageBuffers(")
    bda = sb[sb.index("if (ssbo_bda) {"):sb.index("IR::Type used_types{")]
    need(bda, "binding += desc.count > 1 ? 1 : desc.count;", "a by-address stage's tables take another number of bindings")
    for fn in ("increment_cas_ssbo", "decrement_cas_ssbo", "f32_add_cas", "f16x2_add_cas", "f16x2_min_cas", "f16x2_max_cas",
               "f32x2_add_cas", "f32x2_min_cas", "f32x2_max_cas"):
        need(bda, "cas(%s," % fn, "the by-address branch does not make %s" % fn)
    need(bda, "Bda::DefineCasLoop(", "the by-address CAS loops are not on device addresses")
    cas = block(emit_h, "inline Id DefineCasLoop(")
    for what in ("OpLoopMerge", "max_tries", "OpLogicalOr", "spv::MemoryAccessMask::Aligned"):
        need(cas, what, "Bda::DefineCasLoop lost its ceiling or its aligned load")
    atomic = (SPIRV / "emit_spirv_atomic.cpp").read_text(encoding="utf-8")
    for name in ("Inc32", "Dec32", "AddF32", "AddF16x2", "AddF32x2", "MinF16x2", "MinF32x2", "MaxF16x2", "MaxF32x2"):
        body = block(atomic, "Id EmitStorageAtomic%s(" % name)
        if "BdaCas(ctx, binding, offset" not in body or body.index("BdaCas(") > body.index("ctx.ssbos[binding.U32()]"):
            raise SystemExit("FAIL: EmitStorageAtomic%s reaches the descriptors in a by-address stage" % name)
    push = block(ctx, "void EmitContext::DefineRescalingInputPushConstant()")
    need(push, "MemberDecorate(push_constant_struct, render_are_member_index, spv::Decoration::Offset, RENDERAREA_LAYOUT_OFFSET);",
         "the render area is not at the offset the host pushes it to")
    need(push, "render_area_push_constant = rescaling_push_constants;", "a by-address render-area stage gets two push blocks")
    if push.index("if (area) {") > push.index("rescaling_textures_type = TypeArray("):
        raise SystemExit("FAIL: the rescaling words are declared under the render area")
    need(block(ctx, "void EmitContext::DefineRenderArea("), "Sirit::ValidId(render_area_push_constant)",
         "DefineRenderArea makes a second push block")
    cb = block(ctx, "void EmitContext::DefineConstantBuffers(")
    if cb.index("types |= IR::Type::U32;") > cb.index("if (True(types & IR::Type::U8))"):
        raise SystemExit("FAIL: a by-address stage without u32 constant reads has no view for its address words")
    need(shader, "p.ssbo_bda_wide = p.ssbo_bda && bda_wide();", "RX_VK_DESC_HEAP_BDA=2 does not reach the translator")
    need(shader, "(profile().ssbo_bda_wide ? 4u : 0u)", "the program cache's stamp does not tell =2 from =1")
    sbo = draw[draw.index("if (info.ssbo_bda && !d.is_written) {"):]
    if sbo.index("bda_storage_guess(s, st, d, off, salign)") > sbo.index("bda_storage_read(s, st, d.cbuf_index, off, salign)"):
        raise SystemExit("FAIL: a table entry's words go to bda_storage_read (its size word is no size)")
    e3 = block(draw, "bool eng_draw_descriptors(")
    need(e3, "if (d.count > 1 || (info.native_global_bounds && d.cbuf_index != 0)) {", "E3 reads a table entry as a buffer")
    need(e3, "eng_bda_why_note(p, st)", "E3's refusals do not name the reasons (RX_VK_LOG_BDA_WHY)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--spirv-val", default="spirv-val")
    args = parser.parse_args()
    sources()
    sirit = ROOT / "gpu" / "vk" / "sirit"
    with tempfile.TemporaryDirectory(prefix="bda-emit-") as tmp:
        tmp = Path(tmp)
        cpp, exe = tmp / "fixture.cpp", tmp / "fixture"
        cpp.write_text(FIXTURE, encoding="utf-8")
        r = subprocess.run([args.cxx, "-std=c++20", "-O1", "-I" + str(SPIRV), "-I" + str(sirit / "include"),
                            "-I" + str(sirit / "src"), "-I" + str(args.headers), str(cpp),
                            *map(str, sorted((sirit / "src").rglob("*.cpp"))), "-o", str(exe)],
                           capture_output=True, text=True, timeout=600)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("the fixture did not build")
        for check in ("0", "1"):
            spv = tmp / ("bda_%s.spv" % check)
            r = subprocess.run([str(exe), str(spv), check], capture_output=True, text=True, timeout=30)
            print(r.stdout + r.stderr, end="")
            if r.returncode:
                raise SystemExit("FAIL: the fixture stopped")
            r = subprocess.run([args.spirv_val, "--target-env", "vulkan1.3", str(spv)], capture_output=True, text=True, timeout=60)
            if r.returncode:
                print(r.stdout + r.stderr)
                raise SystemExit("FAIL: spirv-val (%s)" % ("checked" if check == "1" else "plain"))
    print("PASS: loads, stores, atomics and CAS loops through window-table addresses validate, plain and with RX_VK_BDA_CHECK;"
          " RX_VK_DESC_HEAP_BDA=2 wiring")


if __name__ == "__main__":
    main()
