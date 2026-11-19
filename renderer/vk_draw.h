// A draw: the list's program variant, its
// dynamic state, the vertex and index buffers by layer 0, the uniform buffers' address table and the driver banks in the
// frame arena, the push data, the draw call, on the recording thread in the list's command buffer inside the pass
// vk_pass.cpp opened. Ordinary vertex and fragment draws (DRAW, DRAW_IDX) of every list are attempted: in a list that
// records no command buffer yet (jobs, load-time lists: M3) the draw is resolved up to its Vulkan calls and counted.
// Whatever skips a draw is counted by reason. Deferred handle binds: the bank words they fill are copied from the game's
// array at the guest's submit (draw_list_submitted).
#pragma once

#include <cstdint>

#include "vk_lists.h"

namespace hsd {

struct DState;
/// A state op of the list (any list: its state is kept from its first one)
bool draw_log_frame();   // (debug.drawlog's frame)
void draw_state(List* l, uint32_t op, const void* data, uint32_t size);
/// A draw op (DRAW, DRAW_IDX, DRAW_INDIRECT, DRAW_MESH); recording: the list's pass is begun in l->cur
void draw_emit(List* l, uint32_t op, const void* data, uint32_t size, bool recording);
/// The command buffer's state is undefined (a secondary was executed in it): the next draw sets all again
void draw_invalidate(List* l);
/// A compute step was recorded in the list's command buffer: its push data replaced the draws' (push data is shared by all
/// stages); the graphics shaders, dynamic state, vertex input, buffers and the heaps (bound again at the same addresses) hold
void draw_push_invalidate(List* l);
/// The list's draw state goes with it
void draw_list_free(List* l);
/// (gpu_submit, the guest's submit) the deferred binds' bank words of l and its callees copied from the game's arrays now
void draw_list_submitted(const List* l);
/// (vk_pools.cpp) layer 0's window k made at device address a: the BDA window table's entry
void draw_window_made(uint32_t k, uint64_t a);

/// The frame arena of a command slot (vk_cmd.h): bump allocations in mapped device memory, reset with the slot
struct Arena;
void arena_reset(Arena* a);

/// The 600-present lines: draws by reason and list kind, programs, heap, state
void draw_log(double frames);
/// Since the start, all threads: draws recorded, and draws skipped because their program or variant was not ready yet (read or
/// translated by the shader workers). Any thread; the counts of a thread drawing meanwhile may be a draw behind
void draw_totals(int64_t* drawn, int64_t* waiting);
/// (vk_compute.cpp) bytes of the slot's frame arena; the BDA window table's address; 64 KB of zeros
struct CmdSlot;
bool draw_arena(CmdSlot* slot, uint32_t bytes, uint8_t*& map, VkDeviceAddress& addr);
/// the same, with the arena chunk's buffer and the offset in it (an indirect draw's arguments)
bool draw_arena_buffer(CmdSlot* slot, uint32_t bytes, uint8_t*& map, VkDeviceAddress& addr, VkBuffer& buf, VkDeviceSize& off);
VkDeviceAddress draw_window_table();
VkDeviceAddress draw_zero_address();
/// a job has drawn: the game is past its start (before it, a dispatch whose program is missing waits for it)
bool draw_scene_up();
void draw_meshlog_after();

}  // namespace hsd
