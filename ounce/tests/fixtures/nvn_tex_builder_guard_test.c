#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define os_atomic_inc(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)

typedef struct { uint64_t x[32]; } Ctx;
typedef struct { uint32_t swizzle_r, swizzle_g, swizzle_b, swizzle_a; } nvn_texdesc;
typedef struct { nvn_texdesc d; } nvn_tex_builder;
#define GP(a) ((void *)(uintptr_t)(a))
#define P(i) ((void *)(c->x[i] ? GP(c->x[i]) : NULL))
#define RET(v) (c->x[0] = (uint64_t)(v))
#define U32(i) ((uint32_t)c->x[i])
#define NV(name) __attribute__((noinline)) static void n_##name(Ctx *c, void *u_)
static int logs;
static void hos_log(const char *fmt, ...) { (void)fmt; logs++; }
static void die(const char *fmt, ...) { (void)fmt; }
@GUARD@
@TB@
@SWIZZLE@
int main(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[1] = 1, c.x[2] = 2, c.x[3] = 3, c.x[4] = 4;

    /* Before the guard this is the crash: P(0) is NULL and the setter writes NULL + 0x5c. */
    n_nvnTextureBuilderSetSwizzle(&c, NULL);
    if (logs != 1) {
        fprintf(stderr, "FAIL: a null texture builder logged %d times\n", logs);
        return 1;
    }

    /* A real builder still answers as before. */
    nvn_tex_builder b;
    memset(&b, 0, sizeof b);
    c.x[0] = (uint64_t)(uintptr_t)&b;
    n_nvnTextureBuilderSetSwizzle(&c, NULL);
    if (b.d.swizzle_r != 1 || b.d.swizzle_g != 2 || b.d.swizzle_b != 3 || b.d.swizzle_a != 4) {
        fprintf(stderr, "FAIL: a real builder's swizzle = %u %u %u %u\n",
                b.d.swizzle_r, b.d.swizzle_g, b.d.swizzle_b, b.d.swizzle_a);
        return 1;
    }
    if (logs != 1) {
        fprintf(stderr, "FAIL: a real texture builder logged %d times\n", logs);
        return 1;
    }
    puts("PASS: a null texture builder is a scratch one; a real builder keeps its swizzle");
    return 0;
}
