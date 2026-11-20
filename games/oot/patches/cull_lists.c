/* Game patch: gsys's GPU culling lists of OoT's culling objects, as host code over the game's objects
   (docs/direct/engine-classes.md, "The gsys GPU culling lists"). Only GPU work writes object a's lists: every reset is a
   buffer clear recorded on the culling pass's own command buffer, before its work, and the CPU reads only the records
   the back end copied out (nvn_claim_read, named once when object a is made).

   cull_gpu_lists_reset is replaced whole, with its inlined resets, cull_gpu_list_reset_one and report_reader_b: per list,
   flip the copy and read the counts of the copy the GPU filled. A copy no culling pass reset since it was last current
   reads 0, the game's own reset. pass_vegetation_compute, the pass whose compute writes all of object a's lists, records
   the resets of every copy a flip made current since its last run, before its first dispatch: counts to 0, the set count
   to its value at the flip, a mesh draw record to {0, 1, 1, w3} and a dispatch record to {0, 1, 1, 0} (Vulkan's indirect
   layout, docs/direct/task-mesh.md). Object b's gather lists (actor_records_read_a) stay on fills: the render jobs' mesh
   draws write them too. actor_records_read_b's own indirect records are CPU data: they get y = z = 1 where the game
   leaves both 0.

   Check that it applies: the 600-present line "direct patch culling lists". */
#include <stddef.h>
#include <string.h>
#include "patches.h"
#include "os.h"
#include "nvn_claims.h"

/* the game's objects in guest memory; a uint64_t field holds a guest address */
typedef struct { uint32_t count, offset, size; } Member;   /* a declared member */
typedef struct { uint64_t members; } Decl;
typedef struct {              /* agl::UniformBlock (agl::ShaderStorageBlock), 0x90 bytes */
    uint64_t vtable, _08;
    uint64_t decl;            /* Decl */
    uint64_t cpu;             /* the CPU address of the block (report_reader_b: of its current copy) */
    uint64_t offset;          /* in its memory */
    uint64_t _28, pool;
    uint64_t mem;             /* the GPUMemBlockBase: [+0x10] its owner, whose [+0x100] is the CPU base */
    uint32_t size, _44;
    uint16_t elements;
    uint8_t copy;             /* the current copy in the high nibble */
    uint8_t _4b[0x88 - 0x4b];
    uint32_t ssb_align;       /* (ShaderStorageBlock) */
    uint32_t _8c;
} Block;
typedef struct {              /* a culling list: a ring of single-copy blocks */
    uint8_t first;            /* (gather lists: 1 = two more counts) */
    uint8_t _01[0x13];
    uint32_t copies, cur, _1c;
    uint32_t ring_n, _24;
    uint64_t ring;            /* Block[ring_n] */
    uint32_t count, head;     /* read from the copy the GPU filled: a member's count, the first record's w0 */
    uint32_t value;           /* a count the CPU sets */
} List;
typedef struct {              /* report_reader_b's header list */
    uint32_t all, _04;        /* 1: the second member's three counts are read too */
    Block ubo;                /* two copies */
    uint32_t counts[5];
} Header;
typedef struct { uint32_t w[4]; } Record;
_Static_assert(offsetof(Block, cpu) == 0x18 && offsetof(Block, mem) == 0x38 && offsetof(Block, copy) == 0x4a &&
               offsetof(Block, ssb_align) == 0x88 && sizeof(Block) == 0x90, "Block");
_Static_assert(offsetof(List, copies) == 0x14 && offsetof(List, ring) == 0x28 && offsetof(List, value) == 0x38, "List");
_Static_assert(offsetof(Header, ubo) == 8 && offsetof(Header, counts) == 0x98, "Header");

#define M(n) (1u << (n))
enum { LIST, HEADER };
static const struct list_op {
    uint8_t kind;
    uint16_t at;          /* offset in object a */
    int8_t count;         /* the member whose count is read, -1 none */
    uint8_t head;         /* the first record's w0 read */
    uint16_t records;     /* members reset as mesh draw records */
    uint16_t counts;      /* members reset as counts */
    int8_t set;           /* the member set to the list's value, -1 none */
    uint8_t dispatch;     /* the first record a dispatch record */
} ops[] = {   /* cull_gpu_lists_reset's order; the list at 0xb8 is never reset */
    {LIST, 0x000, 1, 0, M(0), M(1), -1, 0},
    {LIST, 0x180, 1, 1, M(0), M(1), -1, 0},   /* (cull_gpu_list_reset_one) */
    {LIST, 0x1b8, 1, 1, M(0), M(1), -1, 0},
    {LIST, 0x030, 11, 1, M(0) | M(2) | M(4) | M(6) | M(8), M(1) | M(3) | M(5) | M(7) | M(9) | M(11), 10, 0},
    {LIST, 0x068, -1, 0, 0, 0, -1, 1},
    {LIST, 0x090, -1, 0, 0, 0, -1, 1},
    {LIST, 0x0e0, -1, 0, 0, 0, -1, 1},
    {LIST, 0x108, 1, 1, M(0) | M(2) | M(4), M(1) | M(3) | M(5), 6, 0},
    {LIST, 0x140, 1, 1, M(0) | M(2) | M(4), M(1) | M(3) | M(5), 6, 0},
    {HEADER, 0x1f0}, {HEADER, 0x2a0}, {HEADER, 0x350}, {HEADER, 0x400}, {HEADER, 0x4b0}, {HEADER, 0x560},   /* (report_reader_b) */
    {LIST, 0x608, -1, 0, 0, 0, -1, 1},
};
#define NOPS (sizeof ops / sizeof ops[0])

/* the blocks' virtual methods the game calls, as the game compares them (its GOT's words): +0x28 the alignment
   (ShaderStorageBlock's f_80365940, UniformBlock's f_8002a580: 0x100), +0x40 the invalidate (f_834bc140, empty) */
#define SSB_ALIGN G64(0x84cb4fa8)
#define UBO_ALIGN G64(0x84cb56e0)
#define NO_INVALIDATE G64(0x84cb4fb0)

/* host state per culling object a: the copies a flip made current that no pass reset yet, the copies a pass reset since
   they became current, the set counts at the flip. Flips and passes run on different game threads */
static struct {
    uint64_t obj;
    uint8_t due[NOPS][2];     /* [op][copy]: made current by a flip since the last pass */
    uint8_t reset[NOPS][2];   /* a pass recorded this copy's resets since it became current */
    uint32_t value[NOPS][2];  /* the set count at that flip */
} objs[4];
static volatile int lock;
static void take(void) { while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {} }
static void give(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }
static struct { volatile int64_t flips, unreset, gathers, passes, pass_b, clears, fixed; } n;

static void *at(uint64_t a, uint64_t size) {
    if (!hos_guest_range(a, size)) hos_fatal_lr(0, "culling lists: 0x%llx outside guest memory", (unsigned long long)a);
    return GP(a);
}
static uint64_t block_at(const List *l, uint32_t i) { return l->ring + (uint64_t)i * sizeof(Block); }
static uint32_t cur(const List *l) { return l->cur < l->ring_n ? l->cur : 0; }
static Block *block(const List *l) { return at(block_at(l, cur(l)), sizeof(Block)); }
static uint64_t member(const Block *b, uint64_t base, int m) {
    const Decl *d = at(b->decl, sizeof(Decl));
    return base + ((const Member *)at(d->members + (uint64_t)m * sizeof(Member), sizeof(Member)))->offset;
}
static uint64_t record(const Block *b, int m) { return member(b, b->cpu, m); }
static uint32_t read32(uint64_t a) { return *(uint32_t *)at(a, 4); }
static void methods(const Block *b, uint64_t align) {
    const uint64_t *vt = at(b->vtable, 0x48);
    if (vt[5] != align || vt[8] != NO_INVALIDATE)
        hos_fatal_lr(0, "culling lists: a block's methods are 0x%llx 0x%llx, not the alignment and the empty invalidate",
                     (unsigned long long)vt[5], (unsigned long long)vt[8]);
}

/* a header's copy: its memory's CPU base + offset + the aligned size x copy x elements */
static uint64_t header_copy(const Header *h, uint32_t copy) {
    const Block *u = &h->ubo;
    const uint32_t align = ((const uint64_t *)at(u->vtable, 0x30))[5] == SSB_ALIGN ? u->ssb_align : 0x100;
    uint64_t base = u->mem ? *(uint64_t *)at(u->mem + 0x10, 8) : 0;
    if (base) base = *(uint64_t *)at(base + 0x100, 8);
    if (!base) hos_fatal_lr(0, "culling lists: a header list has no memory");
    return base + u->offset + (uint64_t)((u->size + align - 1) & -align) * (copy * u->elements);
}
static uint32_t header_cur(const Header *h) { return (h->ubo.copy >> 4) & 1; }
/* a header's four dispatch records: the first member's, the second member's three */
static uint64_t header_record(const Header *h, uint64_t base, int i) {
    return i ? member(&h->ubo, base, 1) + (uint64_t)(i - 1) * sizeof(Record) : member(&h->ubo, base, 0);
}

/* cull_gpu_object_a_create's end (pools.c): the records the CPU reads named, every block's methods checked */
void cull_lists_made(uint64_t obj) {
    for (size_t i = 0; i < NOPS; i++) {
        const struct list_op *o = &ops[i];
        if (o->kind == HEADER) {
            const Header *h = at(obj + o->at, sizeof(Header));
            const uint64_t align = ((const uint64_t *)at(h->ubo.vtable, 0x30))[5];
            methods(&h->ubo, align == SSB_ALIGN ? SSB_ALIGN : UBO_ALIGN);
            for (uint32_t c = 0; c < 2; c++) {
                const uint64_t base = header_copy(h, c);
                nvn_claim_read(member(&h->ubo, base, 0), sizeof(Record));
                nvn_claim_read(member(&h->ubo, base, 1), 3 * sizeof(Record));
            }
            continue;
        }
        const List *l = at(obj + o->at, sizeof(List));
        if (!l->ring_n || l->ring_n > 2) hos_fatal_lr(0, "culling lists: list 0x%x of object a has %u blocks", o->at, l->ring_n);
        for (uint32_t b = 0; b < l->ring_n; b++) {
            const Block *blk = at(block_at(l, b), sizeof(Block));
            methods(blk, SSB_ALIGN);
            if (o->count >= 0) nvn_claim_read(record(blk, o->count) & ~15ull, 16);
            if (o->head) nvn_claim_read(record(blk, 0) & ~15ull, 16);
        }
    }
    take();
    int k = 0;
    while (k < 4 && objs[k].obj && objs[k].obj != obj) k++;
    if (k == 4) hos_fatal_lr(0, "culling lists: a fifth culling object a");
    memset(&objs[k], 0, sizeof objs[k]);
    objs[k].obj = obj;
    give();
}

/* cull_gpu_lists_reset (cull_frame_begin, once a frame), x0 object a: flip each list, read what the GPU left in the copy
   now current when a pass reset it since it was last current, else 0 */
PATCH_FUNC(0x80361c80, word(0xd503237f), body(0x22d8228b3a3f672d), aapcs, none, cull_lists_reset, as(x0, obj)) {
    take();
    int k = 0;
    while (k < 4 && objs[k].obj != obj) k++;
    if (k == 4) hos_fatal_lr(0, "culling lists: cull_gpu_lists_reset on 0x%llx, not a culling object a", (unsigned long long)obj);
    for (size_t i = 0; i < NOPS; i++) {
        const struct list_op *o = &ops[i];
        if (o->kind == HEADER) {
            Header *h = at(obj + o->at, sizeof(Header));
            const uint32_t c = 1 - header_cur(h);
            h->ubo.copy = (uint8_t)((h->ubo.copy & 0xf) | c << 4);
            h->ubo.cpu = header_copy(h, c);
            const int done = objs[k].reset[i][c];
            const Record *r0 = at(header_record(h, h->ubo.cpu, 0), sizeof(Record));
            const Record *r1 = at(header_record(h, h->ubo.cpu, 1), 3 * sizeof(Record));
            h->counts[0] = done ? r0->w[3] : 0, h->counts[1] = done ? r0->w[0] : 0;
            if (h->all == 1)
                for (int j = 0; j < 3; j++) h->counts[2 + j] = done ? r1[j].w[3] : 0;
            objs[k].reset[i][c] = 0, objs[k].due[i][c] = 1;
            n.unreset += !done;
            continue;
        }
        List *l = at(obj + o->at, sizeof(List));
        if (l->copies >= 2) l->cur = 1 - l->cur;
        const uint32_t c = cur(l);
        const int done = objs[k].reset[i][c];
        const Block *b = block(l);
        if (o->count >= 0) l->count = done ? read32(record(b, o->count)) : 0;
        if (o->head) l->head = done ? read32(record(b, 0)) : 0;
        objs[k].reset[i][c] = 0, objs[k].due[i][c] = 1;
        objs[k].value[i][c] = l->value;
        n.unreset += !done;
    }
    give();
    os_atomic_inc(&n.flips);
}

/* actor_records_read_a (f_80586fd0's three gather lists of object b), x19 the list, after the last of its unconditional
   count zeroes: the zeroes made on the GPU before the next submission's work, m1 and m4 only when the list's first byte
   is 1, as the game's next block does. These lists stay on fills: their writers include the render jobs' mesh draws,
   whose order against the culling pass is not established */
PATCH_AT(0x82c1de64, word(0xb8286abf), after, none, cull_lists_gather, as(x19, list)) {
    const List *l = at(list, sizeof(List));
    const Block *b = block(l);
    const uint32_t zeroed = M(0) | M(2) | M(3) | M(6) | M(7) | M(8) | M(9) | M(10) | M(11) | (l->first == 1 ? M(1) | M(4) : 0);
    for (int m = 0; m < 12; m++)
        if (zeroed & M(m)) nvn_claim_fill(record(b, m), 4, 0);
    os_atomic_inc(&n.gathers);
}

/* the resets the last flips left, recorded on the culling pass's command buffer before its work */
uint64_t nvn_native_proc(const char *name);   /* nvn_native.c */
static uint64_t p_clear;
static void clear(GuestCall *g, uint64_t sp, uint64_t cmd, uint64_t a, uint32_t size, uint32_t value) {
    guest_call(g, sp, p_clear, cmd, a, size, value, 0);
    os_atomic_inc(&n.clears);
}
/* a mesh draw record {0, 1, 1, w3}, a dispatch record {0, 1, 1, 0}: Vulkan's indirect layout (docs/direct/task-mesh.md) */
static void mesh_record(GuestCall *g, uint64_t sp, uint64_t cmd, uint64_t a) { clear(g, sp, cmd, a, 4, 0), clear(g, sp, cmd, a + 4, 8, 1); }
static void dispatch_record(GuestCall *g, uint64_t sp, uint64_t cmd, uint64_t a) { mesh_record(g, sp, cmd, a), clear(g, sp, cmd, a + 12, 4, 0); }
static void pass_resets(GuestCall *g, uint64_t sp, uint64_t cmd) {
    if (!p_clear) p_clear = nvn_native_proc("nvnCommandBufferClearBuffer");
    take();
    for (int k = 0; k < 4 && objs[k].obj; k++) {
        const uint64_t obj = objs[k].obj;
        for (size_t i = 0; i < NOPS; i++) {
            const struct list_op *o = &ops[i];
            for (uint32_t c = 0; c < 2; c++) {
                if (!objs[k].due[i][c]) continue;
                objs[k].due[i][c] = 0, objs[k].reset[i][c] = 1;
                if (o->kind == HEADER) {
                    const Header *h = at(obj + o->at, sizeof(Header));
                    for (int r = 0; r < 4; r++) dispatch_record(g, sp, cmd, header_record(h, header_copy(h, c), r));
                    continue;
                }
                const List *l = at(obj + o->at, sizeof(List));
                const Block *b = at(block_at(l, c < l->ring_n ? c : 0), sizeof(Block));
                for (int m = 0; m < 12; m++) {
                    if (o->records & M(m)) mesh_record(g, sp, cmd, record(b, m));
                    if (o->counts & M(m)) clear(g, sp, cmd, record(b, m), 4, 0);
                }
                if (o->set >= 0) clear(g, sp, cmd, record(b, o->set), 4, objs[k].value[i][c]);
                if (o->dispatch) dispatch_record(g, sp, cmd, record(b, 0));
            }
        }
    }
    give();
    os_atomic_inc(&n.passes);
}
/* pass_vegetation_compute: after frame_compute_list_begin made its own compute list (the context at sp + 0x10, its
   command buffer at sp + 0xe8), before the culling calls (f_80290f20, f_808832f0, f_80583d90) */
PATCH_AT(0x80291310, word(0xd0025068), before, none, cull_lists_pass, guest(computed), sp, as(ld(sp, 0xe8), cmd)) {
    pass_resets(g, sp, cmd);
}
/* its other path: the same calls (f_82c21140 first) on the scene's context x19 */
PATCH_AT(0x802917c0, word(0xaa1303e1), before, none, cull_lists_pass_b, guest(computed), sp, as(x19, ctx)) {
    pass_resets(g, sp, G64(ctx + 0xd8));
    os_atomic_inc(&n.pass_b);
}

/* actor_records_read_b (object b's CPU-built indirect mesh records, f_80c5f870's Upload pool), after its three records
   {0, n, n, 0} (x8 the first): n = 0 becomes 1, Vulkan's y and z */
PATCH_AT(0x82c1d4c8, word(0xb9402a68), before, none, cull_lists_cpu_records, as(x8, rec)) {
    for (int i = 0; i < 3; i++) {
        Record *r = at(rec + (uint64_t)i * sizeof(Record), sizeof(Record));
        if (!r->w[1] && !r->w[2]) r->w[1] = 1, r->w[2] = 1, os_atomic_inc(&n.fixed);
    }
}

void cull_lists_log(double frames) {
    static int64_t last[7];
    const int64_t now[7] = {n.flips, n.unreset, n.gathers, n.passes, n.pass_b, n.clears, n.fixed};
    hos_log("direct patch culling lists: %.2f flips, %.2f gather flips, %.2f passes (%lld on the scene context), %.1f clears "
            "a frame; %lld reads of a copy no pass reset; %.2f CPU records given y = z = 1 a frame",
            (now[0] - last[0]) / frames, (now[2] - last[2]) / frames, (now[3] - last[3]) / frames, (long long)(now[4] - last[4]),
            (now[5] - last[5]) / frames, (long long)(now[1] - last[1]), (now[6] - last[6]) / frames);
    for (int i = 0; i < 7; i++) last[i] = now[i];
}
