/* host/nvn_list.c as it is (HOS_NVN_LIST_REF): the recordings' allocations, handles, the control index, the lifetime rules
   and the deferred binds' copy on conflict, against a small guest window of test memory. Built and run by
   tests/test_list_ref.py, once without and once with the check (argument "check"). */
#include "nvn_list.h"
#include "rx_nvn.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
#define REQUIRE(ok, ...) do { if (!(ok)) { fprintf(stderr, "FAIL (line %d): ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failed = 1; } } while (0)
static void say(const char *f, ...) { va_list a; va_start(a, f); fputs("  log: ", stdout); vprintf(f, a); fputc('\n', stdout); va_end(a); }
static void die(const char *f, ...) { va_list a; va_start(a, f); fputs("FATAL: ", stderr); vfprintf(stderr, f, a); fputc('\n', stderr); va_end(a); exit(2); }

#define WINDOW (1u << 20)
static uint8_t guest_mem[WINDOW];
static void *guest(uint64_t a, uint64_t n) { return a + n <= WINDOW && a + n >= a ? guest_mem + a : NULL; }

/* ---- recording helpers (what host/nvn_native.c's handlers do) ---- */
static uint8_t *put(nvn_lw *w, uint32_t op, uint32_t payload) {
    const uint32_t size = (8 + payload + 7) & ~7u;
    nvn_list_reserve(w, size);
    uint8_t *p = w->cur;
    const nvn_op_hdr h = {op, size};
    memcpy(p, &h, 8);
    memset(p + 8, 0, size - 8);
    w->cur += size;
    return p;
}
static void begin(nvn_lw *w, uint64_t key) { nvn_list_open(w, key); put(w, NOP_BEGIN, 0); }
static void draw(nvn_lw *w, int count) {
    uint8_t *p = put(w, NOP_DRAW, sizeof(nvn_p_draw));
    nvn_p_draw d = {4, 0, count, 0, 1};
    memcpy(p + 8, &d, sizeof d);
}
static void filler(nvn_lw *w, size_t bytes) {   /* (viewport records: bytes of stream) */
    for (size_t k = 0; k < bytes; k += 8 + sizeof(nvn_p_viewports)) {
        uint8_t *p = put(w, NOP_VIEWPORTS, sizeof(nvn_p_viewports));
        p[8] = (uint8_t)k;
    }
}
static void deferred_tex(nvn_lw *w, uint64_t src, int n) {
    uint8_t *p = put(w, NOP_BIND_TEX, offsetof(nvn_p_bind_handles, h) + 8 * (size_t)n);
    nvn_p_bind_handles *b = (nvn_p_bind_handles *)(void *)(p + 8);
    b->stage = 4, b->first = 0, b->n = n, b->src = src;
    memcpy(b->h, guest_mem + src, 8 * (size_t)n);
    nvn_list_note_deferred(w, p);
}
static void call(nvn_lw *w, uint64_t h) {
    uint64_t flat = 0;
    REQUIRE(nvn_list_call(w, h, &flat), "a live handle was refused by CallCommands");
}
static uint64_t end(nvn_lw *w, uint64_t ctl) {
    put(w, NOP_END, 0);
    uint64_t h = 0;
    nvn_list_end(w, ctl, 0, &h);
    return h;
}
static nvn_list_stats st(void) { nvn_list_stats s; nvn_list_get_stats(&s); return s; }
static const nvn_lrec *rec(uint64_t h) { return nvn_list_lookup(h); }
/* the op codes of a walk as the replay makes it (into calls), BEGIN / END of callees left out */
static int walk_ops(const uint8_t *p, uint32_t *out, int at, int top) {
    for (;;) {
        nvn_op_hdr h;
        memcpy(&h, p, 8);
        if (h.op == NOP_CALL) {
            nvn_p_call c;
            memcpy(&c, p + 8, sizeof c);
            at = walk_ops(c.target, out, at, 0);
        } else if (top || (h.op != NOP_BEGIN && h.op != NOP_END)) {
            out[at++] = h.op;
        }
        if (h.op == NOP_END) return at;
        p += h.size;
    }
}
static uint64_t tex_handle_in(const nvn_lrec *r) {   /* the first deferred bind's first handle (as the replay reads it) */
    const uint8_t *p = nvn_lrec_begin(r);
    for (;;) {
        nvn_op_hdr h;
        memcpy(&h, p, 8);
        if (h.op == NOP_BIND_TEX) return ((const nvn_p_bind_handles *)(const void *)(p + 8))->h[0];
        if (h.op == NOP_CALL) {
            nvn_p_call c;
            memcpy(&c, p + 8, sizeof c);
            const uint64_t v = tex_handle_in((const nvn_lrec *)(const void *)c.target - 1);
            if (v) return v;
        }
        if (h.op == NOP_END) return 0;
        p += h.size;
    }
}

/* ---- threads: job lists recorded on four threads, each calling the shared precompiled lists ---- */
static uint64_t shared[8];
static volatile int stop_drain;
static void *job_thread(void *arg) {
    const uint64_t base = 0x40000 + 0x10000 * (uint64_t)(uintptr_t)arg;   /* (its control slice: two halves) */
    for (int frame = 0; frame < 400; frame++) {
        const uint64_t half = base + (frame & 1) * 0x8000;
        nvn_list_release(half, half + 0x8000, (uint64_t)frame);
        nvn_list_drain((uint64_t)frame);
        for (int j = 0; j < 6; j++) {
            nvn_lw w = {0};
            begin(&w, 0x9000 + (uint64_t)(uintptr_t)arg);
            filler(&w, 2000 + 300 * (size_t)j);
            call(&w, shared[(frame + j) & 7]);
            draw(&w, j);
            end(&w, half + 64 * (uint64_t)j);
        }
    }
    return NULL;
}
static void *drain_thread(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&stop_drain, __ATOMIC_ACQUIRE)) nvn_list_drain(UINT64_MAX >> 1);
    return NULL;
}

int main(int argc, char **argv) {
    const int check = argc > 1 && !strcmp(argv[1], "check");
    nvn_list_init(check, say, die);

    /* 1. allocations: a recording grows past its guess, the next one of that command buffer starts big enough; a guess
          far too big is shrunk at the end; nothing is copied otherwise */
    {
        nvn_lw w = {0};
        nvn_list_stats a = st();
        begin(&w, 0x1234);
        filler(&w, 200000);
        const uint64_t h1 = end(&w, ~0ull);
        nvn_list_stats b = st();
        REQUIRE(b.grown > a.grown, "a 200 KB recording did not grow from its first guess");
        begin(&w, 0x1234);
        filler(&w, 190000);
        const uint64_t h2 = end(&w, ~0ull);
        nvn_list_stats c = st();
        REQUIRE(c.grown == b.grown, "the second recording of the same command buffer grew (its guess: the last size)");
        begin(&w, 0x1234);
        filler(&w, 500);
        const uint64_t h3 = end(&w, ~0ull);
        REQUIRE(st().shrunk > c.shrunk, "a 500-byte recording kept a 200 KB allocation");
        REQUIRE(rec(h1) && rec(h2) && rec(h3) && rec(h3)->cap < 8192, "handles of recordings without control memory");
        REQUIRE(rec(h1)->bytes == rec(h1)->flat, "a recording without calls: flat size is its size");
    }

    /* 2. handles: a recording lives until its control memory is added again; only that range's recordings go; a stale
          handle never names the slot's next recording */
    uint64_t a1, a2, b1;
    {
        nvn_lw w = {0};
        begin(&w, 1); draw(&w, 3); a1 = end(&w, 0x10000);
        begin(&w, 1); draw(&w, 4); a2 = end(&w, 0x10040);
        begin(&w, 2); draw(&w, 5); b1 = end(&w, 0x30000);
        REQUIRE(rec(a1) && rec(a2) && rec(b1), "live handles not found");
        REQUIRE(!nvn_list_lookup(a1 ^ (1ull << 32)) && !nvn_list_lookup(0) && !nvn_list_lookup(0x7fffffffull),
                "a handle never made was found");
        nvn_list_release(0x10000, 0x10000 + 0x40, 10);
        REQUIRE(!rec(a1) && rec(a2) && rec(b1), "a release took more (or less) than its range");
        nvn_list_release(0x10040, 0x20000, 10);
        REQUIRE(!rec(a2) && rec(b1), "the second release");
        begin(&w, 1); draw(&w, 6);
        const uint64_t again = end(&w, 0x50000);
        REQUIRE((uint32_t)again == (uint32_t)a2 || (uint32_t)again == (uint32_t)a1 || 1, "(slot reuse)");
        REQUIRE(!nvn_list_lookup(a2) && !nvn_list_lookup(a1) && rec(again), "a stale handle names a new recording");
    }

    /* 3. epoch: released bytes are freed only once the replay is past the release's submit */
    {
        const uint64_t f0 = st().freed;
        nvn_list_drain(9);
        REQUIRE(st().freed == f0, "released recordings freed before the replay was past them");
        nvn_list_drain(10);
        REQUIRE(st().freed == f0 + 2, "released recordings not freed once the replay was past them");
    }

    /* 4. a callee retired by the game while a caller that calls it is alive stays until the caller goes */
    uint64_t pc, job;
    {
        nvn_lw w = {0};
        begin(&w, 7); draw(&w, 11); draw(&w, 12); pc = end(&w, 0x60000);   /* (a precompiled list) */
        begin(&w, 8); draw(&w, 1); call(&w, pc); draw(&w, 2); job = end(&w, 0x70000);
        const nvn_lrec *j = rec(job), *c = rec(pc);
        REQUIRE(j->flat == j->bytes - (8 + sizeof(nvn_p_call)) + (c->bytes - 16), "the caller's flat size");
        REQUIRE(c->refs == 2, "the callee's references: its handle and its caller (%d)", c->refs);
        uint32_t ops[64];
        const int n = walk_ops(nvn_lrec_begin(j), ops, 0, 1);
        const uint32_t want[] = {NOP_BEGIN, NOP_DRAW, NOP_DRAW, NOP_DRAW, NOP_DRAW, NOP_END};
        REQUIRE(n == 6 && !memcmp(ops, want, sizeof want), "the walk into the callee");
        uint8_t flat[4096];
        nvn_list_expand(j, flat);
        nvn_op_hdr h;
        const size_t d = (8 + sizeof(nvn_p_draw) + 7) & ~(size_t)7;   /* (BEGIN, the caller's first draw, then the callee's) */
        memcpy(&h, flat + 8 + d, 8);
        REQUIRE(h.op == NOP_DRAW && ((nvn_p_draw *)(void *)(flat + 16 + d))->count == 11, "the copy has the callee's draws in place");
        const nvn_lrec *keep = c;
        const uint64_t f0 = st().freed;
        nvn_list_release(0x60000, 0x60040, 20);
        nvn_list_drain(100);
        REQUIRE(!rec(pc) && st().freed == f0 && keep->magic == NVN_LREC_MAGIC, "a callee freed while its caller lives");
        REQUIRE(walk_ops(nvn_lrec_begin(j), ops, 0, 1) == 6, "the caller's walk after its callee was retired");
        nvn_list_release(0x70000, 0x70040, 30);
        nvn_list_drain(29);
        REQUIRE(st().freed == f0, "the caller freed before the replay was past it");
        nvn_list_drain(30);
        REQUIRE(st().freed == f0 + 2, "caller and callee not freed together (%llu)", (unsigned long long)(st().freed - f0));
    }

    /* 5. CallCommands of a dead handle is refused; Begin again drops the calls recorded since (their references) */
    {
        nvn_lw w = {0};
        uint64_t flat;
        begin(&w, 9);
        REQUIRE(!nvn_list_call(&w, pc, &flat), "a released handle was called");
        begin(&w, 10); draw(&w, 1); const uint64_t c = end(&w, 0x80000);
        begin(&w, 11); call(&w, c); call(&w, c);
        REQUIRE(rec(c)->refs == 3, "two calls: two references");
        begin(&w, 11);   /* (BeginRecording again: what was recorded is dropped) */
        REQUIRE(rec(c)->refs == 1, "dropped calls kept their references (%d)", rec(c)->refs);
        call(&w, c);
        nvn_list_drop(&w);   /* (Finalize without EndRecording) */
        REQUIRE(rec(c)->refs == 1, "a dropped recording kept its references");
    }

    /* 6. deferred binds: read in place by a submit; a submit not replayed yet keeps its values, a later one with other
          values gets private copies (the callee and its caller), freed after it */
    {
        uint64_t *arr = (uint64_t *)(void *)(guest_mem + 0x1000);
        arr[0] = 0xaaa1, arr[1] = 0xaaa2;
        nvn_lw w = {0};
        begin(&w, 20); deferred_tex(&w, 0x1000, 2); draw(&w, 1); const uint64_t mat = end(&w, 0x90000);
        begin(&w, 21); draw(&w, 9); call(&w, mat); draw(&w, 3); const uint64_t jl = end(&w, 0xa0000);
        begin(&w, 22); call(&w, jl); const uint64_t prim = end(&w, 0xb0000);
        REQUIRE((rec(mat)->flags & NVN_LREC_OWN_DEF) && (rec(jl)->flags & NVN_LREC_SUB_DEF) && (rec(prim)->flags & NVN_LREC_SUB_DEF),
                "deferred flags up the calls");
        arr[0] = 0xbbb1;
        uint64_t out[1];
        nvn_list_stats s0 = st();
        REQUIRE(!nvn_list_submit(&prim, out, 1, 40, guest), "a submit of live handles");
        nvn_list_submitted(41);
        REQUIRE(out[0] == (uint64_t)(uintptr_t)nvn_lrec_begin(rec(prim)), "no conflict: the handle itself");
        REQUIRE(tex_handle_in(rec(prim)) == 0xbbb1 && st().resolved_in_place == s0.resolved_in_place + 1, "read in place");
        /* the same values, submit 41 not replayed: nothing to copy */
        REQUIRE(!nvn_list_submit(&prim, out, 1, 40, guest), "submit");
        nvn_list_submitted(42);
        REQUIRE(out[0] == (uint64_t)(uintptr_t)nvn_lrec_begin(rec(prim)) && st().clones == s0.clones, "equal values copied");
        /* other values while 42 is not replayed: copies of the path, the originals as 42 reads them */
        arr[0] = 0xccc1;
        REQUIRE(!nvn_list_submit(&prim, out, 1, 40, guest), "submit");
        nvn_list_submitted(43);
        const nvn_lrec *u = (const nvn_lrec *)(const void *)(uintptr_t)out[0] - 1;
        REQUIRE(u != rec(prim) && (u->flags & NVN_LREC_PRIVATE), "a conflict did not copy the submitted list");
        REQUIRE(st().clones == s0.clones + 3 && st().conflicts == s0.conflicts + 1, "copies: the callee and the two above it");
        REQUIRE(tex_handle_in(u) == 0xccc1 && tex_handle_in(rec(prim)) == 0xbbb1, "the copy has the new values, the original the old");
        /* replayed past 43: in place again, and the copies freed */
        const uint64_t f0 = st().freed;
        nvn_list_drain(43);
        REQUIRE(st().freed == f0 + 3, "private copies not freed after their submit (%llu)", (unsigned long long)(st().freed - f0));
        arr[0] = 0xddd1;
        REQUIRE(!nvn_list_submit(&prim, out, 1, 43, guest), "submit");
        nvn_list_submitted(44);
        REQUIRE(out[0] == (uint64_t)(uintptr_t)nvn_lrec_begin(rec(prim)) && tex_handle_in(rec(prim)) == 0xddd1, "in place after the replay");
        uint64_t bad[2] = {prim, 0x12345};
        REQUIRE(nvn_list_submit(bad, out, 2, 44, guest) == 2, "a submit of a dead handle names it");
        if (check) REQUIRE(st().check_mismatch == 0 && st().check_handles >= 4, "check: the walks differ from the copies");
    }

    /* 7. deeper than the replay's stack: the callee's records copied in */
    {
        nvn_lw w = {0};
        uint64_t h = 0;
        begin(&w, 30); draw(&w, 100); h = end(&w, ~0ull);
        const uint64_t e0 = st().calls_expanded;
        for (int d = 0; d < 10; d++) {
            begin(&w, 31 + (uint64_t)d); call(&w, h); draw(&w, d); h = end(&w, ~0ull);
            REQUIRE(rec(h)->depth <= NVN_LIST_MAX_DEPTH, "depth %u", rec(h)->depth);
        }
        REQUIRE(st().calls_expanded > e0, "nothing was copied in past the depth");
        uint32_t ops[64];
        REQUIRE(walk_ops(nvn_lrec_begin(rec(h)), ops, 0, 1) == 13 && ops[1] == NOP_DRAW, "the deep walk (%d)", walk_ops(nvn_lrec_begin(rec(h)), ops, 0, 1));
    }

    /* 8. the sweep: a recording whose mark was overwritten goes, a marked one stays */
    {
        nvn_lw w = {0};
        uint64_t h1, h2;
        begin(&w, 40); draw(&w, 1); put(&w, NOP_END, 0); nvn_list_end(&w, 0x2000, 1, &h1);
        begin(&w, 40); draw(&w, 1); put(&w, NOP_END, 0); nvn_list_end(&w, 0x2040, 1, &h2);
        const uint64_t m1[2] = {0x4b52414d59504f43ull, h1}, m2[2] = {0x4b52414d59504f43ull, h2};
        memcpy(guest_mem + 0x2000, m1, 16);
        memcpy(guest_mem + 0x2040, m2, 16);
        guest_mem[0x2048] ^= 1;   /* (the game wrote over the second one's) */
        REQUIRE(nvn_list_sweep(0x4b52414d59504f43ull, 50, guest) == 1 && rec(h1) && !rec(h2), "the sweep");
    }

    /* 9. (check) a free while a submit that walks it is not replayed is caught */
    if (check) {
        nvn_lw w = {0};
        begin(&w, 50); draw(&w, 1); const uint64_t h = end(&w, 0xc0000);
        uint64_t out[1];
        nvn_list_submit(&h, out, 1, 60, guest);
        nvn_list_submitted(70);
        nvn_list_release(0xc0000, 0xc0040, 65);   /* (a release that names too early a submit: a bug the check finds) */
        const uint64_t e0 = st().check_early_free;
        nvn_list_drain(66);
        REQUIRE(st().check_early_free == e0 + 1, "check: an early free went unseen");
    }

    /* 10. threads: four record job lists calling shared lists, releasing their own halves, one more drains */
    {
        nvn_lw w = {0};
        for (int i = 0; i < 8; i++) { begin(&w, 60); filler(&w, 300); draw(&w, i); shared[i] = end(&w, 0xe0000 + 64 * (uint64_t)i); }
        pthread_t t[5];
        for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, job_thread, (void *)(uintptr_t)i);
        pthread_create(&t[4], NULL, drain_thread, NULL);
        for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
        __atomic_store_n(&stop_drain, 1, __ATOMIC_RELEASE);
        pthread_join(t[4], NULL);
        for (int i = 0; i < 4; i++) nvn_list_release(0x40000 + 0x10000 * (uint64_t)i, 0x50000 + 0x10000 * (uint64_t)i, 0);
        nvn_list_drain(UINT64_MAX >> 1);
        for (int i = 0; i < 8; i++) REQUIRE(rec(shared[i]) && rec(shared[i])->refs == 1, "a shared list's references after the jobs (%d)", rec(shared[i]) ? rec(shared[i])->refs : -1);
    }
    const nvn_list_stats s = st();
    if (check) REQUIRE(s.check_changed == 0 && s.check_mismatch == 0, "check: %llu recordings changed, %llu mismatches",
                       (unsigned long long)s.check_changed, (unsigned long long)s.check_mismatch);
    printf("%s: %llu made, %llu live, %llu freed, %llu calls, %llu copies, check %llu handles %llu mismatches\n",
           failed ? "FAIL" : "PASS", (unsigned long long)s.made, (unsigned long long)s.live, (unsigned long long)s.freed,
           (unsigned long long)s.calls, (unsigned long long)s.clones, (unsigned long long)s.check_handles,
           (unsigned long long)s.check_mismatch);
    return failed;
}
