// Reports and render conditions on the GPU (docs/direct-backend-plan.md, Reports and conditions).
//
// NVN's counters are running GPU counters: ResetCounter zeroes one, ReportCounter writes {value, timestamp} to guest
// memory. Samples passed becomes Vulkan occlusion queries: from a reset the counter counts while the list renders, one
// query per stretch inside one rendering (a query cannot outlive the rendering it was begun in), and a report's value
// is the sum of the stretches since the reset. Every report also writes a timestamp query where it is recorded. The
// values reach guest memory at the end of the rendering they were recorded in (a job's: its caller's rendering), in
// the list's order: the queries' results are copied into the slot's buffer and one compute step writes each report's
// 16 bytes at its guest address (report_resolve.comp), so the game's CPU readers read plain memory. Query pools and
// the record buffer belong to the thread's command slot (direct_cmd.h) and are reset with it, on the host.
//
// SetRenderEnableConditional(mode, addr) renders when the counters at addr and addr + 16 are equal (0) or differ (1).
// The second word is written by the CPU only, and is 0 in every case seen (HOS_DBG=condcheck counts the others), so
// the condition is conditional rendering on the low word of the first counter: non-zero renders (mode 1), or the
// inverse (mode 0). A condition is begun inside the rendering before a gated draw or clear and ended at the rendering's
// end, at a job's execution (RADV has no inherited conditional rendering) and at the next condition or render enable.
#pragma once

#include <cstdint>
#include <vector>

#include "direct_lists.h"

namespace hsd {

struct QSlot;   // (a command slot's queries and report records)
using CmdOf = VkCommandBuffer (*)(List*);

/// The device's extensions and features this needs (conditional rendering, calibrated timestamps)
void query_device_extensions(const std::vector<VkExtensionProperties>& exts, std::vector<const char*>& want, void**& tail);
/// After the device: functions, the resolve program, the clock calibration
void query_device_created();
/// The usage layer 0's window buffers add (conditional rendering reads guest memory)
VkBufferUsageFlags query_window_usage();
/// A command slot reset (its GPU work done): its queries reset on the host, its records free
void query_slot_reset(QSlot* q);

/// REPORT_COUNTER, RESET_COUNTER, RENDER_COND, RENDER_ENABLE of a list that records
void query_op(List* l, uint32_t op, const void* data, uint32_t size, bool rendering, CmdOf cmd);
/// Before a draw or a clear inside the list's rendering (or a job's secondary): the counter's query and the condition begun
void query_draw(List* l, VkCommandBuffer cb);
/// A clear is under a condition now (it cannot be a load op)
bool query_cond_wanted(const List* l);
/// Before the rendering ends (or a job's secondary ends): its query and condition ended
void query_suspend(List* l, VkCommandBuffer cb);
/// Outside a rendering: the reports recorded so far (and the executed jobs') written to guest memory
void query_flush(List* l, CmdOf cmd);
/// Before / after a job's secondary is executed in the list's rendering
void query_execute(List* l, VkCommandBuffer cb, const List* job);
void query_executed(List* l, const List* job);
void query_list_free(List* l);
void query_stats(char* out, size_t cap, double frames);

}  // namespace hsd
