/* Game patch, direct mode only: the GPU culling's mesh draw argument records are made Vulkan's layout in
   place (docs/direct/task-mesh.md 3; plan, "Data a draw reads"), so no conversion copy exists.

   f_80362ff0 runs once a frame per culling list: it reads the four 16-byte records of the list slot's header (w0 and w3 of
   record 0, w3 of records 1-3) and zeroes them (four stp xzr, xzr). The culling compute shaders (4247f1b4c3ce6cf3,
   f0fed2bbccae7bdc, cb2e06ddfb9d7d27, 3e6fe279463b020e) then write only two words of a record r: atomicMax(+0) (the group
   count, ceil(min(total, 0x1400) / 16)) and atomicAdd(+12) (the items appended, whose result is the list position).
   NVN reads a record as {count = w0, first = w1}; vkCmdDrawMeshTasksIndirectEXT reads {groupCountX, Y, Z} = w0, w1, w2.
   Writing {0, 1, 1, 0} where the game writes zeros leaves {x, 1, 1, appended} after the culling: Vulkan's record, x
   groups from 0, the stride 16. Nothing on the GPU writes w1 or w2, so NVN's first is 0 there, as the back end takes it
   for every indirect mesh draw.

   The site: an At row (nx2/lift2/probes.py DIRECT) after the last of the four stores (`stp xzr, xzr, [x8, #32]`, records 1-3 at x8 + 0 / 16 / 32);
   record 0 is at [x19 + 32] + u32([[x19 + 24]] + 4), records 1-3 at [x19 + 32] + u32([[x19 + 24]] + 16), the addresses the
   function itself computes. The row checks that x8 is that second address (else it writes nothing and counts it).

   Check that it applies: the 600-present line "direct patch mesh args" (headers a frame, mismatches 0). */
#include "direct.h"
#include <windows.h>

static struct { volatile LONG64 headers, mismatch, outside; } n;

static void put(uint64_t rec) {   /* {0, 1, 1, 0}: the counts' words stay as the game zeroed them */
    if (!hos_guest_range(rec, 16)) { InterlockedIncrement64(&n.outside); return; }
    G32(rec + 4) = 1, G32(rec + 8) = 1;
}

/* (x19 the list slot's object, x8 the address of records 1-3) */
void direct_mesh_args_header(uint64_t obj, uint64_t x8) {
    if (!rx_direct()) return;
    if (!hos_guest_range(obj + 24, 16)) { InterlockedIncrement64(&n.outside); return; }
    const uint64_t base = G64(obj + 32), lay = G64(obj + 24);
    if (!hos_guest_range(lay, 8) || !hos_guest_range(G64(lay), 20)) { InterlockedIncrement64(&n.outside); return; }
    const uint64_t layout = G64(lay);
    const uint64_t r0 = base + G32(layout + 4), r1 = base + G32(layout + 16);
    if (r1 != x8) { InterlockedIncrement64(&n.mismatch); return; }
    put(r0), put(r1), put(r1 + 16), put(r1 + 32);
    InterlockedIncrement64(&n.headers);
}

void direct_mesh_args_log(double frames) {
    static LONG64 last[3];
    const LONG64 now[3] = {n.headers, n.mismatch, n.outside};
    hos_log("direct patch mesh args (f_80362ff0's header zeroing -> {0, 1, 1, 0} records): %.2f headers a frame (4 records each); "
            "x8 not the records' address %lld, outside guest memory %lld",
            (now[0] - last[0]) / frames, (long long)(now[1] - last[1]), (long long)(now[2] - last[2]));
    for (int i = 0; i < 3; i++) last[i] = now[i];
}
