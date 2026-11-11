// (native-vk layer 1, stage S2) RX_VK_PACKETS=1: a draw's resolutions taken from the draw before when the state they are made
// from did not change since (work/assets/native/l1s2-impl.md).
//
// The replay (vk_replay.cpp replay()) stamps each group of the NVN state below with a clock when an op sets it; a draw (vk_draw.cpp)
// keeps, per resolution, the stamps it was made at, and while they stand (and the state object, the program and the draw's own
// inputs are the same) it does not make it again: the dynamic state's pack and compare, the shader objects' vertex input, the state
// list's pipeline key, and a stage's texture descriptors (the classic, push-descriptor path; the descriptor heap has its own).
// RX_VK_PACKETS_CHECK=1: every resolution so kept is made anyway and compared (the frame log's "packets:" line counts mismatches).
//
// Only the serial replay (one at a time, under replay_mu) stamps: the stamps are plain words. With RX_VK_PARALLEL the switch is off.
#pragma once
#include <cstdint>

namespace mkwvk {

enum PkGroup : int {
    PKG_DYN,    // viewports, scissor, depth range, stencil, polygon offset, blend colour, restart, clamp, discard, the state objects
    PKG_VI,     // vertex attributes and streams' layouts (BIND_VATTR, BIND_VSTREAM)
    PKG_PROG,   // the program (BIND_PROGRAM)
    PKG_RT,     // the render targets (SET_RT)
    PKG_TEX,    // texture, sampler and image handles, the pools (BIND_TEX/SAMP/IMG, SET_TEX_POOL/SAMP_POOL)
    PKG_N
};

bool packets_on();      // RX_VK_PACKETS=1 (off with RX_VK_PARALLEL)
bool packets_check();   // RX_VK_PACKETS_CHECK=1 (implies RX_VK_PACKETS)

extern uint64_t pk_clock;          // (replay_mu)
extern uint64_t pk_gen[PKG_N];     // (replay_mu) the clock when the group was last set
inline void pk_touch(int g) { pk_gen[g] = ++pk_clock; }
struct Program;
uint32_t answer_stage_mask(const Program& p);   // (vk_shader.cpp: the stages whose c2 handles a program's answers read, ~0: any binding)
uint64_t prog_stage_sig(const nvn_prog& p);     // (vk_shader.cpp: a program's stages and serial, as program_for keys its picks)

inline void pk_touch_all() {
    ++pk_clock;
    for (int g = 0; g < PKG_N; g++) pk_gen[g] = pk_clock;
}

}   // namespace mkwvk
