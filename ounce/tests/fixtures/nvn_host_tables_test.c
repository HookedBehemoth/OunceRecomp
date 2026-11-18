#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include "rx_nvn.h"
#include "nvn_list.h"
static int list_ref_mode;   /* (HOS_NVN_LIST_REF: the slots from each thread's own list; nvn_list_drop at Finalize) */
static int list_ref(void) { return list_ref_mode; }
static int drops;
void nvn_list_drop(nvn_lw *w) { (void)w; drops++; }

#define WINDOW (1u << 20)
static uint8_t *A64_MEM;
#define GP(a) ((void *)(A64_MEM + (uint64_t)(a)))
#define HOS_SPACE_END ((uint64_t)WINDOW)
typedef int os_mutex;
#define OS_MUTEX_INIT 0
static void os_mutex_lock(os_mutex *l) { (void)l; }
static void os_mutex_unlock(os_mutex *l) { (void)l; }
#define os_atomic_inc(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define os_atomic_dec(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define os_atomic_add(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
static void hos_log(const char *f, ...) { (void)f; }
static jmp_buf died;
static int dying;
static void die(const char *fmt, ...) { (void)fmt; if (dying) longjmp(died, 1); fprintf(stderr, "unexpected die\n"); exit(3); }
static uint64_t h2g(const void *p) { return p ? (uint64_t)((const uint8_t *)p - A64_MEM) : 0; }
#define POOL_MAX 26
static int pool_off(void) { return 0; }
static unsigned pool_class(size_t n) { (void)n; return 0; }
static int puts_count;
static void pool_put(void *b, unsigned c) { (void)c; free(b); puts_count++; }
@PIECES@
static void require(int ok, const char *m) { if (!ok) { fprintf(stderr, "FAIL: %s\n", m); exit(1); } }
static int dies(void *(*f)(uint64_t, uint64_t), uint64_t a, uint64_t n) {
    dying = 1;
    if (setjmp(died)) { dying = 0; return 1; }
    f(a, n);
    dying = 0;
    return 0;
}
int main(void) {
    A64_MEM = calloc(1, WINDOW);
    require(A64_MEM != NULL, "test window");
    /* the guest window */
    require(hos_guest_range(0x1000, 16) == A64_MEM + 0x1000, "a range inside the window");
    require(hos_guest_range(WINDOW - 16, 16) != NULL, "a range ending at the window's end");
    require(hos_guest_range(WINDOW - 16, 17) == NULL, "a range past the window's end");
    require(hos_guest_range(WINDOW, 0) == NULL, "an address at the window's end");
    require(hos_guest_range(~0ull - 8, 16) == NULL, "a range that wraps around");
    require(hos_guest_range(0x1000, ~0ull) == NULL, "a size that wraps around");
    require(dies(gw, ~0ull - 0x100, 8), "gw accepted an address below the window");
    require(!dies(gw, 0x2000, 8), "gw refused a range inside the window");
    require(in_window(A64_MEM + 0x100, 8) == A64_MEM + 0x100, "in_window refused an object in the window");
    dying = 1;
    if (!setjmp(died)) { in_window(A64_MEM - 64, 8); require(0, "in_window accepted a pointer below the window"); }
    dying = 0;

    /* command buffers: a slot names its object */
    nvn_cmd *a = (nvn_cmd *)GP(0x4000), *b = (nvn_cmd *)GP(0x5000);
    cmd_rec *ra = cmd_adopt(a), *rb = cmd_adopt(b);
    require(ra != rb && a->slot && b->slot && a->slot != b->slot, "two command buffers share a slot");
    ra->seg = ra->cur = malloc(64), ra->end = ra->seg + 64;
    require(cmd_host(a) == ra, "a command buffer lost its own slot");
    const uint32_t a_slot = a->slot;
    b->slot = a_slot;   /* b's memory names a's slot */
    cmd_rec *forged = cmd_host(b);
    require(forged != ra && b->slot != a_slot && forged->seg == NULL, "a command buffer got another one's recording buffer");
    a->slot = 0x7fffffffu;   /* out of range */
    cmd_rec *fresh = cmd_host(a);
    require(fresh != ra && fresh->seg == NULL, "an out-of-range slot was used");
    a->slot = a_slot;
    require(cmd_host(a) == ra, "the object's own slot was not found again");
    puts_count = 0;
    cmd_release(a);
    require(puts_count == 1 && a->slot == 0 && ra->owner == 0, "Finalize did not give the slot and buffer back");
    b->slot = a_slot;   /* a freed slot is no one's */
    require(cmd_host(b) != NULL && cmd_host(b)->seg == NULL, "a freed slot's buffer was used");

    /* (HOS_NVN_LIST_REF) the same through each thread's free slots: a slot names its object, Finalize gives it back and
       drops the open recording, and a slot given back is no one's */
    list_ref_mode = 1;
    {
        nvn_cmd *c1 = (nvn_cmd *)GP(0x6000), *c2 = (nvn_cmd *)GP(0x7000);
        cmd_rec *r1 = cmd_adopt(c1), *r2 = cmd_adopt(c2);
        require(r1 != r2 && c1->slot && c2->slot && c1->slot != c2->slot, "list mode: two command buffers share a slot");
        require(cmd_host(c1) == r1 && r1->owner == h2g(c1), "list mode: a command buffer lost its own slot");
        const uint32_t s1 = c1->slot;
        c2->slot = s1;
        require(cmd_host(c2) != r1 && c2->slot != s1, "list mode: a command buffer got another one's slot");
        drops = 0;
        cmd_release(c1);
        require(drops == 1 && c1->slot == 0 && r1->owner == 0, "list mode: Finalize did not drop the recording and give the slot back");
        for (int i = 0; i < 200; i++) {   /* (past the thread's list: some go back to the shared one) */
            nvn_cmd *t = (nvn_cmd *)GP(0x8000 + 0x100 * (uint64_t)i);
            cmd_adopt(t);
        }
        for (int i = 0; i < 200; i++) cmd_release((nvn_cmd *)GP(0x8000 + 0x100 * (uint64_t)i));
        nvn_cmd *t = (nvn_cmd *)GP(0x8000);
        t->slot = s1;
        require(cmd_host(t) != NULL && t->slot != 0, "list mode: a freed slot was used by another object");
    }
    list_ref_mode = 0;

    /* recording handles */
    uint64_t h[5000];
    for (int i = 0; i < 5000; i++) { h[i] = 0x7f0000000000ull + (uint64_t)i * 0x40; live_handle_add(h[i]); }
    for (int i = 0; i < 5000; i++) require(live_handle_has(h[i]), "a live handle was not found");
    require(!live_handle_has(0x7f0000000008ull) && !live_handle_has(0) && !live_handle_has(0x1234),
            "a handle that was never made counts as live");
    for (int i = 0; i < 5000; i += 3) live_handle_remove(h[i]);
    for (int i = 0; i < 5000; i++) require(live_handle_has(h[i]) == (i % 3 != 0), "removing handles lost others");
    require(nlive == 5000 - 1667, "the live count");
    /* ReadTexels / WriteTexels: the buffer span the strides reach */
    require(texels_extent(64, 4, 1, 64, 256) == 256, "a packed 64x4 region");
    require(texels_extent(64, 4, 2, 128, 1024) == 1024 + 3 * 128 + 64, "strided rows and images");
    require(texels_extent(0, 4, 1, 0, 0) == 1 && texels_extent(64, 0, 1, 64, 0) == 1, "an empty region");
    require(texels_extent(64, 4, 1, 1ull << 63, 0) == 0, "a row stride that wraps around");
    require(texels_extent(64, 1, 3, 64, 1ull << 63) == 0, "an image stride that wraps around");
    require(!hos_guest_range(0x1000, texels_extent(64, 2, 1, WINDOW, 0)), "a row stride past the window was accepted");
    puts("PASS: guest window ranges, command buffer slots by owner, live recording handles, texel buffer extents");
    return 0;
}
