// (M4c) The task stage of task+mesh programs as a compute step (docs/direct/m4c-report.md; plan, "Task and mesh shaders").
//
// The device has no task stage (taskShader off: native task shaders lost the device on this GPU). A task+mesh draw is two
// pieces of work. Its task stage runs as a compute shader, one workgroup per task workgroup of the draw, and writes into the
// draw's task header (64 bytes in the frame arena, emit_spirv.h TASK_HEADER_*): each mesh workgroup it starts as an entry
// of the draw's map (task workgroup << 8 | its index in it), its payload once into the draw's payload area, and the mesh
// workgroups placed into the header's first word. Its mesh stage is then drawn natively, vkCmdDrawMeshTasksIndirectEXT on
// the header ({groups, 1, 1}), each mesh workgroup reading its map entry and its task's payload by address.
//
// A direct draw's task workgroups are known when it is recorded: its header, map and payload area are made on the CPU in the
// frame arena. An indirect draw's are the game's record's count, known only on the GPU: a one-invocation compute step
// (shaders/task_prep.comp) reads the record and takes its map and payload area from the recording thread's task pool for the
// frame slot, before its task dispatch (which reads its {tasks, 1, 1} from the header).
//
// A compute step cannot run inside a rendering. In a job (a rendering secondary) the step is recorded into the job's own
// two plain secondaries (List::task_pre, task_cb), which the layer list executes at the job's call before executing the job,
// outside its rendering (direct_pass.cpp task_job_step: a begun rendering is ended there and begun again, counted). In a
// list that owns its rendering (a layer list's inline draw) the rendering is ended before the step and begun again after it
// (counted). Either way the step's order against the game's own work is the draw's: after every barrier and dispatch
// recorded before the job call or the draw.
#pragma once

#include "direct_lists.h"

namespace hsd {

struct DrawProg;
/// The task step of the task+mesh draw being recorded in l (its program p; push: the draw's push data, which gets the task
/// header's address; indirect: its record at guest address rec, else tasks [first, first + count)). hb, ho: the mesh draw's
/// indirect arguments. False: not recorded, why says (the draw is skipped and counted)
bool task_step(List* l, const DrawProg& p, bool indirect, uint64_t rec, uint32_t first, uint32_t count, uint8_t* push, VkBuffer& hb,
               VkDeviceSize& ho, const char*& why);
/// A barrier after a task step's part: after the headers' reset and making (false), or after its task dispatches (true)
void task_barrier(VkCommandBuffer cb, bool to_draw);
/// The list's end: its task step's secondaries ended
void task_list_end(List* l);
void task_stats(char* out, size_t cap, double frames);
/// (M6e, HOS_DBG=meshcap) the slot's task pool: its host mapping, device address and size (false: none)
bool task_pool_span(CmdSlot* slot, const uint8_t*& host, uint64_t& dev, uint64_t& bytes);
/// (HOS_DBG=tasklog:<present>) the present after it: that frame's task headers once the GPU has run it
void task_log_after();

}  // namespace hsd
