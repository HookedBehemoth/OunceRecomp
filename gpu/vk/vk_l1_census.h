// (native-vk, RX_VK_L1_CENSUS=1, off by default) The layer-1 census (work/assets/native/layer1-plan.md): what the replay's
// barriers order against what its commands actually touch, the replay's time by op, and where draws find their handles.
//
// Barriers: every full barrier recorded is counted by its reason (and, for "barrier (other)", by its caller), and two
// trackers watch the commands between them (a command: a draw, dispatch, copy, clear, event write of the replay):
// - neighbour: a recorded barrier is "needed" when a command after it touches what a command between it and the barrier
//   before wrote (read after write, write after write) or read (write after read);
// - ideal: the fewest barriers that order every such hazard, placed just before the command that needs one (a greedy
//   placement, which is the minimum for these intervals), by the kind of the hazard (attachment / storage / transfer
//   writer -> attachment / shader / transfer reader, or a write after a read).
// What a command touches: the guest ranges gpu_buffer_bind resolves for it (uniform, storage, vertex, index, indirect,
// copy and update ranges; written ones as writes), the rendered textures its descriptors sample or store, its pass's
// attachments (one pass's draws do not order each other's attachment writes). A draw that keeps the last draw's
// descriptors touches what that build touched.
//
// Off, every hook is one test of a constant read once.
#pragma once

#include <cstdint>

namespace mkwvk {
namespace l1c {

extern const bool on;

enum Kind : uint8_t { K_ATT = 0, K_SHADER = 1, K_TRANSFER = 2, K_COUNT = 3 };

/// A replay op starts (rx_nvn.h's NOP_*): the accesses noted until op_done are its command's (draws, dispatches, copies,
/// clears, event writes; other ops collect nothing)
void op_begin(uint32_t op);
/// The op is done: its command's accesses are checked against the trackers and enter them; its time is counted by op
void op_done(uint32_t op, int64_t ns);
/// (gpu_buffer_bind) a guest range the current command reads or writes
void buf(uint64_t gpu, uint64_t size, bool write);
/// (texture_written, the descriptor build's sampled list) a texture the current command writes or reads
void img(const void* tex, bool write);
/// (begin_pass) a pass begins: its attachments are written by the pass's draws (not ordered against each other)
void pass_begin();
void pass_attachment(const void* tex);
/// (build_descriptors) the descriptor build of a draw or dispatch starts / ends; (draw) a draw keeps the last build's
void desc_begin();
void desc_end();
void desc_reuse();
/// (full_barrier, pass_start_barrier) a barrier was recorded (why: its label; ra: the caller, for the unlabelled ones)
void barrier(const char* why, const void* ra);
/// (build_descriptors) n texture or image handles read from constant buffer index cbuf (0, 2: driver banks; 1: the program's; 3+:
/// a bound uniform buffer, read as submitted); (build_descriptors) a storage buffer's address read the same way
void handle_src(uint32_t cbuf, uint32_t n = 1);
void ssbo_src(uint32_t cbuf);
/// (the replay's frame statistics) the counts over the last `frames` presents, logged
void report(double frames);

}  // namespace l1c
}  // namespace mkwvk
