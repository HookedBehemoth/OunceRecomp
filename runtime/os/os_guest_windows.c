/* The guest window on Windows (os_guest.h): reserved at its address, committed as the runtime hands ranges out
   (os_guest_commit) or at the first touch (a fault handler, first in os.h's list: it commits the 64 KB granule around
   a fault on a reserved page of the window). */
#if defined(_WIN32)   /* (sources.txt lists both systems' files: each compiles on its own) */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "os_guest.h"
#include "os.h"

#include <windows.h>

static uint8_t *window;
static uint64_t window_size;
#define TOUCH_GRANULE 0x10000u

static bool in_window(uintptr_t a) { return window && a >= (uintptr_t)window && a - (uintptr_t)window < window_size; }

/* commits the reserved pages of [lo, hi) (committed ones stay as they are): 0 or < 0 */
static int commit(uintptr_t lo, uintptr_t hi) {
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) return OS_EFAIL;
        uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (end > hi) end = hi;
        if (mi.State == MEM_RESERVE && !VirtualAlloc((void *)a, end - a, MEM_COMMIT, PAGE_READWRITE))
            return GetLastError() == ERROR_COMMITMENT_LIMIT || GetLastError() == ERROR_NOT_ENOUGH_MEMORY ? OS_ENOMEM : OS_EFAIL;
        a = end;
    }
    return 0;
}

/* a fault on a reserved page of the window: its granule committed, and the access made again. Committed pages that
   fault (the null guard, protected pages) are not this handler's */
static int on_fault(const os_fault *f) {
    if (f->kind != OS_FAULT_ACCESS || !in_window(f->addr)) return OS_FAULT_PASS;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)f->addr, &mi, sizeof mi) || mi.State != MEM_RESERVE) return OS_FAULT_PASS;
    uintptr_t lo = f->addr & ~(uintptr_t)(TOUCH_GRANULE - 1), hi = lo + TOUCH_GRANULE;
    const uintptr_t rlo = (uintptr_t)mi.BaseAddress, rhi = rlo + mi.RegionSize;   /* (the reserved run around it) */
    if (lo < rlo) lo = rlo;
    if (hi > rhi) hi = rhi;
    return VirtualAlloc((void *)lo, hi - lo, MEM_COMMIT, PAGE_READWRITE) ? OS_FAULT_RESUME : OS_FAULT_PASS;
}

int os_guest_reserve(void *base, uint64_t size) {
    void *p = VirtualAlloc(base, (SIZE_T)size, MEM_RESERVE, PAGE_READWRITE);
    if (p != base) {
        if (p) VirtualFree(p, 0, MEM_RELEASE);
        return OS_EFAIL;
    }
    window = (uint8_t *)base, window_size = size;
    os_fault_add(on_fault, true);
    return 0;
}

int os_guest_commit(void *p, size_t n) {
    if (!n) return 0;
    const uintptr_t lo = (uintptr_t)p & ~(uintptr_t)4095, hi = ((uintptr_t)p + n + 4095) & ~(uintptr_t)4095;
    if (!in_window(lo) || !in_window(hi - 1)) return OS_EINVAL;
    return commit(lo, hi);
}

void os_guest_discard(void *p, size_t n) {
    const uintptr_t lo = ((uintptr_t)p + 4095) & ~(uintptr_t)4095, hi = ((uintptr_t)p + n) & ~(uintptr_t)4095;
    if (hi <= lo || !in_window(lo) || !in_window(hi - 1)) return;
    /* (only the committed runs: decommitted, and committed again, they read as zero and hold no memory until touched) */
    for (uintptr_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) return;
        uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (end > hi) end = hi;
        if (mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE) {
            VirtualFree((void *)a, end - a, MEM_DECOMMIT);
            VirtualAlloc((void *)a, end - a, MEM_COMMIT, PAGE_READWRITE);   /* (on failure left reserved: the next touch commits) */
        }
        a = end;
    }
}

uint64_t os_guest_resident(void) { return 0; }
#endif
