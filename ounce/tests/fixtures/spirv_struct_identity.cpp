#include <sirit/sirit.h>
#include <cstdio>
#include <string>
#include <vector>
using namespace Sirit;
int main(int argc,char** argv) {
    if(argc!=2)return 1;
    for(unsigned local=0;local<2;local++) {
        Module m{0x00010600};m.AddCapability(spv::Capability::Shader);m.AddCapability(spv::Capability::Int64);
        m.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
        m.SetMemoryModel(spv::AddressingModel::PhysicalStorageBuffer64,spv::MemoryModel::GLSL450);
        const Id u32=m.TypeUInt(32),u64=m.TypeUInt(64),void_t=m.TypeVoid(),zero=m.Constant(u32,0U);
        std::vector<Id> interfaces;
        // Mirrors first-task, Private local memory, and global-memory wrapper declarations.
        auto block=[&]{Id t=m.TypeStruct(u32);m.Decorate(t,spv::Decoration::Block);m.MemberDecorate(t,0U,spv::Decoration::Offset,0U);return t;};
        const Id first=block();
        if(local) {
            const Id element=m.TypeStruct(u32),array=m.TypeArray(element,m.Constant(u32,4U));
            interfaces.push_back(m.AddGlobalVariable(m.TypePointer(spv::StorageClass::Private,array),spv::StorageClass::Private));
        }
        const Id global=block(),member=m.TypePointer(spv::StorageClass::PhysicalStorageBuffer,u32);
        const Id first_ptr=m.TypePointer(spv::StorageClass::PhysicalStorageBuffer,first);
        const Id global_ptr=m.TypePointer(spv::StorageClass::PhysicalStorageBuffer,global);
        const Id fn=m.OpFunction(void_t,spv::FunctionControlMask::MaskNone,m.TypeFunction(void_t));m.AddLabel();
        const Id address=m.Constant(u64,uint64_t{16});
        m.OpLoad(u32,m.OpAccessChain(member,m.OpConvertUToPtr(first_ptr,address),zero),spv::MemoryAccessMask::Aligned,4U);
        const Id ptr=m.OpAccessChain(member,m.OpConvertUToPtr(global_ptr,address),zero);
        m.OpStore(ptr,zero,spv::MemoryAccessMask::Aligned,4U);
        m.OpAtomicIAdd(u32,ptr,m.Constant(u32,1U),zero,zero);
        m.OpReturn();m.OpFunctionEnd();m.AddEntryPoint(spv::ExecutionModel::GLCompute,fn,"main",interfaces);
        m.AddExecutionMode(fn,spv::ExecutionMode::LocalSize,1U,1U,1U);
        auto words=m.Assemble();std::string path=std::string(argv[1])+"/"+std::to_string(local)+".spv";
        FILE* f=std::fopen(path.c_str(),"wb");if(!f)return 2;
        const auto written=std::fwrite(words.data(),4,words.size(),f);std::fclose(f);if(written!=words.size())return 3;
        // Scalar and pointer declarations must retain their existing reuse.
        if(m.TypeUInt(32).value!=u32.value || m.TypePointer(spv::StorageClass::PhysicalStorageBuffer,u32).value!=member.value)return 4;
    }
}
