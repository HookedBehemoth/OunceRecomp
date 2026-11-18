#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct { uint64_t x[32]; } Ctx;
typedef struct {
    uint32_t magic, flags;
    uint8_t *mem;
    uint64_t size, gpu;
    void *vmap;
} nvn_pool;
#define GP(a) ((void *)(uintptr_t)(a))
#define P(i) ((void *)(c->x[i] ? GP(c->x[i]) : NULL))
#define RET(v) (c->x[0] = (uint64_t)(v))
#define NV(name) __attribute__((noinline)) static void n_##name(Ctx *c, void *u_)
#define NVN_MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS 0x1
static int logs;
static void hos_log(const char *fmt, ...) { (void)fmt; logs++; }
static void die(const char *fmt, ...) { (void)fmt; }
static uint64_t h2g(void *p) { return (uint64_t)(uintptr_t)p; }
#define RETP(p) RET(h2g(p))
#define U64(i) (c->x[i])
static int invalidated;
static void rx_nvn_invalidate(uint64_t a, uint64_t b) { (void)a; (void)b; invalidated++; }
static int tl_grid_reader;   /* (the grid reader's PATCH_FUNC marks its run) */
int game_invalidate_unneeded(uint64_t size) { return tl_grid_reader && size == 0x3980; }   /* (the game's hooks.c) */
@GUARD@
@FLAGS@
@SIZE@
@MAP@
@ADDRESS@
@INVALIDATE@
int main(void) {
    Ctx c;
    memset(&c, 0, sizeof c);

    /* Before the guard these crash: P(0) is NULL and the accessor reads through it. */
    n_nvnMemoryPoolGetFlags(&c, NULL);
    if (c.x[0] != 0) {
        fprintf(stderr, "FAIL: flags of no memory pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    n_nvnMemoryPoolGetSize(&c, NULL);
    if (c.x[0] != 0) {
        fprintf(stderr, "FAIL: size of no memory pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    n_nvnMemoryPoolMap(&c, NULL);
    if (c.x[0] != 0) {
        fprintf(stderr, "FAIL: map of no memory pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    n_nvnMemoryPoolGetBufferAddress(&c, NULL);
    if (c.x[0] != 0) {
        fprintf(stderr, "FAIL: buffer address of no memory pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    n_nvnMemoryPoolInvalidateMappedRange(&c, NULL);

    /* A real pool still answers as before. */
    nvn_pool pool;
    memset(&pool, 0, sizeof pool);
    pool.magic = NVN_MAGIC('P', 'O', 'O', 'L');
    pool.flags = 0x40;
    pool.mem = (uint8_t *)(uintptr_t)0x1234;
    pool.size = 0x2000;
    pool.gpu = 0x5678;
    c.x[0] = (uint64_t)(uintptr_t)&pool;
    n_nvnMemoryPoolGetFlags(&c, NULL);
    if (c.x[0] != 0x40) {
        fprintf(stderr, "FAIL: flags of a real pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    c.x[0] = (uint64_t)(uintptr_t)&pool;
    n_nvnMemoryPoolGetSize(&c, NULL);
    if (c.x[0] != 0x2000) {
        fprintf(stderr, "FAIL: size of a real pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    c.x[0] = (uint64_t)(uintptr_t)&pool;
    n_nvnMemoryPoolMap(&c, NULL);
    if (c.x[0] != 0x1234) {
        fprintf(stderr, "FAIL: map of a real pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    c.x[0] = (uint64_t)(uintptr_t)&pool;
    n_nvnMemoryPoolGetBufferAddress(&c, NULL);
    if (c.x[0] != 0x5678) {
        fprintf(stderr, "FAIL: buffer address of a real pool = %#llx\n", (unsigned long long)c.x[0]);
        return 1;
    }
    c.x[0] = (uint64_t)(uintptr_t)&pool;
    n_nvnMemoryPoolInvalidateMappedRange(&c, NULL);
    if (invalidated != 1) {
        fprintf(stderr, "FAIL: a real pool's invalidate reached the renderer %d times\n", invalidated);
        return 1;
    }
    /* OoT's occlusion grid slots (0x3980 bytes, while f_80365f00 runs: its hook marks it): no wait, whatever the return
       address; others as before */
    tl_grid_reader = 1;
    for (int i = 0; i < 4; i++) {
        c.x[0] = (uint64_t)(uintptr_t)&pool, c.x[1] = 0, c.x[2] = 0x3980, c.x[30] = 0x80000000u + 4u * i;
        n_nvnMemoryPoolInvalidateMappedRange(&c, NULL);
    }
    if (invalidated != 1) {
        fprintf(stderr, "FAIL: the occlusion grid's invalidates reached the renderer\n");
        return 1;
    }
    c.x[0] = (uint64_t)(uintptr_t)&pool, c.x[2] = 0xc0;   /* (another size in the reader: waits) */
    n_nvnMemoryPoolInvalidateMappedRange(&c, NULL);
    tl_grid_reader = 0;
    if (invalidated != 2) {
        fprintf(stderr, "FAIL: another size's invalidate in the grid reader skipped the renderer\n");
        return 1;
    }
    c.x[0] = (uint64_t)(uintptr_t)&pool, c.x[2] = 0xc0, c.x[30] = 0x80357d40;   /* (the exposure meter: waits) */
    n_nvnMemoryPoolInvalidateMappedRange(&c, NULL);
    c.x[0] = (uint64_t)(uintptr_t)&pool, c.x[2] = 0x3980, c.x[30] = 0x80357d40;
    n_nvnMemoryPoolInvalidateMappedRange(&c, NULL);
    if (invalidated != 4) {
        fprintf(stderr, "FAIL: an invalidate elsewhere skipped the renderer\n");
        return 1;
    }
    puts("PASS: a null memory pool answers 0; a real pool keeps its flags, size, mapping and address");
    return 0;
}
