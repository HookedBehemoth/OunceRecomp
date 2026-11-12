// A recorded NVN list as the direct back end keeps it (M1: counts, and the ops the frame level needs, in order).
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "direct_api.h"
#include "../render/rx_nvn.h"

namespace hsd {

struct List;
/// One op the frame level needs: SIGNAL_EVENT / WAIT_EVENT (addr, mode, value), REPORT_COUNTER / RESET_COUNTER (counter),
/// CALL (callee)
struct FrameOp {
    uint32_t op = 0, mode = 0, value = 0;
    uint64_t addr = 0;
    nvn_p_counter counter{};
    List* callee = nullptr;
};
struct List {
    std::atomic<int32_t> refs{1};   // (its handle, each caller that calls it, each submission not run yet)
    hsd_list_info info{};
    int32_t site = -1;   // (its return address's entry in the table of the 600-present line)
    bool ended = false;
    uint32_t counts[NOP_COUNT_]{};   // (records by op; the recording thread's alone until it is ended)
    std::vector<FrameOp> ops;
};

void list_ref(List* l);
void list_unref(List* l);   // (any thread; the last one frees it and lets go of its callees)
/// The frame-level ops a submission of l runs, its callees' in place (l ended)
void list_flatten(const List* l, std::vector<const FrameOp*>& out);
/// l handed to hsd_submit (counted by its return address)
void list_submitted(const List* l);

}  // namespace hsd
