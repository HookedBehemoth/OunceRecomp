/* The guest window on Windows (os_guest.h): reserved at its address, committed as the runtime hands ranges out
   (os_guest_commit) or at the first touch (a fault handler, first in os.h's list: it commits the 64 KB granule around
   a fault on a reserved page of the window), its ranges exported as host pointers of its own pages.

   An exported range stays committed for the process's life: the driver has its pages pinned, and decommitted pages
   under an import fault the device. A discard of such a range zeroes it instead (the runtime already does that for
   the ranges the renderer imported, rx_guest_imported; this is the same rule kept here too, by 2 MB granules). */
#if defined(_WIN32)   /* (sources.txt lists both systems' files: each compiles on its own) */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "os_guest.h"
#include "os.h"

#include <windows.h>
#include <string.h>

static uint8_t *window;
static uint64_t window_size;
static volatile uint64_t *exported;   /* (a bit per 2 MB granule of the window: part of an export) */
#define EXPORT_GRANULE_SHIFT 21
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
    const uint64_t granules = (size + (1ull << EXPORT_GRANULE_SHIFT) - 1) >> EXPORT_GRANULE_SHIFT;
    exported = (volatile uint64_t *)VirtualAlloc(NULL, (SIZE_T)((granules + 63) / 64 * 8), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!exported) {
        VirtualFree(base, 0, MEM_RELEASE);
        return OS_ENOMEM;
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

static bool any_exported(uintptr_t lo, uintptr_t hi) {
    for (uint64_t g = (lo - (uintptr_t)window) >> EXPORT_GRANULE_SHIFT; g <= (hi - 1 - (uintptr_t)window) >> EXPORT_GRANULE_SHIFT; g++)
        if (__atomic_load_n(&exported[g >> 6], __ATOMIC_ACQUIRE) >> (g & 63) & 1) return true;
    return false;
}

void os_guest_discard(void *p, size_t n) {
    const uintptr_t lo = ((uintptr_t)p + 4095) & ~(uintptr_t)4095, hi = ((uintptr_t)p + n) & ~(uintptr_t)4095;
    if (hi <= lo || !in_window(lo) || !in_window(hi - 1)) return;
    if (any_exported(lo, hi)) {
        memset((void *)lo, 0, hi - lo);
        return;
    }
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

bool os_guest_exportable(uint64_t *max_bytes, const char **note) {
    if (note) *note = NULL;
    if (!window) {
        if (note) *note = "no guest window";
        return false;
    }
    if (max_bytes) *max_bytes = 1ull << 32;   /* (host pointers have no export limit of their own: layer 0's 16 MB chunks) */
    return true;
}

int os_guest_export(uint64_t offset, uint64_t size, os_gpu_handle *out) {
    out->kind = OS_GPU_NONE, out->fd = -1, out->ptr = NULL, out->size = 0;
    if (!window || !size || offset + size > window_size || (offset | size) & 4095) return OS_EINVAL;
    const uintptr_t lo = (uintptr_t)window + offset, hi = lo + size;
    const int r = commit(lo, hi);
    if (r < 0) return r;
    for (uint64_t g = offset >> EXPORT_GRANULE_SHIFT; g <= (offset + size - 1) >> EXPORT_GRANULE_SHIFT; g++)
        __atomic_fetch_or(&exported[g >> 6], 1ull << (g & 63), __ATOMIC_RELEASE);
    out->kind = OS_GPU_HOST_POINTER, out->ptr = (void *)lo, out->size = size;
    return 0;
}

void os_gpu_handle_close(os_gpu_handle *h) {
    h->kind = OS_GPU_NONE, h->fd = -1, h->ptr = NULL;
}
#endif
