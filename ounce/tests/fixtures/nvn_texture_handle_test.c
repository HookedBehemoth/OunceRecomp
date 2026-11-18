#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint64_t x[32]; } Ctx;
#define I(i) ((int)c->x[i])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define NV(name) static void n_##name(Ctx *c, void *unused)
static void die(const char *fmt, ...) { (void)fmt; exit(86); }
@BODY@
static const struct { uint64_t texture, sampler, expected; } cases[] = {
    {0xffffffff, 264, UINT64_MAX},
    {0, 264, 0},
    {0, 0xffffffff, 0},
    {1, 264, 0x10800001},
    {0xfffff, 0xfff, 0xffffffff},
    {0x100000, 264, 0x10900000},
    {0xfffffffe, 264, UINT64_MAX - 1},
    {1, 0xffffffff, UINT64_C(0xfffffffffff00001)},
    {0x80000000, 0, UINT64_C(0xffffffff80000000)},
    {1, 0x80000000, UINT64_C(0xfff8000000000001)},
    {0xfeedface00000000, 264, 0},
    {0x12345678ffffffff, 264, UINT64_MAX},
    {1, 0xfeedface00000108, 0x10800001},
};
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    unsigned i = (unsigned)atoi(argv[1]);
    if (i >= sizeof cases / sizeof *cases) return 2;
    Ctx c, before;
    memset(&c, 0xa5, sizeof c);
    c.x[1] = cases[i].texture;
    c.x[2] = cases[i].sampler;
    before = c;
    n_nvnDeviceGetTextureHandle(&c, NULL);
    if (c.x[0] != cases[i].expected) {
        fprintf(stderr, "case %u: got %016llx, expected %016llx\n", i,
                (unsigned long long)c.x[0], (unsigned long long)cases[i].expected);
        return 1;
    }
    if (memcmp(c.x + 1, before.x + 1, sizeof c.x - sizeof c.x[0])) return 1;
    return 0;
}
