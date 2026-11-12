// HOS_DBG=dump:<present>[+<frames>] (docs/direct/m2-interface.md, direct_dump): every pass's targets of the frame after that present
// (the submissions handed in between it and the next present), each as its rendering ends, scaled to at most 320 wide and
// written as dump_<present>/NNN_<what>_<gpu>.png in the run's folder (HOS_DBG=dumpdir:<dir>: there), with one log line
// each in the frozen back end's RX_VK_DUMP format (gpu/vk/vk_dump.cpp), so scripts/direct_dump_compare.py reads both.
// Lists are recorded before their frame's submit, so passes are copied from two presents before it on; only the copies
// of the submissions of that frame are numbered (in submission order) and written.
#pragma once

#include <string>

#include "direct_lists.h"

namespace hsd {

struct DumpItem {
    VkCommandBuffer cb = VK_NULL_HANDLE;   // (the list's buffer it is recorded in)
    VkImage image = VK_NULL_HANDLE;        // (colour: the scaled RGBA32 copy)
    Mem* image_mem = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    Mem* buffer_mem = nullptr;
    void* map = nullptr;
    uint32_t w = 0, h = 0, src_w = 0, src_h = 0;
    int kind = 0;   // (0 float, 1 uint, 2 sint)
    int depth_bytes = 0;
    bool depth_float = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint64_t gpu = 0;
    uint32_t id = 0;
    std::string what;
    bool claimed = false;   // (numbered for its frame: the writer frees it, not its list)
};

/// Passes are being copied now (the frame to dump is near)
bool dump_recording();
/// The pass's targets copied (its rendering just ended, outside it), the items kept in the list
void dump_pass(List* l, VkCommandBuffer cb);
/// A submission's items as it is sent, in order: kept for writing when frame (the presents before its hand-in) is the
/// frame dumped (true: it is; its value goes to dump_sent)
bool dump_claim(const std::vector<DumpItem*>& items, uint64_t frame);
/// That submission's timeline value
void dump_sent(uint64_t value);
/// A present: the frame dumped is written once the GPU is done with it (the Presentation Thread)
void dump_present(uint64_t present);
/// A list's items it still owns, freed
void dump_free(std::vector<DumpItem*>& items);
/// An RGBA8 picture as a PNG (its alpha left out)
bool dump_write_png(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h);

}  // namespace hsd
