/* Game patch, direct mode only: the GPU culling lists' mesh draw argument records in Vulkan's layout
.

   vkCmdDrawMeshTasksIndirectEXT reads a record as {groupCountX, Y, Z} = w0, w1, w2; NVN reads {count = w0, first = w1}.
   The culling shaders write only w0 (atomicMax) and w3 (atomicAdd), so {x, 1, 1, n} is Vulkan's record once w1 = w2 = 1.
   M4b's patch covers the lists f_80362ff0 resets. The other GPU lists (the headers at 0x117a4d6000, 0x117caa0000,
   0x117899a000, 0x1178b7e000, 0x117982a000, ... in Kokiri) are not reset there:
     - f_80361c80, once a frame, resets the first record of each of them on the CPU: `str xzr, [x19, layout[+4]]` (w0 and
       w1: NVN's count and first) and `str wzr, [x19, layout[+0x10]]` (the appended count). Found by a page watch on
       0x117a4d6000 (HOS_DBG=memwatch, run m4d_mw_k: lr 0x80362038, every frame, the only CPU writer after the buffer's
       clear at creation). An At row after each of its 64-bit zero stores writes w1 = w2 = 1 after it.
     - f_80361a00, called twice from f_80361c80 (0x80361e08, 0x80361e10: the lists at [x27+0x180], [x27+0x1b8], in
       Kokiri the headers 0x117982a000 and 0x1178b7e000), resets its list's first record the same way (`str xzr, [x22, x8]`
       at 0x80361b3c, x8 = layout[+4]; `str wzr` the appended count). M4d took the w1 = 0 these two kept for a GPU
       write; a page watch (HOS_DBG=memwatch, run m4e_mw_k: guest lr 0x80361b2c < 0x80361e14, every frame) found this
       CPU store, and the culling shader 3de6d86e touches the record only by atomicMax(+0) and atomicAdd(+12) (its IR):
       no shader needs replacing. The same row follows it.
     - their other records are reset on the GPU (w0 and w3 only; w1 and w2 are never written by anyone: M4b's sentinel).
       They get w1 = w2 = 1 where an indirect mesh draw names them (nvnCommandBufferDrawMeshTasksIndirect /
       MultiDrawMeshTasksIndirectCount, nvn_native.c, direct mode), once: only a record whose w1 and w2 are both 0 is
       written, which nothing reads but the draw (NVN's first is 0 there, as the back end takes it for every indirect
       mesh draw).
   Check that it applies: the 600-present line "direct patch mesh lists". */
#include "direct.h"
#include "os.h"

static struct { volatile int64_t resets, outside, records, set, set_logged; } n;

static void put(uint64_t rec) {
    if (!hos_guest_range(rec, 16)) { os_atomic_inc(&n.outside); return; }
    G32(rec + 4) = 1, G32(rec + 8) = 1;
}

/* (At rows, a PATCH_AT declaration, runtime/patch.h) f_80361c80: after each `str xzr, [x19, x8]`; f_80361a00: after its
   `str xzr, [x22, x8]` (a list's first record, w0 and w1, zeroed): the list and the record's offset */
PATCH_AT(0x80361d5c, word(0xf8286a7f), after, none, direct_mesh_lists_reset, as(x19, list), as(x8, off)) {
    put(list + off);
    os_atomic_inc(&n.resets);
}
PATCH_AT_SITE(0x80362048, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x803620b0, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362118, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362180, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x803621e8, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362848, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x803628b0, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362918, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362b80, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362be8, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80362c50, word(0xf8286a7f), after, none, direct_mesh_lists_reset, x19, x8);
PATCH_AT_SITE(0x80361b3c, word(0xf8286adf), after, none, direct_mesh_lists_reset, x22, x8);

/* nvn_native.c draw_mesh (direct mode): the records of an indirect mesh draw (max of them, stride apart; 0: 16) */
uint64_t nvn_gpu_guest(uint64_t gpu);
void direct_mesh_lists_draw(uint64_t gpu, int max, int stride) {
    const uint64_t rec = rx_direct() && gpu ? nvn_gpu_guest(gpu) : 0;
    if (!rec) return;
    if (stride <= 0) stride = 16;
    if (max <= 0) max = 1;
    for (int i = 0; i < max && i < 64; i++) {
        const uint64_t r = rec + (uint64_t)i * (uint64_t)stride;
        os_atomic_inc(&n.records);
        if (!hos_guest_range(r, 16)) { os_atomic_inc(&n.outside); continue; }
        if (G32(r + 4) || G32(r + 8)) continue;
        put(r);
        os_atomic_inc(&n.set);
    }
}

void direct_mesh_lists_log(double frames) {
    static int64_t last[4];
    const int64_t now[4] = {n.resets, n.records, n.set, n.outside};
    hos_log("direct patch mesh lists (f_80361c80's and f_80361a00's first-record resets and the GPU-reset records -> {x, 1, 1, n}): %.2f resets a "
            "frame; records drawn %.2f a frame; set once %lld since the start; outside guest memory %lld",
            (now[0] - last[0]) / frames, (now[1] - last[1]) / frames, (long long)now[2], (long long)(now[3] - last[3]));
    for (int i = 0; i < 4; i++) last[i] = now[i];
}
