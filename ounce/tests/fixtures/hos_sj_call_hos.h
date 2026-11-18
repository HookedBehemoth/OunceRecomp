
#include <stdint.h>
#include <stdio.h>
#define A64_NOESC
typedef struct { uint64_t x[32]; uint64_t pc; } Ctx;
static int hos_dbg_item(const char *w) { (void)w; return 0; }
static void hos_log(const char *f, ...) { (void)f; }
void a64_icall(Ctx *c, uint64_t target);
void **recomp_sj_push(Ctx *c, uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3);
int recomp_sj_setjmp(void **buf);
uint64_t recomp_sj_target(void **buf);
void recomp_sj_pop(void **buf);
