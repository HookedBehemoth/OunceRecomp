// NVN's barriers and the console's implicit waits as Vulkan memory dependencies.
//
// nvnCommandBufferBarrier's bits name what later work waits for (the ORDER bits) and which caches it reads through anew
// (the INVALIDATE bits). Each bit adds its narrowest Vulkan scopes; a barrier is their union, one vkCmdPipelineBarrier2.
// Images are all VK_IMAGE_LAYOUT_GENERAL, so every dependency is a global memory barrier.
//
// | NVN bit                       | waits for (source)              | before (destination)                 | accesses made visible             |
// |-------------------------------|---------------------------------|--------------------------------------|-----------------------------------|
// | ORDER_PRIMITIVES              | all graphics stages, compute    | all graphics stages, compute, copies | attachments, storage writes,      |
// |                               |                                 |                                      | copies, vertex and index reads    |
// | ORDER_FRAGMENTS               | fragment shading, tests, output | fragment stages, compute, copies     | attachments, storage writes,      |
// |                               |                                 |                                      | copies                            |
// | ORDER_FRAGMENTS_TILED         | as ORDER_FRAGMENTS, by region   | as ORDER_FRAGMENTS                   | as ORDER_FRAGMENTS                |
// | ORDER_INDIRECT_DATA           | all graphics stages, compute    | indirect argument reads              | indirect arguments                |
// | INVALIDATE_TEXTURE            | -                               | -                                    | sampled and storage image reads   |
// | INVALIDATE_SHADER             | -                               | -                                    | uniform and storage buffer access |
// | INVALIDATE_TEXTURE_DESCRIPTOR | -                               | -                                    | none: descriptors are the host's  |
// | INVALIDATE_ZCULL              | -                               | -                                    | none: Vulkan has no zcull         |
//
// With ORDER_INDIRECT_DATA alone, an INVALIDATE bit makes all shader stages its destination: the shaders of the work after
// it run after its argument fetch.
//
// The source access is every write of the source stages (shader storage, colour and depth attachments). A barrier with
// no ORDER bit waits for nothing, as on the console: it is recorded as nothing (an invalidate of work already ordered,
// which the dependencies below already made visible).
//
// The console orders some work with no barrier from the game; these dependencies stand in for its waits:
//
// - A dispatch, before the next work in the list that is not a dispatch: the switch from compute to the 3D engine waits
//   for the compute work, so the game puts no barrier after a chain of dispatches. Dispatches follow each other with no
//   wait unless the game asks for one.
// - A copy, a texture clear or a buffer op, before the work after it: these run in the 3D engine's order on the console,
//   and as transfers in Vulkan.
#pragma once

#include <vulkan/vulkan.h>

namespace hsd {

struct Dep {
    VkPipelineStageFlags2 src = 0, dst = 0;
    VkAccessFlags2 src_access = 0, dst_access = 0;
    VkDependencyFlags flags = 0;

    bool empty() const { return !src; }
    Dep& operator|=(const Dep& o) {
        flags = empty() ? o.flags : o.empty() ? flags : flags & o.flags;   // (by region only when both are)
        src |= o.src, dst |= o.dst, src_access |= o.src_access, dst_access |= o.dst_access;
        return *this;
    }
};

/// The dependency an NVN barrier's bits ask for (empty: no ORDER bit)
Dep nvn_barrier_dep(int32_t bits);
/// A dispatch's writes before the non-dispatch work after it
extern const Dep kAfterDispatch;
/// A copy's, texture clear's or buffer op's writes before the work after it
extern const Dep kAfterTransfer;

void record_dep(VkCommandBuffer cb, const Dep& d);

}  // namespace hsd
