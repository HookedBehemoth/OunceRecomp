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
    rule = ctx[ctx.index("bool BdaStage("):ctx.index("} // Anonymous namespace", ctx.index("bool BdaStage("))]
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
    print("PASS: loads, stores and atomics through window-table addresses validate, plain and with RX_VK_BDA_CHECK")


if __name__ == "__main__":
    main()
