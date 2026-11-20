/* debug.poolcensus (diagnosis only): who on the CPU writes and reads the memory pools the GPU uses, and when, relative to
   the submissions that use them (docs/direct/pool-claims.md, "The census"). It never changes what the game or the back end
   does: a run with it differs only in time.

   The value picks the pools: `layer0` (the default for 1), claim names (`vram`, `upload`, `readback`, `loadonce`),
   `by:<text>` (the claiming patch's name holds the text) or a hex guest address (the pool holding it), joined by `+`;
   `whole` arms every page of a picked pool from its creation against writes and reads, not only the pages submissions bind
   (for data shaders reach through stored addresses; a read of a page no bound GPU work writes counts as "unbound").

   The back end tells of each submission at the guest's submit (the guest ranges its lists use, and whether GPU work
   writes them), when its first chunk goes to the GPU, and when it completes (vk_claims.cpp). A page a submission binds
   is made read-only; a page GPU work writes is made inaccessible, so the CPU's reads of it fault too. The first write
   (or read of a GPU-written page) after each arming faults here, is counted by pool and by (writer, guest call chain,
   thread), timed against the submissions using that page (none outstanding / submitted, not started / on the GPU) or,
   for reads, against the one that last wrote it (on the GPU / completed this present / completed before), and the
   page is opened again. A writer patch's report (nvn_claim_write) after a write fault on the same page counts as that
   writer's "reported". Each present arms the opened pages again, so a page is seen at most once per present (twice
   when the GPU writes it in between). A summary goes to poolcensus.txt in the run folder every 300 presents. */
#include "hos.h"
#include "renderer.h"
#include "nvn_claims.h"
#include "rx_nvn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"

extern volatile int64_t hos_present_count;
extern const uint32_t a64_fn_count;
extern const uint32_t a64_fn_addr[];
extern const A64Fn a64_fn_ptr[];

#define PAGE 4096ull
#define NPAGES (HOS_GUEST_SPACE / PAGE)
#define MAXPOOL 16384
#define MAXGROUP 2048
#define NREC 16384
#define NSEQ 65536
#define NRING (1u << 20)
#define CHAIN 12
#define KEYDEPTH 6
#define NUSE 12

enum { PH_NONE, PH_QUEUED, PH_FLIGHT, PH_DONE };
/* a pool's counters: CPU writes by timing and by what the GPU did to the page, CPU reads of GPU-written pages by timing,
   faults on the back end's own threads, bytes the submissions bind, reported writes (nvn_claim_write) */
enum {
    W_IDLE, W_QUEUED, W_FLIGHT, W_GPU_PAGE, W_GPU_GRAIN,
    R_QUEUED, R_FLIGHT, R_SAME, R_LATER, R_UNBOUND,
    HOST_W, HOST_R, B_READ, B_WRITE, SUBS, REPORTED, REPORTED_BYTES, ST_N
};
/* a page's watch: armed bit 1 a write faults, bit 2 a read faults; ever bit 1 bound, 2 GPU-written, 4 CPU-written,
   8 CPU-read after a GPU write */
typedef struct {
    volatile uint8_t armed, ever;
    volatile uint16_t rec;   /* (the record of its last write fault, + 1: a report after it counts there) */
    volatile uint32_t use_seq, write_seq;
} PageSt;
typedef struct {
    uint64_t lo, hi, base, size;   /* (lo, hi: its pages) */
    uint32_t flags, claim;
    const char *by;
    int group;
    int64_t made;
    volatile int32_t dead;
    volatile uint64_t last_sub;
    PageSt *pg;
    volatile uint64_t *grains;   /* (16-byte grains GPU work writes, a bit each) */
    volatile int64_t st[ST_N], by_use[16];
} Pool;
typedef struct {
    uint64_t chain[CHAIN];
    uint32_t flags, claim;
    const char *by;
} Group;
typedef struct {
    volatile uint64_t key;
    int group, write;
    uint64_t pc, lr, f1, f2, off;
    const char *thread;
    volatile int64_t n[5], frames, last_present, reported;
} Rec;

static int on = -1, whole, pick_claim[NVN_CLAIM_KINDS], n_addr, n_by;
static uint64_t pick_addr[8];
static char pick_by[4][64];
static uint16_t *page_pool;   /* (a pool's index + 1 per guest page; 256 MB of address space, touched where pools are) */
static Pool pools[MAXPOOL];
static volatile int32_t n_pools;
static Group groups[MAXGROUP];
static int n_groups;
static os_mutex lock = OS_MUTEX_INIT;   /* (pools and groups made and finalized; the summary) */
static Rec recs[NREC];
static volatile int32_t recs_full, n_faults, ring_lost;
static volatile uint8_t seq_phase[NSEQ];
static volatile int64_t seq_present[NSEQ];
static volatile uint64_t seq_next;
static volatile uint64_t ring[NRING];
static volatile uint32_t ring_head;
static uint32_t ring_tail;
static int64_t start_present = -1;

static void parse(void) {
    char v[512];
    snprintf(v, sizeof v, "%s", setting_str(SET_DBG_POOLCENSUS));
    static const char *const names[NVN_CLAIM_KINDS] = {"", "textures", "targets", "host", "vram", "upload", "readback", "loadonce", "layer0"};
    int any = 0;
    for (char *t = strtok(v, "+, "); t; t = strtok(NULL, "+, ")) {
        if (!strcmp(t, "whole")) { whole = 1; continue; }
        if (!strcmp(t, "1")) { pick_claim[NVN_CLAIM_LAYER0] = any = 1; continue; }
        if (!strncmp(t, "by:", 3) && n_by < 4) { snprintf(pick_by[n_by++], 64, "%s", t + 3), any = 1; continue; }
        int k = 1;
        while (k < NVN_CLAIM_KINDS && strcmp(t, names[k])) k++;
        if (k < NVN_CLAIM_KINDS) { pick_claim[k] = any = 1; continue; }
        if (n_addr < 8 && (t[0] == '0' || strchr("123456789abcdef", t[0]))) pick_addr[n_addr++] = strtoull(t, NULL, 16), any = 1;
    }
    if (!any) pick_claim[NVN_CLAIM_LAYER0] = 1;
}

int poolcensus_on(void) {
    if (on < 0) {
        on = *setting_str(SET_DBG_POOLCENSUS) != 0;
        if (on) {
            parse();
            page_pool = (uint16_t *)calloc(NPAGES, sizeof *page_pool);
            if (!page_pool) on = 0;
        }
    }
    return on;
}

static int phase(uint64_t seq) {
    if (!seq) return PH_NONE;
    if (seq_next - seq >= NSEQ - 4096) return PH_DONE;
    return seq_phase[seq & (NSEQ - 1)];
}
static void push(uint64_t page) {
    const uint32_t i = os_atomic_fetch_add(&ring_head, 1);
    if (i - ring_tail >= NRING - 1) { os_atomic_inc(&ring_lost); return; }
    __atomic_store_n(&ring[i & (NRING - 1)], page + 1, __ATOMIC_RELEASE);
}
static Pool *pool_at(uint64_t a) {
    if (!page_pool || a >= HOS_GUEST_SPACE) return NULL;
    const uint32_t i = __atomic_load_n(&page_pool[a / PAGE], __ATOMIC_ACQUIRE);
    if (!i) return NULL;
    Pool *p = &pools[i - 1];
    return a >= p->lo && a < p->hi && !p->dead ? p : NULL;
}
static int prot_of(uint8_t armed) { return armed & 2 ? OS_PROT_NONE : armed & 1 ? OS_PROT_R : OS_PROT_RW; }

/* (the fault handler) one record per (pool group, host pc, guest lr, write or read) */
static int record(Pool *p, uint64_t a, int write, uintptr_t pc, int cls, int gpu_page, int gpu_grain) {
    HosThread *th = hos_self();
    Ctx *c = th ? hos_ctx() : NULL;
    const uint64_t lr = c ? c->x[30] : 0;
    uint64_t key = ((uint64_t)pc * 0x9e3779b97f4a7c15ull) ^ (lr * 0xff51afd7ed558ccdull) ^ ((uint64_t)p->group << 1 | (uint64_t)write);
    if (!key) key = 1;
    for (uint32_t i = (uint32_t)(key ^ key >> 29) % NREC, n = 0; n < NREC; n++, i = (i + 1) % NREC) {
        uint64_t k = __atomic_load_n(&recs[i].key, __ATOMIC_ACQUIRE);
        if (!k) {
            Rec *r = &recs[i];
            uint64_t expect = 0;
            if (__atomic_compare_exchange_n(&r->key, &expect, ~0ull, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                r->group = p->group, r->write = write, r->pc = pc, r->lr = lr, r->f1 = r->f2 = 0, r->off = a - p->base;
                r->thread = th ? hos_thread_name(th) : "(host)";
                uint64_t fp = c ? c->x[29] : 0;
                if (fp && !(fp & 7) && hos_guest_range(fp, 16) && !pool_at(fp)) {   /* (never a watched page: no fault in here) */
                    r->f1 = G64(fp + 8);
                    const uint64_t up = G64(fp);
                    if (up > fp && !(up & 7) && hos_guest_range(up, 16) && !pool_at(up)) r->f2 = G64(up + 8);
                }
                __atomic_store_n(&r->key, key, __ATOMIC_RELEASE);
                k = key;
            } else {
                k = expect;
            }
        }
        while (k == ~0ull) k = __atomic_load_n(&recs[i].key, __ATOMIC_ACQUIRE);   /* (another fault filling it) */
        if (k == key) {
            Rec *r = &recs[i];
            os_atomic_inc(&r->n[cls]);
            if (write && gpu_page) os_atomic_inc(&r->n[3]);
            if (write && gpu_grain) os_atomic_inc(&r->n[4]);
            const int64_t now = hos_present_count;
            if (os_atomic_xchg(&r->last_present, now) != now) os_atomic_inc(&r->frames);
            return (int)i;
        }
    }
    os_atomic_inc(&recs_full);
    return -1;
}

static int handler(const os_fault *f) {
    if (f->kind != OS_FAULT_ACCESS) return OS_FAULT_PASS;
    const uint8_t *base = (const uint8_t *)GP(0);
    if (f->addr < (uintptr_t)base || f->addr >= (uintptr_t)base + HOS_GUEST_SPACE) return OS_FAULT_PASS;
    const uint64_t a = (uint64_t)(f->addr - (uintptr_t)base);
    Pool *p = pool_at(a);
    if (!p) return OS_FAULT_PASS;
    PageSt *s = &p->pg[(a - p->lo) / PAGE];
    void *page = (void *)(base + (a & ~(PAGE - 1)));
    os_atomic_inc(&n_faults);
    const int host = !hos_self();
    if (f->write) {
        const uint8_t old = os_atomic_xchg(&s->armed, 0);
        os_mem_protect(page, PAGE, OS_PROT_RW);
        if (!old) return OS_FAULT_RESUME;
        os_atomic_fetch_or(&s->ever, 4);
        const int ph = phase(s->use_seq), cls = ph == PH_QUEUED ? 1 : ph == PH_FLIGHT ? 2 : 0;
        const uint64_t g = (a - p->lo) >> 4;
        const int gpu_page = s->write_seq != 0, gpu_grain = gpu_page && (p->grains[g >> 6] >> (g & 63) & 1);
        os_atomic_inc(&p->st[host ? HOST_W : W_IDLE + cls]);
        if (!host && gpu_page) os_atomic_inc(&p->st[W_GPU_PAGE]);
        if (!host && gpu_grain) os_atomic_inc(&p->st[W_GPU_GRAIN]);
        const int ri = record(p, a, 1, f->pc, cls, gpu_page, gpu_grain);
        s->rec = (uint16_t)(ri + 1);
    } else {
        const uint8_t old = os_atomic_fetch_and(&s->armed, (uint8_t)~2);
        os_mem_protect(page, PAGE, old & 1 ? OS_PROT_R : OS_PROT_RW);
        if (!(old & 2)) return OS_FAULT_RESUME;
        os_atomic_fetch_or(&s->ever, 8);
        const uint64_t w = s->write_seq;
        const int ph = phase(w);
        const int cls = !w ? 4 : ph == PH_QUEUED ? 0 : ph == PH_FLIGHT ? 1 : seq_present[w & (NSEQ - 1)] == hos_present_count && seq_next - w < NSEQ - 4096 ? 2 : 3;
        os_atomic_inc(&p->st[host ? HOST_R : R_QUEUED + cls]);
        record(p, a, 0, f->pc, cls, 0, 0);
    }
    push(a / PAGE);
    return OS_FAULT_RESUME;
}

static void start(void) {
    static volatile int32_t done;
    if (os_atomic_xchg(&done, 1)) return;
    start_present = hos_present_count;
    os_fault_add(handler, true);
    hos_log("poolcensus: CPU writes and reads of the GPU's pools are watched (debug.poolcensus=%s): poolcensus.txt",
            setting_str(SET_DBG_POOLCENSUS));
}

static int picked(uint64_t gpu, uint64_t size, uint32_t flags, uint32_t claim, const char *by) {
    if (claim < NVN_CLAIM_KINDS && pick_claim[claim] && !(claim == NVN_CLAIM_LAYER0 && (flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS)))
        return 1;
    for (int i = 0; i < n_by; i++)
        if (by && strstr(by, pick_by[i])) return 1;
    for (int i = 0; i < n_addr; i++)
        if (pick_addr[i] >= gpu && pick_addr[i] < gpu + size) return 1;
    return 0;
}

/* nvnMemoryPoolInitialize: a pool made, on the making thread (its guest call chain names the maker) */
void poolcensus_pool(Ctx *c, uint64_t gpu, uint64_t size, uint32_t flags, uint32_t claim, const char *by) {
    if (!poolcensus_on() || !size || !picked(gpu, size, flags, claim, by)) return;
    start();
    uint64_t chain[CHAIN] = {c->x[30]}, fp = c->x[29];
    for (int k = 1; k < CHAIN && fp && !(fp & 7) && hos_guest_range(fp, 16); k++) chain[k] = G64(fp + 8), fp = G64(fp);
    const uint64_t lo = gpu & ~(PAGE - 1), hi = (gpu + size + PAGE - 1) & ~(PAGE - 1);
    os_mutex_lock(&lock);
    int g = 0;
    while (g < n_groups && (groups[g].flags != flags || groups[g].claim != claim || memcmp(groups[g].chain, chain, KEYDEPTH * 8))) g++;
    if (g == n_groups && n_groups < MAXGROUP) {
        memcpy(groups[g].chain, chain, sizeof chain);
        groups[g].flags = flags, groups[g].claim = claim, groups[g].by = by;
        n_groups++;
    }
    if (g < MAXGROUP && n_pools < MAXPOOL) {
        Pool *p = &pools[n_pools];
        memset(p, 0, sizeof *p);
        p->lo = lo, p->hi = hi, p->base = gpu, p->size = size, p->flags = flags, p->claim = claim, p->by = by, p->group = g;
        p->made = hos_present_count;
        p->pg = (PageSt *)calloc((hi - lo) / PAGE, sizeof *p->pg);
        p->grains = (volatile uint64_t *)calloc(((hi - lo) / 16 + 63) / 64, 8);
        if (p->pg && p->grains) {
            if (whole) {
                for (uint64_t i = 0; i < (hi - lo) / PAGE; i++) p->pg[i].armed = 3;
            }
            __atomic_store_n(&n_pools, n_pools + 1, __ATOMIC_RELEASE);
            for (uint64_t a = lo; a < hi; a += PAGE) __atomic_store_n(&page_pool[a / PAGE], (uint16_t)n_pools, __ATOMIC_RELEASE);
            if (whole) os_mem_protect(GP(lo), hi - lo, OS_PROT_NONE);
        }
    }
    os_mutex_unlock(&lock);
}
/* nvnMemoryPoolFinalize: its pages open again and forgotten (its counters stay for the summary) */
void poolcensus_pool_gone(uint64_t gpu, uint64_t size) {
    if (on <= 0 || !size) return;
    os_mutex_lock(&lock);
    for (int i = 0; i < n_pools; i++) {
        Pool *p = &pools[i];
        if (p->dead || p->base != gpu || p->size != size) continue;
        p->dead = 1;
        for (uint64_t a = p->lo; a < p->hi; a += PAGE)
            if (page_pool[a / PAGE] == i + 1) page_pool[a / PAGE] = 0;
        os_mem_protect(GP(p->lo), p->hi - p->lo, OS_PROT_RW);
        break;
    }
    os_mutex_unlock(&lock);
}

/* (nn_fs.c) a file read into [guest, guest + n): the kernel cannot fault, so its pages are opened first */
void poolcensus_kernel_write(uint64_t guest, uint64_t n) {
    if (on <= 0) return;
    for (uint64_t a = guest & ~(PAGE - 1); a < guest + n; a += PAGE) {
        Pool *p = pool_at(a);
        if (!p) continue;
        PageSt *s = &p->pg[(a - p->lo) / PAGE];
        if (os_atomic_xchg(&s->armed, 0)) os_mem_protect(GP(a), PAGE, OS_PROT_RW), push(a / PAGE);
    }
}
/* (nvn_claim_write) a writer patch's report */
void poolcensus_reported(uint64_t addr, uint64_t size) {
    if (on <= 0) return;
    Pool *p = pool_at(addr);
    if (!p) return;
    os_atomic_inc(&p->st[REPORTED]), os_atomic_add(&p->st[REPORTED_BYTES], (int64_t)size);
    PageSt *s = &p->pg[(addr - p->lo) / PAGE];
    const uint16_t r = os_atomic_xchg(&s->rec, 0);   /* (the page's last write fault: its writer's write reported) */
    if (r) os_atomic_inc(&recs[r - 1].reported);
}

/* ---- the back end's submissions (MkwVkHost census_*) ---- */
uint64_t poolcensus_begin(void) {
    if (on <= 0) return 0;
    const uint64_t s = os_atomic_inc(&seq_next);
    seq_present[s & (NSEQ - 1)] = hos_present_count;
    seq_phase[s & (NSEQ - 1)] = PH_QUEUED;
    return s;
}
void poolcensus_phase(uint64_t seq, int ph) {
    if (on > 0 && seq) seq_phase[seq & (NSEQ - 1)] = ph == 1 ? PH_FLIGHT : PH_DONE;
}
/* a submission's use of guest [lo, lo + size): its pages armed, GPU-written ones against reads too */
void poolcensus_use(uint64_t seq, uint64_t lo, uint64_t size, int use, int writes) {
    if (on <= 0 || !seq || !size) return;
    Pool *p = pool_at(lo);
    if (!p) return;
    const uint64_t hi = lo + size < p->hi ? lo + size : p->hi;
    os_atomic_add(&p->st[writes ? B_WRITE : B_READ], (int64_t)(hi - lo));
    if (use >= 0 && use < NUSE) os_atomic_add(&p->by_use[use], (int64_t)(hi - lo));
    if (os_atomic_xchg(&p->last_sub, seq) != seq) os_atomic_inc(&p->st[SUBS]);
    if (writes)
        for (uint64_t g = (lo - p->lo) >> 4; g < (hi - p->lo + 15) >> 4; g++)
            if (!(p->grains[g >> 6] >> (g & 63) & 1)) os_atomic_fetch_or(&p->grains[g >> 6], 1ull << (g & 63));
    uint64_t run = 0, run_end = 0;
    int run_prot = -1;
    for (uint64_t a = lo & ~(PAGE - 1); a < hi; a += PAGE) {
        PageSt *s = &p->pg[(a - p->lo) / PAGE];
        s->use_seq = (uint32_t)seq;
        int prot = -1;
        if (writes) {
            s->write_seq = (uint32_t)seq;
            os_atomic_fetch_or(&s->ever, 3);
            os_atomic_xchg(&s->armed, 3);
            prot = OS_PROT_NONE;
        } else if (!(s->ever & 1)) {
            os_atomic_fetch_or(&s->ever, 1);
            const uint8_t was = os_atomic_fetch_or(&s->armed, 1);
            if (!(was & 1)) prot = prot_of((uint8_t)(was | 1));
        }
        if (prot != run_prot || a != run_end) {
            if (run_prot >= 0) os_mem_protect(GP(run), run_end - run, run_prot);
            run = a, run_prot = prot;
        }
        run_end = a + PAGE;
    }
    if (run_prot >= 0) os_mem_protect(GP(run), run_end - run, run_prot);
}

/* ---- the summary ---- */
typedef struct { uintptr_t ptr; uint32_t addr; } ByPtr;
static ByPtr *by_ptr;
static int by_ptr_cmp(const void *a, const void *b) {
    const uintptr_t x = ((const ByPtr *)a)->ptr, y = ((const ByPtr *)b)->ptr;
    return x < y ? -1 : x > y;
}
/* a host pc as the lifted function holding it (f_<guest address>+<host offset>), else its module and offset */
static void pc_name(uintptr_t pc, char *out, size_t cap) {
    if (!by_ptr && (by_ptr = (ByPtr *)malloc(sizeof *by_ptr * a64_fn_count))) {
        for (uint32_t i = 0; i < a64_fn_count; i++) by_ptr[i].ptr = (uintptr_t)a64_fn_ptr[i], by_ptr[i].addr = a64_fn_addr[i];
        qsort(by_ptr, a64_fn_count, sizeof *by_ptr, by_ptr_cmp);
    }
    char mod[512];
    const uintptr_t mb = os_module_of((const void *)pc, mod, sizeof mod);
    if (by_ptr && mb == os_exe_base()) {
        uint32_t lo = 0, hi = a64_fn_count;
        while (lo < hi) {
            const uint32_t mid = (lo + hi) / 2;
            if (by_ptr[mid].ptr <= pc) lo = mid + 1;
            else hi = mid;
        }
        if (lo) {   /* (the nearest lifted function before it: host code of the runtime between them is named so too, so
                       the exe offset follows, for tools/re/samap.py) */
            snprintf(out, cap, "f_%08x+%#lx(exe+%#lx)", by_ptr[lo - 1].addr, (unsigned long)(pc - by_ptr[lo - 1].ptr), (unsigned long)(pc - mb));
            return;
        }
    }
    const char *slash = strrchr(mod, '/');
    snprintf(out, cap, "%s+%#lx", mb ? (slash ? slash + 1 : mod) : "?", (unsigned long)(pc - mb));
}

static const char *const kClaim[NVN_CLAIM_KINDS] = {"none", "textures", "targets", "host", "vram", "upload", "readback", "load once", "layer 0"};

static void summary(void) {
    FILE *f = fopen("poolcensus.txt", "w");
    if (!f) return;
    const int64_t frames = hos_present_count - start_present > 0 ? hos_present_count - start_present : 1;
    fprintf(f, "# debug.poolcensus=%s: %lld presents watched, %d pools in %d groups, %d faults (%d records not kept, %d re-arms lost)\n",
            setting_str(SET_DBG_POOLCENSUS), (long long)frames, (int)n_pools, n_groups, (int)n_faults, (int)recs_full, (int)ring_lost);
    fprintf(f, "# per group (pools of one claim and flags made through the same guest call chain):\n"
               "#  GPU: submissions using it, MB bound a present (read / written by GPU work), pages bound / GPU-written\n"
               "#  CPU W: page writes a present (first write per page per arming), by timing: idle (no submission using the page\n"
               "#    outstanding), queued (submitted, not started), flight (on the GPU); gpu-page / gpu-grain: on a page / 16-byte\n"
               "#    grain GPU work writes; pages ever written\n"
               "#  CPU R: reads of GPU-written pages a present, by the writing submission: queued, flight, same (completed this\n"
               "#    present), later; pages ever read\n"
               "#  host: faults on the back end's threads (its own copies); reported: nvn_claim_write calls (bytes)\n"
               "#  W/R rows: writer or reader: host place (f_<guest fn>+<host offset>), guest lr < caller < caller, thread, faults by\n"
               "#    timing, presents seen, an example offset in the pool\n");
    os_mutex_lock(&lock);
    for (int g = 0; g < n_groups; g++) {
        int64_t st[ST_N] = {0}, by_use[NUSE] = {0};
        uint64_t bytes = 0, pages = 0, bound = 0, gw = 0, cw = 0, cr = 0;
        int np = 0, live = 0;
        for (int i = 0; i < n_pools; i++) {
            Pool *p = &pools[i];
            if (p->group != g) continue;
            np++, live += !p->dead, bytes += p->size;
            for (int k = 0; k < ST_N; k++) st[k] += p->st[k];
            for (int k = 0; k < NUSE; k++) by_use[k] += p->by_use[k];
            for (uint64_t j = 0; j < (p->hi - p->lo) / PAGE; j++) {
                const uint8_t e = p->pg[j].ever;
                pages++, bound += e & 1, gw += (e >> 1) & 1, cw += (e >> 2) & 1, cr += (e >> 3) & 1;
            }
        }
        const Group *G = &groups[g];
        fprintf(f, "\n== G%d: %s by %s, flags %#x, %d pools (%d live), %.2f MB; made at", g, kClaim[G->claim < NVN_CLAIM_KINDS ? G->claim : 0],
                G->by ? G->by : "?", G->flags, np, live, bytes / 1048576.0);
        for (int k = 0; k < CHAIN && G->chain[k]; k++) fprintf(f, "%s%llx", k ? " < " : " ", (unsigned long long)G->chain[k]);
        fprintf(f, "\n  GPU: %lld subs; %.3f MB read, %.3f MB written a present; pages %llu bound, %llu GPU-written of %llu\n",
                (long long)st[SUBS], st[B_READ] / 1048576.0 / frames, st[B_WRITE] / 1048576.0 / frames, (unsigned long long)bound,
                (unsigned long long)gw, (unsigned long long)pages);
        static const char *const uses[NUSE] = {"uniform", "vertex", "index", "indirect", "storage", "storage written", "copy source",
                                                "copy destination", "fill", "texel copy", "condition", "task record"};
        fprintf(f, "  uses (MB a present):");
        for (int k = 0; k < NUSE; k++)
            if (by_use[k]) fprintf(f, " %s %.3f", uses[k], by_use[k] / 1048576.0 / frames);
        fprintf(f, "\n  CPU W: %.2f a present (idle %lld, queued %lld, flight %lld; gpu-page %lld, gpu-grain %lld); %llu pages ever\n",
                (double)(st[W_IDLE] + st[W_QUEUED] + st[W_FLIGHT]) / frames, (long long)st[W_IDLE], (long long)st[W_QUEUED],
                (long long)st[W_FLIGHT], (long long)st[W_GPU_PAGE], (long long)st[W_GPU_GRAIN], (unsigned long long)cw);
        fprintf(f, "  CPU R: %.2f a present (queued %lld, flight %lld, same %lld, later %lld, unbound %lld); %llu pages ever\n",
                (double)(st[R_QUEUED] + st[R_FLIGHT] + st[R_SAME] + st[R_LATER] + st[R_UNBOUND]) / frames, (long long)st[R_QUEUED],
                (long long)st[R_FLIGHT], (long long)st[R_SAME], (long long)st[R_LATER], (long long)st[R_UNBOUND], (unsigned long long)cr);
        fprintf(f, "  host: W %lld, R %lld; reported %lld (%lld bytes)\n", (long long)st[HOST_W], (long long)st[HOST_R],
                (long long)st[REPORTED], (long long)st[REPORTED_BYTES]);
        for (int w = 1; w >= 0; w--)
            for (int i = 0; i < NREC; i++) {
                const Rec *r = &recs[i];
                const uint64_t k = r->key;
                if (!k || k == ~0ull || r->group != g || r->write != w) continue;
                char pc[96];
                pc_name(r->pc, pc, sizeof pc);
                if (w)
                    fprintf(f, "  W %s lr %llx < %llx < %llx \"%s\": idle %lld, queued %lld, flight %lld; gpu-page %lld, gpu-grain %lld; reported %lld; %lld presents; +%#llx\n",
                            pc, (unsigned long long)r->lr, (unsigned long long)r->f1, (unsigned long long)r->f2, r->thread,
                            (long long)r->n[0], (long long)r->n[1], (long long)r->n[2], (long long)r->n[3], (long long)r->n[4],
                            (long long)r->reported, (long long)r->frames, (unsigned long long)r->off);
                else
                    fprintf(f, "  R %s lr %llx < %llx < %llx \"%s\": queued %lld, flight %lld, same %lld, later %lld, unbound %lld; %lld presents; +%#llx\n",
                            pc, (unsigned long long)r->lr, (unsigned long long)r->f1, (unsigned long long)r->f2, r->thread,
                            (long long)r->n[0], (long long)r->n[1], (long long)r->n[2], (long long)r->n[3], (long long)r->n[4],
                            (long long)r->frames, (unsigned long long)r->off);
            }
    }
    os_mutex_unlock(&lock);
    fclose(f);
}

/* (each present) the pages opened since are armed again; the summary every 300 presents */
void poolcensus_frame(void) {
    if (on <= 0 || start_present < 0) return;
    const uint32_t head = __atomic_load_n(&ring_head, __ATOMIC_ACQUIRE);
    for (; ring_tail != head; ring_tail++) {
        uint64_t v = 0;
        for (int spin = 0; spin < 1000 && !(v = os_atomic_xchg(&ring[ring_tail & (NRING - 1)], 0)); spin++) {}
        if (!v) continue;
        const uint64_t a = (v - 1) * PAGE;
        Pool *p = pool_at(a);
        if (!p) continue;
        PageSt *s = &p->pg[(a - p->lo) / PAGE];
        const uint8_t armed = s->ever & 2 || whole ? 3 : 1;
        os_atomic_xchg(&s->armed, armed);
        os_mem_protect(GP(a), PAGE, prot_of(armed));
    }
    if ((hos_present_count - start_present) % 300 == 299) summary();
}
