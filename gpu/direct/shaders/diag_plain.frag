// (the Hyrule Field hang, direct_draw.cpp HOS_DBG=progfs:<program key>) A diagnosis's fragment stage: one float colour, a
// constant; it reads nothing. Bound in place of one program's fragment stage, so that the program's triangles are filled
// as before (coverage, depth test and write, blending) while none of the game's fragment code runs.
// Regenerate the header: glslangValidator -V --target-env vulkan1.3 --vn diag_plain_frag_spv -o diag_plain_frag_spv.h diag_plain.frag
#version 460
layout(location = 0) out vec4 o0;
void main() { o0 = vec4(0.25, 0.5, 0.75, 1.0); }
