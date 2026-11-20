/* Game patch: gsys's GPU culling lists of OoT's culling object a, as host code over the game's objects
   (docs/direct/engine-classes.md, "The gsys GPU culling lists"). The lists' pools are VRAM: each reset the CPU makes is a
   GPU fill (nvn_claim_fill), and the records the CPU reads are copied out after each submission (nvn_claim_read, named
   once when the object is made).

   cull_gpu_lists_reset is replaced whole, with its inlined resets, cull_gpu_list_reset_one and report_reader_b: per list,
   flip the copy, read the counts of the copy the GPU filled, reset its records. The flushes and invalidates go (the
   pools are VRAM); the guest stores stay, so a frame with no culling submission reads its own resets back. A mesh draw
   record is reset to {0, 1, 1, w3} and a dispatch record to {0, 1, 1, 0}: Vulkan's indirect layout in place
   (docs/direct/task-mesh.md). actor_records_read_a (object b's gather lists, Readback) keeps its code; one row after its
   zeroes makes them on the GPU.

   Check that it applies: the 600-present line "direct patch culling lists". */
#include <stddef.h>
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
    uint8_t _00[0x14];
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
    uint16_t records;     /* members zeroed as mesh draw records */
    uint16_t counts;      /* members zeroed as counts */
    int8_t set;           /* the member set to the list's value, -1 none */
    uint8_t dispatch;     /* the first record set to {0, 1, 1, 0} */
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

static struct { volatile int64_t resets, headers, actor, records, set, outside; } n;
static uint64_t made[4];

static void *at(uint64_t a, uint64_t size) {
    if (!hos_guest_range(a, size)) hos_fatal_lr(0, "culling lists: 0x%llx outside guest memory", (unsigned long long)a);
    return GP(a);
}
static uint64_t block_at(const List *l, uint32_t i) { return l->ring + (uint64_t)i * sizeof(Block); }
static Block *block(const List *l) { return at(block_at(l, l->cur < l->ring_n ? l->cur : 0), sizeof(Block)); }
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
static void put(uint64_t a, uint32_t v) { *(uint32_t *)at(a, 4) = v, nvn_claim_fill(a, 4, v); }
static void mesh_record(uint64_t a) {
    Record *r = at(a, sizeof(Record));
    r->w[0] = 0, r->w[1] = 1, r->w[2] = 1;
    nvn_claim_fill(a, 4, 0), nvn_claim_fill(a + 4, 8, 1);
}
static void dispatch_record(uint64_t a) { mesh_record(a), put(a + 12, 0); }

/* a header's copy: its memory's CPU base + offset + the aligned size x copy x elements */
static uint64_t header_copy(const Header *h, uint32_t copy) {
    const Block *u = &h->ubo;
    const uint32_t align = ((const uint64_t *)at(u->vtable, 0x30))[5] == SSB_ALIGN ? u->ssb_align : 0x100;
    uint64_t base = u->mem ? *(uint64_t *)at(u->mem + 0x10, 8) : 0;
    if (base) base = *(uint64_t *)at(base + 0x100, 8);
    if (!base) hos_fatal_lr(0, "culling lists: a header list has no memory");
    return base + u->offset + (uint64_t)((u->size + align - 1) & -align) * (copy * u->elements);
}
static void header_reset(Header *h) {
    Block *u = &h->ubo;
    const uint32_t copy = (1 - (u->copy >> 4)) & 0xf;
    u->copy = (uint8_t)((u->copy & 0xf) | copy << 4);
    u->cpu = header_copy(h, copy);
    const uint64_t a0 = member(u, u->cpu, 0), a1 = member(u, u->cpu, 1);
    const Record *r0 = at(a0, sizeof(Record)), *r1 = at(a1, 3 * sizeof(Record));
    h->counts[0] = r0->w[3], h->counts[1] = r0->w[0];
    if (h->all == 1) h->counts[2] = r1[0].w[3], h->counts[3] = r1[1].w[3], h->counts[4] = r1[2].w[3];
    for (int i = 0; i < 4; i++) dispatch_record(i ? a1 + (uint64_t)(i - 1) * sizeof(Record) : a0);
    os_atomic_inc(&n.headers);
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
        if (!l->ring_n) hos_fatal_lr(0, "culling lists: list 0x%x of object a has no blocks", o->at);
        for (uint32_t b = 0; b < l->ring_n; b++) {
            const Block *blk = at(block_at(l, b), sizeof(Block));
            methods(blk, SSB_ALIGN);
            if (o->count >= 0) nvn_claim_read(record(blk, o->count) & ~15ull, 16);
            if (o->head) nvn_claim_read(record(blk, 0) & ~15ull, 16);
        }
    }
    for (int i = 0; i < 4; i++)
        if (!made[i] || made[i] == obj) { made[i] = obj; return; }
    hos_fatal_lr(0, "culling lists: a fifth culling object a");
}

/* cull_gpu_lists_reset (cull_frame_begin, main thread, once a frame), x0 object a */
PATCH_FUNC(0x80361c80, word(0xd503237f), body(0x22d8228b3a3f672d), aapcs, none, cull_lists_reset, as(x0, obj)) {
    int known = 0;
    for (int i = 0; i < 4; i++) known |= made[i] == obj;
    if (!known) hos_fatal_lr(0, "culling lists: cull_gpu_lists_reset on 0x%llx, not a culling object a", (unsigned long long)obj);
    for (size_t i = 0; i < NOPS; i++) {
        const struct list_op *o = &ops[i];
        if (o->kind == HEADER) { header_reset(at(obj + o->at, sizeof(Header))); continue; }
        List *l = at(obj + o->at, sizeof(List));
        if (l->copies >= 2) l->cur = 1 - l->cur;
        const Block *b = block(l);
        if (o->count >= 0) l->count = read32(record(b, o->count));
        if (o->head) l->head = read32(record(b, 0));
        for (int m = 0; m < 12; m++) {
            if (o->records & M(m)) mesh_record(record(b, m));
            if (o->counts & M(m)) put(record(b, m), 0);
        }
        if (o->set >= 0) put(record(b, o->set), l->value);
        if (o->dispatch) dispatch_record(record(b, 0));
        os_atomic_inc(&n.resets);
    }
}

/* actor_records_read_a (f_80586fd0, three gather lists of object b): after the last of its unconditional count zeroes,
   all of them made on the GPU; m1 and m4 only when the list's first byte is 1, as the game's next block does */
PATCH_AT(0x82c1de64, word(0xb8286abf), after, none, cull_lists_actor_counts, as(x19, list)) {
    const List *l = at(list, sizeof(List));
    const Block *b = block(l);
    const uint32_t zeroed = M(0) | M(2) | M(3) | M(6) | M(7) | M(8) | M(9) | M(10) | M(11) | (*(uint8_t *)l == 1 ? M(1) | M(4) : 0);
    for (int m = 0; m < 12; m++)
        if (zeroed & M(m)) nvn_claim_fill(record(b, m), 4, 0);
    os_atomic_inc(&n.actor);
}

/* nvn_native.c draw_mesh: the records of an indirect mesh draw (max of them, stride apart; 0: 16) that the GPU resets
   (w0 and w3 only) get w1 = w2 = 1 once */
uint64_t nvn_gpu_guest(uint64_t gpu);
void game_indirect_mesh_draw(uint64_t gpu, int max, int stride) {
    const uint64_t rec = gpu ? nvn_gpu_guest(gpu) : 0;
    if (!rec) return;
    if (stride <= 0) stride = 16;
    if (max <= 0) max = 1;
    for (int i = 0; i < max && i < 64; i++) {
        const uint64_t a = rec + (uint64_t)i * (uint64_t)stride;
        os_atomic_inc(&n.records);
        if (!hos_guest_range(a, sizeof(Record))) { os_atomic_inc(&n.outside); continue; }
        Record *r = GP(a);
        if (r->w[1] || r->w[2]) continue;
        r->w[1] = 1, r->w[2] = 1;
        nvn_claim_fill(a + 4, 8, 1);
        os_atomic_inc(&n.set);
    }
}

void cull_lists_log(double frames) {
    static int64_t last[6];
    const int64_t now[6] = {n.resets, n.headers, n.actor, n.records, n.set, n.outside};
    hos_log("direct patch culling lists: %.2f list resets, %.2f header resets, %.2f gather list resets a frame; indirect "
            "mesh records %.2f a frame, set once %lld since the start, outside guest memory %lld",
            (now[0] - last[0]) / frames, (now[1] - last[1]) / frames, (now[2] - last[2]) / frames, (now[3] - last[3]) / frames,
            (long long)now[4], (long long)(now[5] - last[5]));
    for (int i = 0; i < 6; i++) last[i] = now[i];
}
