// The back end's interface for a fragment stage, as the translator's fragment stages use it (gpu/direct/eden, direct_shader.cpp
// finish), for the decompiled fragment stages beside this file (docs/direct/m6h-report.md). Not used by the game.
//
// - Uniform buffers: each bank the stage reads (c1 the program's constants, c2 the driver bank of texture handles, c3.. the game's
//   buffers) is a block of 16384 words at its binding; the back end maps the binding to its address in the table pushed at 120
//   (INDIRECT_ADDRESS). The bindings are numbered after the program's vertex stage's (BINDING_BASE: the vertex stage's bank count),
//   in the stage's own order: the banks, then one binding a texture descriptor.
// - Textures: a handle is image id (bits 0-19) and sampler id (bits 20-31); each texture descriptor is a runtime array of images
//   of its view type indexed by the image id, the samplers one runtime array (binding 1000; depth compare ones 1001) indexed by the
//   sampler id.
// - Arithmetic: the translator marks every float operation NoContraction (`precise` here: the outputs are precise, which glslang
//   carries back to every operation they depend on), saturates with NMax then NMin, and guards each colour output of a float
//   target against NaN and infinity (`guard`).
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#ifndef BINDING_BASE
#define BINDING_BASE 0
#endif

#define BANK(name, slot) layout(set = 0, binding = BINDING_BASE + (slot), scalar) uniform name##_block { uint w[16384]; } name
#define F(bank, byte) uintBitsToFloat(bank.w[(byte) / 4])
#define U(bank, byte) (bank.w[(byte) / 4])

layout(set = 0, binding = 1000) uniform sampler heap_samplers[];
#define IMAGE(h) nonuniformEXT((h) & 0xFFFFFu)
#define SAMPLER(h) heap_samplers[nonuniformEXT((h) >> 20)]

float sat(float x) { return min(max(x, 0.0), 1.0); }
// (the translator leaves its reciprocals (OpFDiv 1, x) contractible: a function keeps glslang from carrying `precise` into them)
float rcp(float x) { return 1.0 / x; }
float guard(float x) { return isnan(x) ? 0.0 : min(max(x, -1.00000002e+30), 1.00000002e+30); }
