// The GPU's time by pass over a window of presents, for `ounce profile` (gpu_measure: bench asks for it when its
// measured frames begin; measurement only).
//
// While the window is open, every primary command buffer gets a timestamp at its start (top of pipe) and every unit of
// work in it a timestamp after it (all commands): a rendering's end (its jobs' secondaries included), a dispatch, a job's
// task step, a copy, a texture clear and the present's own work. Sorted by time on the GPU, each end stamp's time minus
// the one before it is what that unit added to the queue's timeline; a start stamp later than the last end is time the
// GPU waited for work. The units are summed by label (what it was, the list's kind and return address, its
// SetRenderTargets number, the pass's size and formats, its first job's pass key) and written to passtime.csv in the
// working directory (the run's folder), with one "passtime:" line in the log. Off: one relaxed load per unit.
#pragma once

#include <atomic>
#include <cstdint>

#include <vulkan/vulkan.h>

namespace hsd {

extern std::atomic<bool> passtime_rec;   // (stamping now)
inline bool passtime_on() { return passtime_rec.load(std::memory_order_relaxed); }

enum PassWhat : uint8_t { PT_RENDER, PT_DISPATCH, PT_TASK, PT_COPY, PT_CLEAR, PT_PRESENT, PT_KINDS };
struct PassLabel {
    PassWhat what = PT_RENDER;
    uint32_t kind = 0;          // (the list's kind, GPU_LIST_*)
    uint64_t lr = 0;            // (the list's return address)
    uint32_t rt = 0;            // (its SetRenderTargets number in the list)
    uint32_t w = 0, h = 0;      // (the pass's size; a dispatch: grid x, y)
    uint32_t targets = 0;       // (colour targets; a dispatch: grid z)
    int32_t format = -1, depth = -1;   // (first colour target's and the depth target's VkFormat)
    uint64_t pass_key = 0;      // (the first job executed in the rendering: its pass key)
    uint64_t prog = 0;          // (the rendering's last drawn program; a dispatch: its code key)
    uint32_t draws = 0, jobs = 0;   // (draws recorded in it, its own and its jobs'; jobs executed)
};
/// A primary command buffer's start
void passtime_top(VkCommandBuffer cb);
/// After a unit of work
void passtime_end(VkCommandBuffer cb, const PassLabel& l);
/// At each present: the window opened or closed, its results read once the GPU is past it
void passtime_present(uint64_t present);
/// After vkCreateDevice
void passtime_init();

}  // namespace hsd
