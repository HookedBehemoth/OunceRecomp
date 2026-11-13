// meshtool: one NVN2 mesh stage (and the task stage before it) through the direct back end's translator fork (gpu/direct/eden),
// as direct_shader.cpp translates a task+mesh or mesh-only program, offline: the IR (as the translator reads the game's code),
// a structured print of it, the mesh stage's SPIR-V, and a summary of what the stage reads and writes. For the inventory and
// the decompilation of the game's mesh stages (docs/direct/m6e-report.md); nothing of the game is in this file.
//
//   meshtool <mesh stage .bin> <out prefix> [<task stage .bin>]
// A stage .bin: the stage's control section (0xd8 bytes) then its code area, as the romfs seed holds it (M0's seed.py).
// Writes <prefix>.ir, <prefix>.ir.c, <prefix>.flat.c, <prefix>.spv and prints the summary (one "key value" line each).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "offline.h"
#include "ir_pretty.h"

using offline::Code;
using offline::Env;
using offline::profile;
using offline::regs_of;

int main(int argc, char** argv) {
    if (argc < 3) return std::fprintf(stderr, "meshtool <mesh.bin> <out prefix> [task.bin]\n"), 2;
    try {
        const Code m = offline::load_file(argv[1]);
        Code t;
        const bool task = argc > 3;
        if (task) t = offline::load_file(argv[3]);
        Env env(m, Shader::Stage::Mesh, regs_of(m, task ? &t : nullptr));
        auto tr = offline::translate(env);
        Shader::IR::Program& p = tr->program;
        const std::string out = argv[2];
        { std::ofstream o(out + ".ir"); o << Shader::IR::DumpProgram(p); }
        Shader::RuntimeInfo rt{};
        rt.previous_stage_stores.mask.set();
        rt.convert_depth_mode = true;
        rt.mesh_without_task = true;
        rt.task_buffer = task;
        Shader::Backend::Bindings b{};
        const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile(), rt, p, b);
        { std::ofstream o(out + ".spv", std::ios::binary); o.write((const char*)spv.data(), spv.size() * 4); }
        { std::ofstream o(out + ".ir.c"); o << pretty::print(p); }
        { std::ofstream o(out + ".flat.c"); o << pretty::print(p, true); }
        const auto& in = p.info;
        std::printf("native_instructions %u\nlocal_memory %u\nthreads %u\nmax_vertices %u\nmax_primitives %u\ntopology %u\npayload_words %u\n", m.native,
                    m.local_mem, p.workgroup_size[0], p.mesh.max_vertices, p.mesh.max_primitives, p.mesh.topology, p.mesh.payload_words);
        std::printf("vertex_lines");
        for (u32 l : p.mesh.vertex_lines) std::printf(" %s", Shader::IR::NameOf(static_cast<Shader::IR::Attribute>(l)).c_str());
        std::printf("\nprimitive_lines");
        for (u32 l : p.mesh.primitive_lines) std::printf(" %s", Shader::IR::NameOf(static_cast<Shader::IR::Attribute>(l)).c_str());
        std::printf("\ncbufs");
        for (u32 i = 0; i < Shader::Info::MAX_CBUFS; i++)
            if (in.constant_buffer_mask & (1u << i)) std::printf(" c%u:%u", i, in.constant_buffer_used_sizes[i]);
        std::printf("\nssbos");
        for (const auto& d : in.storage_buffers_descriptors) std::printf(" c%u+%#x%s", d.cbuf_index, d.cbuf_offset, d.is_written ? ":written" : "");
        std::printf("\ntextures %zu\nimages %zu\nglobal_memory %d\n", in.texture_descriptors.size(), in.image_descriptors.size(), (int)in.uses_global_memory);
        // (the IR's opcodes: what the stage does, counted)
        std::map<std::string, int> ops;
        for (Shader::IR::Block* bl : p.blocks)
            for (const Shader::IR::Inst& i : bl->Instructions()) ops[std::string(Shader::IR::NameOf(i.GetOpcode()))]++;
        std::printf("ops");
        for (const auto& [k, n] : ops) std::printf(" %s:%d", k.c_str(), n);
        std::printf("\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
    return 0;
}
