#pragma once
#include <stdint.h>
#include <stddef.h>
#include "patch.h"
extern uint8_t *fake_heap;
#define HOS_HEAP_BASE 0x1000000000ULL
#define A64_PTR(a) (fake_heap + ((uint64_t)(a) - HOS_HEAP_BASE))
#define GP(a) ((void *)A64_PTR(a))
extern uint64_t hos_heap_size;
void hos_log(const char *fmt, ...);
int64_t hos_now_ns(void);
