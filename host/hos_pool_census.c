/* (native-vk) The pool census: which lifted functions read or write NVN memory pool pages, how often and how much.

   Built only with RX_POOL_CENSUS=1 (nx2/build.py adds -DA64_POOL_CENSUS to the lifted code and the runtime). The
   lifted loads and stores (not the atomics) look their page up in a64_pool_page (rt/a64.h A64_CENSUS); a page of a live pool calls
   a64_census_hit, whose return address is the access's place in the lifted function. The hits are counted per
   (place, pool, direction) and written every HOS_POOL_CENSUS_SEC seconds (10) to HOS_POOL_CENSUS=<file>;
   scripts/pool_census.py names the places from the executable's symbols.

   Why: a native renderer puts the pools in Vulkan memory (device-local and host-visible, placed at the guest
   address). CPU writes into it are cheap, CPU reads from it are not, and texel data the CPU writes in the
   console's block-linear layout means nothing to a Vulkan image. The census lists the code that has to change. */
#ifdef A64_POOL_CENSUS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

void hos_log(const char *fmt, ...);
static double hos_census_seconds(void) {
    static struct timespec t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!t0.tv_sec) t0 = t;
    return (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
}

#define CENSUS_PAGES (1ull << 27)
uint16_t a64_pool_page[CENSUS_PAGES];   /* (512 GB window / 4 KB pages: the pool's index + 1, 0 for none; untouched pages stay uncommitted) */

#define NPOOL 65535
typedef struct { uint64_t base, size; uint32_t flags; uint32_t serial; } CensusPool;
static CensusPool pools[NPOOL + 1];
static uint32_t npools;   /* (indices are never reused: a finalized pool's hits keep its record) */
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* key: the place (offset in the executable, 32 bits) | write << 32 | (the guest LR - 0x80000000, 27 bits) << 33. The
   caller (x30) names the call site when the place is a leaf such as memcpy (sdk f_8575cdc0: 90% of the traffic); in
   other functions it is the function's own caller. The pool is a field: the last one seen, and every kind seen. */
#define NHIT (1u << 21)
typedef struct { uint64_t key; uint64_t count; uint64_t bytes; uint32_t pool, flags; } Hit;
static Hit hits[NHIT];
static uint64_t dropped;
extern char __executable_start;

void hos_census_pool(uint64_t base, uint64_t size, uint32_t flags, int on) {
    pthread_mutex_lock(&pool_lock);
    const uint64_t lo = base >> 12, hi = (base + size + 0xFFF) >> 12;
    if (on && npools < NPOOL) {
        const uint32_t i = ++npools;
        pools[i] = (CensusPool){base, size, flags, i};
        for (uint64_t p = lo; p < hi && p < CENSUS_PAGES; p++) a64_pool_page[p] = (uint16_t)i;
    } else if (!on) {
        for (uint64_t p = lo; p < hi && p < CENSUS_PAGES; p++) a64_pool_page[p] = 0;
    }
    pthread_mutex_unlock(&pool_lock);
}

__attribute__((noinline)) void a64_census_hit(uint64_t a, uint32_t n, int w, uint64_t lr) {
    const uint64_t place = (uint64_t)((uintptr_t)__builtin_return_address(0) - (uintptr_t)&__executable_start) & 0xFFFFFFFFull;
    const uint32_t pi = a64_pool_page[(a >> 12) & (CENSUS_PAGES - 1)];
    if (!pi) return;
    const uint64_t caller = lr >= 0x80000000ull && lr < 0x88000000ull ? lr - 0x80000000ull : 0x7FFFFFFull;
    const uint64_t key = place | (uint64_t)(w != 0) << 32 | caller << 33;
    uint64_t h = key * 0x9E3779B97F4A7C15ULL;
    for (uint32_t probe = 0; probe < 64; probe++) {
        Hit *e = &hits[(h >> 43 ^ probe) & (NHIT - 1)];   /* (h >> 43: 21 bits) */
        uint64_t k = __atomic_load_n(&e->key, __ATOMIC_ACQUIRE);
        if (!k) {
            uint64_t zero = 0;
            if (!__atomic_compare_exchange_n(&e->key, &zero, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) k = zero;
            else k = key;
        }
        if (k != key) continue;
        __atomic_fetch_add(&e->count, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&e->bytes, n, __ATOMIC_RELAXED);
        e->pool = pi;
        __atomic_fetch_or(&e->flags, pools[pi].flags, __ATOMIC_RELAXED);
        return;
    }
    __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
}

static void census_write(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    /* (version 2: places are offsets from __executable_start, scripts/pool_census.py names them) */
    fprintf(f, "version 2\ntime %.3f\ndropped %llu\n", hos_census_seconds(), (unsigned long long)dropped);
    pthread_mutex_lock(&pool_lock);
    for (uint32_t i = 1; i <= npools; i++)
        fprintf(f, "pool %u %#llx %#llx %#x\n", i, (unsigned long long)pools[i].base, (unsigned long long)pools[i].size,
                pools[i].flags);
    pthread_mutex_unlock(&pool_lock);
    for (uint32_t i = 0; i < NHIT; i++) {
        const uint64_t k = __atomic_load_n(&hits[i].key, __ATOMIC_ACQUIRE);
        if (!k) continue;
        /* hit <place offset> <r|w> <caller guest address> <last pool> <pool flags seen> <count> <bytes> */
        fprintf(f, "hit %#llx %c %#llx %u %#x %llu %llu\n", (unsigned long long)(k & 0xFFFFFFFFull), (k >> 32 & 1) ? 'w' : 'r',
                (unsigned long long)((k >> 33) + 0x80000000ull), hits[i].pool, hits[i].flags,
                (unsigned long long)hits[i].count, (unsigned long long)hits[i].bytes);
    }
    fclose(f);
    rename(tmp, path);
}

static void *census_thread(void *arg) {
    const char *path = (const char *)arg;
    const char *s = getenv("HOS_POOL_CENSUS_SEC");
    const int sec = s && atoi(s) > 0 ? atoi(s) : 10;
    char numbered[4096];
    for (int n = 1;; n++) {
        sleep((unsigned)sec);
        snprintf(numbered, sizeof numbered, "%s.%03d", path, n);   /* (every dump kept: a phase is the difference of two) */
        census_write(numbered);
        census_write(path);
    }
    return NULL;
}

__attribute__((constructor)) static void census_start(void) {
    const char *path = getenv("HOS_POOL_CENSUS");
    if (!path || !*path) return;
    (void)hos_census_seconds();   /* (time 0: the process start) */
    pthread_t t;
    pthread_create(&t, NULL, census_thread, (void *)strdup(path));
    pthread_detach(t);
}

void hos_census_flush(void) {
    const char *path = getenv("HOS_POOL_CENSUS");
    if (path && *path) census_write(path);
}
#else
#include <stdint.h>
void hos_census_pool(uint64_t base, uint64_t size, uint32_t flags, int on) { (void)base; (void)size; (void)flags; (void)on; }
void hos_census_flush(void) {}
#endif
