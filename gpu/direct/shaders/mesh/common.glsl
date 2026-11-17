// What every replacement mesh stage shares: the direct back end's interface for a mesh stage (push data, the task header,
// the guest's memory by address) and small helpers. Written for gpu/direct (docs/direct/task-mesh.md):
// the replacement takes the translator's place for one stage and keeps its heap mappings, so its uniform buffers are the
// bindings the translator gave the stage (one a buffer the stage reads, in ascending buffer index), and the rest is read by
// address as the translator reads it.
#extension GL_EXT_mesh_shader : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// (the vertex outputs' built-in block as the translator declares it: the position alone, invariant)
out gl_MeshPerVertexEXT {
    invariant vec4 gl_Position;
} gl_MeshVerticesEXT[];

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer Words { uint w[]; };
layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer Addresses { uint64_t a[]; };

// (the push data block the back end writes for a mesh draw: Eden's, emit_spirv.h; only what a mesh stage reads)
layout(push_constant, scalar) uniform DrawPush {
    layout(offset = 64) uint64_t task_header;   // (a task+mesh draw's task header: emit_spirv.h TASK_HEADER_*)
    layout(offset = 88) uint64_t window_table;  // (layer 0: the window of each 2 GB of guest memory)
    layout(offset = 96) uint64_t virtual_lo;    // (a pool at virtual addresses, from 1 TB: its range and where it is)
    uint64_t virtual_hi;
    uint64_t virtual_delta;
} draw;

// A guest address as the device sees it (gpu/direct/layer0_windows.h; the translator's bda_emit.h, its alignment kept)
uint64_t device_address(uint64_t guest) {
    if (guest >= draw.virtual_lo && guest < draw.virtual_hi) return (guest & ~3ul) + draw.virtual_delta;
    return Addresses(draw.window_table).a[uint(min(guest >> 31, 512ul))] + (guest & 0x7ffffffcul);
}

// The task header's words (emit_spirv.h): the map at 4-5, the payloads at 6-7, the payload words at 13
struct TaskInput {
    uint index;     // (this mesh workgroup's number among its task workgroup's: NVN's IsbeRead(0))
    uint64_t payload;
};
TaskInput task_input() {
    Words header = Words(draw.task_header);
    const uint64_t map = uint64_t(header.w[4]) | uint64_t(header.w[5]) << 32;
    const uint entry = Words(map).w[gl_WorkGroupID.x];   // (task workgroup << 8 | index)
    const uint64_t payloads = uint64_t(header.w[6]) | uint64_t(header.w[7]) << 32;
    TaskInput t;
    t.index = entry & 0xffu;
    t.payload = payloads + uint64_t((entry >> 8) * header.w[13]) * 4ul;
    return t;
}
// A word of the task's payload (as the translator reads it: held to the payload's words)
uint payload_word(TaskInput t, uint word, uint words) { return Words(t.payload).w[min(word, words - 1u)]; }

// NVN's clip space to Vulkan's: depth from [-w, w] to [0, w] (the back end's convert_depth_mode, Eden's ConvertDepth)
vec4 vulkan_clip(vec4 p) {
    precise float z = (p.z + p.w) * 0.5;
    return vec4(p.x, p.y, z, p.w);
}

// The bits [start, start + count) of a little-endian bit stream whose 32-bit words w0, w1, w2 hold bit 0 at w0's bit 0, for
// start < 96 and count < 32 (the meshlet streams' fields never cross three words)
uint stream_bits(uint w0, uint w1, uint w2, uint start, uint count) {
    const uint64_t pair = start > 63u ? uint64_t(w2) : start > 31u ? (uint64_t(w1) | uint64_t(w2) << 32) : (uint64_t(w0) | uint64_t(w1) << 32);
    return uint(pair >> (start & 31u)) & ((1u << count) - 1u);
}
