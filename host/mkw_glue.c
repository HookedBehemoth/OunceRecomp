/* mkw-recomp's pieces of the standalone runtime that TOTKPort's hos/ keeps elsewhere: the 64-bit guest window's
   commit (TOTK commits its whole 4 GB window up front; MKW's 512 GB window is only reserved), and stand-ins for the
   hooks of files not taken into host/ (the renderer and display, the profiler, mods, TOTK's own patches). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dlfcn.h>
#include <execinfo.h>

/* ---- the guest window ---------------------------------------------------------------------------------------
   The runtime commits what it hands out (image, system region, heap, address regions: hos_commit), because host
   APIs given guest buffers (ReadFile into a game buffer) fail on reserved pages without raising an exception. A
   guest or host access to a page nothing committed is committed on first touch, 64 KB at a time, by the handler. */
void hos_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    if (!hos_guest_range(addr, size)) {   /* (hookshot: guest pages only; the posix layer's commit changes any page's protection) */
        hos_log("mem: commit of %#llx bytes at %#llx: outside the guest window", (unsigned long long)size, (unsigned long long)addr);
        return;
    }
    const uint64_t lo = addr & ~0xFFFFULL, hi = (addr + size + 0xFFFF) & ~0xFFFFULL;
    if (!VirtualAlloc(GP(lo), (SIZE_T)(hi - lo), MEM_COMMIT, PAGE_READWRITE)) {
        const DWORD err = GetLastError();
        hos_log("mem: commit of %#llx bytes at %#llx failed (error %lu)", (unsigned long long)(hi - lo),
                (unsigned long long)lo, err);
        if (err == ERROR_COMMITMENT_LIMIT) {   /* (1455: RAM and page file full; the game crashes soon after. Said once, on its own thread) */
            static volatile LONG said;
            if (InterlockedExchange(&said, 1) == 0) {
                hos_log("mem: Windows is out of memory (RAM and page file): close other programs, or let Windows manage the page file size");
                extern DWORD WINAPI hos_out_of_memory_box(LPVOID);
                CloseHandle(CreateThread(NULL, 0, hos_out_of_memory_box, NULL, 0, NULL));
            }
        }
    }
}

DWORD WINAPI hos_out_of_memory_box(LPVOID unused) {
    (void)unused;
    MessageBoxA(NULL,
                "Windows has run out of memory (RAM plus the page file), so Hookshot may close.\n\n"
                "Close other programs, and in Windows' System > About > Advanced system settings > Performance > Advanced > Virtual "
                "memory, choose \"Automatically manage paging file size\" (or give it at least 16 GB). A lower Quality preset in the "
                "Insert menu also uses less memory.",
                "Hookshot: out of memory", MB_OK | MB_ICONWARNING | MB_TOPMOST);
    return 0;
}

static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS *info) {
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t *p = (const uint8_t *)rec->ExceptionInformation[1];
    if (p < A64_MEM || p >= A64_MEM + HOS_GUEST_SPACE) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t guest = (uint64_t)(p - A64_MEM);
    if (guest < 0x100000) return EXCEPTION_CONTINUE_SEARCH;   /* (a null pointer: the game's own fault) */
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_RESERVE) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG touched;
    if (InterlockedIncrement(&touched) <= 16)
        hos_log("mem: %#llx committed on first touch (nothing handed it out)", (unsigned long long)guest);
    return VirtualAlloc(GP(guest & ~0xFFFFULL), 0x10000, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION
                                                                                      : EXCEPTION_CONTINUE_SEARCH;
}

/* A crash nothing handles: what, where (hookshot+offset: nx2/samap.py names the lifted function), and the guest
   state of the thread, as of its last call out of lifted code, with its frame chain from x29. */
static uint64_t peek_guest(uint64_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (a >= HOS_GUEST_SPACE - 8 || !VirtualQuery(GP(a), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    return G64(a);
}

static LONG WINAPI report_crash(EXCEPTION_POINTERS *info) {
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    const uint64_t rip = info->ContextRecord->Rip;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rip, &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    const char *base = strrchr(name, '\\');
    const uint64_t addr = rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0;
    hos_log("CRASH: exception %#lx (%s %#llx, guest %#llx) at %s+%#llx", rec->ExceptionCode,
            rec->NumberParameters > 1 && rec->ExceptionInformation[0] == 1 ? "write" : "read",
            (unsigned long long)addr, (unsigned long long)(addr - (uint64_t)A64_MEM), base ? base + 1 : name,
            (unsigned long long)(rip - (uint64_t)mod));
    {   /* (hookshot) Linux: the host stack as module+offset, from glibc (the frames above the signal's included): the
           x64 unwind data is Windows' */
        void *pcs[24];
        const int got = backtrace(pcs, 24);
        char hb[1024];
        int n = snprintf(hb, sizeof hb, "CRASH: host backtrace:");
        for (int i = 0; i < got && n < (int)sizeof hb - 48; i++) {
            Dl_info d;
            const char *mn = "?", *mb;
            uint64_t off = (uint64_t)(uintptr_t)pcs[i];
            if (dladdr(pcs[i], &d) && d.dli_fname) mn = d.dli_fname, off -= (uint64_t)(uintptr_t)d.dli_fbase;
            mb = strrchr(mn, '/');
            n += snprintf(hb + n, sizeof hb - n, " %s+%#llx", mb ? mb + 1 : mn, (unsigned long long)off);
        }
        hos_log("%s", hb);
    }
    Ctx *c = hos_ctx();
    if (c) {
        hos_log("CRASH: thread %s, sp %#llx", hos_thread_name(hos_self()), (unsigned long long)c->x[31]);
        for (int i = 0; i < 31; i += 4)
            hos_log("CRASH:   x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx", i,
                    (unsigned long long)c->x[i], i + 1, (unsigned long long)c->x[i + 1], i + 2,
                    (unsigned long long)c->x[i + 2], i + 3, i + 3 < 31 ? (unsigned long long)c->x[i + 3] : 0ULL);
        char bt[1024];
        int n = snprintf(bt, sizeof bt, "CRASH: guest backtrace: lr %#llx", (unsigned long long)c->x[30]);
        uint64_t fp = c->x[29];
        for (int i = 0; i < 40 && fp && !(fp & 15) && n < (int)sizeof bt - 24; i++) {
            const uint64_t lr = peek_guest(fp + 8), next = peek_guest(fp);
            n += snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)lr);
            if (next <= fp) break;
            fp = next;
        }
        hos_log("%s", bt);
        /* the memory x0, x1 and x19 point to (the object a crash is usually about) */
        static const int dumped[] = {0, 1, 19};
        for (int k = 0; k < 3; k++) {
            const int r = dumped[k];
            const uint64_t base = c->x[r];
            if (base < 0x100000 || !peek_guest(base) && !peek_guest(base + 8) && !peek_guest(base + 0x100)) continue;
            for (int row = 0; row < 0x120; row += 32)
                hos_log("CRASH: [x%d+%#05x] %016llx %016llx %016llx %016llx", r, row,
                        (unsigned long long)peek_guest(base + row), (unsigned long long)peek_guest(base + row + 8),
                        (unsigned long long)peek_guest(base + row + 16), (unsigned long long)peek_guest(base + row + 24));
        }
    }
    hos_log_flush();
    void hos_hostcall_stats(void);
    hos_hostcall_stats();   /* (HOS_HOSTCALL_STATS=1: the host calls made, and the last 256 of them) */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* (hos_prof.c's, for hos_fatal) the current thread's guest frame chain from x29 */
void hos_backtrace(void) {
    Ctx *c = hos_ctx();
    if (!c) return;
    char bt[1024];
    int n = snprintf(bt, sizeof bt, "guest backtrace: lr %#llx", (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 40 && fp && !(fp & 15) && n < (int)sizeof bt - 24; i++) {
        const uint64_t lr = peek_guest(fp + 8), next = peek_guest(fp);
        n += snprintf(bt + n, sizeof bt - n, " %#llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    fprintf(stderr, "[hos] %s\n", bt);
    fflush(stderr);
}

/* HOS_STATS_AFTER=<seconds> (with HOS_HOSTCALL_STATS=1): the host call counts and the last calls at that time, for
   runs that are stopped rather than exit (what a quiet game is doing) */
static DWORD WINAPI stats_after(void *p) {
    Sleep((DWORD)(uintptr_t)p * 1000);
    void hos_hostcall_stats(void);
    hos_log("stats: after %u s", (unsigned)(uintptr_t)p);
    void hos_thread_dump(void);
    hos_thread_dump();
    hos_log_flush();
    hos_hostcall_stats();
    return 0;
}

/* heap corruption and fail-fast reports go to the process's end without reaching the unhandled filter: reported here
   first (the backtrace is where the heap noticed, after the bad write) */
static LONG CALLBACK report_fatal(EXCEPTION_POINTERS *info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000374 || code == 0xC0000409) report_crash(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

void hos_commit_init(void) {
    AddVectoredExceptionHandler(1, commit_on_touch);
    AddVectoredExceptionHandler(0, report_fatal);
    SetUnhandledExceptionFilter(report_crash);
    const char *after = getenv("HOS_STATS_AFTER");
    if (after && atoi(after) > 0) CreateThread(NULL, 0, stats_after, (void *)(uintptr_t)atoi(after), 0, NULL);
}

/* ---- stand-ins --------------------------------------------------------------------------------------------- */
/* HOS_PROFILE=<seconds> (in place of TOTKPort's hos_prof.c): every interval, the process's busiest threads and their
   share of a core (guest threads by their guest names, host threads by their descriptions): which one a frame rate
   waits for */
void hos_mem_checkpoint(const char *what) { (void)what; }        /* (hos_prof.c: HOS_MEM_TIMELINE) */
int hos_blank_cached_icon(const char *guest, const char *host) { (void)guest, (void)host; return 0; }   /* (TOTK) */
int hos_ultrawide_redirect(const char *guest, char *out, size_t cap) { (void)guest, (void)out, (void)cap; return 0; }
uint64_t hos_title_input(uint64_t buttons) { return buttons; }   /* (totk_title.c) */
uint64_t hos_frame_step_buttons(uint64_t buttons) { return buttons; }   /* (nn_vi.c) */
/* (hookshot) hos_sdl_pads, hos_sdl_rumble: host/pad_sdl.c (from TOTKPort) */

/* TOTK's emulated GPU (gpu.c, its Maxwell command processor), which nvdrv.c and the debug commands call into: MKW
   has none (its NVN2 calls go to nvn_native.c), so a submission is only counted. */
int hos_gpu_async(void) { return 0; }
void hos_gpu_submit(int channel_fd, const uint64_t *entries, uint32_t n) {
    static volatile LONG seen;
    if (InterlockedIncrement(&seen) <= 4) hos_log("gpu: submit of %u entries on channel %d (no GPU yet)", n, channel_fd);
    (void)entries;
}
void hos_gpu_queue_submit(int channel_fd, const uint64_t *entries, uint32_t n, int incr_id) {
    (void)incr_id;
    hos_gpu_submit(channel_fd, entries, n);
}

/* (nx2/rtgen.py) the lifted code's loop poll calls this when Ctx.poll is set; nothing sets it any more (the virtual
   cores that did are gone), so it is never reached. It stays until the lifter stops emitting the polls */
void a64_poll_cur(void) {
    void hos_vcore_checkpoint(void);
    hos_vcore_checkpoint();
}

/* CNTPCT_EL0 at the Switch 2's 31.25 MHz (rt/a64_rt.c's default counts the Switch 1's 19.2 MHz). The SDK checks
   nn::os::GetSystemTickFrequency against 31250000 at init and aborts otherwise. */
uint64_t a64_cntpct_host(void) {
    static LARGE_INTEGER base;
    static uint64_t mult;
    LARGE_INTEGER now;
    if (!__atomic_load_n(&mult, __ATOMIC_ACQUIRE)) {
        LARGE_INTEGER f, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&b);
        base = b;
        __atomic_store_n(&mult, (HOS_TICK_HZ << 32) / (uint64_t)f.QuadPart, __ATOMIC_RELEASE);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)(((unsigned __int128)(uint64_t)(now.QuadPart - base.QuadPart) * mult) >> 32);
}

/* ---- what the renderer asks of the profiler and ultrawide code ---- */
int hos_ab(void) { return 0; }                    /* (hos_prof.c: A/B measurement phases) */
/* (with the renderer's mem: lines) HOS_MEM_MAP=1: how much of the guest window is committed, and its resident part */
void hos_memory_map_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("HOS_MEM_MAP") != NULL;
    if (!on) return;
    uint64_t committed = 0, regions = 0;
    for (uint64_t a = 0; a < HOS_GUEST_SPACE;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(GP(a), &m, sizeof m)) break;
        const uint64_t end = (uint64_t)((uint8_t *)m.BaseAddress + m.RegionSize - (uint8_t *)GP(0));
        if (m.State == MEM_COMMIT) committed += m.RegionSize, regions++;
        a = end > a ? end : a + 0x1000;
    }
    hos_log("mem: guest window %llu MB committed in %llu regions", (unsigned long long)(committed >> 20),
            (unsigned long long)regions);
    /* the rest of the address space: private memory by allocation, the largest */
    struct { uint64_t base, bytes; } top[8] = {{0}};
    uint64_t other = 0, cur_base = 0, cur = 0;
    const uint64_t g0 = (uint64_t)(uintptr_t)GP(0), g1 = g0 + HOS_GUEST_SPACE;
    for (uint64_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)(uintptr_t)a, &m, sizeof m)) break;
        const uint64_t base = (uint64_t)(uintptr_t)m.AllocationBase, end = (uint64_t)(uintptr_t)m.BaseAddress + m.RegionSize;
        if (base != cur_base) {
            for (int k = 0; k < 8 && cur; k++)
                if (cur > top[k].bytes) {
                    memmove(&top[k + 1], &top[k], (size_t)(7 - k) * sizeof top[0]);
                    top[k].base = cur_base, top[k].bytes = cur;
                    break;
                }
            cur_base = base, cur = 0;
        }
        if (m.State == MEM_COMMIT && m.Type == MEM_PRIVATE && (a < g0 || a >= g1)) other += m.RegionSize, cur += m.RegionSize;
        a = end > a ? end : a + 0x1000;
    }
    hos_log("mem: private outside the guest window %llu MB; largest: %llx %llu MB, %llx %llu MB, %llx %llu MB, %llx %llu "
            "MB, %llx %llu MB, %llx %llu MB", (unsigned long long)(other >> 20), (unsigned long long)top[0].base,
            (unsigned long long)(top[0].bytes >> 20), (unsigned long long)top[1].base, (unsigned long long)(top[1].bytes >> 20),
            (unsigned long long)top[2].base, (unsigned long long)(top[2].bytes >> 20), (unsigned long long)top[3].base,
            (unsigned long long)(top[3].bytes >> 20), (unsigned long long)top[4].base, (unsigned long long)(top[4].bytes >> 20),
            (unsigned long long)top[5].base, (unsigned long long)(top[5].bytes >> 20));
}
void hos_ultrawide_apply(void) {}                 /* (totk_ultrawide.c) */
float hos_view_aspect;                            /* (totk_ultrawide.c: 0, the game's own 16:9) */
void hos_host_thread_register_self(const char *name) {   /* (hos_prof.c: names for its stats; here the thread's description) */
    wchar_t w[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64)) SetThreadDescription(GetCurrentThread(), w);
}
void hos_process_memory(uint64_t *private_bytes, uint64_t *working_set) {
    typedef struct {
        DWORD cb, PageFaultCount;
        SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
            QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
    } Counters;
    typedef BOOL(WINAPI * Fn)(HANDLE, Counters *, DWORD);
    static Fn fn;
    if (!fn) fn = (Fn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    Counters m;
    memset(&m, 0, sizeof m);
    m.cb = sizeof m;
    if (!fn || !fn(GetCurrentProcess(), &m, sizeof m)) memset(&m, 0, sizeof m);
    *private_bytes = m.PrivateUsage;
    *working_set = m.WorkingSetSize;
}
