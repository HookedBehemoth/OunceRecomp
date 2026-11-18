
#include "hos_setjmp.c"
#include <stdio.h>
#define RA 0x81000010u
#define RB 0x81000020u
static uint64_t icall_target[8]; static uint64_t icall_x0[8]; static int icall_depth[8]; static int icalls;
static int tail_a, tail_b, after_inner;
static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf(__VA_ARGS__); printf("\n"); bad = 1; } } while (0)
static int depth(void) { return stack ? stack->depth : 0; }
/* the code at a setjmp return address, as lifted: x0 is longjmp's value */
void a64_icall(Ctx *c, uint64_t target) {
    icall_target[icalls] = target; icall_depth[icalls] = stack->depth; icall_x0[icalls++] = c->x[0]; c->x[1] = 0x1234;
}
/* the guest longjmp's BR x30: a64_ijump_hook with the registers restored (x0 the value) */
__attribute__((noinline)) static void guest_longjmp(Ctx *c, uint64_t to, uint64_t value) {
    c->x[0] = value;
    a64_ijump_hook(c, to);
    printf("longjmp to %#llx came back\n", (unsigned long long)to); bad = 1;
}
static void owner_returns(Ctx *c) { c->x[0] = 42; }
static void owner_jumps(Ctx *c) { guest_longjmp(c, RA, 7); tail_a = 1; }
static void inner_to_outer(Ctx *c) { CHECK(depth() == 2, "nested: depth %d in the inner owner", depth()); guest_longjmp(c, RA, 9); tail_b = 1; }
static void inner_to_self(Ctx *c) { guest_longjmp(c, RB, 5); tail_b = 1; }
static void outer_to_outer(Ctx *c) { hos_sj_call(c, inner_to_outer, RB, 0, 0, 0); after_inner = 1; }
static void outer_to_inner(Ctx *c) { hos_sj_call(c, inner_to_self, RB, 0, 0, 0); after_inner = 1; c->x[2] = 77; }
int main(void) {
    Ctx c = {0};
    /* a direct return: the owner's result, no icall, the slot popped */
    hos_sj_call(&c, owner_returns, RA, 0, 0, 0);
    CHECK(c.x[0] == 42 && icalls == 0 && depth() == 0, "direct: x0 %llu, icalls %d, depth %d", (unsigned long long)c.x[0], icalls, depth());
    /* a jump to an address no owner pushed is not a longjmp: the hook returns */
    a64_ijump_hook(&c, RA);
    CHECK(icalls == 0, "a hook without a slot took a jump");
    /* a longjmp from the owner's callee: back in hos_sj_call, the code at RA with x0 = 7, the owner's tail skipped */
    hos_sj_call(&c, owner_jumps, RA, 0, 0, 0);
    CHECK(icalls == 1 && icall_target[0] == RA && icall_x0[0] == 7 && !tail_a && depth() == 0 && c.x[1] == 0x1234,
          "longjmp: icalls %d target %#llx x0 %llu tail %d depth %d", icalls, (unsigned long long)icall_target[0],
          (unsigned long long)icall_x0[0], tail_a, depth());
    /* nested, the inner owner's callee longjmps to the outer owner: the inner slot is dropped, the outer caller runs RA */
    icalls = 0;
    hos_sj_call(&c, outer_to_outer, RA, 0, 0, 0);
    CHECK(icall_depth[0] == 1, "nested to outer: %d slots while the outer caller runs RA (the inner one not dropped)", icall_depth[0]);
    CHECK(icalls == 1 && icall_target[0] == RA && icall_x0[0] == 9 && !tail_b && !after_inner && depth() == 0,
          "nested to outer: icalls %d target %#llx x0 %llu tail %d after %d depth %d", icalls,
          (unsigned long long)icall_target[0], (unsigned long long)icall_x0[0], tail_b, after_inner, depth());
    /* nested, to the inner owner's own setjmp: the inner caller runs RB, then the outer owner goes on and returns */
    icalls = 0; c.x[2] = 0;
    hos_sj_call(&c, outer_to_inner, RA, 0, 0, 0);
    CHECK(icalls == 1 && icall_target[0] == RB && icall_x0[0] == 5 && !tail_b && after_inner && c.x[2] == 77 && depth() == 0,
          "nested to inner: icalls %d target %#llx x0 %llu tail %d after %d x2 %llu depth %d", icalls,
          (unsigned long long)icall_target[0], (unsigned long long)icall_x0[0], tail_b, after_inner,
          (unsigned long long)c.x[2], depth());
    /* many longjmps in a row: the host stack does not grow (each comes back to its own hos_sj_call frame) */
    icalls = 0;
    for (int i = 0; i < 100000; i++) { hos_sj_call(&c, owner_jumps, RA, 0, 0, 0); icalls = 0; }
    CHECK(depth() == 0, "repeated: depth %d", depth());
    if (!bad) printf("ok\n");
    return bad;
}
