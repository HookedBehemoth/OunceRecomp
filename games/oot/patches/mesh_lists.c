/* Game patch, direct mode only: the GPU culling lists' resets made on the GPU, and the counts the CPU reads copied out of
   them, so the lists are VRAM (docs/direct/pool-users.md, "The GPU culling lists").

   The culling object (cull_gpu_object_a_create) holds double-buffered lists whose first records the culling programs
   append to (atomicMax(+0), the mesh-draw group count; atomicAdd(+12), the items appended). At the frame's start,
   cull_gpu_lists_reset (f_80361c80) and cull_gpu_list_reset_one (f_80361a00) read the counts of the half the GPU filled two
   frames ago (`ldr w8, [x8, x19]`) and reset its records on the CPU: `str xzr` (w0, w1), `str wzr` (the appended count),
   `str w19` (a count the CPU sets), and three `stp` of {0, 1, 1, 0} (0x803624d0, 0x80362624, 0x80362e18). On a PC those
   records are the GPU's. Each store keeps its effect in guest memory and is followed by an At row that makes the same
   reset on the GPU before the next submission's work (nvn_claim_fill); each read is preceded by one that names its record
   as read by the CPU (nvn_claim_read): after a submission that writes the list, the GPU copies those 16 bytes out, not
   the list's megabytes.

   The mesh draw argument records are made Vulkan's layout in place (docs/direct/task-mesh.md): vkCmdDrawMeshTasksIndirectEXT
   reads {groupCountX, Y, Z} = w0, w1, w2 where NVN reads {count = w0, first = w1}, and the culling programs write only w0
   and w3, so {x, 1, 1, n} is Vulkan's record once w1 = w2 = 1. The resets of w0 and w1 set w1 = w2 = 1 with them. The
   other records are reset by the GPU (w0 and w3 only; w1 and w2 are never written by anyone): they get w1 = w2 = 1 where
   an indirect mesh draw names them (nvnCommandBufferDrawMeshTasksIndirect / MultiDrawMeshTasksIndirectCount,
   nvn_native.c), once: only a record whose w1 and w2 are both 0 in guest memory.

   Check that it applies: the 600-present line "direct patch mesh lists". */
#include "patches.h"
#include "os.h"
#include "nvn_claims.h"

static struct { volatile int64_t resets, reads, records, set, outside; } n;

/* the GPU's {0, 1, 1, w3}: a first record's w0 and w1 reset */
static void reset_head(uint64_t rec) {
    if (!hos_guest_range(rec, 16)) { os_atomic_inc(&n.outside); return; }
    G32(rec + 4) = 1, G32(rec + 8) = 1;
    nvn_claim_fill(rec, 4, 0);
    nvn_claim_fill(rec + 4, 8, 1);
    os_atomic_inc(&n.resets);
}

/* (At rows, a PATCH_AT declaration, runtime/patch.h) cull_gpu_lists_reset: after each `str xzr, [x19, x8]`;
   cull_gpu_list_reset_one: after its `str xzr, [x22, x8]` (a list's first record, w0 and w1, zeroed) */
PATCH_AT(0x80361d5c, word(0xf8286a7f), after, none, mesh_lists_reset, as(x19, list), as(x8, off)) { reset_head(list + off); }
PATCH_AT_SITE(0x80362048, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x803620b0, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362118, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362180, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x803621e8, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362848, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x803628b0, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362918, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362b80, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362be8, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362c50, word(0xf8286a7f), after, none, mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80361b3c, word(0xf8286adf), after, none, mesh_lists_reset, x22, x8);

/* the appended counts zeroed (`str wzr, [list, x8]`) */
PATCH_AT(0x80361d8c, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, as(x19, list), as(x8, off)) {
    nvn_claim_fill(list + off, 4, 0);
}
PATCH_AT_SITE(0x8036207c, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x803620e4, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x8036214c, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x803621b4, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x8036221c, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x80362288, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x8036287c, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x803628e4, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x8036294c, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x80362bb4, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x80362c1c, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x80362c84, word(0xb8286a7f), after, none, mesh_lists_count_zeroed, x19, x8);
PATCH_AT_SITE(0x80361b6c, word(0xb8286adf), after, none, mesh_lists_count_zeroed, x22, x8);

/* a count the CPU sets (`str w19, [x22, x8]`; w19 from the object, [x27 + 0x68] at the first) */
PATCH_AT(0x80362254, word(0xb8286ad3), after, none, mesh_lists_count_set, as(x22, list), as(x8, off), as(w19, value)) {
    nvn_claim_fill(list + off, 4, value);
}
PATCH_AT_SITE(0x80362980, word(0xb8286ad3), after, none, mesh_lists_count_set, x22, x8, w19);
PATCH_AT_SITE(0x80362cb8, word(0xb8286ad3), after, none, mesh_lists_count_set, x22, x8, w19);

/* a dispatch record set to {0, 1, 1, 0} (`stp x9, x23, [x8]`, x9 = 1 << 32, x23 = 1; the third `stp x9, x10`) */
PATCH_AT(0x803624d0, word(0xa9005d09), after, none, mesh_lists_record_set, as(x8, rec), as(x9, lo), as(x23, hi)) {
    const uint32_t w[4] = {(uint32_t)lo, (uint32_t)(lo >> 32), (uint32_t)hi, (uint32_t)(hi >> 32)};
    for (int i = 0; i < 4; i++) nvn_claim_fill(rec + 4 * (uint64_t)i, 4, w[i]);
}
PATCH_AT_SITE(0x80362624, word(0xa9005d09), after, none, mesh_lists_record_set, x8, x9, x23);
PATCH_AT_SITE(0x80362e18, word(0xa9002909), after, none, mesh_lists_record_set, x8, x9, x10);

/* the counts the CPU reads (`ldr w8, [x8, x19]`, cull_gpu_list_reset_one's `[x8, x22]`): their records copied out */
PATCH_AT(0x80361d10, word(0xb8736908), before, none, mesh_lists_count_read, as(x8, off), as(x19, list)) {
    nvn_claim_read((list + off) & ~15ull, 16);
    os_atomic_inc(&n.reads);
}
PATCH_AT_SITE(0x80361e7c, word(0xb8736908), before, none, mesh_lists_count_read, x8, x19);
PATCH_AT_SITE(0x80361ee4, word(0xb8736908), before, none, mesh_lists_count_read, x8, x19);
PATCH_AT_SITE(0x80362724, word(0xb8736908), before, none, mesh_lists_count_read, x8, x19);
PATCH_AT_SITE(0x8036278c, word(0xb8736908), before, none, mesh_lists_count_read, x8, x19);
PATCH_AT_SITE(0x80362a5c, word(0xb8736908), before, none, mesh_lists_count_read, x8, x19);
PATCH_AT_SITE(0x80362ac4, word(0xb8736908), before, none, mesh_lists_count_read, x8, x19);
PATCH_AT_SITE(0x80361a88, word(0xb8766908), before, none, mesh_lists_count_read, x8, x22);
PATCH_AT_SITE(0x80361af0, word(0xb8766908), before, none, mesh_lists_count_read, x8, x22);

/* nvn_native.c draw_mesh (direct mode): the records of an indirect mesh draw (max of them, stride apart; 0: 16) */
uint64_t nvn_gpu_guest(uint64_t gpu);
void game_indirect_mesh_draw(uint64_t gpu, int max, int stride) {
    const uint64_t rec = rx_direct() && gpu ? nvn_gpu_guest(gpu) : 0;
    if (!rec) return;
    if (stride <= 0) stride = 16;
    if (max <= 0) max = 1;
    for (int i = 0; i < max && i < 64; i++) {
        const uint64_t r = rec + (uint64_t)i * (uint64_t)stride;
        os_atomic_inc(&n.records);
        if (!hos_guest_range(r, 16)) { os_atomic_inc(&n.outside); continue; }
        if (G32(r + 4) || G32(r + 8)) continue;
        G32(r + 4) = 1, G32(r + 8) = 1;
        nvn_claim_fill(r + 4, 8, 1);
        os_atomic_inc(&n.set);
    }
}

void mesh_lists_log(double frames) {
    static int64_t last[5];
    const int64_t now[5] = {n.resets, n.reads, n.records, n.set, n.outside};
    hos_log("direct patch mesh lists (cull_gpu_lists_reset's and cull_gpu_list_reset_one's resets on the GPU, their counts' "
            "records copied out; GPU-reset records -> {x, 1, 1, n}): %.2f first-record resets a frame, %.2f count reads; records "
            "drawn %.2f a frame; set once %lld since the start; outside guest memory %lld",
            (now[0] - last[0]) / frames, (now[1] - last[1]) / frames, (now[2] - last[2]) / frames, (long long)now[3],
            (long long)(now[4] - last[4]));
    for (int i = 0; i < 5; i++) last[i] = now[i];
}
