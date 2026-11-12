// meshlab: a mesh shader's outputs written to a buffer as well, so that two mesh shaders can be compared by what they emit.
//
// A mesh shader may not read its outputs (VUID-StandaloneSpirv-MeshEXT-07107), so the module is rewritten: after every store
// to an output (through an access chain on an Output variable) the same words are stored to a record of the workgroup in a
// buffer, at a fixed device address baked into the module, and after OpSetMeshOutputsEXT its two counts. The record of
// workgroup w is at base + w * stride: 4 header words (vertex count, primitive count, workgroup, 0x4d455348), then each Output
// variable's words in declaration order, flattened (arrays and structs in order, one word per scalar). Words never stored keep
// the fill the harness wrote before the draw. Everything else in the module is left as it is: the shader computes what it did.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace meshlab {

/// A leaf of an output variable: one per scalar block (a vector or a scalar) within one array element
struct OutField {
    std::string name;   // ("pos", "psize", "loc3", "idx", "prim_id", "layer", "cull", "b<n>")
    uint32_t word, words;   // (within the element)
    bool per_primitive, is_float;
};
struct OutVar {
    uint32_t id, elements, elem_words, offset;   // (offset: words from the record's start)
    bool per_primitive;
    std::vector<OutField> fields;
};
struct OutLayout {
    std::vector<OutVar> vars;
    uint32_t record_words = 0;
    uint32_t max_vertices = 0, max_primitives = 0, local_size = 0;
};

/// The module with the dump added (empty and err set: not possible). base: the buffer's device address, its records
/// record_words apart (layout.record_words)
std::vector<uint32_t> add_output_dump(const std::vector<uint32_t>& in, uint64_t base, OutLayout& layout, std::string& err);
/// Only the layout (the record size, for the buffer's size before the module is made)
bool output_layout(const std::vector<uint32_t>& in, OutLayout& layout, std::string& err);

}  // namespace meshlab
