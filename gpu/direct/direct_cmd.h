// Command buffers: each recording thread has a command pool per frame slot
// (three in rotation), its buffers kept and handed out again after the slot's reset. A slot is reset as a whole when its
// thread first records in a new frame and takes it: one last recorded in three frames ago or more, once the GPU is past
// the last submission that held one of its buffers and no list holding one is alive. A list alive past that (one kept
// for more than three frames, as in a pause) sets its slot aside untouched until the list is gone; the thread gets one
// slot more when it then has none to reset (counted). Each thread also has a small ring of upload buffers for the CPU's
// texel writes and image initialization, sent at once on the one queue with their own timeline value.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "direct_internal.h"

namespace hsd {

struct Arena;   // (direct_draw.cpp)
struct QSlot;   // (direct_query.cpp)
struct TaskPool;   // (direct_task.cpp)
void query_slot_reset(QSlot* q);
void arena_reset(Arena* a);
void task_pool_reset(TaskPool* p);
struct CmdSlot;
struct CmdSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> bufs, secs;   // (the recording thread's alone)
    size_t used = 0, secs_used = 0;
    uint64_t frame = UINT64_MAX;
    std::atomic<uint64_t> value{0};   // (the newest submission that held one of its buffers)
    std::atomic<int32_t> live{0};     // (lists alive holding its buffers)
    Arena* arena = nullptr;   // (M2b: its frame arena, reset with it)
    QSlot* q = nullptr;       // (M5: its query pools and report records, reset with it)
    TaskPool* task = nullptr; // (M4c: the indirect task+mesh draws' maps and payloads, reset with it)
};

/// This thread's slot for the frame now (reset first when this is its first use in a new frame), with one list more
/// holding it
CmdSlot* cmd_slot_take();
/// A primary command buffer of the slot, begun (SIMULTANEOUS_USE: a list may be called twice in a submission)
VkCommandBuffer cmd_buffer(CmdSlot* s);
/// A secondary command buffer of the slot, begun to continue a dynamic rendering of those formats
struct PassFormats;
VkCommandBuffer cmd_secondary(CmdSlot* s, const PassFormats& f, VkRenderingFlags flags);
/// A secondary command buffer of the slot, begun outside any rendering (a job's task step)
VkCommandBuffer cmd_secondary_plain(CmdSlot* s);
/// A list holding the slot's buffers is gone
void cmd_slot_drop(CmdSlot* s);
/// A submission holding the slot's buffers was sent with timeline value v
void cmd_slot_sent(CmdSlot* s, uint64_t v);

struct Upload {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
};
/// This thread's next upload: a begun command buffer and staging room for bytes (0: none)
Upload upload_begin(uint64_t bytes);
/// Ends it with a barrier that makes its writes visible to all later work on the queue, and sends it: its timeline value
uint64_t upload_end(Upload& u);
/// The newest upload's timeline value (any thread's): a submission after it waits for it 
uint64_t upload_value();

void cmd_stats(char* out, size_t cap, double frames);
/// What the command pools hold, in counts (the driver gives no bytes); returns the length written
size_t cmd_held_stats(char* out, size_t cap);
/// Present n: the command buffers frame n - 2 took counted into the most a frame took (what a slot is grown to at its reset)
void cmd_frame_presented(uint64_t n);

}  // namespace hsd
