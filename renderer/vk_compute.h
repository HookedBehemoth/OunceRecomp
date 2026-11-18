// Dispatches and the buffer ops.
//
// A list's compute state is its own (NVN keeps the compute program and stage 5's bindings beside the graphics ones): the
// compute program, stage 5's uniform and storage buffers, texture and image handles, and the pushed compute constants. A
// dispatch (direct or indirect) in a list that records is recorded outside its rendering: the variant of the bound
// program for the bound textures' answers, its driver bank (NVN2 compute's packed c0: the grid, the pushed constants, the
// uniform and storage buffer tables, the texture and image handles) and its uniform buffers' address table in the frame
// arena, the push data, vkCmdDispatch or vkCmdDispatchIndirect on the arguments in layer 0 (the grid reaches the program as
// NumWorkgroups: the translator folds c0's grid words into it, so the GPU reads an indirect dispatch's counts). Programs are
// translated by the forked translator with the step-B interface on the shared shader workers (vk_work.h; a missing variant skips the
// dispatch, counted), kept in hookshot_direct_compute.bin, and their kept variants made when the game sets the program.
// Copies between buffers and buffer clears are recorded where they are, outside a rendering.
#pragma once

#include <cstdint>

#include "vk_lists.h"

namespace hsd {

/// Any op of a list that records (its compute state)
void compute_state(List* l, uint32_t op, const void* data, uint32_t size);
/// The list's command buffer (vk_pass.cpp's: a waiting barrier recorded first)
using CmdOf = VkCommandBuffer (*)(List*);
/// DISPATCH / DISPATCH_INDIRECT in a list that records, its rendering ended
void compute_dispatch(List* l, uint32_t op, const void* data, uint32_t size, CmdOf cmd);
/// COPY_B2B / CLEAR_BUF in a list that records, its rendering ended
void compute_buffer_op(List* l, uint32_t op, const void* data, uint32_t size, CmdOf cmd);
void compute_list_free(List* l);
/// Another compute program was bound in the list's command buffer (vk_query.cpp's resolve step)
void compute_invalidate(List* l);
/// (hsd_submit) the deferred handle words of l's and its callees' banks copied from the game's arrays now
void compute_list_submitted(const List* l);
/// (nvnProgramSetShaders) a compute program's kept variants made, on a worker
void compute_program_set(const nvn_prog* p);
/// The 600-present lines
void compute_log(double frames);
int64_t compute_objects_made();   // (none is ever destroyed)

}  // namespace hsd
