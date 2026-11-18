#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t x[32]; } Ctx;
typedef struct {
    uint32_t magic, flags;
    uint64_t mem;
    uint64_t size, gpu;
    uint64_t reserved;
} nvn_pool;
typedef struct { uint32_t magic; nvn_pool *pool; int64_t off; uint64_t size; } nvn_buffer;
#define GP(a) ((void *)(uintptr_t)(a))
#define P(i) ((void *)(c->x[i] ? GP(c->x[i]) : NULL))
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETP(p) RET((uint64_t)(uintptr_t)(p))
#define POOL(p) ((const nvn_pool *)(p))
#define NV(name) static void n_##name(Ctx *c, void *u_)

/* Single-threaded fixture: the host lock and allocator are stand-ins, not tested implementations. */
typedef int os_mutex;
#define OS_MUTEX_INIT 0
static void os_mutex_lock(os_mutex *lock) { (void)lock; }
static void os_mutex_unlock(os_mutex *lock) { (void)lock; }
static void hos_log(const char *format, ...) { (void)format; }
static void *allocations[4];
static unsigned allocation_count;
static uint64_t allocation_size;
static uint64_t hos_sys_alloc(uint64_t size, uint64_t alignment) {
    (void)alignment;
    if (!size || size > 1024 * 1024 || allocation_count == 4) exit(2);
    void *p = calloc(1, (size_t)size);
    if (!p) exit(2);
    allocations[allocation_count++] = p;
    allocation_size = size;
    return (uint64_t)(uintptr_t)p;
}
@SCRATCH@
@MAPPING@
static void require(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static uint8_t *map(nvn_buffer *buffer) {
    Ctx c = {0};
    c.x[0] = (uint64_t)(uintptr_t)buffer;
    c.x[30] = 0x1234;
    n_nvnBufferMap(&c, NULL);
    return (uint8_t *)(uintptr_t)c.x[0];
}
static uint8_t *fresh_scratch(nvn_buffer *buffer, uint64_t required) {
    uint8_t *p = map(buffer);
    require(p != NULL, "pool-less buffer map returned zero");
    require(allocation_count && p == allocations[allocation_count - 1] && allocation_size >= required,
            "pool-less buffer map did not provide the required storage");
    for (uint64_t i = 0; i < required; i++) require(p[i] == 0, "new scratch was not zeroed");
    p[0] = 0x2a;
    p[required - 1] = 0x7d;
    require(p[0] == 0x2a && p[required - 1] == 0x7d, "scratch endpoints were not writable");
    return p;
}
int main(int argc, char **argv) {
    (void)argv;
    nvn_buffer buffer = {0};
    if (argc > 1) {
        fresh_scratch(&buffer, 0x1000);  /* Fresh process: size zero must bootstrap a usable map. */
        puts("PASS: a zero-size pool-less buffer gets writable initial scratch");
    } else {
        buffer.size = 0x3d0;
        uint8_t *first = fresh_scratch(&buffer, buffer.size);
        require(map(&buffer) == first && first[0] == 0x2a && first[buffer.size - 1] == 0x7d,
                "mapping the same buffer discarded its pointer or contents");
        buffer.size = 0x20001;
        fresh_scratch(&buffer, buffer.size);
        require(first[0] == 0x2a && first[0x3cf] == 0x7d, "growing scratch invalidated the earlier map");
        unsigned before = allocation_count;
        uint8_t memory[32] = {0};
        nvn_pool pool = {0};
        pool.mem = (uint64_t)(uintptr_t)memory;
        buffer.pool = &pool;
        buffer.off = 7;
        buffer.size = 8;
        require(map(&buffer) == memory + 7, "a pooled map changed its pointer or offset");
        pool.mem = 0;
        require(map(&buffer) == NULL, "a pool without CPU memory must remain unmapped");
        require(map(NULL) == NULL, "a null buffer object must remain unmapped");
        require(allocation_count == before, "pooled or null inputs allocated scratch");
        puts("PASS: pool-less scratch size, contents and lifetime; pooled identity and null inputs");
    }
    for (unsigned i = 0; i < allocation_count; i++) free(allocations[i]);
    return 0;
}
