
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct GuestContext { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
#define A64_NOESC
/* (a 64 KB guest window, then a page the test watches) */
extern uint8_t *window;
#define WINDOW 0x10000ull
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)(window + (a)))
#define HOS_WRITTEN(p, n) ((void)0)
typedef struct GuestCall GuestCall;
typedef uint64_t hos_q __attribute__((vector_size(16)));
#include "patch.h"
static inline void *hos_guest_range(uint64_t a, uint64_t n) { return a < WINDOW && n <= WINDOW - a ? GP(a) : (void *)0; }
void hos_fatal(Ctx *c, const char *fmt, ...);
