#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sirit/sirit.h>
namespace IR { enum Attribute { InstanceId, VertexId, BaseInstance, BaseVertex, DrawID }; }
std::vector<uint32_t> program(unsigned mask,bool supported) {
    using namespace Sirit;
    // (EmitContext's own member the block reads: a task program's DrawIndex; off here, as for every vertex program)
    struct Ctx : Module { using Module::Module; bool wants_mesh_first{}, mesh_fallback{}; } ctx{0x00010600};ctx.AddCapability(spv::Capability::Shader);
    ctx.SetMemoryModel(spv::AddressingModel::Logical,spv::MemoryModel::GLSL450);
    struct { bool support_vertex_instance_id; } profile{supported};
    struct { std::array<bool,5> loads; } info{};
    for(unsigned i=0;i<5;i++)info.loads[i]=(mask>>i)&1;
@CAPABILITY@
    const Id u32=ctx.TypeUInt(32),ptr=ctx.TypePointer(spv::StorageClass::Input,u32),void_t=ctx.TypeVoid();
    std::vector<Id> inputs;
    auto input=[&](spv::BuiltIn builtin){
        Id v=ctx.AddGlobalVariable(ptr,spv::StorageClass::Input);ctx.Decorate(v,spv::Decoration::BuiltIn,builtin);inputs.push_back(v);
    };
    // DefineInputs emits these directly, or synthesizes Base* for Vulkan's Instance/VertexIndex path.
    if(info.loads[IR::BaseInstance] || (!supported && info.loads[IR::InstanceId]))input(spv::BuiltIn::BaseInstance);
    if(info.loads[IR::BaseVertex] || (!supported && info.loads[IR::VertexId]))input(spv::BuiltIn::BaseVertex);
    if(info.loads[IR::DrawID])input(spv::BuiltIn::DrawIndex);
    const Id main=ctx.OpFunction(void_t,spv::FunctionControlMask::MaskNone,ctx.TypeFunction(void_t));
    ctx.AddLabel();ctx.OpReturn();ctx.OpFunctionEnd();ctx.AddEntryPoint(spv::ExecutionModel::Vertex,main,"main",inputs);
    return ctx.Assemble();
}
int main(int argc,char**argv) {
    if(argc!=2)return 1;
    for(unsigned support=0;support<2;support++)for(unsigned mask=0;mask<32;mask++) {
        auto code=program(mask,support!=0);std::string name=std::string(argv[1])+"/"+std::to_string(support*32+mask)+".spv";
        FILE*f=std::fopen(name.c_str(),"wb");if(!f)return 2;
        auto n=std::fwrite(code.data(),4,code.size(),f);std::fclose(f);if(n!=code.size())return 3;
    }
}
