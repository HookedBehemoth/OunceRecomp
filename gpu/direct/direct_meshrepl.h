// Hand-written mesh stages in place of the translator's (docs/direct/mesh-harness.md section 6, docs/direct/hang-fix.md):
// a game mesh stage, by its code key, is drawn with a stage of ours (gpu/direct/shaders/mesh, listed in swap.txt) that emits
// the same geometry bit for bit. The translator still translates the stage (its Info drives the heap mappings, the push data
// and the fragment stage's inputs, and the program cache keeps its SPIR-V); the replacement takes its place when the shader
// object is made, if its interface is the translated stage's. HOS_DBG=nomeshrepl draws with the translator's stages.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hsd {

struct MeshRepl {
    uint64_t code;          // (the game mesh stage's code key)
    bool task;              // (the mesh stage of a task+mesh program)
    const uint32_t* words;  // (its SPIR-V)
    uint32_t count;
    uint8_t ubos[8];        // (the game's uniform buffer indices its bindings 0.. read, in order)
    uint32_t ubo_count;
};

/// The replacement for a mesh stage's code in a program of this shape, or null (none, or HOS_DBG=nomeshrepl)
const MeshRepl* mesh_repl_find(uint64_t code, bool task);
/// Whether the replacement can stand for the translated stage: the same execution modes (threads, vertices, primitives,
/// topology), the same output locations, no output built-in of its own, and the uniform buffers at its bindings the
/// translated stage's (`ubos`: the translated stage's buffer indices in binding order from `first_binding`). `why` names
/// the first difference
bool mesh_repl_fits(const MeshRepl& r, const std::vector<uint32_t>& translated, const std::vector<uint32_t>& ubos, uint32_t first_binding,
                    std::string& why);

}  // namespace hsd
