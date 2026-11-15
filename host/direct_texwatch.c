/* HOS_DBG=texwatch (direct mode, diagnosis only): which code writes texels into a texture's storage with
   the CPU. The direct back end never reads texture storage (docs/direct-backend-plan.md, Textures), so a texture whose
   texels the CPU stores there, with no NVN call, never gets them: each such writer needs a game patch that hands the
   bytes to the back end (hsd_texels), as the glyph writer has (docs/direct/terrain-texels.md).

   Every texture initialized (in the guest window) has its storage's pages made read-only; the first write to a page
   faults here, is noted by (texture, writer: the host place in the lifted code, the guest lr and two frames), and the
   page is made writable again; every 2 s the live textures' pages are protected again, so later writers are seen too.
   File reads straight into a texture's storage (the kernel's writes cannot fault) are noted from nn_fs.c. New pairs are
   logged by a thread of their own ("texwatch: ..."), and a summary every 600 presents. The fault costs a page's
   protection change: a debug run only. With HOS_DBG unset nothing here runs. */
#include "hos.h"
#include "direct.h"
#include "../gpu/api/rx_nvn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <sys/mman.h>

extern volatile LONG64 hos_present_count;
uint64_t nvn_gpu_guest(uint64_t gpu);

static int dbg_has(const char *name) {
    const char *d = getenv("HOS_DBG");
    if (!d) return 0;
    const size_t n = strlen(name);
    for (const char *p = d; (p = strstr(p, name)); p += n)
        if ((p == d || p[-1] == ',') && (p[n] == 0 || p[n] == ',' || p[n] == ':')) return 1;
    return 0;
}

/* the text after "<name>:" in HOS_DBG, up to the next comma (into out); 0: not there */
static int dbg_val(const char *name, char *out, size_t cap) {
    const char *d = getenv("HOS_DBG");
    if (!d) return 0;
    const size_t n = strlen(name);
    for (const char *p = d; (p = strstr(p, name)); p += n)
        if ((p == d || p[-1] == ',') && p[n] == ':') {
            const char *v = p + n + 1, *e = strchr(v, ',');
            const size_t k = e ? (size_t)(e - v) : strlen(v);
            snprintf(out, cap, "%.*s", (int)k, v);
            return 1;
        }
    return 0;
}

/* HOS_DBG=memwatch:<hex>[+<hex>...] (diagnosis): the pages of those guest addresses watched the same way from the first
   texture on, every write to the 64-byte line of the address named with its writer (at most 40 lines a watch);
   HOS_DBG=memfind:<hex>[+<hex>...]@<present>: at that present the resident pages of the heap are searched for those 64-bit
   values (8-byte aligned), each place logged (at most 64): which object holds an address */
#define NMW 8
static uint64_t mw_addr[NMW], mf_val[NMW];
static int n_mw, n_mf;
static int64_t mf_present = -1;
static volatile LONG mw_logged[NMW];
static void parse_list(const char *v, uint64_t *out, int *n) {
    for (const char *p = v; *p && *n < NMW;) {
        out[(*n)++] = strtoull(p, NULL, 16);
        const char *q = strchr(p, '+');
        if (!q) break;
        p = q + 1;
    }
}

static int on_ = -1;
int direct_texwatch_on(void) {
    if (on_ < 0) {
        char v[512];
        if (rx_direct() && dbg_val("memwatch", v, sizeof v)) parse_list(v, mw_addr, &n_mw);
        if (rx_direct() && dbg_val("memfind", v, sizeof v)) {
            parse_list(v, mf_val, &n_mf);
            const char *at = strchr(v, '@');
            mf_present = at ? atoll(at + 1) : 1200;
        }
        on_ = rx_direct() && (dbg_has("texwatch") || n_mw || n_mf);
    }
    return on_;
}
static int textures_on(void) { return direct_texwatch_on() && dbg_has("texwatch"); }

#define NTEX 65536
typedef struct {
    volatile uint64_t lo, hi;   /* the storage, guest addresses; lo 0: a free slot */
    uint64_t serial, init_lr, gpu;
    uint32_t w, h, d, levels, fmt, target, flags, stride;
    int64_t present;
    volatile LONG dead;
} Tex;
static Tex tex[NTEX];
static volatile LONG ntex;   /* (slots used: entries are filled before ntex covers them) */
static SRWLOCK tex_lock = SRWLOCK_INIT;   /* (created / released / the re-arm; never taken by the fault handler) */

#define NPAIR 4096
typedef struct {
    volatile uint64_t key;   /* texture slot << 40 | host offset of the writer */
    uint64_t lr, f1, f2, addr;
    const char *thread;
    int64_t present;
    volatile LONG count, logged;
} Pair;
static Pair pairs[NPAIR];
static volatile LONG n_faults, n_outside, n_pairs_full, n_files;
static uintptr_t exe_base;

static void protect(const Tex *t, DWORD how) {
    const uintptr_t lo = (uintptr_t)GP(t->lo) & ~(uintptr_t)4095, hi = ((uintptr_t)GP(t->hi) + 4095) & ~(uintptr_t)4095;
    DWORD old;
    if (hi > lo) VirtualProtect((void *)lo, hi - lo, how, &old);
}

static void note(int slot, uint64_t where, uint64_t addr, const char *thread_name) {
    Ctx *c = hos_ctx();
    const uint64_t key = (uint64_t)slot << 40 | (where & ((1ull << 40) - 1));
    for (uint32_t i = (uint32_t)((key ^ key >> 23) * 2654435761u) % NPAIR, n = 0; n < NPAIR; n++, i = (i + 1) % NPAIR) {
        uint64_t k = __atomic_load_n(&pairs[i].key, __ATOMIC_ACQUIRE);
        if (!k) {
            Pair *p = &pairs[i];
            p->lr = c ? c->x[30] : 0;
            uint64_t fp = c ? c->x[29] : 0;
            p->f1 = p->f2 = 0;
            if (fp && !(fp & 7) && hos_guest_range(fp, 16)) {
                p->f1 = G64(fp + 8);
                const uint64_t up = G64(fp);
                if (up > fp && !(up & 7) && hos_guest_range(up, 16)) p->f2 = G64(up + 8);
            }
            p->addr = addr, p->thread = thread_name, p->present = hos_present_count;
            uint64_t expect = 0;
            if (__atomic_compare_exchange_n(&pairs[i].key, &expect, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) k = key;
            else k = expect;
        }
        if (k == key) {
            InterlockedIncrement(&pairs[i].count);
            return;
        }
    }
    InterlockedIncrement(&n_pairs_full);
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t *p = (const uint8_t *)r->ExceptionInformation[1];
    const uint8_t *base = (const uint8_t *)GP(0);
    if (p < base || p >= base + HOS_GUEST_SPACE) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t a = (uint64_t)(p - base), page = a & ~4095ull;
    int found = 0, inside = 0;
    const LONG n = __atomic_load_n(&ntex, __ATOMIC_ACQUIRE);
    HosThread *th = hos_self();
    const char *tn = th ? hos_thread_name(th) : "?";
    const uint64_t where = (uint64_t)(uintptr_t)r->ExceptionAddress - exe_base;
    for (int i = 0; i < n_mw; i++)
        if ((mw_addr[i] & ~4095ull) == page) {
            found = 1;
            if ((a & ~63ull) == (mw_addr[i] & ~63ull) && InterlockedIncrement(&mw_logged[i]) <= 40) {
                hos_log("memwatch: %#llx written at %#llx by exe+%#llx on %s at present %lld (it held %#llx)", (unsigned long long)mw_addr[i],
                        (unsigned long long)a, (unsigned long long)where, tn, (long long)hos_present_count,
                        (unsigned long long)G64(mw_addr[i] & ~7ull));
                void hos_backtrace(void);
                hos_backtrace();
            }
        }
    for (LONG i = 0; i < n; i++) {
        const uint64_t lo = tex[i].lo, hi = tex[i].hi;
        if (!lo || tex[i].dead || page >= ((hi + 4095) & ~4095ull) || page + 4096 <= (lo & ~4095ull)) continue;
        found = 1;
        if (a >= lo && a < hi) inside = 1, note((int)i, where, a, tn);
    }
    if (!found) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&n_faults);
    if (!inside) InterlockedIncrement(&n_outside);
    DWORD old;
    VirtualProtect((void *)(base + page), 4096, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void log_pairs(void) {
    for (int i = 0; i < NPAIR; i++) {
        Pair *p = &pairs[i];
        const uint64_t k = __atomic_load_n(&p->key, __ATOMIC_ACQUIRE);
        if (!k || p->logged) continue;
        p->logged = 1;
        const Tex *t = &tex[k >> 40];
        hos_log("texwatch: texture %#llx (+%#llx) %ux%ux%u, %u levels, NVN format %#x, target %d, flags %#x, stride %u, %llu bytes "
                "(serial %llu, initialized by lr %#llx at present %lld) written by exe+%#llx (guest lr %#llx < %#llx < %#llx) "
                "on %s at present %lld",
                (unsigned long long)t->lo, (unsigned long long)(p->addr - t->lo), t->w, t->h, t->d, t->levels, t->fmt, t->target,
                t->flags, t->stride, (unsigned long long)(t->hi - t->lo), (unsigned long long)t->serial,
                (unsigned long long)t->init_lr, (long long)t->present, (unsigned long long)(k & ((1ull << 40) - 1)),
                (unsigned long long)p->lr, (unsigned long long)p->f1, (unsigned long long)p->f2, p->thread ? p->thread : "?",
                (long long)p->present);
    }
}

static DWORD WINAPI rearm(void *arg) {
    (void)arg;
    for (int k = 0;; k++) {
        Sleep(500);
        log_pairs();
        if (k % 4) continue;
        AcquireSRWLockShared(&tex_lock);
        const LONG n = ntex;
        for (LONG i = 0; i < n; i++)
            if (tex[i].lo && !tex[i].dead) protect(&tex[i], PAGE_READONLY);
        ReleaseSRWLockShared(&tex_lock);
        for (int i = 0; i < n_mw; i++) {
            DWORD old;
            VirtualProtect(GP(mw_addr[i] & ~4095ull), 4096, PAGE_READONLY, &old);
        }
    }
    return 0;
}

static void start(void) {
    static volatile LONG done;
    if (InterlockedExchange(&done, 1)) return;
    exe_base = (uintptr_t)GetModuleHandleA(NULL);
    AddVectoredExceptionHandler(1, handler);
    CreateThread(NULL, 0, rearm, NULL, 0, NULL);
    hos_log("texwatch: CPU writes into texture storage are watched (HOS_DBG=texwatch; writers named as exe+offset: "
            "nx2/samap.py)");
}

void direct_texwatch_created(const void *texture, uint64_t init_lr) {
    if (!direct_texwatch_on()) return;
    start();
    if (!textures_on()) return;
    const nvn_tex *o = (const nvn_tex *)texture;
    if (!o->gpu || o->gpu >= 0x10000000000ull || o->d.target == NVN_TEXTURE_TARGET_BUFFER) return;   /* (virtual pools: not watched) */
    const uint64_t g = nvn_gpu_guest(o->gpu);
    if (!g || !o->L.size || !hos_guest_range(g, o->L.size)) return;
    start();
    AcquireSRWLockExclusive(&tex_lock);
    LONG at = -1;
    for (LONG i = 0; i < ntex; i++)
        if (tex[i].lo && tex[i].dead) { at = i; break; }
    if (at < 0 && ntex < NTEX) at = ntex;
    if (at >= 0) {
        Tex *t = &tex[at];
        t->lo = 0;
        t->hi = g + o->L.size;
        t->gpu = o->gpu, t->serial = o->serial, t->init_lr = init_lr, t->present = hos_present_count;
        t->w = o->d.width, t->h = o->d.height, t->d = o->d.depth, t->levels = o->d.levels, t->fmt = (uint32_t)o->d.format;
        t->target = (uint32_t)o->d.target, t->flags = (uint32_t)o->d.flags;
        t->stride = nvn_tex_is_linear(&o->d) ? nvn_tex_linear_stride(&o->d) : 0;
        __atomic_store_n(&t->dead, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&t->lo, g, __ATOMIC_RELEASE);
        if (at == ntex) __atomic_store_n(&ntex, ntex + 1, __ATOMIC_RELEASE);
        protect(t, PAGE_READONLY);
    }
    ReleaseSRWLockExclusive(&tex_lock);
}

/* (a released slot is reused by a later texture, which overwrites its description: pairs are logged within 0.5 s of
   their fault, normally before that) */
void direct_texwatch_released(const void *texture) {
    if (!direct_texwatch_on()) return;
    const nvn_tex *o = (const nvn_tex *)texture;
    AcquireSRWLockExclusive(&tex_lock);
    for (LONG i = 0; i < ntex; i++)
        if (tex[i].lo && !tex[i].dead && tex[i].serial == o->serial && tex[i].gpu == o->gpu) {
            tex[i].dead = 1;
            protect(&tex[i], PAGE_READWRITE);
            break;
        }
    ReleaseSRWLockExclusive(&tex_lock);
}

/* nn_fs.c: a file read into [guest, guest + n): its pages made writable first (the kernel cannot fault), the texture noted */
void direct_texwatch_kernel_write(uint64_t guest, uint64_t n, const char *what) {
    if (!direct_texwatch_on() || !n) return;
    for (int i = 0; i < n_mw; i++)
        if (mw_addr[i] >= guest && mw_addr[i] < guest + n) hos_log("memwatch: %#llx in a file read: %s", (unsigned long long)mw_addr[i], what);
    AcquireSRWLockShared(&tex_lock);
    for (LONG i = 0; i < ntex; i++) {
        const uint64_t lo = tex[i].lo, hi = tex[i].hi;
        if (!lo || tex[i].dead || guest >= ((hi + 4095) & ~4095ull) || guest + n <= (lo & ~4095ull)) continue;
        if (guest < hi && guest + n > lo && InterlockedIncrement(&n_files) <= 40)
            hos_log("texwatch: a file read into texture %#llx %ux%ux%u NVN format %#x: %s, %llu bytes to %#llx", (unsigned long long)lo,
                    tex[i].w, tex[i].h, tex[i].d, tex[i].fmt, what, (unsigned long long)n, (unsigned long long)guest);
    }
    ReleaseSRWLockShared(&tex_lock);
    const uintptr_t plo = (uintptr_t)GP(guest) & ~(uintptr_t)4095, phi = ((uintptr_t)GP(guest + n) + 4095) & ~(uintptr_t)4095;
    DWORD old;
    VirtualProtect((void *)plo, phi - plo, PAGE_READWRITE, &old);
}

static void mem_find(void) {
    const uint64_t lo = 0x1000000000ull, hi = lo + hos_heap_size;
    const long pg = 4096;
    unsigned char v[256];
    int found = 0;
    const int64_t t0 = hos_now_ns();
    for (uint64_t a = lo; a < hi && found < 64; a += sizeof v * (uint64_t)pg) {
        const uint64_t len = hi - a < sizeof v * (uint64_t)pg ? hi - a : sizeof v * (uint64_t)pg;
        if (mincore(GP(a), (size_t)len, v)) continue;
        for (uint64_t k = 0; k < len / (uint64_t)pg && found < 64; k++) {
            if (!(v[k] & 1)) continue;
            const uint64_t *w = (const uint64_t *)GP(a + k * (uint64_t)pg);
            for (int j = 0; j < 512; j++)
                for (int i = 0; i < n_mf; i++)
                    if (w[j] == mf_val[i] && found++ < 64)
                        hos_log("memfind: %#llx at %#llx", (unsigned long long)mf_val[i], (unsigned long long)(a + k * (uint64_t)pg + 8 * (uint64_t)j));
        }
    }
    hos_log("memfind: done at present %lld (%d places, %.0f ms)", (long long)hos_present_count, found, (hos_now_ns() - t0) / 1e6);
}

void direct_texwatch_frame(void) {
    if (!direct_texwatch_on()) return;
    if (n_mf && hos_present_count == mf_present) mem_find();
    if (!textures_on() || hos_present_count % 600) return;
    LONG pairs_n = 0;
    for (int i = 0; i < NPAIR; i++) pairs_n += pairs[i].key != 0;
    hos_log("texwatch: %ld textures watched (slots), %ld faults (%ld outside a texture), %ld (texture, writer) pairs (%ld not kept), "
            "%ld file reads into one", (long)ntex, (long)n_faults, (long)n_outside, (long)pairs_n, (long)n_pairs_full, (long)n_files);
}
