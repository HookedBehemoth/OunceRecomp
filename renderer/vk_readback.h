// GPU writes copied into guest memory once the submission that makes them completes: the reports' 16 bytes today
// (vk_query.cpp), and the Readback claims' ranges (docs/direct/pool-claims.md). Each item is host-visible memory the GPU
// writes and the guest range it belongs at. A submission's items are copied before any CPU wait on it returns
// (nvn_wait, nvn_finish) and otherwise as soon as the GPU is past it, by a thread of their own.
#pragma once

#include <cstdint>
#include <vector>

#include "vk_internal.h"

namespace hsd {

struct ReadbackItem {
    const uint8_t* src;   // (host-visible memory the GPU writes)
    Mem* mem;             // (its allocation: invalidated before the copy)
    uint64_t guest;
    uint32_t size;
};
void readback_init();
/// The items of the submission whose last timeline value is `value` (in value order: the sender's)
void readback_add(uint64_t value, std::vector<ReadbackItem>&& items);
/// Every item whose submission has completed copied into guest memory
void readback_drain();
void readback_stats(char* out, size_t cap, double frames);

}  // namespace hsd
