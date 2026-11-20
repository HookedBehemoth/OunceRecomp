/* The native NVN layer's API (HOS_NVN_NATIVE=1): TOTK's NVN calls are answered here instead of by the lifted NVN
   driver over an emulated Switch GPU (hos/gpu.c). See renderer/api/rx_nvn.h for the design; the replay of what is
   recorded here is gpu/legacy/render/rx_nvn.cpp.

   - Objects with semantics (device, memory pools, buffers, textures, samplers, descriptor pools, queue, window, sync,
     events, command buffer memory) are implemented as NVN defines them, in place in the game's object memory.
   - Calls that only change GPU state or do GPU work (state objects bound, draws, clears, copies...) are recorded as
     commands in host memory; a submit replays them (rx_nvn_submit) at the submit call, which is when the console reads
     what they point at: TOTK fills deferred texture tables and constant pools for a frame just before its submit and
     rewrites them just after.
   - A function TOTK has not been seen calling stops the game with its name when called (no silent wrong result).

   Calls come from lifted code: arguments are in the guest registers (x0-x7, floats in v0-v7), pointers are guest
   addresses. Adapted from the MIT-licensed NVN layer of d2r-mobile / MacNativeGames (jhaizhou), which runs TOTK on
   Metal: its semantics, TOTK findings and command records; simplified here where noted.

   mkw-recomp: TOTKPort's hos/nvn_native.c (e88744a) for Mario Kart World's NVN2 (API 231.39): the Switch 2 driver's
   device limits, GPU addresses over the 64-bit guest window (identity below 1 TB, virtual pools above), and NVN2's
   own procs (program builders, native images, sparse tile layouts, recording modes). The SDK's exports reach the
   same functions as the proc table (nvn2_dispatch, from gen/nvn2_exports.c). */
#include "hos.h"
#include "crit.h"
#include "rx_nvn.h"
#include "nvn_list.h"
#include "hos_ledger.h"
#include "game_hooks.h"
#include "nvn_claims.h"
#include "renderer.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
uint64_t hos_hostcall(const char *name, void (*fn)(Ctx *, void *), void *user);   /* hos_hostcall.c */
void hos_gpu_track(uint64_t guest, uint64_t size, int on);                          /* nvdrv.c */
void rx_nvn_pool_native(uint64_t gpu, uint64_t size, uint32_t flags, uint32_t claim, const char *by, int on);   /* rx_backend.c */

/* ---- calling convention: arguments from the guest registers ---- */
static void die(const char *fmt, ...);
/* (runtime) guest memory this layer reads or writes for the game: the n bytes at a, which must lie in the guest window
   (die otherwise). Objects and arrays are guest memory, so their addresses and sizes are checked before use. An
   object argument of no known size (P) is checked for its start (NVN_OBJ_MIN bytes): the window's top region ends at
   the window's end, so an object there may end there too; arrays and objects of known size are checked whole. */
#define NVN_OBJ_MIN 1u
static void *gw(uint64_t a, uint64_t n) {
    void *p = hos_guest_range(a, n);
    if (!p) die("guest memory %#llx + %#llx is outside the guest window", (unsigned long long)a, (unsigned long long)n);
    return p;
}
/* (runtime) a host pointer read from guest memory (a pool or texture an object names): it must point into the window */
static const void *in_window(const void *p, size_t n) {
    if (!p) return NULL;
    const uintptr_t q = (uintptr_t)p, base = (uintptr_t)A64_MEM;
    if (q < base || !hos_guest_range(q - base, n)) die("object pointer %p is outside the guest window", p);
    return p;
}
#define POOL(p) ((const nvn_pool *)in_window((p), sizeof(nvn_pool)))
#define P(i) ((void *)(c->x[i] ? gw(c->x[i], NVN_OBJ_MIN) : NULL))   /* host pointer of a guest pointer argument */
#define PN(i, n) ((void *)(c->x[i] ? gw(c->x[i], (n)) : NULL))     /* the same for an array of n bytes */
#define I(i) ((int)c->x[i])
#define U32(i) ((uint32_t)c->x[i])
#define U64(i) (c->x[i])
#define B(i) ((int)(c->x[i] & 0xff))                   /* NVNboolean: callers do not extend it */
static float fl(const Ctx *c, int i) { float f; memcpy(&f, &c->vreg[i][0], 4); return f; }
#define F(i) fl(c, i)                                  /* the i-th float argument (v0-v7) */
static void retf(Ctx *c, float f) { memset(c->vreg[0], 0, 16); memcpy(&c->vreg[0][0], &f, 4); }
static uint64_t h2g(const void *p) { return p ? (uint64_t)((const uint8_t *)p - A64_MEM) : 0; }
static void *g2h(uint64_t a) { return a ? gw(a, NVN_OBJ_MIN) : NULL; }
#define RETP(p) RET(h2g(p))

static int nvn_log_on(void) { static int v = -1; if (v < 0) v = setting_bool(SET_NVN_LOG_OBJECTS); return v; }
#define OLOG(...) do { if (nvn_log_on()) hos_log("nvn: " __VA_ARGS__); } while (0)
static void die(const char *fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    hos_fatal(NULL, "native NVN: %s", b);
}

/* ---- serials, GPU addresses, virtual pools ---- */
static volatile int64_t serials;
static uint64_t new_serial(void) { return (uint64_t)os_atomic_inc(&serials); }

#define VPAGE 0x10000ull                     /* NVN virtual mapping granularity (64 KiB) */
#define VBASE 0x10000000000ull               /* virtual pools' GPU addresses start at 1 TB: past the 512 GB guest window */
/* A virtual pool exists only as a claim of render-target storage (NVN_CLAIM_TARGETS): its GPU range is an address space
   the textures' images are keyed by, with no page table and no guest memory behind it. Nothing reads or writes it. */
static uint64_t next_vbase = VBASE;
static os_mutex vbase_lock = OS_MUTEX_INIT;
/* (rt_slots.c) GPU addresses left unused after each virtual pool: the transient targets' slots lie after the end of the
   pool of the game's DynamicTextureAllocator, as offsets past its size, so that the game's arenas keep the whole pool
   for their bookkeeping and the slots can be as large as the targets need */
uint64_t nvn_virtual_pool_tail(void) { return 2ull << 30; }

/* ---- pool claims (nvn_claims.h) ---- */
enum { CLAIM_DEPTH = 8 };
static _Thread_local struct {
    int depth;
    struct { int kind; nvn_claim_rule rule; const char *by; } at[CLAIM_DEPTH];
} tl_claim;
void nvn_claim_begin(int kind, const char *by) {
    if (tl_claim.depth < CLAIM_DEPTH) tl_claim.at[tl_claim.depth].kind = kind, tl_claim.at[tl_claim.depth].rule = NULL, tl_claim.at[tl_claim.depth].by = by;
    tl_claim.depth++;
}
void nvn_claim_begin_rule(nvn_claim_rule rule, const char *by) {
    if (tl_claim.depth < CLAIM_DEPTH) tl_claim.at[tl_claim.depth].kind = NVN_CLAIM_NONE, tl_claim.at[tl_claim.depth].rule = rule, tl_claim.at[tl_claim.depth].by = by;
    tl_claim.depth++;
}
void nvn_claim_end(void) {
    if (tl_claim.depth > 0) tl_claim.depth--;
}
/* The claim of a pool made now: the innermost kind a patch named, else the outermost rule's; by: its patch */
static uint32_t claim_now(uint32_t flags, uint64_t size, const char **by) {
    const int n = tl_claim.depth < CLAIM_DEPTH ? tl_claim.depth : CLAIM_DEPTH;
    *by = NULL;
    for (int i = n - 1; i >= 0; i--)
        if (tl_claim.at[i].kind != NVN_CLAIM_NONE) return *by = tl_claim.at[i].by, (uint32_t)tl_claim.at[i].kind;
    for (int i = 0; i < n; i++)
        if (tl_claim.at[i].rule) return *by = tl_claim.at[i].by, (uint32_t)tl_claim.at[i].rule(flags, size);
    return NVN_CLAIM_NONE;
}
void poolcensus_reported(uint64_t addr, uint64_t size);   /* poolcensus.c */
void nvn_claim_write(uint64_t addr, uint64_t size) {
    poolcensus_reported(addr, size);
    const gpu_fns *d = rx_direct();
    if (d) d->claim_write(addr, size);
}
void nvn_claim_fill(uint64_t addr, uint64_t size, uint32_t value) {
    poolcensus_reported(addr, size);
    const gpu_fns *d = rx_direct();
    if (d) d->claim_fill(addr, size, value);
}
void nvn_claim_read(uint64_t addr, uint64_t size) {
    const gpu_fns *d = rx_direct();
    if (d) d->claim_read(addr, size);
}
static const char *const kClaimNames[NVN_CLAIM_KINDS] = {"none", "textures", "targets", "host", "vram", "upload", "readback", "load once", "layer 0"};
/* An NVN memory call the back end does not serve: one loud error with the guest's call chain, and a strict event */
static void guest_chain(Ctx *c, char *b, size_t cap);
static void pool_error(Ctx *c, const char *reason, const char *fmt, ...) {
    char what[300], chain[400], line[800];
    va_list a;
    va_start(a, fmt);
    vsnprintf(what, sizeof what, fmt, a);
    va_end(a);
    guest_chain(c, chain, sizeof chain);
    snprintf(line, sizeof line, "%s, called from %s", what, chain);
    hos_log("nvn: ERROR: %s: %s", reason, line);
    const gpu_fns *d = rx_direct();
    if (d && d->host_event) d->host_event(reason, line);
}

uint64_t nvn_gpu_guest(uint64_t gpu) {
    /* (mkw-recomp: nothing is mapped below the system region at 16 MB: a small address is an unbound buffer, which
       TOTK's fully committed 4 GB window read as zeros and MKW's sparse window faults on). A virtual pool's address has
       no guest memory */
    return gpu >= HOS_SYSTEM_BASE && gpu < HOS_SPACE_END ? gpu : 0;
}
uint8_t *nvn_gpu_host(uint64_t gpu) {
    const uint64_t g = nvn_gpu_guest(gpu);
    return g ? (uint8_t *)GP(g) : NULL;
}
uint64_t nvn_gpu_contiguous(uint64_t gpu, uint64_t n) {   /* (runtime: up to the guest window's end, not past it) */
    if (gpu < HOS_SYSTEM_BASE || gpu >= HOS_SPACE_END) return 0;
    return n < HOS_SPACE_END - gpu ? n : HOS_SPACE_END - gpu;
}
int nvn_gpu_read(uint64_t gpu, void *dst, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        const uint64_t k = nvn_gpu_contiguous(gpu, n);
        const uint8_t *s = k ? nvn_gpu_host(gpu) : NULL;
        if (!s) return 0;
        memcpy(d, s, k);
        d += k, gpu += k, n -= k;
    }
    return 1;
}
int nvn_gpu_write(uint64_t gpu, const void *src, uint64_t n) {
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        const uint64_t k = nvn_gpu_contiguous(gpu, n);
        uint8_t *d = k ? nvn_gpu_host(gpu) : NULL;
        if (!d) return 0;
        memcpy(d, s, k);
        s += k, gpu += k, n -= k;
    }
    return 1;
}

/* ---- device ---- */
static volatile int32_t depth_minus_w = 1;   /* (OoT's mode from the start: the shaders prepared before the game made its device were
                                            translated without the depth conversion, and the menu's text and logo fell outside the depth range
                                            on a first start. d3zessete, PR 54) */
int nvn_depth_minus_w(void) { return depth_minus_w != 0; }
uint64_t nvn_native_proc(const char *name);

NV(nvnDeviceBuilderSetDefaults) { memset(P(0), 0, sizeof(nvn_dev_builder)); }
NV(nvnDeviceBuilderSetFlags) { ((nvn_dev_builder *)P(0))->flags = U32(1); }
NV(nvnDeviceInitialize) {
    nvn_dev *o = (nvn_dev *)P(0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('D', 'E', 'V', 0);
    o->flags = ((const nvn_dev_builder *)P(1))->flags;
    hos_log("nvn: native NVN device (flags %#x): no Switch GPU emulation", o->flags);
    RET(1);
}
NV(nvnDeviceFinalize) { ((nvn_dev *)P(0))->magic = 0; }
NV(nvnDeviceGetProcAddress) { RET(U64(1) ? nvn_native_proc((const char *)GP(U64(1))) : 0); }
/* (runtime) API version 231.40: OoT (SDK 22.2.8) skips its graphics setup unless the minor is >= 40 (main
   0x8348c11c); MKW's driver said 39, which MKW accepts too. */
/* The Switch 2 NVN2 driver's device limits: its nvnDeviceGetInteger (sdk+0x65b680) run for every id under
   Unicorn with a zeroed device. The few it reads from device state (0 there) keep NVN1's value (TOTKPort). */
static const int devinfo[] = {
    [0x00] = 231, [0x01] = 40, [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 32, [0x07] = 4,
    [0x08] = 16, [0x09] = 32, [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 32, [0x0F] = 32,
    [0x10] = 32, [0x11] = 256, [0x12] = 256, [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2,
    [0x18] = 256, [0x19] = 4096, [0x1A] = 16384, [0x1B] = 16, [0x1C] = 1024, [0x1D] = 1024, [0x1E] = 64,
    [0x1F] = 1024, [0x20] = 2147483647, [0x21] = 65535, [0x22] = 65535, [0x23] = 8, [0x24] = 1048576,
    [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 262144, [0x2C] = 16,
    [0x2D] = 2048, [0x2E] = 201, [0x2F] = 201, [0x30] = 20, [0x31] = 5, [0x32] = 1, [0x33] = 8, [0x34] = 8,
    [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1,
    [0x44] = 1, [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1,
    [0x4D] = 16, [0x4E] = 2048, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256,
    [0x54] = 16384, [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096,
    [0x5C] = 16384, [0x5D] = 4096, [0x5E] = 128, [0x5F] = 32, [0x60] = 65535, [0x61] = 1, [0x62] = 0, [0x63] = 256,
    [0x64] = 16, [0x65] = 65535, [0x66] = 65535, [0x67] = 256, [0x68] = 1, [0x69] = 16, [0x6A] = 16, [0x6B] = 16,
    [0x6C] = 1, [0x6D] = 1, [0x6E] = 1, [0x6F] = 256, [0x70] = 0, [0x71] = 16, [0x72] = 8, [0x73] = 16,
    [0x74] = 32, [0x75] = 31, [0x76] = 536870912, [0x77] = 16777216, [0x78] = 16777216, [0x79] = 1073741824,
    [0x7A] = 32, [0x7B] = 1, [0x7C] = 64, [0x7D] = 0, [0x7E] = 1, [0x7F] = 2147483647, [0x80] = 32, [0x81] = 32,
    [0x82] = 4194303, [0x83] = 512, [0x84] = 256, [0x85] = 32, [0x86] = 16383, [0x87] = 0, [0x88] = 8, [0x89] = 0,
    [0x8A] = 0, [0x8B] = 4, [0x8C] = 256, [0x8D] = 16, [0x8E] = 0, [0x8F] = 1, [0x90] = 31, [0x91] = 0,
    [0x92] = 64, [0x93] = 1, [0x94] = 58368, [0x95] = 24576, [0x96] = 0, [0x97] = 256, [0x98] = 262144, [0x99] = 0,
    [0x9A] = 0, [0x9B] = 0, [0x9C] = 201, [0x9D] = 14, [0x9E] = 201, [0x9F] = 20, [0xA0] = 163840, [0xA1] = 8,
    [0xA2] = 4, [0xA3] = 256, [0xA4] = 256, [0xA5] = 4, [0xA6] = 65536, [0xA7] = 201, [0xA8] = 5, [0xA9] = 1,
    [0xAA] = 0, [0xAB] = 63, [0xAC] = 1, [0xAD] = 0, [0xAE] = 1, [0xAF] = 0, [0xB0] = 4096, [0xB1] = 32768,
    [0xB2] = 0, [0xB3] = 0, [0xB4] = 0, [0xB5] = 0, [0xB6] = 0, [0xB7] = 0, [0xB8] = 0, [0xB9] = 0, [0xBA] = 0,
    [0xBB] = 0, [0xBC] = 0, [0xBD] = 0, [0xBE] = 0, [0xBF] = 0,
};
NV(nvnDeviceGetInteger) {
    const unsigned w = U32(1);
    if (w >= sizeof devinfo / sizeof *devinfo) die("device property %#x", w);
    static int devinfo_log = -1;
    if (devinfo_log < 0) devinfo_log = setting_bool(SET_NVN_LOG_DEVINFO);
    if (devinfo_log) hos_log("nvn: nvnDeviceGetInteger(%u) -> %d", w, devinfo[w]);
    *(int *)P(2) = devinfo[w];
}
NV(nvnDeviceRegisterFastClearColor) { RET(0); }   /* "not registered": the game keeps its normal clears */
NV(nvnDeviceRegisterFastClearColori) { RET(0); }
NV(nvnDeviceRegisterFastClearColorui) { RET(0); }
NV(nvnDeviceRegisterFastClearDepth) { RET(0); }
NV(nvnDeviceGetImageHandle) { RET((uint64_t)I(1)); }
/* combined handle = texture id (20 bits) | sampler id << 20: the Maxwell layout, which the shaders' handle reads expect */
NV(nvnDeviceGetTextureHandle) {
    const int t = I(1), s = I(2);
    /* The SDK sign-extends both ids before combining them, and texture 0 always means no handle.
       A missing texture can have id -1 (Castle Town's guard house): it stays the all-ones sentinel,
       rather than stopping the game here. Keep the signed ids, including their upper bits. */
    RET(t ? (uint64_t)t | (uint64_t)s << 20 : 0);
}
NV(nvnDeviceGetTexelFetchHandle) { RET((uint64_t)I(1)); }
NV(nvnDeviceGetSeparateTextureHandle) { RET((uint64_t)I(1)); }
NV(nvnDeviceGetSeparateSamplerHandle) { RET((uint64_t)I(1) << 20); }
NV(nvnDeviceInstallDebugCallback) {}
NV(nvnDeviceSetWindowOriginMode) {
    ((nvn_dev *)P(0))->origin = U32(1);
    hos_log("nvn: window origin %s", U32(1) ? "upper left" : "lower left");
}
NV(nvnDeviceSetDepthMode) {
    ((nvn_dev *)P(0))->depth_mode = U32(1);
    const int32_t was = os_atomic_xchg(&depth_minus_w, U32(1) == NVN_DEPTH_MODE_NEAR_IS_MINUS_W);
    hos_log("nvn: depth mode %s", U32(1) == NVN_DEPTH_MODE_NEAR_IS_MINUS_W ? "near = -w (GL)" : "near = 0");
    if (was != (int32_t)(depth_minus_w != 0))
        hos_log("nvn: the depth mode is not the one the prepared shaders were made for: their vertex stages draw wrong");
}
NV(nvnDeviceFinalizeCommandHandle) {}
/* (mkw-recomp) GPU time on the CPU's tick clock (31.25 MHz: 32 ns a tick), as on the console: MKW's frame pacer
   (0x806bced0) takes a timestamp report, ConvertToTick's it and subtracts its own GetSystemTick of the frame's start.
   From hos_now_ns (since boot, where the ticks count from the start) that came to the PC's uptime: always late, the
   game stayed at 30 fps */
NV(nvnDeviceGetCurrentTimestampInNanoseconds) { RET(hos_ticks() * (1000000000ull / HOS_TICK_HZ)); }
/* counters hold Tegra GPU ticks (19.2 MHz): ns = ticks * 625 / 12 */
NV(nvnDeviceGetTimestampInNanoseconds) {
    const uint64_t t = ((const NVNcounterData *)P(1))->timestamp;
    RET(t / 12 * 625 + t % 12 * 625 / 12);
}
NV(nvnDeviceSetDebugLabel) {}
NV(nvnDeviceIsExternalDebuggerAttached) { RET(0); }
NV(nvnDeviceSetIntermediateShaderCache) {}

/* ---- memory pools ---- */
NV(nvnMemoryPoolBuilderSetDefaults) { memset(P(0), 0, sizeof(nvn_pool_builder)); }
NV(nvnMemoryPoolBuilderSetDevice) { ((nvn_pool_builder *)P(0))->dev = P(1); }
NV(nvnMemoryPoolBuilderSetStorage) { ((nvn_pool_builder *)P(0))->mem = U64(1); ((nvn_pool_builder *)P(0))->size = U64(2); }
NV(nvnMemoryPoolBuilderSetFlags) { ((nvn_pool_builder *)P(0))->flags = U32(1); }
NV(nvnMemoryPoolInitialize,
   reads(x29, "the frame-pointer chain: the error of a pool no claim covers, and nvn.log_pools' line")) {
    const nvn_pool_builder *b = (const nvn_pool_builder *)P(1);
    nvn_pool *o = (nvn_pool *)P(0);
    memset(o, 0, sizeof *o);
    o->flags = b->flags;
    o->size = b->size;
    const char *by;
    o->claim = claim_now(o->flags, o->size, &by);
    if (o->claim == NVN_CLAIM_NONE || o->claim >= NVN_CLAIM_KINDS) {
        pool_error(c, "a memory pool no claim covers", "nvnMemoryPoolInitialize flags %#x size %#llx", o->flags,
                   (unsigned long long)o->size);
        RET(0);
        return;
    }
    if ((o->flags & (NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_PHYSICAL)) && o->claim != NVN_CLAIM_TARGETS) {
        pool_error(c, "a virtual or physical memory pool not claimed as render-target storage",
                   "nvnMemoryPoolInitialize flags %#x size %#llx, claimed %s by %s", o->flags, (unsigned long long)o->size,
                   kClaimNames[o->claim], by);
        RET(0);
        return;
    }
    o->magic = NVN_MAGIC('P', 'O', 'O', 'L');
    if (o->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) {   /* (an address range only: NVN_CLAIM_TARGETS) */
        if (b->mem || !o->size || o->size % VPAGE || o->size >= VBASE)
            die("virtual pool storage %#llx size %#llx", (unsigned long long)b->mem, (unsigned long long)o->size);
        os_mutex_lock(&vbase_lock);
        o->gpu = next_vbase;
        next_vbase += o->size + nvn_virtual_pool_tail();
        os_mutex_unlock(&vbase_lock);
        hos_log("nvn: virtual pool %#llx bytes at GPU %#llx: render-target storage (claimed by %s)", (unsigned long long)o->size,
                (unsigned long long)o->gpu, by ? by : "?");
        RET(1);
        return;
    }
    if (!b->mem) die("memory pool without storage (flags %#x size %#llx)", o->flags, (unsigned long long)o->size);
    gw(b->mem, o->size);   /* (runtime: the storage lies in guest memory) */
    o->mem = b->mem;
    o->gpu = b->mem;
    hos_gpu_track(o->gpu, o->size, 1);
    if (setting_bool(SET_NVN_LOG_POOLS)) {   /* (debugging) who makes each pool */
        char b[400];
        guest_chain(c, b, sizeof b);
        hos_log("nvn: pool %#llx size %#llx flags %#x claimed %s by %s, made by %s", (unsigned long long)o->gpu,
                (unsigned long long)o->size, o->flags, kClaimNames[o->claim], by, b);
    }
    rx_nvn_pool_native(o->gpu, o->size, o->flags, o->claim, by, 1);   /* (the back end serves it by its claim, before any binding) */
    rx_nvn_pool_created(o->gpu, o->size);
    void poolcensus_pool(Ctx *c, uint64_t gpu, uint64_t size, uint32_t flags, uint32_t claim, const char *by);   /* poolcensus.c */
    poolcensus_pool(c, o->gpu, o->size, o->flags, o->claim, by);
    OLOG("pool %#llx size %#llx flags %#x", (unsigned long long)o->gpu, (unsigned long long)o->size, o->flags);
    RET(1);
}
/* (runtime) A memory-pool call made with no pool: the game passed 0 while the banc changed (thread
   OptimizeChangeBanc, Kakariko's day/night transition), and flags at +4 of it crashed. 0 then, and who asked, once.
   P(0) returns NULL for a 0 argument, but dereferencing that NULL is undefined behaviour: the compiler folds the
   check away and reads A64_MEM + offset instead, so the guard has to come before the dereference. */
static int no_pool(Ctx *c, const char *what) {
    if (c->x[0]) return 0;
    static int told;
    if (told++ < 4) hos_log("nvn: %s of no memory pool (lr %#llx): 0", what, (unsigned long long)c->x[30]);
    return 1;
}
NV(nvnMemoryPoolFinalize) {
    if (no_pool(c, "finalize")) return;
    nvn_pool *o = (nvn_pool *)P(0);
    if (o->mem && o->magic == NVN_MAGIC('P', 'O', 'O', 'L')) {   /* (a virtual pool's GPU range is not reused) */
        void poolcensus_pool_gone(uint64_t gpu, uint64_t size);   /* poolcensus.c */
        poolcensus_pool_gone(o->gpu, o->size);
        rx_nvn_pool_native(o->gpu, o->size, o->flags, o->claim, NULL, 0);
        hos_gpu_track(o->gpu, o->size, 0);
    }
    o->magic = 0;
}
NV(nvnMemoryPoolGetFlags) {
    if (no_pool(c, "flags")) { RET(0); return; }
    RET(((const nvn_pool *)P(0))->flags);
}
NV(nvnMemoryPoolGetSize) {
    if (no_pool(c, "size")) { RET(0); return; }
    RET(((const nvn_pool *)P(0))->size);
}
NV(nvnMemoryPoolMap) {
    if (no_pool(c, "map")) { RET(0); return; }
    const nvn_pool *o = (const nvn_pool *)P(0);
    if (o->magic != NVN_MAGIC('P', 'O', 'O', 'L') || (o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) { RET(0); return; }
    if (!o->mem) die("CPU mapping of a virtual pool");
    RET(o->mem);
}
NV(nvnMemoryPoolGetBufferAddress) {
    if (no_pool(c, "buffer address")) { RET(0); return; }
    RET(((const nvn_pool *)P(0))->gpu);
}
NV(nvnMemoryPoolFlushMappedRange) {}
NV(nvnMemoryPoolInvalidateMappedRange) {   /* the CPU is about to read what the GPU wrote */
    if (no_pool(c, "invalidate mapped range")) return;
    const nvn_pool *o = (const nvn_pool *)P(0);
    if (game_invalidate_unneeded(U64(2))) return;   /* (a readback the game can do without this frame) */
    if ((int64_t)U64(1) >= 0) rx_nvn_invalidate(o->gpu + U64(1), U64(2));
}
NV(nvnMemoryPoolSetDebugLabel) {}
/* nvnMemoryPoolMapVirtual(pool, n, NVNmappingRequest[n]): accepted, with nothing to do, only on a virtual pool claimed
   as render-target storage (nothing reads its pages); anything else is an error */
NV(nvnMemoryPoolMapVirtual, reads(x29, "named in the error of a mapping no claim covers")) {
    if (no_pool(c, "map virtual")) return;
    const nvn_pool *v = (const nvn_pool *)P(0);
    if (v->magic != NVN_MAGIC('P', 'O', 'O', 'L') || !(v->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || v->claim != NVN_CLAIM_TARGETS) {
        pool_error(c, "nvnMemoryPoolMapVirtual on a pool no claim covers", "pool flags %#x size %#llx, %d requests", v->flags,
                   (unsigned long long)v->size, I(1));
        RET(0);
        return;
    }
    RET(1);
}

/* ---- buffers (a range of a pool) ---- */
NV(nvnBufferBuilderSetDevice) { ((nvn_buffer_builder *)P(0))->dev = P(1); }
NV(nvnBufferBuilderSetDefaults) {
    nvn_buffer_builder *b = (nvn_buffer_builder *)P(0);
    void *d = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = d;
}
NV(nvnBufferBuilderSetStorage) {
    nvn_buffer_builder *b = (nvn_buffer_builder *)P(0);
    b->pool = (nvn_pool *)P(1);
    b->off = (int64_t)U64(2);
    b->size = U64(3);
}
NV(nvnBufferInitialize) {
    const nvn_buffer_builder *b = (const nvn_buffer_builder *)P(1);
    const nvn_pool *bp = POOL(b->pool);
    if (!bp || bp->magic != NVN_MAGIC('P', 'O', 'O', 'L') || b->off < 0 || (uint64_t)b->off > bp->size ||
        b->size > bp->size - (uint64_t)b->off) { RET(0); return; }
    nvn_buffer *o = (nvn_buffer *)P(0);
    o->magic = NVN_MAGIC('B', 'U', 'F', 0);
    o->pool = b->pool;
    o->off = b->off;
    o->size = b->size;
    RET(1);
}
NV(nvnBufferFinalize) { memset(P(0), 0, sizeof(nvn_buffer)); }
/* (runtime, issue 287) A buffer with no memory pool, mapped: the game filled 0x3d0 bytes at what it got back, 0, and crashed
   on a scene change (Hyrule Castle / Castle Town / Hyrule Field, Kakariko day to night; thread OptimizeChangeBanc, right after
   "flags of no memory pool"). Zeroed scratch memory in the game's address space instead, as large as the biggest asked for
   (never freed: it comes rarely), and who asked, once. */
static uint64_t scratch_map(uint64_t size, uint64_t lr) {
    static os_mutex lock = OS_MUTEX_INIT;
    static uint64_t at, have;
    static int told;
    os_mutex_lock(&lock);
    if (size > have) {
        have = (size + 0xffff) & ~0xffffull;
        at = hos_sys_alloc(have, 0x1000);
    }
    const uint64_t r = at;
    if (told++ < 4) hos_log("nvn: a buffer of no memory pool mapped (lr %#llx): %#llx bytes of scratch", (unsigned long long)lr,
                            (unsigned long long)size);
    os_mutex_unlock(&lock);
    return r;
}
NV(nvnBufferMap) {
    const nvn_buffer *o = (const nvn_buffer *)P(0);
    if (!o) { RET(0); return; }
    if (!o->pool) { RET(scratch_map(o->size ? o->size : 0x1000, c->x[30])); return; }
    const nvn_pool *pool = POOL(o->pool);
    if (!pool->mem) { RET(0); return; }
    RET(pool->mem + (uint64_t)o->off);
}
NV(nvnBufferGetAddress) {
    const nvn_buffer *o = (const nvn_buffer *)P(0);
    RET(o->pool ? POOL(o->pool)->gpu + (uint64_t)o->off : 0);
}
NV(nvnBufferGetMemoryPool) { RETP(((const nvn_buffer *)P(0))->pool); }
NV(nvnBufferGetMemoryOffset) { RET((uint64_t)((const nvn_buffer *)P(0))->off); }
NV(nvnBufferGetSize) { RET(((const nvn_buffer *)P(0))->size); }
NV(nvnBufferFlushMappedRange) {}
NV(nvnBufferInvalidateMappedRange) {
    const nvn_buffer *o = (const nvn_buffer *)P(0);
    if (o->pool) rx_nvn_invalidate(POOL(o->pool)->gpu + (uint64_t)o->off + U64(1), U64(2));
}
NV(nvnBufferSetDebugLabel) {}

/* ---- descriptor pools ---- */
NV(nvnTexturePoolInitialize) {
    nvn_dpool *o = (nvn_dpool *)P(0);
    o->pool = (const nvn_pool *)P(1);
    o->off = (int64_t)U64(2);
    o->n = I(3);
    rx_nvn_set_texture_pool_size(o->n);
    hos_log("nvn: texture pool of %d descriptors", o->n);
    RET(1);
}
NV(nvnTexturePoolFinalize) {}
NV(nvnTexturePoolSetDebugLabel) {}
NV(nvnTexturePoolRegisterTexture) { rx_nvn_register_texture(I(1), (const nvn_tex *)P(2), (const nvn_view *)P(3)); }
NV(nvnSamplerPoolInitialize) {
    nvn_dpool *o = (nvn_dpool *)P(0);
    o->pool = (const nvn_pool *)P(1);
    o->off = (int64_t)U64(2);
    o->n = I(3);
    /* (the renderer's sampler heap holds 4,096 ids a region: a handle's sampler id is its 12 high bits) */
    hos_log("nvn: sampler pool of %d descriptors%s", o->n, o->n > 4096 ? " (over the 4,096 a handle can name)" : "");
    RET(1);
}
NV(nvnSamplerPoolFinalize) {}
NV(nvnSamplerPoolSetDebugLabel) {}
NV(nvnSamplerPoolRegisterSampler) { rx_nvn_register_sampler(I(1), &((const nvn_samp *)P(2))->p); }

/* ---- textures ---- */
/* (runtime) A texture builder of no address: a scratch one instead, said once. The game passed 0 after a long freeze (the
   ocarina note sheet's first build, combat in Hyrule Field) and nvnTextureBuilderSetSwizzle read A64_MEM + 0x5c (issue 500);
   a builder set up wrongly is a missing texture, not a crash. As PR 510 for memory pools. */
static nvn_tex_builder *tb_or_scratch(void *p, uint64_t lr) {
    static nvn_tex_builder scratch;
    if (p) return (nvn_tex_builder *)p;
    static volatile int32_t told;
    if (os_atomic_inc(&told) <= 4) hos_log("nvn: a texture builder of no address (lr %#llx): a scratch one", (unsigned long long)lr);
    memset(&scratch, 0, sizeof scratch);
    return &scratch;
}
#define TB(p) tb_or_scratch((p), c->x[30])
#define CTB(p) ((const nvn_tex_builder *)tb_or_scratch((p), c->x[30]))
#define TX(p) ((nvn_tex *)(p))
#define CTX(p) ((const nvn_tex *)(p))
NV(nvnTextureBuilderSetDefaults) {
    nvn_tex_builder *b = TB(P(0));
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->d.swizzle_r = 2, b->d.swizzle_g = 3, b->d.swizzle_b = 4, b->d.swizzle_a = 5;
    b->d.target = NVN_TEXTURE_TARGET_2D;
    b->d.width = b->d.height = b->d.depth = b->d.levels = 1;
}
NV(nvnTextureBuilderSetDevice) { TB(P(0))->dev = P(1); }
NV(nvnTextureBuilderSetFlags) { TB(P(0))->d.flags = I(1); }
NV(nvnTextureBuilderSetTarget) { TB(P(0))->d.target = I(1); }
NV(nvnTextureBuilderSetWidth) { TB(P(0))->d.width = U32(1); }
NV(nvnTextureBuilderSetHeight) { TB(P(0))->d.height = U32(1); }
NV(nvnTextureBuilderSetDepth) { TB(P(0))->d.depth = U32(1); }
NV(nvnTextureBuilderSetSize1D) { nvn_tex_builder *b = TB(P(0)); b->d.width = U32(1); b->d.height = b->d.depth = 1; }
NV(nvnTextureBuilderSetSize2D) {
    nvn_tex_builder *b = TB(P(0));
    b->d.width = U32(1), b->d.height = U32(2), b->d.depth = 1;
    {   /* (runtime) nvn.log_sizes: who asks for the picture's sizes (the output and the scene: for wider screens) */
        static int on = -1;
        static volatile int32_t left = 24;
        if (on < 0) on = setting_bool(SET_NVN_LOG_SIZES);
        const uint32_t w = U32(1);
        if (on && (w == 2560 || w == 2048 || w == 1536 || w == 3840 || w == 1920) && os_atomic_dec(&left) >= 0) {
            void hos_backtrace(void);
            hos_log("nvn: texture size %ux%u (lr %#llx)", w, U32(2), (unsigned long long)c->x[30]);
            hos_backtrace();
        }
    }
}
NV(nvnTextureBuilderSetSize3D) { nvn_tex_builder *b = TB(P(0)); b->d.width = U32(1); b->d.height = U32(2); b->d.depth = U32(3); }
NV(nvnTextureBuilderSetLevels) { TB(P(0))->d.levels = U32(1); }
NV(nvnTextureBuilderSetFormat) { TB(P(0))->d.format = I(1); }
NV(nvnTextureBuilderSetDepthStencilMode) {
    if (I(1) != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH) die("stencil-plane texture sampling");
}
NV(nvnTextureBuilderSetSwizzle) {
    nvn_tex_builder *b = TB(P(0));
    for (int i = 1; i <= 4; i++) if (U32(i) > 5) die("texture swizzle %u", U32(i));
    b->d.swizzle_r = U32(1), b->d.swizzle_g = U32(2), b->d.swizzle_b = U32(3), b->d.swizzle_a = U32(4);
}
NV(nvnTextureBuilderSetSamples) { TB(P(0))->d.samples = I(1); }
NV(nvnTextureBuilderSetStride) { TB(P(0))->d.stride = (uint32_t)U64(1); }
NV(nvnTextureBuilderSetStorage) { TB(P(0))->pool = (nvn_pool *)P(1); TB(P(0))->off = (int64_t)U64(2); }
NV(nvnTextureBuilderSetPackagedTextureLayout) {
    nvn_tex_builder *b = TB(P(0));
    const NVNpackagedTextureLayout *l = (const NVNpackagedTextureLayout *)P(1);
    b->has_layout = l != NULL;
    if (l) memcpy(b->layout, l->layout, 8);
    b->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
NV(nvnTextureBuilderSetPackagedTextureData) { TB(P(0))->pkg_data = U64(1); }
NV(nvnTextureBuilderGetPackagedTextureData) { RET(CTB(P(0))->pkg_data); }
/* storage classes are opaque identifiers here: 0 buffer textures, 1 the rest (not a Tegra memory kind) */
static int storage_class(const nvn_texdesc *d) { return d->target == NVN_TEXTURE_TARGET_BUFFER ? 0 : 1; }
NV(nvnTextureBuilderGetStorageClass) { RET(storage_class(&CTB(P(0))->d)); }
NV(nvnTextureBuilderGetFlags) { RET((uint32_t)CTB(P(0))->d.flags); }
/* (runtime, issues 284 and 287) A size or alignment asked of no builder: the game passed 0 here while a scene loaded (thread
   OptimizeChangeBanc, entering Castle Town), and the format read at +4 of it crashed the game. Size 0 then, and who asked, once */
static int no_builder(Ctx *c, const char *what) {
    if (c->x[0]) return 0;
    static int told;
    if (told++ < 4) hos_log("nvn: %s of no texture builder (lr %#llx): 0", what, (unsigned long long)c->x[30]);
    return 1;
}
NV(nvnTextureBuilderGetStorageSize) {
    if (no_builder(c, "storage size")) { RET(0); return; }
    nvn_texlayout L;
    nvn_tex_layout(&CTB(P(0))->d, &L);
    if (!L.bpb) hos_log("nvn: texture format %#x unknown: storage size 0", CTB(P(0))->d.format);
    RET(L.size);
}
NV(nvnTextureBuilderGetStorageAlignment) {
    if (no_builder(c, "storage alignment")) { RET(512); return; }
    nvn_texlayout L;
    nvn_tex_layout(&CTB(P(0))->d, &L);
    RET(L.align);
}
NV(nvnTextureBuilderGetZCullStorageSize) { RET(0); }   /* no ZCull: nothing to store */
NV(nvnTextureBuilderGetTarget) { RET((uint32_t)CTB(P(0))->d.target); }
NV(nvnTextureBuilderGetWidth) { RET(CTB(P(0))->d.width); }
NV(nvnTextureBuilderGetHeight) { RET(CTB(P(0))->d.height); }
NV(nvnTextureBuilderGetDepth) { RET(CTB(P(0))->d.depth); }
NV(nvnTextureBuilderGetLevels) { RET(CTB(P(0))->d.levels); }
NV(nvnTextureBuilderGetFormat) { RET((uint32_t)CTB(P(0))->d.format); }
NV(nvnTextureBuilderGetSamples) { RET((uint32_t)CTB(P(0))->d.samples); }
NV(nvnTextureBuilderGetStride) { RET(CTB(P(0))->d.stride); }
NV(nvnTextureBuilderGetMemoryOffset) { RET((uint64_t)CTB(P(0))->off); }
NV(nvnTextureBuilderGetViewOffset) {
    if (no_builder(c, "view offset")) { RET(0); return; }
    const nvn_tex_builder *b = CTB(P(0));
    const nvn_view *w = (const nvn_view *)P(1);
    if (!w) { RET(0); return; }
    nvn_texlayout L;
    nvn_tex_layout(&b->d, &L);
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&b->d, w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * L.layer_size;
    RET(off);
}

/* the texture currently initialised at each storage address (nvn_tex_live_at) */
#define LIVE_SLOTS 65536
static struct { uint64_t gpu; nvn_tex t; } live[LIVE_SLOTS];
static os_rwlock live_lock = OS_RWLOCK_INIT;
static unsigned live_slot(uint64_t gpu) { return (unsigned)((gpu >> 9) * 0x9E3779B97F4A7C15ull >> 48) & (LIVE_SLOTS - 1); }
static void live_put(const nvn_tex *t) {
    if (!t->gpu) return;
    os_rwlock_lock(&live_lock);
    unsigned s = live_slot(t->gpu), k;
    for (k = 0; k < 64; k++) {   /* (open addressing over a short run: a stale entry there is simply replaced) */
        const unsigned j = (s + k) & (LIVE_SLOTS - 1);
        if (live[j].gpu == t->gpu || !live[j].gpu) {
            live[j].gpu = t->gpu;
            live[j].t = *t;
            break;
        }
    }
    if (k == 64) {
        live[s].gpu = t->gpu;
        live[s].t = *t;
    }
    os_rwlock_unlock(&live_lock);
}
static void live_del(const nvn_tex *t) {
    if (!t->gpu) return;
    os_rwlock_lock(&live_lock);
    const unsigned s = live_slot(t->gpu);
    for (unsigned k = 0; k < 64; k++) {
        const unsigned j = (s + k) & (LIVE_SLOTS - 1);
        if (live[j].gpu == t->gpu) {
            if (live[j].t.serial == t->serial) live[j].gpu = 0, live[j].t.serial = 0;
            break;
        }
    }
    os_rwlock_unlock(&live_lock);
}
int nvn_tex_live_at(uint64_t gpu, nvn_tex *out) {
    int found = 0;
    os_rwlock_lock_shared(&live_lock);
    const unsigned s = live_slot(gpu);
    for (unsigned k = 0; k < 64; k++) {
        const unsigned j = (s + k) & (LIVE_SLOTS - 1);
        if (live[j].gpu == gpu) {
            *out = live[j].t;
            found = 1;
            break;
        }
    }
    os_rwlock_unlock_shared(&live_lock);
    return found;
}
/* (games/oot/patches/assets.c) the live texture whose storage holds gpu (a walk of the table: rare calls) */
int nvn_tex_live_containing(uint64_t gpu, nvn_tex *out) {
    int found = 0;
    os_rwlock_lock_shared(&live_lock);
    for (unsigned j = 0; j < LIVE_SLOTS && !found; j++)
        if (live[j].gpu && gpu >= live[j].gpu && gpu < live[j].gpu + live[j].t.L.size) *out = live[j].t, found = 1;
    os_rwlock_unlock_shared(&live_lock);
    return found;
}

void hos_watch_guest_writes(const uint8_t *data, uint64_t size);   /* (below) */
volatile int64_t nvn_texture_inits;   /* (texture initializations, for the patches' logs) */
extern volatile int64_t hos_present_count;   /* display.c */
/* nvn.log_tex=<w>x<h> (debugging): each initialization and finalize of a texture that size, with its caller */
static int log_tex(const nvn_tex *o) {
    static int on = -1;
    static unsigned lw, lh;
    if (on < 0) on = sscanf(setting_str(SET_NVN_LOG_TEX), "%ux%u", &lw, &lh) == 2;
    return on && o->d.width == lw && o->d.height == lh;
}
static _Thread_local uint64_t tl_moved_serial;   /* (M6f: the serial a texture moved by compaction keeps) */
/* (the back end, at the texture's creation on this thread) the guest's call of nvnTextureInitialize: its return address
   and its caller's */
static _Thread_local uint64_t tl_init_site[2];
void nvn_texture_site(uint64_t out[2]) { out[0] = tl_init_site[0], out[1] = tl_init_site[1]; }
NV(nvnTextureInitialize, reads(x29, "the caller's frame: its return address names the texture's creator")) {
    os_atomic_inc(&nvn_texture_inits);
    const nvn_tex_builder *b = CTB(P(1));
    game_texture_initializing(c->x[0], b->pool ? POOL(b->pool)->gpu + (uint64_t)b->off : 0, &b->d, c->x[30]);
    nvn_tex *o = TX(P(0));
    if (o->magic == NVN_MAGIC('T', 'E', 'X', 0) && o->serial) {   /* initialised again without Finalize */
        if (rx_direct()) texwatch_released(o);
        live_del(o);
        rx_nvn_texture_released(o);
    }
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('T', 'E', 'X', 0);
    o->d = b->d;
    o->pool = b->pool;
    o->off = b->off;
    o->has_layout = b->has_layout;
    memcpy(o->layout, b->layout, 8);
    nvn_tex_layout(&o->d, &o->L);
    o->gpu = b->pool ? POOL(b->pool)->gpu + (uint64_t)b->off : 0;
    o->packaged = b->pkg_data != 0;   /* (never copied into the storage: the back end takes its texels from the store) */
    tl_init_site[0] = c->x[30];
    tl_init_site[1] = c->x[29] && !(c->x[29] & 7) && hos_guest_range(c->x[29] + 8, 8) ? G64(c->x[29] + 8) : 0;
    const int moved = tl_moved_serial && o == game_texture_moving();   /* (M6f: the second half of a move) */
    o->serial = moved ? tl_moved_serial : new_serial();
    tl_moved_serial = 0;
    live_put(o);
    if (!moved) rx_nvn_texture_created(o);
    game_texture_initialized(o);
    if (rx_direct()) texwatch_created(o, c->x[30]);   /* (M4d: debug.texwatch, diagnosis) */
    {   /* nvn.watch_tex_size=<w>x<h> (debugging): who writes the storage of textures that size (a page watch, with call
           stacks: hos_watch_guest_writes), for writes the dirty tracking misses */
        static int on = -1;
        static unsigned ww, wh;
        if (on < 0) on = sscanf(setting_str(SET_NVN_WATCH_TEX_SIZE), "%ux%u", &ww, &wh) == 2;
        if (on && o->d.width == ww && o->d.height == wh && o->gpu && o->gpu < VBASE)
            hos_watch_guest_writes(GP(o->gpu), o->L.size);
        static uint64_t at = 1;   /* nvn.watch_tex_at=<hex>: the texture at that address only */
        if (at == 1) at = strtoull(setting_str(SET_NVN_WATCH_TEX_AT), NULL, 16);
        if (at && o->gpu == at && o->gpu < VBASE) hos_watch_guest_writes(GP(o->gpu), o->L.size);
    }
    if (log_tex(o))
        hos_log("nvn: texture %#llx initialized: %ux%ux%u levels %u format %#x target %d at %#llx (pool %#llx + %#llx), serial %llu, "
                "lr %#llx, present %lld", (unsigned long long)c->x[0], o->d.width, o->d.height, o->d.depth, o->d.levels, o->d.format,
                o->d.target, (unsigned long long)o->gpu, (unsigned long long)(b->pool ? POOL(b->pool)->gpu : 0),
                (unsigned long long)o->off, (unsigned long long)o->serial, (unsigned long long)c->x[30], (long long)hos_present_count);
    OLOG("texture %ux%ux%u levels %u format %#x target %d flags %#x at %#llx size %#llx", o->d.width, o->d.height,
         o->d.depth, o->d.levels, o->d.format, o->d.target, o->d.flags, (unsigned long long)o->gpu,
         (unsigned long long)o->L.size);
    RET(1);
}
NV(nvnTextureFinalize) {
    nvn_tex *o = TX(P(0));
    if (rx_direct() && o == game_texture_moving() && o->magic == NVN_MAGIC('T', 'E', 'X', 0)) {
        /* a compaction move (texdata.c): the back end keeps the texture's image and registration; the
           initialization that follows over the new storage keeps its serial */
        tl_moved_serial = o->serial;
        live_del(o);
        o->magic = 0;
        return;
    }
    if (o->magic == NVN_MAGIC('T', 'E', 'X', 0) && log_tex(o))
        hos_log("nvn: texture %#llx finalized: %ux%u at %#llx, serial %llu, lr %#llx, present %lld", (unsigned long long)c->x[0],
                o->d.width, o->d.height, (unsigned long long)o->gpu, (unsigned long long)o->serial, (unsigned long long)c->x[30],
                (long long)hos_present_count);
    if (rx_direct()) texwatch_released(o);
    live_del(o);
    rx_nvn_texture_released(o);
    o->magic = 0;
}
NV(nvnTextureGetStorageClass) { RET(storage_class(&CTX(P(0))->d)); }
NV(nvnTextureGetFlags) { RET((uint32_t)CTX(P(0))->d.flags); }
NV(nvnTextureGetWidth) { RET(CTX(P(0))->d.width); }
NV(nvnTextureGetHeight) { RET(CTX(P(0))->d.height); }
NV(nvnTextureGetDepth) { RET(CTX(P(0))->d.depth); }
NV(nvnTextureGetLevels) { RET(CTX(P(0))->d.levels); }
NV(nvnTextureGetFormat) { RET((uint32_t)CTX(P(0))->d.format); }
NV(nvnTextureGetSamples) { RET((uint32_t)CTX(P(0))->d.samples); }
NV(nvnTextureGetTarget) { RET((uint32_t)CTX(P(0))->d.target); }
NV(nvnTextureGetStride) { RET(CTX(P(0))->d.stride); }
NV(nvnTextureGetStorageSize) { RET(CTX(P(0))->L.size); }
NV(nvnTextureGetZCullStorageSize) { RET(0); }
NV(nvnTextureGetTextureAddress) { RET(CTX(P(0))->gpu); }
NV(nvnTextureGetMemoryOffset) { RET((uint64_t)CTX(P(0))->off); }
NV(nvnTextureGetDebugID) { RET(CTX(P(0))->serial); }
NV(nvnTextureSetDebugLabel) {}
NV(nvnTextureGetSwizzle) {
    const nvn_texdesc *d = &CTX(P(0))->d;
    *(int *)P(1) = d->swizzle_r, *(int *)P(2) = d->swizzle_g, *(int *)P(3) = d->swizzle_b, *(int *)P(4) = d->swizzle_a;
}
NV(nvnTextureCompare) {
    const nvn_tex *x = CTX(P(0)), *y = CTX(P(1));
    RET(!memcmp(&x->d, &y->d, sizeof x->d) && x->gpu == y->gpu);
}
NV(nvnTextureGetRawStorageClass) { RET(1); }   /* (no prototype; callers compare it with 1) */
#define VW(p) ((nvn_view *)(p))
#define CVW(p) ((const nvn_view *)(p))
NV(nvnTextureViewSetDefaults) { memset(P(0), 0, sizeof(nvn_view)); }
NV(nvnTextureViewSetLevels) {
    if (U32(1) > 255 || U32(2) > 255) die("texture view levels %u/%u", U32(1), U32(2));
    nvn_view *v = VW(P(0));
    v->set_levels = 1, v->base_level = (uint8_t)U32(1), v->nlevels = (uint8_t)U32(2);
}
NV(nvnTextureViewSetLayers) {
    if (U32(1) > 65535 || U32(2) > 65535) die("texture view layers %u/%u", U32(1), U32(2));
    nvn_view *v = VW(P(0));
    v->set_layers = 1, v->min_layer = (uint16_t)U32(1), v->nlayers = (uint16_t)U32(2);
}
NV(nvnTextureViewSetFormat) { VW(P(0))->set_format = 1; VW(P(0))->format = (uint16_t)U32(1); }
NV(nvnTextureViewSetTarget) { VW(P(0))->set_target = 1; VW(P(0))->target = (uint8_t)U32(1); }
NV(nvnTextureViewSetSwizzle) {
    nvn_view *v = VW(P(0));
    for (int i = 1; i <= 4; i++) if (U32(i) > 5) die("texture view swizzle %u", U32(i));
    v->set_swizzle = 1;
    for (int i = 0; i < 4; i++) v->swz[i] = (uint8_t)U32(i + 1);
}
NV(nvnTextureViewSetDepthStencilMode) {
    if (I(1) != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH) die("stencil-plane texture view sampling");
}
NV(nvnTextureViewGetLevels) { const nvn_view *v = CVW(P(0)); *(int *)P(1) = v->base_level; *(int *)P(2) = v->nlevels; RET(v->set_levels); }
NV(nvnTextureViewGetLayers) { const nvn_view *v = CVW(P(0)); *(int *)P(1) = v->min_layer; *(int *)P(2) = v->nlayers; RET(v->set_layers); }
NV(nvnTextureViewGetFormat) { const nvn_view *v = CVW(P(0)); *(int *)P(1) = v->format; RET(v->set_format); }
NV(nvnTextureViewGetTarget) { const nvn_view *v = CVW(P(0)); *(int *)P(1) = v->target; RET(v->set_target); }
NV(nvnTextureGetViewOffset) {
    const nvn_tex *t = CTX(P(0));
    const nvn_view *w = CVW(P(1));
    if (!w) { RET(0); return; }
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&t->d, w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * t->L.layer_size;
    RET(off);
}
/* nvn.watch_texels=<w>x<h> (debugging): after each WriteTexels into a texture that size its source buffer's pages are made read-only, and the
   next write to them is logged with the writer's call stack (f_XXXXXXXX frames are guest code), then allowed */
static uintptr_t wts_lo[16], wts_hi[16];
static int wts_n;
static volatile int32_t wts_reports;
void hos_backtrace(void);
static int wts_handler(const os_fault *f) {
    if (f->kind != OS_FAULT_ACCESS || !f->write) return OS_FAULT_PASS;
    const uintptr_t a = f->addr;
    for (int i = 0; i < wts_n; i++)
        if (a >= wts_lo[i] && a < wts_hi[i]) {
            HosThread *th = hos_self();
            const char *tn = th ? hos_thread_name(th) : "?";
            static uint64_t after = ~0ull;   /* nvn.watch_after=<seconds>: only writes after that are reported */
            if (after == ~0ull) after = (uint64_t)(os_now_ns() / 1000000) + (uint64_t)setting_int(SET_NVN_WATCH_AFTER) * 1000;
            static int all_threads = -1;   /* nvn.watch_all: the Resource Memory thread's writes too */
            if (all_threads < 0) all_threads = setting_bool(SET_NVN_WATCH_ALL);
            if ((all_threads || strcmp(tn, "Resource Memory")) && (uint64_t)(os_now_ns() / 1000000) >= after && os_atomic_inc(&wts_reports) <= 12) {   /* (not the allocation's zero fill) */
                hos_log("nvn: write to a WriteTexels source at guest %#llx by %s (host pc exe+%#llx)", (unsigned long long)(a - (uintptr_t)GP(0)), tn,
                        (unsigned long long)(f->pc - os_exe_base()));
                hos_backtrace();
            }
            os_mem_protect((void *)(a & ~(uintptr_t)4095), 4096, OS_PROT_RW);
            return OS_FAULT_RESUME;
        }
    return OS_FAULT_PASS;
}
static void wts_rearm(void *p) {   /* (every 2 ms: the pages a write made writable are watched again) */
    (void)p;
    for (int k = 0; k < 300000 && wts_reports < 12; k++) {
        os_sleep_ns(2LL * 1000000);
        for (int i = 0; i < wts_n; i++) os_mem_protect((void *)wts_lo[i], wts_hi[i] - wts_lo[i], OS_PROT_R);
    }
}
/* (also for other debugging hooks: nn_image_debug.c) the pages of [data, data + size) watched as above */
void hos_watch_guest_writes(const uint8_t *data, uint64_t size) {
    const uintptr_t lo = (uintptr_t)data & ~(uintptr_t)4095, hi = ((uintptr_t)data + size + 4095) & ~(uintptr_t)4095;
    for (int i = 0; i < wts_n; i++) if (wts_lo[i] == lo) return;
    if (wts_n >= 16) return;
    static int handler;
    if (!handler++) {
        os_fault_add(wts_handler, true);
        os_thread_release(os_thread_start(wts_rearm, NULL, 0, "nvn watch"));
    }
    wts_lo[wts_n] = lo, wts_hi[wts_n] = hi, wts_n++;
    os_mem_protect((void *)lo, hi - lo, OS_PROT_R);
    hos_log("nvn: watching writes to guest %#llx..%#llx", (unsigned long long)(lo - (uintptr_t)GP(0)),
            (unsigned long long)(hi - (uintptr_t)GP(0)));
}
static void watch_texels_source(const nvn_tex *t, const uint8_t *data, uint64_t size) {
    static int on = -1;
    static unsigned want_w, want_h;   /* nvn.watch_texels=<width>x<height>: only uploads into textures that size */
    if (on < 0) on = sscanf(setting_str(SET_NVN_WATCH_TEXELS_SRC), "%ux%u", &want_w, &want_h) == 2;
    if (!on || t->d.width != want_w || t->d.height != want_h || size < 4096 || wts_n >= 16) return;
    const uintptr_t lo = ((uintptr_t)data + 4095) & ~(uintptr_t)4095, hi = ((uintptr_t)data + size) & ~(uintptr_t)4095;
    if (hi <= lo) return;
    for (int i = 0; i < wts_n; i++) if (wts_lo[i] == lo) return;
    static int handler;
    if (!handler++) {
        os_fault_add(wts_handler, true);
        os_thread_release(os_thread_start(wts_rearm, NULL, 0, "nvn watch"));
    }
    wts_lo[wts_n] = lo, wts_hi[wts_n] = hi, wts_n++;
    os_mem_protect((void *)lo, hi - lo, OS_PROT_R);
    hos_log("nvn: watching WriteTexels source guest %#llx..%#llx", (unsigned long long)(lo - (uintptr_t)GP(0)),
            (unsigned long long)(hi - (uintptr_t)GP(0)));
}
/* (runtime) the bytes of the game's buffer that ReadTexels / WriteTexels touch at row and image strides (as
   texture_read_texels walks it: depth images of height rows of wb bytes); 0 when that overflows */
static uint64_t texels_extent(uint64_t wb, uint64_t hb, uint64_t depth, uint64_t row, uint64_t img) {
    uint64_t a = 0, b = 0, n = 0;
    if (!wb || !hb) return 1;
    if (__builtin_mul_overflow(depth - 1, img, &a) || __builtin_mul_overflow(hb - 1, row, &b) ||
        __builtin_add_overflow(a, b, &n) || __builtin_add_overflow(n, wb, &n))
        return 0;
    return n;
}
static void texels(Ctx *c, int write, int strided) {
    const nvn_tex *t = CTX(PN(0, sizeof(nvn_tex)));
    const nvn_view *v = CVW(PN(1, sizeof(nvn_view)));
    const NVNcopyRegion *r = (const NVNcopyRegion *)PN(2, sizeof(NVNcopyRegion));
    int bw, bh;
    const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    const uint64_t wb = (uint64_t)((r->width > 0 ? r->width : 0) + bw - 1) / bw * (uint64_t)(bpb > 0 ? bpb : 0);
    const uint64_t hb = (uint64_t)((r->height > 0 ? r->height : 0) + bh - 1) / bh;
    uint64_t row = strided ? U64(4) : 0, img = strided ? U64(5) : 0;
    if (!row) row = wb;
    if (!img) img = row * hb;
    /* (runtime) the strides are the game's 64-bit values: the whole span they reach must lie in guest memory, not only
       the buffer's first byte (the renderer reads or writes texels at data + k * img + y * row) */
    const uint64_t extent = texels_extent(wb, hb, r->depth > 1 ? (uint64_t)r->depth : 1, row, img);
    if (!extent) die("%sTexels of %#llx rows of %#llx, %#llx apart", write ? "Write" : "Read", (unsigned long long)hb,
                     (unsigned long long)wb, (unsigned long long)row);
    uint8_t *data = (uint8_t *)PN(3, extent);
    const unsigned level = v && v->set_levels ? v->base_level : 0, layer0 = v && v->set_layers ? v->min_layer : 0;
    if (v && v->set_format && v->format != t->d.format) die("%sTexels through a format view", write ? "Write" : "Read");
    static int texels_log = -1;
    if (texels_log < 0) texels_log = setting_bool(SET_NVN_LOG_TEXELS);
    if (texels_log) {   /* (debugging: who, from where, and how much of the data is not zero) */
        size_t nz = 0;
        for (uint64_t i = 0; data && i < extent && i < (64u << 20); i++) nz += data[i] != 0;
        uint64_t chain[6] = {c->x[30]}, fp = c->x[29];
        for (int k = 1; k < 6 && fp && fp < 0x8000000000ull && !(fp & 7); k++) chain[k] = G64(fp + 8), fp = G64(fp);
        hos_log("nvn: %sTexels data %#llx (%zu nonzero) from %llx < %llx < %llx < %llx < %llx < %llx",
                write ? "Write" : "Read", (unsigned long long)(P(3) ? (uint8_t *)P(3) - (uint8_t *)GP(0) : 0), nz,
                (unsigned long long)chain[0], (unsigned long long)chain[1], (unsigned long long)chain[2],
                (unsigned long long)chain[3], (unsigned long long)chain[4], (unsigned long long)chain[5]);
    }
    if (write) rx_nvn_write_texels(t, level, layer0, r, data, row, img), watch_texels_source(t, data, img);
    else rx_nvn_read_texels(t, level, layer0, r, data, row, img);
}
NV(nvnTextureReadTexels,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) { texels(c, 0, 0); }
NV(nvnTextureReadTexelsStrided,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) { texels(c, 0, 1); }
NV(nvnTextureWriteTexels,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) { texels(c, 1, 0); }
NV(nvnTextureWriteTexelsStrided,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) { texels(c, 1, 1); }
NV(nvnTextureFlushTexels) {}
NV(nvnTextureInvalidateTexels) {}

/* ---- samplers ---- */
#define SB(x) (&((nvn_samp_builder *)(x))->p)
#define CSB(x) (&((const nvn_samp_builder *)(x))->p)
#define CSP(x) (&((const nvn_samp *)(x))->p)
NV(nvnSamplerBuilderSetDefaults) {
    nvn_samp_builder *b = (nvn_samp_builder *)P(0);
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->p.minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR;
    b->p.magf = NVN_MAG_FILTER_LINEAR;
    b->p.ws = b->p.wt = b->p.wr = NVN_WRAP_MODE_REPEAT;
    b->p.cmp_func = NVN_COMPARE_FUNC_LESS;
    b->p.lod_min = -1000.f, b->p.lod_max = 1000.f, b->p.aniso = 1.f;
}
NV(nvnSamplerBuilderSetDevice) { ((nvn_samp_builder *)P(0))->dev = P(1); }
NV(nvnSamplerBuilderSetMinMagFilter) { SB(P(0))->minf = (uint8_t)U32(1); SB(P(0))->magf = (uint8_t)U32(2); }
NV(nvnSamplerBuilderSetWrapMode) { nvn_samp_par *p = SB(P(0)); p->ws = (uint8_t)U32(1), p->wt = (uint8_t)U32(2), p->wr = (uint8_t)U32(3); }
NV(nvnSamplerBuilderSetLodClamp) { SB(P(0))->lod_min = F(0); SB(P(0))->lod_max = F(1); }
NV(nvnSamplerBuilderSetLodBias) { SB(P(0))->lod_bias = F(0); }
NV(nvnSamplerBuilderSetCompare) { SB(P(0))->cmp_mode = (uint8_t)U32(1); SB(P(0))->cmp_func = (uint8_t)U32(2); }
NV(nvnSamplerBuilderSetBorderColor) { memcpy(SB(P(0))->border, PN(1, 16), 16); }
NV(nvnSamplerBuilderSetBorderColori) { const int32_t *v = (const int32_t *)P(1); for (int i = 0; i < 4; i++) SB(P(0))->border[i] = (float)v[i]; }
NV(nvnSamplerBuilderSetBorderColorui) { const uint32_t *v = (const uint32_t *)P(1); for (int i = 0; i < 4; i++) SB(P(0))->border[i] = (float)v[i]; }
NV(nvnSamplerBuilderSetMaxAnisotropy) { SB(P(0))->aniso = F(0); }
NV(nvnSamplerBuilderSetLodSnap) { SB(P(0))->lod_snap = F(0); }
NV(nvnSamplerBuilderSetReductionFilter) {
    if (U32(1) > 2) die("sampler reduction %u", U32(1));
    SB(P(0))->reduction = (uint8_t)U32(1);
}
NV(nvnSamplerBuilderGetMinMagFilter) { *(int *)P(1) = CSB(P(0))->minf; *(int *)P(2) = CSB(P(0))->magf; }
NV(nvnSamplerBuilderGetWrapMode) { const nvn_samp_par *p = CSB(P(0)); *(int *)P(1) = p->ws; *(int *)P(2) = p->wt; *(int *)P(3) = p->wr; }
NV(nvnSamplerBuilderGetReductionFilter) { RET(CSB(P(0))->reduction); }
NV(nvnSamplerInitialize) {
    nvn_samp *o = (nvn_samp *)P(0);
    o->magic = NVN_MAGIC('S', 'M', 'P', 0);
    o->p = *CSB(P(1));
    RET(1);
}
NV(nvnSamplerFinalize) { ((nvn_samp *)P(0))->magic = 0; }
NV(nvnSamplerSetDebugLabel) {}
NV(nvnSamplerGetMinMagFilter) { *(int *)P(1) = CSP(P(0))->minf; *(int *)P(2) = CSP(P(0))->magf; }
NV(nvnSamplerGetWrapMode) { const nvn_samp_par *p = CSP(P(0)); *(int *)P(1) = p->ws; *(int *)P(2) = p->wt; *(int *)P(3) = p->wr; }
NV(nvnSamplerGetLodClamp) { *(float *)P(1) = CSP(P(0))->lod_min; *(float *)P(2) = CSP(P(0))->lod_max; }
NV(nvnSamplerGetLodBias) { retf(c, CSP(P(0))->lod_bias); }
NV(nvnSamplerGetCompare) { *(int *)P(1) = CSP(P(0))->cmp_mode; *(int *)P(2) = CSP(P(0))->cmp_func; }
NV(nvnSamplerGetBorderColor) { memcpy(PN(1, 16), CSP(P(0))->border, 16); }
NV(nvnSamplerGetMaxAnisotropy) { retf(c, CSP(P(0))->aniso); }
NV(nvnSamplerGetReductionFilter) { RET(CSP(P(0))->reduction); }
NV(nvnSamplerGetDebugID) { RET(U64(0)); }

/* ---- programs: the stages' code addresses and control sections (translated by the replay) ---- */
NV(nvnProgramInitialize) {
    memset(P(0), 0, sizeof(nvn_prog));
    ((nvn_prog *)P(0))->magic = NVN_MAGIC('P', 'R', 'O', 'G');
    RET(1);
}
NV(nvnProgramFinalize) {
    rx_nvn_program_released((const nvn_prog *)P(0));
    ((nvn_prog *)P(0))->magic = 0;
}
/* nvn.shader_dump=<dir> (mkw-recomp, debugging): each stage's code (from its GPU address, up to 64 KiB while mapped)
   and control section (0x800 bytes) as <code address>.code / .ctl, for gpu/shadertool */
static void dump_stage(uint64_t code, uint64_t ctl) {
    static const char *dir = (const char *)-1;
    if (dir == (const char *)-1) dir = *setting_str(SET_SHADER_DUMP) ? setting_str(SET_SHADER_DUMP) : NULL;
    if (!dir || !code || !ctl) return;
    char path[OS_PATH_MAX];
    snprintf(path, sizeof path, "%s/%010llx.code", dir, (unsigned long long)code);
    if (os_path_kind(path) != OS_PATH_NONE) return;
    os_mkdir(dir);
    uint64_t n = 0;
    while (n < 0x10000 && hos_guest_range(code + n, 0x1000)) n += 0x1000;
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(GP(code), 1, (size_t)n, f); fclose(f); }
    snprintf(path, sizeof path, "%s/%010llx.ctl", dir, (unsigned long long)code);
    f = fopen(path, "wb");
    if (f) { fwrite(GP(ctl), 1, 0x800, f); fclose(f); }
}
static int set_shaders(nvn_prog *o, int n, const uint8_t *sd, int stride) {
    if (n < 0 || n > 6) return 0;
    rx_nvn_program_released(o);
    o->nstage = (uint32_t)n;
    for (int i = 0; i < n; i++) {   /* {data (GPU address), control (guest pointer)[, reserved]} */
        memcpy(&o->sd[i].data, sd + stride * i, 8);
        memcpy(&o->sd[i].control, sd + stride * i + 8, 8);
        dump_stage(o->sd[i].data, o->sd[i].control);
        if (stride == 0x18) {
            uint64_t reserved;
            memcpy(&reserved, sd + stride * i + 16, 8);
            if (reserved) hos_log("nvn: ProgramSetShadersExt: stage %d field %#llx", i, (unsigned long long)reserved);
        }
    }
    o->serial = new_serial();
    rx_nvn_program_set(o);   /* (the back end prefetches the program's stages) */
    return 1;
}
NV(nvnProgramSetShaders) { RET(set_shaders((nvn_prog *)P(0), I(1), (const uint8_t *)P(2), 16)); }
/* NVNshaderDataExt: {data, control, reserved}, 0x18 bytes (seen in TOTK's calls; reserved 0) */
NV(nvnProgramSetShadersExt) { RET(set_shaders((nvn_prog *)P(0), I(1), (const uint8_t *)P(2), 0x18)); }
NV(nvnProgramSetDebugLabel) {}

/* ---- sync, events ---- */
NV(nvnSyncInitialize) { nvn_sync *s = (nvn_sync *)P(0); s->magic = NVN_MAGIC('S', 'Y', 'N', 'C'); s->fenced = 0; s->fence = 0; RET(1); }
NV(nvnSyncFinalize) { ((nvn_sync *)P(0))->magic = 0; }
NV(nvnSyncSetDebugLabel) {}
/* a fence: everything submitted to the queue so far is done */
/* nvn.sync_log or the live "!synclog" (debugging): fences, sync waits and event reads, with the caller */
volatile int hos_nvn_sync_log_on;   /* (also the live input file's "!synclog": nn_hid.c) */
static int sync_log(void) {   /* (the first 30000 calls; nvn.sync_log=<n>: from the n-th present on, 400 calls) */
    static int v = -1;
    static long long from;
    static volatile int32_t n;
    extern volatile int64_t hos_present_count;   /* display.c */
    if (v < 0) v = setting_given(SET_NVN_SYNC_LOG), from = atoll(setting_str(SET_NVN_SYNC_LOG));
    if (v && from > 1) return hos_present_count >= from && os_atomic_inc(&n) <= 400;
    return (v || hos_nvn_sync_log_on) && os_atomic_inc(&n) <= 30000;
}
/* (runtime) A submission is handed to the back end at the submit, not held until the queue is flushed: MKW writes some of a
   frame's data in place right after its submit, as the console's GPU has read it by then, and holding made its terrain
   flash far more often */
NV(nvnQueueFenceSync) {
    nvn_sync *s = (nvn_sync *)P(1); s->fenced = 1;
    /* (runtime) the queue's own work: its last submission (a wait for it no longer waits for another queue's work stopped at
       an event wait) */
    s->fence = rx_nvn_fence_q((uint64_t)(uintptr_t)P(0));
    if (sync_log()) hos_log("nvn: FenceSync %p -> %llu (lr %#llx)", (void *)s, (unsigned long long)s->fence, (unsigned long long)c->x[30]);
}
/* (the GPU waits the game makes: count and time by kind, logged every 5 s) */
static volatile int64_t wait_ns[3], wait_n[3];   /* sync waits that blocked, sync polls that timed out, queue finishes */
static void wait_note(int kind, int64_t t0) {
    os_atomic_add(&wait_ns[kind], hos_now_ns() - t0);
    os_atomic_inc(&wait_n[kind]);
    static volatile int64_t last;
    const int64_t now = hos_now_ns(), l = last;
    if (now - l >= 5000000000LL && os_atomic_cas(&last, l, now) == l && l) {
        hos_log("nvn: GPU waits in 5 s: %lld sync waits (%.0f ms), %lld polls timed out (%.0f ms), %lld queue finishes (%.0f ms)",
                (long long)os_atomic_xchg(&wait_n[0], 0), os_atomic_xchg(&wait_ns[0], 0) / 1e6,
                (long long)os_atomic_xchg(&wait_n[1], 0), os_atomic_xchg(&wait_ns[1], 0) / 1e6,
                (long long)os_atomic_xchg(&wait_n[2], 0), os_atomic_xchg(&wait_ns[2], 0) / 1e6);
    } else if (!l) {
        os_atomic_cas(&last, 0, now);
    }
}
/* never fenced: times out, as on the console; fenced: released once the GPU reached the fence */
NV(nvnSyncWait) {
    const nvn_sync *s = (const nvn_sync *)P(0);
    if (sync_log()) hos_log("nvn: SyncWait %p fence %llu timeout %llu (lr %#llx); latest %llu, done %llu", (void *)s,
                            (unsigned long long)s->fence, (unsigned long long)U64(1), (unsigned long long)c->x[30],
                            (unsigned long long)rx_nvn_fence(), (unsigned long long)rx_nvn_replayed());
    if (!s->fenced) { RET(NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED); return; }
    if (rx_nvn_wait(s->fence, 0)) { RET(NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED); return; }
    const int away = hos_block_begin(CRIT_BLOCK_OTHER, (const void *)1);
    const int64_t t0 = hos_now_ns();
    const int ok = rx_nvn_wait(s->fence, U64(1));
    wait_note(ok ? 0 : 1, t0);
    if (away) hos_block_end();
    RET(ok ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
}
NV(nvnEventBuilderSetDefaults) { memset(P(0), 0, sizeof(nvn_event_builder)); }
NV(nvnEventBuilderSetStorage) { nvn_event_builder *b = (nvn_event_builder *)P(0); b->pool = (const nvn_pool *)P(1); b->off = (int64_t)U64(2); }
NV(nvnEventInitialize) {
    const nvn_event_builder *b = (const nvn_event_builder *)P(1);
    if (!b->pool) { RET(0); return; }
    nvn_event *e = (nvn_event *)P(0);
    e->magic = NVN_MAGIC('E', 'V', 'N', 'T');
    e->gpu = POOL(b->pool)->gpu + (uint64_t)b->off;
    RET(1);
}
NV(nvnEventFinalize) { ((nvn_event *)P(0))->magic = 0; }
/* (runtime) an NVN event's value from the host (hos_thread.c's present poll): 0 when it is no event */
int hos_nvn_event_value(uint64_t guest_event, uint32_t *v) {
    const nvn_event *e = (const nvn_event *)hos_guest_range(guest_event, sizeof(nvn_event));
    if (!guest_event || !e || e->magic != NVN_MAGIC('E', 'V', 'N', 'T')) return 0;
    *v = 0;
    nvn_gpu_read(e->gpu, v, 4);
    return 1;
}
NV(nvnEventGetValue) {
    uint32_t v = 0; nvn_gpu_read(((const nvn_event *)P(0))->gpu, &v, 4); RET(v);
    if (sync_log()) hos_log("nvn: EventGetValue %p = %u (lr %#llx)", P(0), v, (unsigned long long)c->x[30]);
}
/* The call traces below: debug.lognvn */
static int lognvn(void) {
    static int on = -1;
    if (on < 0) on = hos_log_switch(SET_DBG_LOGNVN);
    return on;
}
NV(nvnEventSignal) {
    const nvn_event *e = (const nvn_event *)P(0);
    {   /* (the CPU's signals, 40 then every 5000: what the GPU's waits above wait for) */
        static volatile int32_t n;
        const int32_t k = os_atomic_inc(&n);
        if (lognvn() && (k <= 40 || k % 5000 == 0))
            hos_log("nvn: EventSignal (CPU) #%ld: event %#llx mode %u value %u (lr %#llx)", (long)k,
                    (unsigned long long)(e ? e->gpu : 0), U32(1), U32(2), (unsigned long long)c->x[30]);
    }
    uint32_t v = 0;
    if (U32(1) == NVN_EVENT_SIGNAL_MODE_ADD) nvn_gpu_read(e->gpu, &v, 4);
    v = U32(1) == NVN_EVENT_SIGNAL_MODE_ADD ? v + U32(2) : U32(2);
    nvn_gpu_write(e->gpu, &v, 4);
    rx_nvn_event_signal(e->gpu, v);
}

/* ---- queue ---- */
#define QB(p) ((nvn_queue_builder *)(p))
NV(nvnQueueBuilderSetDefaults) {
    nvn_queue_builder *b = QB(P(0));
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->cmd_sz = 65536, b->comp_sz = 262144, b->ctl_sz = 16384, b->flush = 4096;   /* the device's defaults */
}
NV(nvnQueueBuilderSetDevice) { QB(P(0))->dev = P(1); }
NV(nvnQueueBuilderSetFlags) { QB(P(0))->flags = U32(1); }
NV(nvnQueueBuilderSetCommandMemorySize) { QB(P(0))->cmd_sz = U64(1); }
NV(nvnQueueBuilderSetComputeMemorySize) { QB(P(0))->comp_sz = U64(1); }
NV(nvnQueueBuilderSetControlMemorySize) { QB(P(0))->ctl_sz = U64(1); }
NV(nvnQueueBuilderSetCommandFlushThreshold) { QB(P(0))->flush = U64(1); }
/* the game allocates this much (we do not use it): the three blocks, rounded to 4 KiB */
NV(nvnQueueBuilderGetQueueMemorySize) { const nvn_queue_builder *b = QB(P(0)); RET((b->cmd_sz + b->comp_sz + b->ctl_sz + 4095) & ~4095ull); }
NV(nvnQueueBuilderSetQueueMemory) { QB(P(0))->mem = U64(1); QB(P(0))->mem_sz = U64(2); }
NV(nvnQueueInitialize) { nvn_queue *q = (nvn_queue *)P(0); q->magic = NVN_MAGIC('Q', 'U', 'E', 'U'); q->submits = 0; RET(1); }
NV(nvnQueueFinalize) { ((nvn_queue *)P(0))->magic = 0; }
NV(nvnQueueGetError) { if (P(1)) memset(P(1), 0, sizeof(NVNqueueErrorInfo)); RET(NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR); }
NV(nvnQueueFlush) {}
NV(nvnQueueFinish) {
    const int away = hos_block_begin(CRIT_BLOCK_OTHER, (const void *)2);
    const int64_t t0 = hos_now_ns();
    rx_nvn_finish();
    wait_note(2, t0);
    if (away) hos_block_end();
}
NV(nvnQueueSetDebugLabel) {}

/* ---- command buffers ---- */
#define CB(p) ((nvn_cmd *)(p))
static void *rec(nvn_cmd *o, uint32_t op, size_t payload);
#define OP_RESERVE 24

/* Direct mode keeps each recording as a list by reference (runtime/nvn_list.h has the design and the lifetime rules): a
   recording is written once into an allocation of its own and its handle names it; CallCommands and CopyCommands hand the
   callee's list to the back end instead of copying records; no copy_lock, no rec_node, no copy at EndRecording. The back
   end's list of each one is in its user. The renderers that replay records take the copies below. */
static void released_hook(nvn_lrec *r) { rx_direct()->list_released(r->user); r->user = NULL; }
static int list_ref(void) {
    static int on = -1;
    if (on < 0) {
        on = rx_direct() != NULL;
        if (on) {
            nvn_list_init(0, hos_log, die);
            nvn_list_set_user_hooks(released_hook, NULL);
            hos_log("nvn: direct mode: command lists by reference, their records handed to the back end as they are made");
        }
    }
    return on;
}
static void *guest_or_null(uint64_t a, uint64_t n) { return hos_guest_range(a, n); }
/* (the replay's progress as the last submit read it: AddControlMemory frees by it. rx_nvn_replayed polls the batched queue
   under the present lock, which the replay holds while it submits: the workers' AddControlMemory waited there, up to
   0.9 ms, and a poll could hand the batch to the GPU from a worker) */
static volatile uint64_t list_done;
static void list_log(const char *why) {
    nvn_list_stats t;
    nvn_list_get_stats(&t);
    hos_log("nvn: lists (%s): %llu recordings made, %llu live (%.1f MB), %llu released, %llu freed; %llu calls by reference, "
            "%llu copied in (too deep); deferred binds: %llu read in place, %llu private copies for %llu conflicts; %llu grown, "
            "%llu shrunk (%.1f MB moved), %llu swept", why, (unsigned long long)t.made, (unsigned long long)t.live, t.live_bytes / 1048576.0,
            (unsigned long long)t.released, (unsigned long long)t.freed, (unsigned long long)t.calls,
            (unsigned long long)t.calls_expanded, (unsigned long long)t.resolved_in_place, (unsigned long long)t.clones,
            (unsigned long long)t.conflicts, (unsigned long long)t.grown, (unsigned long long)t.shrunk, t.copied / 1048576.0, (unsigned long long)t.swept);
}

/* Recording copies and command buffer segments come from power-of-two free lists and go back to them. Through the
   process heap (~13k copies a frame, segments freed at Finalize and made again), the frees decommitted pages that the
   next allocations faulted in again: NtFreeVirtualMemory ~0.6% of the recording thread (ModuleSystemWorker1), more in
   page faults. A class keeps at most POOL_KEEP bytes; the rest goes back to the heap. */
#define POOL_MIN 6       /* 64 bytes */
#define POOL_MAX 26      /* 64 MiB: bigger blocks use the heap */
#define POOL_KEEP (64ull << 20)
static os_mutex pool_lock = OS_MUTEX_INIT;
static void *pool_head[POOL_MAX + 1];
static uint64_t pool_bytes[POOL_MAX + 1];
static unsigned pool_class(size_t n) {
    unsigned c = POOL_MIN;
    while (c <= POOL_MAX && ((size_t)1 << c) < n) c++;
    return c;
}
/* a block of at least n bytes; *cls its class (POOL_MAX + 1: from the heap, n bytes) */
static void *pool_get(size_t n, unsigned *cls) {
    const unsigned c = pool_class(n);
    *cls = c;
    if (c > POOL_MAX) return hos_ledger_took(HL_NVN_CMD, malloc(n));
    os_mutex_lock(&pool_lock);
    void *b = pool_head[c];
    if (b) {
        pool_head[c] = *(void **)b;
        pool_bytes[c] -= (uint64_t)1 << c;
    }
    os_mutex_unlock(&pool_lock);
    return b ? b : hos_ledger_took(HL_NVN_CMD, malloc((size_t)1 << c));
}
static void pool_put(void *b, unsigned cls) {
    if (!b) return;
    if (cls > POOL_MAX) {
        hos_ledger_frees(HL_NVN_CMD, b);
        free(b);
        return;
    }
    os_mutex_lock(&pool_lock);
    const int keep = pool_bytes[cls] + ((uint64_t)1 << cls) <= POOL_KEEP;
    if (keep) {
        *(void **)b = pool_head[cls];
        pool_head[cls] = b;
        pool_bytes[cls] += (uint64_t)1 << cls;
    }
    os_mutex_unlock(&pool_lock);
    if (!keep) hos_ledger_frees(HL_NVN_CMD, b), free(b);
}

/* (runtime) A command buffer's recording buffer lives in host memory, and its pointers in this table, not in the
   object (guest memory). The object holds its slot's number; a slot names the object it belongs to, so a slot number
   that is not the object's own is not used: the object gets a new slot, as an object never initialized does. Slots are
   in chunks that are never freed, so a lookup needs no lock (a command buffer records on one thread at a time). */
/* (HOS_NVN_LIST_REF=1, runtime/nvn_list.h) seg, cur and end are the open recording's place in its own allocation (lw) */
typedef struct {
    uint64_t owner;
    union { struct { uint8_t *seg, *cur, *end; }; nvn_lw lw; };
    void *volatile dl;             /* (direct mode) the open recording's list in the back end (NULL: none open) */
    volatile int32_t dl_pending;   /* (direct mode) its last record waits on a thread for its flush (runtime/render_lists.c) */
} cmd_rec;
#define CMD_CHUNK_BITS 10
#define CMD_CHUNKS 4096u                     /* (4M command buffers at once) */
static cmd_rec *volatile cmd_chunks[CMD_CHUNKS];
static os_mutex cmd_lock = OS_MUTEX_INIT;
static uint32_t cmd_next = 1, *cmd_free;     /* (slot 0: none) */
static size_t ncmd_free, cap_cmd_free;
static cmd_rec *cmd_slot(uint32_t s) {
    if (!s || (s >> CMD_CHUNK_BITS) >= CMD_CHUNKS) return NULL;
    cmd_rec *k = cmd_chunks[s >> CMD_CHUNK_BITS];
    return k ? &k[s & ((1u << CMD_CHUNK_BITS) - 1)] : NULL;
}
static int list_ref(void);
/* a free slot, its chunk made (cmd_lock held) */
static uint32_t cmd_slot_new_locked(void) {
    const uint32_t s = ncmd_free ? cmd_free[--ncmd_free] : cmd_next++;
    if ((s >> CMD_CHUNK_BITS) >= CMD_CHUNKS) die("too many command buffers");
    if (!cmd_chunks[s >> CMD_CHUNK_BITS]) {
        cmd_rec *k = (cmd_rec *)calloc((size_t)1 << CMD_CHUNK_BITS, sizeof *k);
        if (!k) die("command buffer table");
        cmd_chunks[s >> CMD_CHUNK_BITS] = k;
    }
    return s;
}
/* (HOS_NVN_LIST_REF) free slots kept by each thread: the job workers' Initialize / Finalize pairs (132 a frame, a command
   buffer on the worker's stack for each job) take no lock but once in 32 */
static _Thread_local uint32_t cmd_tl_free[64];
static _Thread_local uint32_t cmd_tl_n;
static uint32_t cmd_slot_take(void) {
    if (!cmd_tl_n) {
        os_mutex_lock(&cmd_lock);
        while (cmd_tl_n < 32) cmd_tl_free[cmd_tl_n++] = cmd_slot_new_locked();
        os_mutex_unlock(&cmd_lock);
    }
    return cmd_tl_free[--cmd_tl_n];
}
static void cmd_slot_give(uint32_t s) {
    if (cmd_tl_n == 64) {
        os_mutex_lock(&cmd_lock);
        while (cmd_tl_n > 32) {
            if (ncmd_free == cap_cmd_free) {
                cap_cmd_free = cap_cmd_free ? cap_cmd_free * 2 : 256;
                cmd_free = (uint32_t *)realloc(cmd_free, cap_cmd_free * sizeof *cmd_free);
                if (!cmd_free) die("command buffer table");
            }
            cmd_free[ncmd_free++] = cmd_tl_free[--cmd_tl_n];
        }
        os_mutex_unlock(&cmd_lock);
    }
    cmd_tl_free[cmd_tl_n++] = s;
}
static cmd_rec *cmd_adopt(nvn_cmd *o) {
    const uint64_t me = h2g(o);
    if (list_ref()) {   /* (the slot is this object's alone from here: no lock for the rest) */
        const uint32_t s = cmd_slot_take();
        cmd_rec *r = cmd_slot(s);
        memset(r, 0, sizeof *r);
        __atomic_store_n(&r->owner, me, __ATOMIC_RELEASE);
        o->slot = s;
        return r;
    }
    os_mutex_lock(&cmd_lock);
    const uint32_t s = cmd_slot_new_locked();
    cmd_rec *r = cmd_slot(s);
    memset(r, 0, sizeof *r);
    r->owner = me;
    os_mutex_unlock(&cmd_lock);
    o->slot = s;
    return r;
}
static cmd_rec *cmd_host(nvn_cmd *o) {
    cmd_rec *r = cmd_slot(o->slot);
    if (r && r->owner == h2g(o)) return r;
    static volatile int32_t told;
    if (o->slot && os_atomic_inc(&told) <= 4)
        hos_log("nvn: command buffer %#llx names slot %u, which is not its own: a new one", (unsigned long long)h2g(o), o->slot);
    return cmd_adopt(o);
}
static volatile int64_t seg_bytes, seg_count;   /* (the command buffers' segments, for the recordings' log line) */
static void cmd_release(nvn_cmd *o) {   /* the object's slot and recording buffer back */
    cmd_rec *r = cmd_slot(o->slot);
    if (!r || r->owner != h2g(o)) {
        o->slot = 0;
        return;
    }
    if (list_ref()) {   /* (a recording never ended dropped; the ended ones live on by their handles) */
        nvn_list_drop(&r->lw);
        memset(r, 0, sizeof *r);
        cmd_slot_give(o->slot);
        o->slot = 0;
        return;
    }
    if (r->seg) {
        os_atomic_add(&seg_bytes, -(int64_t)(r->end - r->seg));
        os_atomic_dec(&seg_count);
        pool_put(r->seg, pool_class((size_t)(r->end - r->seg)));
    }
    os_mutex_lock(&cmd_lock);
    memset(r, 0, sizeof *r);
    if (ncmd_free == cap_cmd_free) {
        cap_cmd_free = cap_cmd_free ? cap_cmd_free * 2 : 256;
        cmd_free = (uint32_t *)realloc(cmd_free, cap_cmd_free * sizeof *cmd_free);
        if (!cmd_free) die("command buffer table");
    }
    cmd_free[ncmd_free++] = o->slot;
    os_mutex_unlock(&cmd_lock);
    o->slot = 0;
}
static uint8_t *cmd_alloc(nvn_cmd *o, uint32_t size);
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size);
/* (direct mode, M3a game patches: games/oot/patches/passes.c, job_sync.c) the back end's open list of the guest command
   buffer at cmd (this thread's waiting record handed over first, so the list has every record before this point); NULL:
   none, or not direct mode */
void *cmd_list(uint64_t cmd) {
    if (!rx_direct() || !cmd) return NULL;
    nvn_cmd *o = (nvn_cmd *)g2h(cmd);
    if (!o || o->magic != NVN_MAGIC('C', 'M', 'D', 'B')) return NULL;
    gpu_flush();
    cmd_rec *r = cmd_slot(o->slot);
    return r && r->owner == cmd ? r->dl : NULL;
}
/* (direct mode) a command buffer finalized or initialized again: its list begun and not ended dropped (before cmd_release) */
static void list_drop(nvn_cmd *o) {
    cmd_rec *r = cmd_slot(o->slot);
    if (!rx_direct() || !r || r->owner != h2g(o) || !r->dl) return;
    gpu_flush();
    void *dl = r->dl;
    r->dl = NULL;
    rx_direct()->list_released(dl);
}
NV(nvnCommandBufferInitialize) {
    nvn_cmd *o = CB(P(0));
    list_drop(o);
    cmd_release(o);   /* (initialized again without Finalize: its recording buffer back) */
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('C', 'M', 'D', 'B');
    o->inline_cur = ~0ull;
    o->inline_end = 0;
    cmd_adopt(o);
    RET(1);
}
NV(nvnCommandBufferFinalize) {
    nvn_cmd *o = CB(P(0));
    list_drop(o);
    cmd_release(o);
    o->magic = 0;
}
NV(nvnCommandBufferSetMemoryCallback) { CB(P(0))->cb = U64(1); }
NV(nvnCommandBufferSetMemoryCallbackData) { CB(P(0))->cb_data = U64(1); }
NV(nvnCommandBufferGetMemoryCallback) { RET(CB(P(0))->cb); }
NV(nvnCommandBufferGetMemoryCallbackData) { RET(CB(P(0))->cb_data); }
NV(nvnCommandBufferSetCommandMemoryCallbackEnabled) {}
NV(nvnCommandBufferSetControlMemoryCallbackEnabled) {}
NV(nvnCommandBufferSetDebugLabel) {}

static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) { nvn_op_hdr h = {op, size}; memcpy(p, &h, sizeof h); }
static void cmd_reserve_rec(cmd_rec *o, size_t need) {
    if (rx_direct()) return;   /* (no bytes: the records go to the back end, runtime/render_lists.c) */
    if (o->seg && need + OP_RESERVE <= (size_t)(o->end - o->cur)) return;
    const size_t used = o->seg ? (size_t)(o->cur - o->seg) : 0;
    size_t cap = o->seg ? (size_t)(o->end - o->seg) : 0;
    if (!cap) cap = 64 << 10;
    while (used + need + OP_RESERVE > cap) cap *= 2;
    unsigned cls;
    uint8_t *b = (uint8_t *)pool_get(cap, &cls);
    if (!b) die("recording buffer (%zu bytes)", cap);
    if (o->seg) {
        memcpy(b, o->seg, used);
        os_atomic_add(&seg_bytes, -(int64_t)(o->end - o->seg));
        pool_put(o->seg, pool_class((size_t)(o->end - o->seg)));
    } else {
        os_atomic_inc(&seg_count);
    }
    os_atomic_add(&seg_bytes, (int64_t)cap);
    o->seg = b, o->cur = b + used, o->end = b + cap;
}
static uint8_t *cmd_alloc(nvn_cmd *o, uint32_t size) {
    size = (size + 7) & ~7u;
    cmd_rec *r = cmd_host(o);
    if (rx_direct()) {   /* (the accounting as below; the record in this thread's scratch, to the back end at the next one) */
        uint8_t *p = gpu_scratch(&r->dl, &r->dl_pending, size);
        o->cmd_used += size;
        o->hw_used += size;
        o->last_rec = size;
        put_hdr(p, 0, size);
        return p;
    }
    cmd_reserve_rec(r, size);
    uint8_t *p = r->cur;
    r->cur += size;
    o->cmd_used += size;
    o->hw_used += size;
    o->last_rec = size;
    put_hdr(p, 0, size);
    return p;
}
/* the game sums GetCommandMemoryUsed over recordings to size its resource heaps: report the real encoding's size where
   it is known (Maxwell methods of a real recording, from the reference) instead of our record's */
static void hw_fix(nvn_cmd *o, uint64_t bytes) { o->hw_used += bytes; o->hw_used -= o->last_rec; o->last_rec = bytes; }

NV(nvnCommandBufferAddCommandMemory) {
    nvn_cmd *o = CB(P(0));
    o->game_size = U64(3);
    o->cmd_used = o->hw_used = 0;
}

/* Recordings: a handle is a host copy of the finished recording. Games reuse command memory for later recordings while
   still calling earlier handles, so the copy lives until the control memory it was made in is added again (the console
   driver's handle is a descriptor in that control memory). (The reference also drops copies whose control memory was
   reused for other data or unmapped, by a mark written there; not done here yet: long sessions can keep more copies.) */
/* live copies by the 64 KiB block of their control memory position: AddControlMemory releases a range's copies without
   looking at the others (a scan of every copy at each call was ~4% of TOTK's frame threads: ~22000 live copies). By
   4 KiB page a 2 MiB range walked ~500 buckets under the exclusive lock the recording threads also take
   (totk-pc-recomp dc69797: 1.4.3 field walk 72-77 -> 78-80 fps with 64 KiB blocks). nvn.rec_shift=12: by page. */
typedef struct rec_node { uint64_t ctl; uint8_t *copy; struct rec_node *next; int marked; } rec_node;
#define REC_BUCKETS 65536u
static rec_node **rec_buckets;
static size_t ncopies;
static size_t npins;   /* (tex_copy: the texture copies, logged with the recordings) */
static unsigned rec_shift = 16;   /* (set when the buckets are made) */
static unsigned rec_bucket(uint64_t ctl) { return (unsigned)(((ctl >> rec_shift) * 0x9E3779B97F4A7C15ull) >> 48) & (REC_BUCKETS - 1); }
static os_rwlock copy_lock = OS_RWLOCK_INIT;
static volatile int64_t copy_bytes;
#define COPY_MAGIC 0x59504f43444d43ull   /* "CMDCOPY" */
typedef struct { uint64_t magic, bytes, cls; } copy_prefix;   /* (cls: its pool class) */
/* released copies wait until the replay passed every submit made before their release (the replay runs behind) */
typedef struct { copy_prefix *p; uint64_t after; } dead_copy;
static dead_copy *dead;
static size_t ndead, cap_dead;
/* (runtime) The live handles, a host set (open addressing, copy_lock held): a handle the game passes back is a host
   address, so it is used only when it is in the set, never on what the memory at that address holds. */
static uint64_t *live_handles;
static size_t live_cap, nlive;
static size_t live_at(uint64_t h, size_t cap) { return (size_t)((h * 0x9E3779B97F4A7C15ull) >> 24) & (cap - 1); }
static void live_handle_add(uint64_t h) {   /* (copy_lock held exclusively) */
    if ((nlive + 1) * 2 > live_cap) {
        const size_t cap = live_cap ? live_cap * 2 : 65536;
        uint64_t *t = (uint64_t *)calloc(cap, sizeof *t);
        if (!t) die("recording handle set");
        for (size_t i = 0; i < live_cap; i++) {
            if (!live_handles[i]) continue;
            size_t k = live_at(live_handles[i], cap);
            while (t[k]) k = (k + 1) & (cap - 1);
            t[k] = live_handles[i];
        }
        free(live_handles);
        live_handles = t, live_cap = cap;
    }
    size_t k = live_at(h, live_cap);
    while (live_handles[k] && live_handles[k] != h) k = (k + 1) & (live_cap - 1);
    if (!live_handles[k]) live_handles[k] = h, nlive++;
}
static int live_handle_has(uint64_t h) {   /* (copy_lock held) */
    if (!h || !live_cap) return 0;
    for (size_t k = live_at(h, live_cap); live_handles[k]; k = (k + 1) & (live_cap - 1))
        if (live_handles[k] == h) return 1;
    return 0;
}
static void live_handle_remove(uint64_t h) {   /* (copy_lock held exclusively; backward shift, no tombstones) */
    if (!h || !live_cap) return;
    size_t k = live_at(h, live_cap);
    while (live_handles[k] && live_handles[k] != h) k = (k + 1) & (live_cap - 1);
    if (!live_handles[k]) return;
    live_handles[k] = 0, nlive--;
    for (size_t j = (k + 1) & (live_cap - 1); live_handles[j]; j = (j + 1) & (live_cap - 1)) {
        const size_t want = live_at(live_handles[j], live_cap);
        /* (move it into the hole when its probe run starts at or before the hole) */
        if (((j - want) & (live_cap - 1)) >= ((j - k) & (live_cap - 1))) {
            live_handles[k] = live_handles[j], live_handles[j] = 0;
            k = j;
        }
    }
}
static void free_dead_copies(void) {   /* (copy_lock held) */
    const uint64_t done = rx_nvn_replayed();
    for (size_t i = 0; i < ndead;) {
        if (dead[i].after <= done) {
            os_atomic_add(&copy_bytes, -(int64_t)dead[i].p->bytes);
            pool_put(dead[i].p, (unsigned)dead[i].p->cls);
            dead[i] = dead[--ndead];
        } else {
            i++;
        }
    }
}
static void release_copies(uint64_t lo, uint64_t hi) {
    if (hi <= lo || !rec_buckets) return;
    const uint64_t after = rx_nvn_pushed();
    os_rwlock_lock(&copy_lock);
    free_dead_copies();
    /* (a range over 4 GiB: every bucket once instead of every block) */
    const uint64_t pages = ((hi - 1) >> rec_shift) - (lo >> rec_shift) + 1;
    const uint64_t steps = pages > REC_BUCKETS ? REC_BUCKETS : pages;
    for (uint64_t k = 0; k < steps; k++) {
        const unsigned b = pages > REC_BUCKETS ? (unsigned)k : rec_bucket(((lo >> rec_shift) + k) << rec_shift);
        for (rec_node **pn = &rec_buckets[b]; *pn;) {
            rec_node *n = *pn;
            if (n->ctl < lo || n->ctl >= hi) {
                pn = &n->next;
                continue;
            }
            copy_prefix *p = (copy_prefix *)n->copy - 1;
            p->magic = 0;   /* (no submit names it any more) */
            live_handle_remove((uint64_t)(uintptr_t)n->copy);
            if (ndead == cap_dead) {
                cap_dead = cap_dead ? cap_dead * 2 : 1024;
                dead = (dead_copy *)realloc(dead, cap_dead * sizeof *dead);
            }
            dead[ndead].p = p;
            dead[ndead].after = after;
            ndead++;
            *pn = n->next;
            free(n);
            ncopies--;
        }
    }
    os_rwlock_unlock(&copy_lock);
}
/* Copies whose control memory the game reused for other data: the console driver's handle is a descriptor written in
   the 64 bytes of control memory a recording takes; a mark written there in its place (the copy's address, and a magic)
   that the game has overwritten means it reused that memory, and the handle is dead on the console too. Copies whose
   control memory was never added again (MKW frees and reallocates its control memory) were kept for good otherwise:
   ~40 per 1000 recordings, ~100,000 after 20 minutes. Swept every 25,000 recordings. */
#define COPY_MARK 0x4b52414d59504f43ull   /* "COPYMARK" */
static void sweep_overwritten_copies(void) {   /* (copy_lock held) */
    if (!rec_buckets) return;
    free_dead_copies();
    const uint64_t after = rx_nvn_pushed();
    size_t swept = 0;
    for (unsigned b = 0; b < REC_BUCKETS; b++) {
        for (rec_node **pn = &rec_buckets[b]; *pn;) {
            rec_node *n = *pn;
            uint64_t mark[2];
            if (!n->marked) {
                pn = &n->next;
                continue;
            }
            void *at = hos_guest_range(n->ctl, sizeof mark);
            if (!at) {
                pn = &n->next;
                continue;
            }
            memcpy(mark, at, sizeof mark);
            if (mark[0] == COPY_MARK && mark[1] == (uint64_t)(uintptr_t)n->copy) {
                pn = &n->next;
                continue;
            }
            copy_prefix *pre = (copy_prefix *)n->copy - 1;
            pre->magic = 0;
            live_handle_remove((uint64_t)(uintptr_t)n->copy);
            if (ndead == cap_dead) {
                cap_dead = cap_dead ? cap_dead * 2 : 1024;
                dead = (dead_copy *)realloc(dead, cap_dead * sizeof *dead);
            }
            dead[ndead].p = pre;
            dead[ndead].after = after;
            ndead++;
            *pn = n->next;
            free(n);
            ncopies--;
            swept++;
        }
    }
    static int logged;
    if (swept && logged++ < 12) hos_log("nvn: %zu copies of recordings whose control memory was reused let go (%zu live)", swept, ncopies);
}
NV(nvnCommandBufferAddControlMemory) {
    nvn_cmd *o = CB(P(0));
    if (list_ref()) {   /* (the range's recordings released: their bytes go once the replay is past what was handed in) */
        nvn_list_release(U64(1), U64(1) + U64(2), rx_nvn_pushed());
        nvn_list_drain(__atomic_load_n(&list_done, __ATOMIC_ACQUIRE));
    } else {
        release_copies(U64(1), U64(1) + U64(2));
    }
    o->ctl = U64(1);
    o->ctl_size = U64(2);
    o->ctl_used = 0;
}
NV(nvnCommandBufferGetControlMemorySize) { RET(CB(P(0))->ctl_size); }
NV(nvnCommandBufferGetControlMemoryUsed) { const nvn_cmd *o = CB(P(0)); RET(o->ctl_used < o->ctl_size ? o->ctl_used : o->ctl_size); }
NV(nvnCommandBufferGetControlMemoryFree) { const nvn_cmd *o = CB(P(0)); RET(o->ctl_size - (o->ctl_used < o->ctl_size ? o->ctl_used : o->ctl_size)); }
NV(nvnCommandBufferGetCommandMemorySize) { RET(CB(P(0))->game_size); }
NV(nvnCommandBufferGetCommandMemoryUsed) { const nvn_cmd *o = CB(P(0)); RET(o->hw_used < o->game_size ? o->hw_used : o->game_size); }
NV(nvnCommandBufferGetCommandMemoryFree) { const nvn_cmd *o = CB(P(0)); RET(o->game_size - (o->hw_used < o->game_size ? o->hw_used : o->game_size)); }

NV(nvnCommandBufferBeginRecording,
   reads(x29, "the frame pointer kept with the list's info as a log label (runtime/render_lists.c gpu_classify)")) {
    nvn_cmd *o = CB(P(0));
    if (o->recording) hos_log("nvn: BeginRecording while recording");
    o->recording = 1;
    if (hos_bench_on()) hos_bench_recording(1);
    cmd_rec *r = cmd_host(o);
    if (list_ref()) nvn_list_open(&r->lw, h2g(o));   /* (its own allocation: no copy at its end) */
    else r->cur = r->seg;
    if (rx_direct()) {   /* (direct mode: the back end's list, its kind from the caller; then BEGIN as below) */
        gpu_flush();
        if (r->dl) {   /* (begun again unended: dropped, as the records) */
            void *dl = r->dl;
            r->dl = NULL;
            rx_direct()->list_released(dl);
        }
        gpu_list_info info = {0};
        gpu_classify(c, &info);
        info.cmd = h2g(o);
        if (info.kind == GPU_LIST_JOB && !game_job_list(c->x[0], &info.pass_key, &info.view)) info.pass_key = info.view = 0;
        r->dl = rx_direct()->list_begin(&info);
        o->rec_start = o->cmd_used;
        uint8_t *p = cmd_alloc(o, 8);
        put_hdr(p, NOP_BEGIN, 8);
        o->hw_used -= 8;   /* BEGIN / END have no hardware equivalent */
        o->hw_start = o->hw_used;
        return;
    }
    o->rec_start = o->cmd_used;
    uint8_t *p = cmd_alloc(o, 8);
    put_hdr(p, NOP_BEGIN, 8);
    o->hw_used -= 8;   /* BEGIN / END have no hardware equivalent */
    o->hw_start = o->hw_used;
}
/* the recording, BEGIN to END, is contiguous in the command buffer's segment: one copy of it */
static uint8_t *flatten(const uint8_t *p, size_t n) {
    unsigned cls;
    uint8_t *out = (uint8_t *)pool_get(sizeof(copy_prefix) + n, &cls);
    if (!out) die("recording copy (%zu bytes)", n);
    memcpy(out + sizeof(copy_prefix), p, n);
    copy_prefix *pre = (copy_prefix *)out;
    pre->cls = cls;
    pre->magic = COPY_MAGIC;
    pre->bytes = sizeof(copy_prefix) + n;
    os_atomic_add(&copy_bytes, (int64_t)pre->bytes);
    return out + sizeof(copy_prefix);
}
/* (the watches below) the guest's call chain, by frame pointers: "lr < caller < ..." */
static void guest_chain(Ctx *c, char *b, size_t cap) {
    uint64_t lr = c->x[30], fp = c->x[29];
    int n = snprintf(b, cap, "%llx", (unsigned long long)lr);
    for (int k = 1; k < 12 && fp && fp < 0x8000000000ull && !(fp & 7) && n < (int)cap; k++)
        n += snprintf(b + n, cap - n, " < %llx", (unsigned long long)G64(fp + 8)), fp = G64(fp);
}
/* (nvn.watch_draw: watch_draw below) */
static void *watch_draw_cmd;
static uint64_t watch_handles[16];
static int nwatch_handles;
static void watch_handle_called(Ctx *c, uint64_t h, const char *how) {
    static volatile int32_t logged;
    for (int i = 0; i < nwatch_handles; i++) {
        if (watch_handles[i] != h || os_atomic_inc(&logged) > 60) continue;
        char b[400];
        guest_chain(c, b, sizeof b);
        hos_log("nvn: watched recording %llx %s at %.3f s from %s", (unsigned long long)h, how, hos_now_ns() * 1e-9, b);
    }
}
/* (direct mode) the recording as it is is the handle's: filed by its control memory position, no copy, no lock but
   its bucket run's */
static uint64_t end_recording_ref(nvn_cmd *o) {
    cmd_rec *r = cmd_host(o);
    const uint64_t at = o->ctl + (o->ctl_used < o->ctl_size ? o->ctl_used : 0);
    o->ctl_used += 64;
    const int marked = o->ctl && o->ctl_used <= o->ctl_size;
    uint64_t h = 0;
    nvn_list_end(&r->lw, o->ctl ? at : ~0ull, marked, &h);
    if (marked) {   /* (the mark in the descriptor's place: see sweep_overwritten_copies) */
        const uint64_t mark[2] = {COPY_MARK, h};
        memcpy(gw(at, sizeof mark), mark, sizeof mark);
    }
    static volatile int64_t made;
    const int64_t k = os_atomic_inc(&made);
    if (k % 25000 == 0) {
        const size_t swept = nvn_list_sweep(COPY_MARK, rx_nvn_pushed(), guest_or_null);
        static int logged;
        if (swept && logged++ < 12) hos_log("nvn: %zu recordings whose control memory was reused let go", swept);
    }
    if (k % 50000 == 0) list_log("every 50000 recordings");
    return h;
}
/* (direct mode) the recording's end: END to the back end, its list in the handle's user; the handle as with lists by reference */
static uint64_t end_recording_direct(nvn_cmd *o) {
    cmd_rec *r = cmd_host(o);
    uint8_t *p = cmd_alloc(o, 8);
    put_hdr(p, NOP_END, 8);
    o->hw_used -= 8;
    o->recording = 0;
    gpu_flush();
    if (__atomic_load_n(&r->dl_pending, __ATOMIC_ACQUIRE)) {   /* (a record of it waits on another thread: it arrives late) */
        static volatile int32_t n;
        if (os_atomic_inc(&n) <= 8) hos_log("nvn: direct mode: a recording ended with a record of it pending on another thread");
    }
    void *dl = r->dl;
    r->dl = NULL;
    if (dl) rx_direct()->list_end(dl);
    if (hos_bench_on()) hos_bench_recording(0);
    r->lw.user = dl;
    r->lw.flat_extra = o->cmd_used - o->rec_start;   /* (no bytes: flat is the accounting, what a call of it adds) */
    return end_recording_ref(o);
}
NV(nvnCommandBufferEndRecording) {
    nvn_cmd *o = CB(P(0));
    if (rx_direct()) {
        RET(end_recording_direct(o));
        return;
    }
    uint8_t *p = cmd_alloc(o, 8);
    put_hdr(p, NOP_END, 8);
    o->hw_used -= 8;
    o->recording = 0;
    cmd_rec *r = cmd_host(o);
    uint8_t *copy = flatten(r->seg, (size_t)(r->cur - r->seg));
    /* the copy is tied to the control memory position the console driver's descriptor would take */
    const uint64_t at = o->ctl + (o->ctl_used < o->ctl_size ? o->ctl_used : 0);
    o->ctl_used += 64;
    rec_node *n = (rec_node *)malloc(sizeof *n);
    n->ctl = o->ctl ? at : ~0ull;
    n->copy = copy;
    n->marked = 0;
    if (o->ctl && o->ctl_used <= o->ctl_size) {   /* (the mark in the descriptor's place: see sweep_overwritten_copies) */
        const uint64_t mark[2] = {COPY_MARK, (uint64_t)(uintptr_t)copy};
        memcpy(gw(at, sizeof mark), mark, sizeof mark);
        n->marked = 1;
    }
    os_rwlock_lock(&copy_lock);
    if (!rec_buckets) {
        rec_shift = (unsigned)setting_int(SET_NVN_REC_SHIFT);
        rec_buckets = (rec_node **)calloc(REC_BUCKETS, sizeof *rec_buckets);
    }
    const unsigned b = rec_bucket(n->ctl);
    n->next = rec_buckets[b];
    rec_buckets[b] = n;
    ncopies++;
    live_handle_add((uint64_t)(uintptr_t)copy);
    static uint64_t made;
    if (++made % 25000 == 0) sweep_overwritten_copies();
    if (made % 50000 == 0)
        hos_log("nvn: %llu recordings made, %zu live copies (%.1f MB), %zu texture copies; %lld command buffers' segments "
                "(%.1f MB)", (unsigned long long)made, ncopies, copy_bytes / 1048576.0, npins, (long long)seg_count,
                seg_bytes / 1048576.0);
    os_rwlock_unlock(&copy_lock);
    if (watch_draw_cmd && watch_draw_cmd == (void *)o && nwatch_handles < 16) {
        watch_handles[nwatch_handles++] = (uint64_t)(uintptr_t)copy;
        watch_draw_cmd = NULL;
        hos_log("nvn: watched draw's recording is %llx", (unsigned long long)(uintptr_t)copy);
    }
    RET((uint64_t)(uintptr_t)copy);
}
NV(nvnCommandBufferIsRecording) { RET(CB(P(0))->recording != 0); }
static void check_handle(uint64_t h) {
    os_rwlock_lock_shared(&copy_lock);
    const int live = live_handle_has(h);
    os_rwlock_unlock_shared(&copy_lock);
    if (!live || ((const copy_prefix *)(uintptr_t)h - 1)->magic != COPY_MAGIC)
        die("command handle %#llx is not a live recording", (unsigned long long)h);
}
/* a called recording is copied into the caller at the call (it is complete now; the game may retire it before the
   caller is submitted) */
static void inline_recording(nvn_cmd *o, uint64_t h) {
    check_handle(h);
    const copy_prefix *pre = (const copy_prefix *)(uintptr_t)h - 1;
    const size_t total = (size_t)pre->bytes - sizeof *pre;   /* BEGIN, the records, END: the records in one copy */
    if (total <= 16) return;
    const size_t n = total - 16;
    cmd_rec *r = cmd_host(o);
    cmd_reserve_rec(r, n);
    memcpy(r->cur, (const uint8_t *)(uintptr_t)h + 8, n);
    r->cur += n;
    o->cmd_used += n;
    o->hw_used += n;
    o->last_rec = 0;
}
/* (direct mode) CallCommands / CopyCommands: the callees' lists to the back end, in order; the accounting as with lists by
   reference (the callee's flat size, what the game sizes its heaps from) */
static void call_direct(nvn_cmd *o, const uint64_t *h, int n) {
    gpu_flush();
    void *users[64];
    cmd_rec *r = cmd_host(o);
    for (int i = 0; i < n; i += 64) {
        uint32_t k = 0;
        for (int j = i; j < n && j < i + 64; j++) {
            const nvn_lrec *l = nvn_list_lookup(h[j]);
            if (!l) die("command handle %#llx is not a live recording", (unsigned long long)h[j]);
            if (l->flat > 16) {
                o->cmd_used += l->flat - 16;
                o->hw_used += l->flat - 16;
                o->last_rec = 0;
            }
            if (l->user) users[k++] = l->user;
        }
        if (r->dl && k) rx_direct()->list_call(r->dl, users, k);
    }
}
NV(nvnCommandBufferCallCommands,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) {
    const uint64_t *h = (const uint64_t *)PN(2, 8ull * (uint32_t)I(1));
    if (rx_direct()) {
        call_direct(CB(P(0)), h, I(1));
        return;
    }
    for (int i = 0; i < I(1); i++) {
        if (nwatch_handles) watch_handle_called(c, h[i], "called");
        inline_recording(CB(P(0)), h[i]);
    }
}
NV(nvnCommandBufferCopyCommands,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) {
    const uint64_t *h = (const uint64_t *)PN(2, 8ull * (uint32_t)I(1));
    if (rx_direct()) {
        call_direct(CB(P(0)), h, I(1));
        return;
    }
    for (int i = 0; i < I(1); i++) {
        if (nwatch_handles) watch_handle_called(c, h[i], "copied");
        inline_recording(CB(P(0)), h[i]);
    }
}

/* records: payload after the header, zeroed */
static void *rec(nvn_cmd *o, uint32_t op, size_t payload) {
    const uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(o, OP, T) ((T *)rec((o), (OP), sizeof(T)))
#define RECN(o, OP, T, ARR, N) ((T *)rec((o), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))

NV(nvnCommandBufferSignalEvent) {
    {   /* (the GPU's signals as recorded, 40 then every 5000) */
        static volatile int32_t n;
        const int32_t k = os_atomic_inc(&n);
        if (lognvn() && (k <= 40 || k % 5000 == 0))
            hos_log("nvn: CommandBufferSignalEvent #%ld: event %#llx mode %u value %u (lr %#llx)", (long)k,
                    (unsigned long long)((const nvn_event *)P(1))->gpu, U32(2), U32(5), (unsigned long long)c->x[30]);
    }
    nvn_p_signal *s = REC(CB(P(0)), NOP_SIGNAL_EVENT, nvn_p_signal);
    s->gpu = ((const nvn_event *)P(1))->gpu;
    s->mode = U32(2);
    s->value = U32(5);
}
/* recorded: the Vulkan back end replays the game's queues on one, and a command buffer that waits for another queue's
   event goes on once that queue's work has signalled it (vk_replay.cpp, submit_job). MK World's worker queue and its
   presentation queue pass stages back and forth this way several times a frame. (Logged, 40 calls then every 5000) */
NV(nvnCommandBufferWaitEvent) {
    static volatile int32_t n;
    const int32_t k = os_atomic_inc(&n);
    const nvn_event *e = (const nvn_event *)P(1);
    if (lognvn() && (k <= 40 || k % 5000 == 0)) {
        uint32_t now = 0;
        if (e) nvn_gpu_read(e->gpu, &now, 4);
        hos_log("nvn: CommandBufferWaitEvent #%ld: event %#llx mode %u value %u (now %u) (lr %#llx)", (long)k,
                (unsigned long long)(e ? e->gpu : 0), U32(2), U32(3), now, (unsigned long long)c->x[30]);
    }
    if (!e) return;
    nvn_p_wait_event *w = REC(CB(P(0)), NOP_WAIT_EVENT, nvn_p_wait_event);
    w->gpu = e->gpu, w->mode = U32(2), w->value = U32(3);
}
NV(nvnCommandBufferWaitSync) {
    static volatile int32_t n;
    const int32_t k = os_atomic_inc(&n);
    if (lognvn() && (k <= 40 || k % 5000 == 0)) hos_log("nvn: CommandBufferWaitSync #%ld (lr %#llx)", (long)k, (unsigned long long)c->x[30]);
}
/* (runtime) nvnCommandBufferPush{Compute,Graphics}Constants(cmdbuf, stage, offset, size, const void *data): push constants.
   The NVN2 driver sends at most 0x100 bytes, in whole words, and nothing below one word. Compute programs read them from
   their driver bank (c0) after the workgroup counts: OoT pushes a colour matrix (0x40 bytes) for the shader that turns its
   movie's NV12 planes into RGBA. Graphics ones go to the stage's root table 0 (3D method 0x504, its first word the stage's
   low 3 bits and the offset's low 8), which Ampere shaders read as bank 24: DKB pushes a target's (1/w, 1/h, w, h) to the
   vertex stage for its fullscreen passes. Only that use is known; another stage or offset stops the game until its
   numbering and unit are known. Copied at record time, as UpdateUniformBuffer; the replay keeps them in the list's
   state (renderer/vk_compute.cpp, renderer/vk_state.cpp). */
static void push_const(Ctx *c, uint32_t stage, uint32_t off) {
    const uint32_t n = (U32(3) < 0x100 ? U32(3) : 0x100) & ~3u;
    if (!n) return;
    if (!P(4)) die("push constants from a null pointer (lr %#llx)", (unsigned long long)c->x[30]);
    nvn_cmd *o = CB(P(0));
    const uint32_t sz = (uint32_t)(8 + sizeof(nvn_p_push_const) + n);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, NOP_PUSH_CONST, (sz + 7) & ~7u);
    nvn_p_push_const u = {stage, off, n};
    memcpy(p + 8, &u, sizeof u);
    memcpy(p + 8 + sizeof u, PN(4, n), n);
}
NV(nvnCommandBufferPushComputeConstants) {
    if (U64(2) > 0x400) die("compute push constants at offset %#llx (lr %#llx)", (unsigned long long)U64(2), (unsigned long long)c->x[30]);
    push_const(c, 5, (uint32_t)U64(2));
}
NV(nvnCommandBufferPushGraphicsConstants) {
    if ((U32(1) & 7) != 0 || (U32(2) & 0xff) != 0)
        die("graphics push constants for stage %u at offset %#x: only the vertex stage at 0 is known (lr %#llx)", U32(1) & 7,
            U32(2) & 0xff, (unsigned long long)c->x[30]);
    push_const(c, 0, 0);
}
/* (runtime) nvnQueueWaitSync(queue, sync): the queue's later work waits on the GPU for the sync's fence. Every
   NVN queue replays in one stream in submit order, so a fence submitted before this already came first; a sync not
   fenced yet (a wait on a fence another queue submits later) cannot be honored that way and is logged. */
NV(nvnQueueWaitSync) {
    const nvn_sync *s = (const nvn_sync *)P(1);
    static volatile int32_t n;
    if (s && !s->fenced && os_atomic_inc(&n) <= 8)
        hos_log("nvn: QueueWaitSync on a sync not fenced yet (%p, lr %#llx): not waited for", (void *)s,
                (unsigned long long)c->x[30]);
    /* (direct mode) the queue's next submission waits for the fence's (renderer/vk_sync.cpp) */
    if (rx_direct() && s && s->fenced == 1 && s->fence) rx_direct()->queue_wait_fence((uint64_t)(uintptr_t)P(0), s->fence);
}
NV(nvnCommandBufferResetCounter) { REC(CB(P(0)), NOP_RESET_COUNTER, nvn_p_counter)->type = U32(1); }
/* (conditions are read by the back end, in the stream's order: read at record, most of them read reports earlier in the same
   stream and gate single lights, so an early read left the scene white or threw white blobs over it) */
NV(nvnCommandBufferReportCounter) {
    nvn_p_counter *r = REC(CB(P(0)), NOP_REPORT_COUNTER, nvn_p_counter);
    r->type = U32(1);
    {   /* (which counters the game reads: the first report of each type, with its caller) */
        static volatile int32_t seen[64];
        /* (read first: an exchange at every report is a locked write to a line every recording thread shares) */
        if (U32(1) < 64 && !seen[U32(1)] && !os_atomic_xchg(&seen[U32(1)], 1))
            hos_log("nvn: the game reports counter type %u (lr %#llx)", U32(1), (unsigned long long)c->x[30]);
    }
    r->addr = U64(2);
}
/* (runtime) nvnCommandBufferClearBuffer(cmdbuf, BufferAddress dst, size_t size, uint32_t value): OoT's gameplay
   clears small GPU buffers (counters, 8 bytes) with it */
NV(nvnCommandBufferClearBuffer) {
    nvn_p_clear_buf *r = REC(CB(P(0)), NOP_CLEAR_BUF, nvn_p_clear_buf);
    r->dst = U64(1), r->size = U64(2), r->value = U32(3), r->pad = 0;
}
NV(nvnCommandBufferCopyBufferToBuffer) {
    nvn_p_copy_bb *r = REC(CB(P(0)), NOP_COPY_B2B, nvn_p_copy_bb);
    r->src = U64(1), r->dst = U64(2), r->size = U64(3);
}
/* the data is copied at record time (the caller may change it afterwards); written in GPU order at replay */
NV(nvnCommandBufferUpdateUniformBuffer) {
    const uint64_t n = U64(4);
    /* (runtime) NVN's limit is 64 KiB an update; a larger size is not recorded (its record size is 32-bit) */
    if (n > 0x10000) die("UpdateUniformBuffer of %#llx bytes", (unsigned long long)n);
    nvn_cmd *o = CB(P(0));
    const uint32_t sz = (uint32_t)(8 + sizeof(nvn_p_update_ub) + n);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, NOP_UPDATE_UB, (sz + 7) & ~7u);
    nvn_p_update_ub u = {U64(1) + U64(3), n};
    memcpy(p + 8, &u, sizeof u);
    memcpy(p + 8 + sizeof u, PN(5, n), n);
}

static void set_view(nvn_view *dst, uint8_t *has, uint64_t v) {
    *has = v != 0;
    if (v) memcpy(dst, gw(v, sizeof *dst), sizeof *dst);
}
static void tex_copy(const nvn_tex **dst, uint64_t t);
/* nvn.watch_rt=<guest hex> (debugging): who sets that render target, as guest call chains (each new one once),
   with the texture's description and view */
static void watch_rt(Ctx *c, const uint64_t *ct, const uint64_t *cv, int n) {
    static uint64_t want = ~0ull;
    if (want == ~0ull) want = strtoull(setting_str(SET_NVN_WATCH_RT), NULL, 16);
    if (!want || !ct) return;
    for (int i = 0; i < n; i++) {
        if (!ct[i] || nvn_gpu_guest(((const nvn_tex *)GP(ct[i]))->gpu) != want) continue;
        char b[400];
        guest_chain(c, b, sizeof b);
        static uint64_t seen[64];
        static int nseen;
        uint64_t key = 0;
        for (const char *q = b; *q; q++) key = key * 0x100000001B3ull ^ (uint8_t)*q;
        for (int k = 0; k < nseen; k++) if (seen[k] == key) return;
        if (nseen < 64) seen[nseen++] = key;
        const nvn_tex *t = (const nvn_tex *)GP(ct[i]);
        const nvn_view *v = cv && cv[i] ? (const nvn_view *)GP(cv[i]) : NULL;
        hos_log("nvn: render target %llx (target %d format %#x %ux%ux%u levels %u; view format %d:%#x layers %d:%u+%u) "
                "set from %s", (unsigned long long)want, t->d.target, t->d.format, t->d.width, t->d.height, t->d.depth,
                t->d.levels, v ? v->set_format : -1, v ? v->format : 0, v ? v->set_layers : -1, v ? v->min_layer : 0,
                v ? v->nlayers : 0, b);
        return;
    }
}
NV(nvnCommandBufferSetRenderTargets,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) {
    nvn_cmd *o = CB(P(0));
    int n = I(1);
    if (n < 0 || n > 8) die("SetRenderTargets with %d targets", n);
    const uint64_t *ct = (const uint64_t *)PN(2, 8 * (size_t)n), *cv = (const uint64_t *)PN(3, 8 * (size_t)n);
    watch_rt(c, ct, cv, n);
    nvn_p_set_rt *r = REC(o, NOP_SET_RT, nvn_p_set_rt);
    r->n = n;
    for (int i = 0; i < n; i++) {
        tex_copy(&r->color[i], ct ? ct[i] : 0);
        set_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : 0);
    }
    tex_copy(&r->depth, U64(4));
    set_view(&r->dview, &r->has_dview, U64(5));
}
NV(nvnCommandBufferClearColor) {
    nvn_p_clear_color *r = REC(CB(P(0)), NOP_CLEAR_COLOR, nvn_p_clear_color);
    r->index = I(1);
    memcpy(r->rgba, PN(2, 16), 16);
    r->mask = I(3);
}
NV(nvnCommandBufferClearDepthStencil) {
    nvn_p_clear_ds *r = REC(CB(P(0)), NOP_CLEAR_DS, nvn_p_clear_ds);
    r->depth = F(0);
    r->depth_mask = B(1);
    r->stencil = I(2);
    r->stencil_mask = I(3);
}
NV(nvnCommandBufferSetViewports) {
    const int first = I(1), n = I(2);
    if (n < 0 || first < 0 || first + n > 16) die("SetViewports %d+%d", first, n);
    nvn_p_viewports *r = REC(CB(P(0)), NOP_VIEWPORTS, nvn_p_viewports);
    r->first = first, r->n = n;
    memcpy(r->v, PN(3, 16 * (size_t)n), 16 * (size_t)n);
}
NV(nvnCommandBufferSetViewport) {
    nvn_p_viewports *r = REC(CB(P(0)), NOP_VIEWPORTS, nvn_p_viewports);
    r->first = 0, r->n = 1;
    r->v[0][0] = (float)I(1), r->v[0][1] = (float)I(2), r->v[0][2] = (float)I(3), r->v[0][3] = (float)I(4);
}
NV(nvnCommandBufferSetScissor) {
    nvn_p_scissor *r = REC(CB(P(0)), NOP_SCISSOR, nvn_p_scissor);
    r->x = I(1), r->y = I(2), r->w = I(3), r->h = I(4);
}
/* one scissor and depth range are replayed (index 0); differing others are reported */
NV(nvnCommandBufferSetScissors) {
    const int first = I(1), n = I(2);
    const int *v = (const int *)P(3);
    for (int i = 1; i < n; i++)
        if (memcmp(v + 4 * i, v, 16)) { static int once; if (!once++) hos_log("nvn: SetScissors: scissor %d differs (index 0 used)", first + i); }
    if (first == 0 && n > 0) {
        nvn_p_scissor *r = REC(CB(P(0)), NOP_SCISSOR, nvn_p_scissor);
        r->x = v[0], r->y = v[1], r->w = v[2], r->h = v[3];
    }
}
NV(nvnCommandBufferSetDepthRange) {
    nvn_p_depth_range *r = REC(CB(P(0)), NOP_DEPTH_RANGE, nvn_p_depth_range);
    r->n = F(0), r->f = F(1);
}
NV(nvnCommandBufferSetDepthRanges) {
    const int first = I(1), n = I(2);
    const float *v = (const float *)P(3);
    if (first == 0 && n > 0) {
        nvn_p_depth_range *r = REC(CB(P(0)), NOP_DEPTH_RANGE, nvn_p_depth_range);
        r->n = v[0], r->f = v[1];
    }
}
NV(nvnCommandBufferSetDepthClamp) { REC(CB(P(0)), NOP_DEPTH_CLAMP, nvn_p_flag)->value = B(1) != 0; }
NV(nvnCommandBufferSetRenderEnable) { REC(CB(P(0)), NOP_RENDER_ENABLE, nvn_p_flag)->value = B(1) != 0; }
/* rendering on when the two NVNcounterData.counter values at addr, addr + 16 are equal (mode 0) / differ (1). TOTK gates
   clears with it: the CPU writes counter = 1 to ask for a clear and the TIMESTAMP_TOP report after it resets it */
NV(nvnCommandBufferSetRenderEnableConditional) {
    if (I(1) != 0 && I(1) != 1) die("SetRenderEnableConditional mode %d", I(1));
    nvn_p_render_cond *r = REC(CB(P(0)), NOP_RENDER_COND, nvn_p_render_cond);
    r->mode = I(1);
    r->addr = U64(2);
    r->snapped = 0;
    r->x = r->y = 0;
}
NV(nvnCommandBufferSetRasterizerDiscard) { REC(CB(P(0)), NOP_RASTER_DISCARD, nvn_p_flag)->value = B(1) != 0; }
NV(nvnCommandBufferBindProgram) {
    nvn_p_bind_program *r = REC(CB(P(0)), NOP_BIND_PROGRAM, nvn_p_bind_program);
    r->prog = (const nvn_prog *)P(1);
    r->stages = I(2);
}
static void bind_state(Ctx *c, int kind, size_t n) {
    nvn_p_bind_state *r = REC(CB(P(0)), NOP_BIND_STATE, nvn_p_bind_state);
    r->kind = kind;
    memcpy(r->bytes, PN(1, n), n);
}
NV(nvnCommandBufferBindBlendState) { bind_state(c, 0, sizeof(nvn_blend_st)); }
NV(nvnCommandBufferBindColorState) { bind_state(c, 1, sizeof(nvn_color_st)); }
NV(nvnCommandBufferBindChannelMaskState) { bind_state(c, 2, sizeof(nvn_chmask_st)); }
NV(nvnCommandBufferBindMultisampleState) { bind_state(c, 3, sizeof(nvn_ms_st)); }
NV(nvnCommandBufferBindPolygonState) { bind_state(c, 4, sizeof(nvn_poly_st)); }
NV(nvnCommandBufferBindDepthStencilState) { bind_state(c, 5, sizeof(nvn_ds_st)); }
NV(nvnCommandBufferBindVertexAttribState) {
    const int n = I(1);
    if (n < 0 || n > 16) die("BindVertexAttribState %d", n);
    nvn_cmd *o = CB(P(0));
    nvn_p_bind_vattr *r = RECN(o, NOP_BIND_VATTR, nvn_p_bind_vattr, st, n);
    r->first = 0, r->n = n;
    memcpy(r->st, PN(2, 4 * (size_t)n), 4 * (size_t)n);
    hw_fix(o, 68);   /* the driver writes all 16 attribute formats: 17 words */
}
NV(nvnCommandBufferBindVertexStreamState) {
    const int n = I(1);
    if (n < 0 || n > 16) die("BindVertexStreamState %d", n);
    nvn_cmd *o = CB(P(0));
    nvn_p_bind_vstream *r = RECN(o, NOP_BIND_VSTREAM, nvn_p_bind_vstream, st, n);
    r->first = 0, r->n = n;
    memcpy(r->st, PN(2, 8 * (size_t)n), 8 * (size_t)n);
    hw_fix(o, 4 * (16 + (uint64_t)n));
}
static void bind_vbufs(nvn_cmd *o, int first, int n, const NVNbufferRange *rg) {
    if (n < 0 || first < 0 || first + n > 16) die("BindVertexBuffers %d+%d", first, n);
    nvn_p_bind_vbuf *r = RECN(o, NOP_BIND_VBUF, nvn_p_bind_vbuf, r, n);
    r->first = first, r->n = n;
    memcpy(r->r, rg, sizeof *rg * (size_t)n);
    hw_fix(o, 24 * (uint64_t)n);
}
NV(nvnCommandBufferBindVertexBuffers) { bind_vbufs(CB(P(0)), I(1), I(2), (const NVNbufferRange *)P(3)); }
NV(nvnCommandBufferBindVertexBuffer) { NVNbufferRange r = {U64(2), U64(3)}; bind_vbufs(CB(P(0)), I(1), 1, &r); }
static void bind_buf(nvn_cmd *o, uint32_t op, int stage, int index, uint64_t addr, uint64_t size) {
    nvn_p_bind_buf *r = REC(o, op, nvn_p_bind_buf);
    r->stage = stage, r->index = index, r->addr = addr, r->size = size;
    hw_fix(o, 20);
}
NV(nvnCommandBufferBindUniformBuffer) {
    bind_buf(CB(P(0)), NOP_BIND_UBO, I(1), I(2), U64(3), U64(4));
}
NV(nvnCommandBufferBindStorageBuffer) { bind_buf(CB(P(0)), NOP_BIND_SSBO, I(1), I(2), U64(3), U64(4)); }
static void bind_bufs(Ctx *c, uint32_t op) {
    const NVNbufferRange *r = (const NVNbufferRange *)PN(4, I(3) > 0 ? sizeof *r * (uint64_t)I(3) : 0);
    for (int i = 0; i < I(3); i++)
        bind_buf(CB(P(0)), op, I(1), I(2) + i, r[i].address, r[i].size);
}
NV(nvnCommandBufferBindUniformBuffers) { bind_bufs(c, NOP_BIND_UBO); }
NV(nvnCommandBufferBindStorageBuffers) { bind_bufs(c, NOP_BIND_SSBO); }
/* deferred: the ranges are read from the game's array at submit (TOTK fills it for the frame just before) */
static void bind_bufs_deferred(Ctx *c, int ssbo) {
    const int n = I(3);
    if (n < 0 || n > 16) die("deferred bind of %d buffers", n);
    nvn_p_bind_bufs *r = RECN(CB(P(0)), NOP_BIND_BUFS, nvn_p_bind_bufs, r, n);
    r->ssbo = ssbo, r->stage = I(1), r->first = I(2), r->n = n, r->src = U64(4);
    memcpy(r->r, PN(4, sizeof(NVNbufferRange) * (size_t)n), sizeof(NVNbufferRange) * (size_t)n);
    if (r->src && list_ref() && !rx_direct()) nvn_list_note_deferred(&cmd_host(CB(P(0)))->lw, (const uint8_t *)r - 8);
    hw_fix(CB(P(0)), 16 + 16 * (uint64_t)n);
}
NV(nvnCommandBufferBindUniformBuffersDeferred) { bind_bufs_deferred(c, 0); }
NV(nvnCommandBufferBindStorageBuffersDeferred) { bind_bufs_deferred(c, 1); }
static void bind_handles(nvn_cmd *o, uint32_t op, int stage, int first, int n, const uint64_t *h, uint64_t src,
                         uint64_t hw) {
    if (n < 0 || n > 32) die("bind of %d handles", n);
    nvn_p_bind_handles *r = RECN(o, op, nvn_p_bind_handles, h, n);
    r->stage = stage, r->first = first, r->n = n, r->src = src;
    memcpy(r->h, h, 8 * (size_t)n);
    if (src && list_ref() && !rx_direct()) nvn_list_note_deferred(&cmd_host(o)->lw, (const uint8_t *)r - 8);
    hw_fix(o, hw ? hw : 16 + 4 * (uint64_t)n);
}
/* nvn.watch_tex=<handle hex> (debugging): who binds that texture handle, as guest call chains (frame pointers),
   each new chain logged once */
static void watch_tex(Ctx *c, const uint64_t *h, int n) {
    static uint64_t want = ~0ull;
    if (want == ~0ull) want = strtoull(setting_str(SET_NVN_WATCH_TEX), NULL, 16);
    if (!want) return;
    for (int i = 0; i < n; i++) {
        if ((h[i] & 0xFFFFFFFFull) != want) continue;
        uint64_t chain[8] = {c->x[30]}, fp = c->x[29];
        for (int k = 1; k < 8 && fp && fp < 0x8000000000ull && !(fp & 7); k++) chain[k] = G64(fp + 8), fp = G64(fp);
        static uint64_t seen[64];
        static int nseen;
        uint64_t key = 0;
        for (int k = 0; k < 8; k++) key = key * 0x9E3779B97F4A7C15ull + chain[k];
        for (int k = 0; k < nseen; k++) if (seen[k] == key) return;
        if (nseen < 64) seen[nseen++] = key;
        hos_log("nvn: texture %llx bound from %llx < %llx < %llx < %llx < %llx < %llx < %llx < %llx",
                (unsigned long long)want, (unsigned long long)chain[0], (unsigned long long)chain[1],
                (unsigned long long)chain[2], (unsigned long long)chain[3], (unsigned long long)chain[4],
                (unsigned long long)chain[5], (unsigned long long)chain[6], (unsigned long long)chain[7]);
        return;
    }
}
NV(nvnCommandBufferBindTexture,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) {
    const uint64_t h = U64(3); watch_tex(c, &h, 1); bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), 1, &h, 0, 0);
}
NV(nvnCommandBufferBindTextures,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) {
    watch_tex(c, (const uint64_t *)P(4), I(3));
    bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), (const uint64_t *)P(4), 0, 0);
}
NV(nvnCommandBufferBindImage) { const uint64_t h = U64(3); bind_handles(CB(P(0)), NOP_BIND_IMG, I(1), I(2), 1, &h, 0, 24); }
NV(nvnCommandBufferBindImages) { bind_handles(CB(P(0)), NOP_BIND_IMG, I(1), I(2), I(3), (const uint64_t *)P(4), 0, 16 + 8 * (uint64_t)I(3)); }
NV(nvnCommandBufferBindSeparateTexture) { const uint64_t h = U64(3); bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), 1, &h, 0, 24); }
NV(nvnCommandBufferBindSeparateTextures) { bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), (const uint64_t *)P(4), 0, 16 + 8 * (uint64_t)I(3)); }
NV(nvnCommandBufferBindSeparateSampler) { const uint64_t h = U64(3); bind_handles(CB(P(0)), NOP_BIND_SAMP, I(1), I(2), 1, &h, 0, 24); }
NV(nvnCommandBufferBindSeparateSamplers) { bind_handles(CB(P(0)), NOP_BIND_SAMP, I(1), I(2), I(3), (const uint64_t *)P(4), 0, 16 + 8 * (uint64_t)I(3)); }
/* NVN 55 *Deferred binds (what the inline fast paths call: always, see nvn_cmd.inline_cur): the console driver only
   records the game's handle array and reads it at submit; the game fills material tables after recording */
NV(nvnCommandBufferBindTexturesDeferred,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) {
    watch_tex(c, (const uint64_t *)P(4), I(3));
    bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), (const uint64_t *)P(4), U64(4), 0);
}
NV(nvnCommandBufferBindImagesDeferred) { bind_handles(CB(P(0)), NOP_BIND_IMG, I(1), I(2), I(3), (const uint64_t *)P(4), U64(4), 16 + 8 * (uint64_t)I(3)); }
NV(nvnCommandBufferBindSeparateTexturesDeferred) { bind_handles(CB(P(0)), NOP_BIND_TEX, I(1), I(2), I(3), (const uint64_t *)P(4), U64(4), 16 + 8 * (uint64_t)I(3)); }
NV(nvnCommandBufferBindSeparateSamplersDeferred) { bind_handles(CB(P(0)), NOP_BIND_SAMP, I(1), I(2), I(3), (const uint64_t *)P(4), U64(4), 16 + 8 * (uint64_t)I(3)); }
static void stencil(Ctx *c, int which) {
    nvn_p_stencil *r = REC(CB(P(0)), NOP_STENCIL, nvn_p_stencil);
    r->face = I(1), r->which = which;
    if (which == 0) r->value_mask = I(2);
    else if (which == 1) r->mask = I(2);
    else r->ref = I(2);
}
NV(nvnCommandBufferSetStencilValueMask) { stencil(c, 0); }
NV(nvnCommandBufferSetStencilMask) { stencil(c, 1); }
NV(nvnCommandBufferSetStencilRef) { stencil(c, 2); }
NV(nvnCommandBufferSetPolygonOffsetClamp) {
    nvn_p_poly_offset *r = REC(CB(P(0)), NOP_POLY_OFFSET, nvn_p_poly_offset);
    r->factor = F(0), r->units = F(1), r->clamp = F(2);
}
NV(nvnCommandBufferSetPrimitiveRestart) {
    nvn_p_restart *r = REC(CB(P(0)), NOP_PRIMITIVE_RESTART, nvn_p_restart);
    r->enable = B(1), r->index = I(2);
}
NV(nvnCommandBufferDrawArrays) {
    nvn_p_draw *r = REC(CB(P(0)), NOP_DRAW, nvn_p_draw);
    r->prim = I(1), r->first = I(2), r->count = I(3), r->instances = 1;
}
NV(nvnCommandBufferDrawArraysInstanced) {
    if (I(5) <= 0) return;   /* no instances: nothing */
    nvn_p_draw *r = REC(CB(P(0)), NOP_DRAW, nvn_p_draw);
    r->prim = I(1), r->first = I(2), r->count = I(3), r->base_instance = I(4), r->instances = I(5);
}
/* nvn.watch_draw=<index count> (debugging): who records indexed draws of that size (the first 60, call chains);
   the recording that holds one is followed: each call of it is logged (the first 60) */
static void watch_draw(Ctx *c, int count) {
    static int want = -1;
    static int64_t after;
    static volatile int32_t logged;
    if (want < 0) {   /* (<count>,<after s>: from that much time on) */
        const char *e = setting_str(SET_NVN_WATCH_DRAW);
        want = atoi(e);
        after = hos_now_ns() + (strchr(e, ',') ? (int64_t)(atof(strchr(e, ',') + 1) * 1e9) : 0);
    }
    if (!want || count != want || hos_now_ns() < after) return;
    const char *only = *setting_str(SET_NVN_WATCH_DRAW_THREAD) ? setting_str(SET_NVN_WATCH_DRAW_THREAD) : NULL;   /* (only draws recorded on threads of that name) */
    if (only && (!hos_self() || !strstr(hos_thread_name(hos_self()), only))) return;
    if (os_atomic_inc(&logged) > 60) return;
    watch_draw_cmd = P(0);
    char b[400];
    guest_chain(c, b, sizeof b);
    hos_log("nvn: draw of %d indices recorded from %s", count, b);
}
static void draw_idx(Ctx *c, int base_vertex, int base_instance, int instances, uint64_t hw) {
    nvn_cmd *o = CB(P(0));
    watch_draw(c, I(3));
    nvn_p_draw_idx *r = REC(o, NOP_DRAW_IDX, nvn_p_draw_idx);
    r->prim = I(1), r->itype = I(2), r->count = I(3), r->ibuf = U64(4);
    r->base_vertex = base_vertex, r->base_instance = base_instance, r->instances = instances;
    hw_fix(o, hw);
}
NV(nvnCommandBufferDrawElements,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) {
    draw_idx(c, 0, 0, 1, 48);
}
NV(nvnCommandBufferDrawElementsBaseVertex,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) {
    draw_idx(c, I(5), 0, 1, 44);
}
NV(nvnCommandBufferDrawElementsInstanced,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) {
    if (I(7) > 0) draw_idx(c, I(5), I(6), I(7), 44);
}
NV(nvnCommandBufferDispatchCompute) {
    nvn_p_dispatch *r = REC(CB(P(0)), NOP_DISPATCH, nvn_p_dispatch);
    r->x = I(1), r->y = I(2), r->z = I(3);
}
NV(nvnCommandBufferBarrier) { *(int32_t *)rec(CB(P(0)), NOP_BARRIER, 8) = I(1); }
NV(nvnCommandBufferSetTexturePool) { REC(CB(P(0)), NOP_SET_TEX_POOL, nvn_p_set_pool)->pool = (const nvn_dpool *)P(1); }
NV(nvnCommandBufferSetSamplerPool) { REC(CB(P(0)), NOP_SET_SAMP_POOL, nvn_p_set_pool)->pool = (const nvn_dpool *)P(1); }
NV(nvnCommandBufferSetShaderScratchMemory) {}

/* Records keep textures by pointer to an immutable copy: the game may finalize or reuse a texture object after
   recording, before the submit. One copy per distinct content (the object with its serial left out): TOTK initializes
   its transient targets again every frame, the same texture under a new serial, and one copy per serial filled the
   table in a long session ("texture copies table full", 2026-10-20). The replay reads the copies' contents only.
   Copies live for the session (~100 bytes each); the table doubles when half full. */
static nvn_tex **pins;
static size_t pin_slots;
static os_mutex pin_lock = OS_MUTEX_INIT;
static uint64_t pin_hash(const nvn_tex *o) {   /* (FNV-1a over the object, its serial as 0) */
    nvn_tex k = *o;
    k.serial = 0;
    const uint8_t *p = (const uint8_t *)&k;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof k; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static int pin_same(const nvn_tex *a, const nvn_tex *b) {
    nvn_tex x = *a, y = *b;
    x.serial = y.serial = 0;
    return !memcmp(&x, &y, sizeof x);
}
static void pins_grow(void) {
    const size_t n = pin_slots ? pin_slots * 2 : (1u << 16);
    nvn_tex **t = (nvn_tex **)calloc(n, sizeof *t);
    if (!t) die("texture copies table (%zu slots)", n);
    for (size_t i = 0; i < pin_slots; i++)
        if (pins[i]) {
            size_t s = (size_t)(pin_hash(pins[i]) >> 20) & (n - 1);
            while (t[s]) s = (s + 1) & (n - 1);
            t[s] = pins[i];
        }
    free(pins);
    pins = t, pin_slots = n;
}
static void tex_copy(const nvn_tex **dst, uint64_t t) {
    if (!t) { *dst = NULL; return; }
    const nvn_tex *o = (const nvn_tex *)gw(t, sizeof(nvn_tex));
    if (!o->serial) die("recording refers to an uninitialized texture object %#llx", (unsigned long long)t);
    const uint64_t h = pin_hash(o);
    os_mutex_lock(&pin_lock);
    if (npins * 2 >= pin_slots) pins_grow();
    size_t s = (size_t)(h >> 20) & (pin_slots - 1);
    while (pins[s] && !pin_same(pins[s], o)) s = (s + 1) & (pin_slots - 1);
    if (!pins[s]) {
        pins[s] = (nvn_tex *)malloc(sizeof(nvn_tex));
        *pins[s] = *o;
        npins++;
    }
    *dst = pins[s];
    os_mutex_unlock(&pin_lock);
}
NV(nvnCommandBufferCopyBufferToTexture,
   reads(x29, "the caller's frame-pointer chain, walked for a log line (guest_chain, watch_*)")) {
    nvn_p_copy_bt *r = REC(CB(P(0)), NOP_COPY_B2T, nvn_p_copy_bt);
    r->buf = U64(1);
    tex_copy(&r->tex, U64(2));
    set_view(&r->view, &r->has_view, U64(3));
    r->r = *(const NVNcopyRegion *)P(4);
    r->flags = I(5);
    {   /* (runtime, nvn.log_b2t) who records the big copies: the caller's chain, each new source once */
        static int on = -1;
        if (on < 0) on = setting_bool(SET_NVN_LOG_B2T);
        static uint64_t seen[16];
        static volatile int32_t nseen;
        int k;
        for (k = 0; k < nseen && seen[k] != r->buf; k++) {}
        if (on && r->r.width >= 960 && k == nseen && nseen < 16) {
            seen[os_atomic_inc(&nseen) - 1] = r->buf;
            char b[400];
            guest_chain(c, b, sizeof b);
            hos_log("nvn: CopyBufferToTexture from %#llx (%dx%d): %s", (unsigned long long)r->buf, r->r.width, r->r.height, b);
        }
    }
}
NV(nvnCommandBufferCopyTextureToBuffer) {
    nvn_p_copy_bt *r = REC(CB(P(0)), NOP_COPY_T2B, nvn_p_copy_bt);
    tex_copy(&r->tex, U64(1));
    set_view(&r->view, &r->has_view, U64(2));
    r->r = *(const NVNcopyRegion *)P(3);
    r->buf = U64(4);
    r->flags = I(5);
}
NV(nvnCommandBufferCopyTextureToTexture) {
    nvn_p_copy_tt *r = REC(CB(P(0)), NOP_COPY_T2T, nvn_p_copy_tt);
    tex_copy(&r->src, U64(1));
    set_view(&r->sv, &r->has_sv, U64(2));
    r->sr = *(const NVNcopyRegion *)P(3);
    tex_copy(&r->dst, U64(4));
    set_view(&r->dv, &r->has_dv, U64(5));
    r->dr = *(const NVNcopyRegion *)P(6);
    r->flags = I(7);
}
static void clear_tex(Ctx *c, int is_uint) {
    nvn_p_clear_tex *r = REC(CB(P(0)), NOP_CLEAR_TEX, nvn_p_clear_tex);
    tex_copy(&r->tex, U64(1));
    set_view(&r->view, &r->has_view, U64(2));
    r->r = *(const NVNcopyRegion *)P(3);
    memcpy(r->c.f, PN(4, 16), 16);
    r->mask = I(5);
    r->is_uint = (uint8_t)is_uint;
}
NV(nvnCommandBufferClearTexture) { clear_tex(c, 0); }
NV(nvnCommandBufferClearTextureui) { clear_tex(c, 1); }
NV(nvnCommandBufferSetCopyRowStride) { REC(CB(P(0)), NOP_COPY_STRIDE, nvn_p_copy_stride)->stride = (int64_t)U64(1); }
/* (runtime) the buffer stride between layers of a 3D / array copy; OoT sets 0 (tightly packed, the default the
   replay assumes). A real stride is logged: the replay would need it. */
NV(nvnCommandBufferSetCopyImageStride) {
    static int once;
    if (U64(1) && !once++) hos_log("nvn: SetCopyImageStride(%lld): not replayed (layers copied tightly packed)", (long long)U64(1));
}
NV(nvnCommandBufferSetBlendColor) { memcpy(REC(CB(P(0)), NOP_BLEND_COLOR, nvn_p_blend_color)->rgba, PN(1, 16), 16); }

/* hints and state with no effect on the picture here: lines and points come out of the shaders, ZCull and the tiled
   cache are Tegra hardware; non-default values are reported once */
NV(nvnCommandBufferSetLineWidth) { if (F(0) != 1.0f) { static int once; if (!once++) hos_log("nvn: SetLineWidth %g: lines are one pixel", F(0)); } }
NV(nvnCommandBufferSetPointSize) {}
NV(nvnCommandBufferSetDepthBounds) {
    if (B(1) && (F(0) > 0.0f || F(1) < 1.0f)) { static int once; if (!once++) hos_log("nvn: SetDepthBounds [%g, %g]: no depth bounds test", F(0), F(1)); }
}
NV(nvnCommandBufferSetTiledCacheAction) {}
NV(nvnCommandBufferSetTiledCacheTileSize) {}
NV(nvnCommandBufferSetSubpixelPrecisionBias) {}
NV(nvnCommandBufferDiscardColor) {}
NV(nvnCommandBufferDiscardDepthStencil) {}
NV(nvnCommandBufferSaveZCullData) {}
NV(nvnCommandBufferRestoreZCullData) {}
NV(nvnCommandBufferSetAlphaRef) {}
NV(nvnCommandBufferSetSampleMask) {}
NV(nvnCommandBufferSetConservativeRasterEnable) {}
NV(nvnCommandBufferSetConservativeRasterDilate) {}
NV(nvnCommandBufferSetPatchSize) {}
NV(nvnCommandBufferPushDebugGroupStatic) {}
NV(nvnCommandBufferPushDebugGroupDynamic) {}
NV(nvnCommandBufferPushDebugGroup) {}
NV(nvnCommandBufferPopDebugGroup) {}
NV(nvnCommandBufferPopDebugGroupId) {}
NV(nvnCommandBufferInsertDebugMarkerStatic) {}
NV(nvnCommandBufferInsertDebugMarkerDynamic) {}
NV(nvnCommandBufferInsertDebugMarker) {}

/* ---- state objects: packed in place ---- */
#define ST(T, p) ((T *)(p))
NV(nvnBlendStateSetDefaults) {
    nvn_blend_st *b = ST(nvn_blend_st, P(0));
    memset(b, 0, sizeof *b);
    b->src_rgb = NVN_BLEND_FUNC_ONE, b->dst_rgb = NVN_BLEND_FUNC_ZERO, b->src_a = NVN_BLEND_FUNC_ONE, b->dst_a = NVN_BLEND_FUNC_ZERO;
    b->eq_rgb = NVN_BLEND_EQUATION_ADD, b->eq_a = NVN_BLEND_EQUATION_ADD;
}
NV(nvnBlendStateSetBlendTarget) { ST(nvn_blend_st, P(0))->target = U32(1); }
NV(nvnBlendStateSetBlendFunc) {
    nvn_blend_st *b = ST(nvn_blend_st, P(0));
    b->src_rgb = U32(1), b->dst_rgb = U32(2), b->src_a = U32(3), b->dst_a = U32(4);
}
NV(nvnBlendStateSetBlendEquation) { ST(nvn_blend_st, P(0))->eq_rgb = U32(1); ST(nvn_blend_st, P(0))->eq_a = U32(2); }
NV(nvnBlendStateSetAdvancedMode) { if (I(1)) die("advanced blend mode %d", I(1)); }
NV(nvnBlendStateSetAdvancedOverlap) {}
NV(nvnBlendStateSetAdvancedPremultipliedSrc) {}
NV(nvnBlendStateSetAdvancedNormalizedDst) {}
NV(nvnColorStateSetDefaults) {
    nvn_color_st *s = ST(nvn_color_st, P(0));
    memset(s, 0, sizeof *s);
    s->logic_op = NVN_LOGIC_OP_COPY;
    s->alpha_func = NVN_ALPHA_FUNC_ALWAYS;
}
NV(nvnColorStateSetBlendEnable) {
    nvn_color_st *s = ST(nvn_color_st, P(0));
    const int i = I(1);
    if (i < 0 || i > 7) return;
    s->blend_en = B(2) ? (s->blend_en | 1u << i) : (s->blend_en & ~(1u << i));
}
NV(nvnColorStateSetLogicOp) { ST(nvn_color_st, P(0))->logic_op = U32(1); }
NV(nvnColorStateSetAlphaTest) { ST(nvn_color_st, P(0))->alpha_func = U32(1); }
NV(nvnChannelMaskStateSetDefaults) { ST(nvn_chmask_st, P(0))->mask = 0xffffffffu; }
NV(nvnChannelMaskStateSetChannelMask) {
    static int chmask_log = -1;   /* (asked once) */
    if (chmask_log < 0) chmask_log = setting_bool(SET_NVN_LOG_CHMASK);
    if (chmask_log) {   /* (debugging: NVN2's arguments) */
        static volatile int32_t n;
        if (os_atomic_inc(&n) <= 40)
            hos_log("nvn: SetChannelMask(%#llx, %#llx, %#llx, %#llx, %#llx, %#llx) lr %#llx", (unsigned long long)c->x[0],
                    (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                    (unsigned long long)c->x[4], (unsigned long long)c->x[5], (unsigned long long)c->x[30]);
    }
    const int i = I(1);
    if (i < 0 || i > 7) return;
    const uint32_t m = (B(2) ? 1 : 0) | (B(3) ? 2 : 0) | (B(4) ? 4 : 0) | (B(5) ? 8 : 0);
    nvn_chmask_st *s = ST(nvn_chmask_st, P(0));
    s->mask = (s->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NV(nvnMultisampleStateSetDefaults) { memset(P(0), 0, sizeof(nvn_ms_st)); ST(nvn_ms_st, P(0))->enable = 1; }
NV(nvnMultisampleStateSetMultisampleEnable) { ST(nvn_ms_st, P(0))->enable = B(1) != 0; }
NV(nvnMultisampleStateSetSamples) { ST(nvn_ms_st, P(0))->samples = (uint8_t)I(1); }
NV(nvnMultisampleStateSetAlphaToCoverageEnable) { ST(nvn_ms_st, P(0))->a2c = B(1) != 0; }
NV(nvnMultisampleStateSetAlphaToCoverageDither) { ST(nvn_ms_st, P(0))->a2c_dither = B(1) != 0; }
NV(nvnMultisampleStateSetRasterSamples) { ST(nvn_ms_st, P(0))->raster_samples = (uint8_t)I(1); }
NV(nvnMultisampleStateSetSampleLocationsEnable) { ST(nvn_ms_st, P(0))->sl_en = B(1) != 0; }
NV(nvnMultisampleStateSetSampleLocations) {}
NV(nvnPolygonStateSetDefaults) {
    nvn_poly_st *s = ST(nvn_poly_st, P(0));
    memset(s, 0, sizeof *s);
    s->cull = NVN_FACE_NONE, s->front = NVN_FRONT_FACE_CCW, s->mode = NVN_POLYGON_MODE_FILL;
}
NV(nvnPolygonStateSetCullFace) { ST(nvn_poly_st, P(0))->cull = U32(1); }
NV(nvnPolygonStateSetFrontFace) { ST(nvn_poly_st, P(0))->front = U32(1); }
NV(nvnPolygonStateSetPolygonMode) { ST(nvn_poly_st, P(0))->mode = U32(1); }
NV(nvnPolygonStateSetPolygonOffsetEnables) { ST(nvn_poly_st, P(0))->offset_en = U32(1); }
NV(nvnDepthStencilStateSetDefaults) {
    nvn_ds_st *s = ST(nvn_ds_st, P(0));
    memset(s, 0, sizeof *s);
    s->dw = 1, s->df = NVN_DEPTH_FUNC_LESS, s->sf_f = s->sf_b = NVN_STENCIL_FUNC_ALWAYS;
    s->fail_f = s->zfail_f = s->zpass_f = s->fail_b = s->zfail_b = s->zpass_b = NVN_STENCIL_OP_KEEP;
}
NV(nvnDepthStencilStateSetDepthTestEnable) { ST(nvn_ds_st, P(0))->dt = B(1) != 0; }
NV(nvnDepthStencilStateSetDepthWriteEnable) { ST(nvn_ds_st, P(0))->dw = B(1) != 0; }
NV(nvnDepthStencilStateSetDepthFunc) { ST(nvn_ds_st, P(0))->df = U32(1); }
NV(nvnDepthStencilStateSetStencilTestEnable) { ST(nvn_ds_st, P(0))->st = B(1) != 0; }
NV(nvnDepthStencilStateSetStencilFunc) {
    nvn_ds_st *s = ST(nvn_ds_st, P(0));
    if (U32(1) & NVN_FACE_FRONT) s->sf_f = U32(2);
    if (U32(1) & NVN_FACE_BACK) s->sf_b = U32(2);
}
NV(nvnDepthStencilStateSetStencilOp) {
    nvn_ds_st *s = ST(nvn_ds_st, P(0));
    if (U32(1) & NVN_FACE_FRONT) s->fail_f = U32(2), s->zfail_f = U32(3), s->zpass_f = U32(4);
    if (U32(1) & NVN_FACE_BACK) s->fail_b = U32(2), s->zfail_b = U32(3), s->zpass_b = U32(4);
}
NV(nvnVertexAttribStateSetDefaults) { memset(P(0), 0, sizeof(nvn_va_st)); }
NV(nvnVertexAttribStateSetFormat) {
    if (U64(2) >= (1u << 19)) hos_log("nvn: vertex attribute offset %#llx too large", (unsigned long long)U64(2));
    ST(nvn_va_st, P(0))->fmt = U32(1);
    ST(nvn_va_st, P(0))->offset = (uint32_t)U64(2);
}
NV(nvnVertexAttribStateSetStreamIndex) { ST(nvn_va_st, P(0))->stream = U32(1); }
NV(nvnVertexStreamStateSetDefaults) { memset(P(0), 0, sizeof(nvn_vs_st)); }
NV(nvnVertexStreamStateSetStride) { ST(nvn_vs_st, P(0))->stride = (uint32_t)U64(1); }
NV(nvnVertexStreamStateSetDivisor) { ST(nvn_vs_st, P(0))->divisor = U32(1); }

/* ---- submit ---- */
/* deferred binds read the game's handle arrays now (it fills them for the next frame as soon as the submit returns) */
static void resolve_deferred(uint8_t *p) {
    for (;;) {
        nvn_op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= NOP_COUNT_) die("corrupt recording at submit (op %u size %u)", h.op, h.size);
        if (h.op == NOP_END) return;
        if (h.op == NOP_BIND_TEX || h.op == NOP_BIND_IMG || h.op == NOP_BIND_SAMP) {
            nvn_p_bind_handles *b = (nvn_p_bind_handles *)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) {
                memcpy(b->h, gw(b->src, 8 * (size_t)b->n), 8 * (size_t)b->n);
            }
        }
        if (h.op == NOP_BIND_BUFS) {
            nvn_p_bind_bufs *b = (nvn_p_bind_bufs *)(p + 8);
            if (b->src && b->n > 0 && b->n <= 16) memcpy(b->r, gw(b->src, sizeof(NVNbufferRange) * (size_t)b->n), sizeof(NVNbufferRange) * (size_t)b->n);
        }
        p += h.size;
    }
}
NV(nvnQueueSubmitCommands) {
    { void hos_renderer_wait(void); static volatile int32_t up; if (!up) { hos_renderer_wait(); up = 1; } }   /* (display.c) */
    nvn_queue *q = (nvn_queue *)P(0);
    int n = I(1);
    uint64_t h[64];
    if (n < 0 || n > 64) die("submit of %d handles", n);
    memcpy(h, PN(2, 8 * (size_t)n), 8 * (size_t)n);
    q->submits++;
    {   /* (which queues submit, 60 lines then every 20000: the game's queues are replayed on one, in arrival order) */
        static volatile int32_t k;
        const int32_t i = os_atomic_inc(&k);
        if (lognvn() && (i <= 60 || i % 20000 == 0))
            hos_log("nvn: QueueSubmitCommands #%ld: queue %p (its #%llu), %d handles (lr %#llx)", (long)i, (void *)q,
                    (unsigned long long)q->submits, n, (unsigned long long)c->x[30]);
    }
    if (rx_direct()) {   /* (direct mode: the handles' lists to the back end, which reads their deferred arrays itself) */
        gpu_flush();
        void *lists[64];
        for (int i = 0; i < n; i++) {
            const nvn_lrec *l = nvn_list_lookup(h[i]);
            if (!l) die("command handle %#llx is not a live recording", (unsigned long long)h[i]);
            lists[i] = l->user;
        }
        __atomic_store_n(&list_done, rx_nvn_replayed(), __ATOMIC_RELEASE);
        rx_direct()->submit((uint64_t)(uintptr_t)q, lists, (uint32_t)n);
        return;
    }
    if (list_ref()) {   /* (the handles' BEGIN records for the replay, their deferred binds read now: runtime/nvn_list.c) */
        const uint64_t done = rx_nvn_replayed();
        __atomic_store_n(&list_done, done, __ATOMIC_RELEASE);
        const int bad = nvn_list_submit(h, h, n, done, gw);
        if (bad) die("command handle %#llx is not a live recording", (unsigned long long)h[bad - 1]);
        static volatile int32_t k;
        if (nvn_list_check_on() && os_atomic_inc(&k) % 600 == 0) list_log("check, every 600 submits");
    } else {
        for (int i = 0; i < n; i++) {
            check_handle(h[i]);
            resolve_deferred((uint8_t *)(uintptr_t)h[i]);
        }
    }
    {
        /* (the replay runs here, on this thread, for ~9 ms a frame (vk_replay.cpp submit_q, RX_VK_ASYNC off): the virtual
           core is given up for it, as Present and AcquireTexture do, so the game's other threads run on it meanwhile.
           Nothing in the replay runs guest code or needs the core) */
        const int away = hos_block_begin(CRIT_BLOCK_OTHER, (const void *)5);
        rx_nvn_submit_q(h, n, (uint64_t)(uintptr_t)q);
        if (away) hos_block_end();
        if (list_ref()) nvn_list_submitted(rx_nvn_pushed());   /* (after the hand-over: what names it) */
    }
}

/* ---- window ---- */
#define WB(p) ((nvn_win_builder *)(p))
/* (runtime) a window texture: a guest address, checked at each use (the window object is guest memory) */
static const nvn_tex *win_tex(const nvn_win *o, int i) {
    if (i < 0 || i >= 4 || !o->tex[i]) die("window texture %d", i);
    return (const nvn_tex *)gw(o->tex[i], sizeof(nvn_tex));
}
void hos_vi_native_acquire(int active);
void hos_vi_native_presented(void);
NV(nvnWindowBuilderSetDefaults) {
    nvn_win_builder *b = WB(P(0));
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->interval = 1;
}
NV(nvnWindowBuilderSetDevice) { WB(P(0))->dev = P(1); }
NV(nvnWindowBuilderSetNativeWindow) { WB(P(0))->native = U64(1); }
NV(nvnWindowBuilderSetTextures) {
    nvn_win_builder *b = WB(P(0));
    int n = I(1);
    if (n < 0 || n > 4) die("window with %d textures", n);
    b->ntex = n;
    memcpy(b->tex, PN(2, 8 * (size_t)n), 8 * (size_t)n);
}
NV(nvnWindowBuilderSetNumActiveTextures) { if (I(1) < 2 || I(1) > 4) die("%d active window textures", I(1)); WB(P(0))->active = I(1); }
NV(nvnWindowBuilderSetPresentInterval) { WB(P(0))->interval = I(1); }
NV(nvnWindowBuilderGetNativeWindow) { RET(WB(P(0))->native); }
NV(nvnWindowBuilderGetPresentInterval) { RET((uint32_t)WB(P(0))->interval); }
NV(nvnWindowInitialize) {
    const nvn_win_builder *b = WB(P(1));
    nvn_win *o = (nvn_win *)P(0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('W', 'I', 'N', 'D');
    o->ntex = b->ntex;
    o->interval = b->interval;
    o->active = b->active ? b->active : b->ntex;
    if (o->ntex < 2 || o->ntex > 4 || o->active < 2 || o->active > o->ntex) { RET(0); return; }
    for (int i = 0; i < o->ntex; i++) o->tex[i] = b->tex[i];
    const nvn_tex *t0 = win_tex(o, 0);
    hos_log("nvn: window of %d textures (%d active), %dx%d format %#x", o->ntex, o->active, (int)t0->d.width,
            (int)t0->d.height, t0->d.format);
    RET(1);
}
NV(nvnWindowFinalize) {}
NV(nvnWindowSetDebugLabel) {}
NV(nvnWindowSetNumActiveTextures) { nvn_win *o = (nvn_win *)P(0); if (I(1) < 2 || I(1) > o->ntex) die("%d active window textures", I(1)); o->active = I(1); }
NV(nvnWindowGetNumActiveTextures) { const nvn_win *o = (const nvn_win *)P(0); RET((uint32_t)(o->active ? o->active : o->ntex)); }
NV(nvnWindowGetPresentInterval) { RET((uint32_t)((const nvn_win *)P(0))->interval); }
/* the game picks its interval from its frame time; in real-time mode (nn_vi.c) frames are paced by our limiter */
NV(nvnWindowSetPresentInterval) {
    nvn_win *o = (nvn_win *)P(0);
    {   /* (OoT changes it several times a second: logged with a backtrace only the first times, it was a syscall-heavy cost) */
        static int told;
        if (o->interval != I(1) && told++ < 6) hos_log("nvn: the game sets present interval %d -> %d", o->interval, I(1)), hos_backtrace();
    }
    int hos_vi_native_interval(Ctx *c, int requested);
    o->interval = hos_vi_native_interval(c, I(1));
}
NV(nvnWindowSetCrop) {
    nvn_win *o = (nvn_win *)P(0);
    const int v[4] = {I(1), I(2), I(3), I(4)};
    if (memcmp(o->crop, v, sizeof v)) hos_log("nvn: window crop %d,%d %dx%d", v[0], v[1], v[2], v[3]);
    memcpy(o->crop, v, sizeof v);
}
NV(nvnWindowGetCrop) {
    const nvn_win *o = (const nvn_win *)P(0);
    NVNrectangle *r = (NVNrectangle *)P(1);
    if (!o->crop[2] && !o->crop[3] && o->ntex) {
        const nvn_tex *t0 = win_tex(o, 0);
        r->x = r->y = 0, r->width = (int)t0->d.width, r->height = (int)t0->d.height;
        return;
    }
    r->x = o->crop[0], r->y = o->crop[1], r->width = o->crop[2], r->height = o->crop[3];
}
/* frames are paced here (the console blocks here when every window texture waits for display), not in the present:
   the game presents with locks held */
NV(nvnWindowAcquireTexture) {
    nvn_win *o = (nvn_win *)P(0);
    nvn_sync *s = (nvn_sync *)P(1);
    *(int *)P(2) = o->active ? o->next++ % o->active : 0;
    {
        const int away = hos_block_begin(CRIT_BLOCK_OTHER, (const void *)3);
        hos_vi_native_acquire(o->active ? o->active : o->ntex);
        if (away) hos_block_end();
    }
    /* the acquire sync: this texture is no longer read by the display path (presents copy it in GPU order) */
    if (s) s->fenced = 3, s->fence = 0;
    RET(NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS);
}
NV(nvnQueuePresentTexture) {
    nvn_win *o = (nvn_win *)P(1);
    const int i = I(2);
    if (i < 0 || i >= o->ntex) die("present of window texture %d", i);
    {
        const int away = hos_block_begin(CRIT_BLOCK_OTHER, (const void *)4);
        if (rx_direct()) rx_direct()->present_queue((uint64_t)(uintptr_t)P(0));   /* (direct mode: the present runs in its queue's order) */
        rx_nvn_present(win_tex(o, i), o->crop);
        if (away) hos_block_end();
    }
    o->presents++;
    game_frame();
    if (rx_direct()) texwatch_frame();   /* (debug.texwatch= its line) */
    void poolcensus_frame(void);   /* poolcensus.c */
    poolcensus_frame();
    extern volatile int64_t hos_present_count;   /* display.c */
    os_atomic_inc(&hos_present_count);
    hos_vi_native_presented();
    void hos_display_presented(void);   /* (display_sdl.c: a settled resize sizes the game's frame) */
    hos_display_presented();
}

/* ---- NVN2 (mkw-recomp): what the Switch 2's NVN adds, as MKW uses it ---- */

/* program builders: {device, stage count, NVNshaderData* (16-byte entries)}; the program takes the stages at
   InitializeFromBuilder */
typedef struct { void *dev; int32_t n, pad; uint64_t data; } nvn_prog_builder;
NV(nvnProgramBuilderSetDefaults) { memset(P(0), 0, sizeof(nvn_prog_builder)); }
NV(nvnProgramBuilderSetDevice) { ((nvn_prog_builder *)P(0))->dev = P(1); }
NV(nvnProgramBuilderSetShaders) { ((nvn_prog_builder *)P(0))->n = I(1); ((nvn_prog_builder *)P(0))->data = U64(2); }
NV(nvnProgramInitializeFromBuilder) {
    const nvn_prog_builder *b = (const nvn_prog_builder *)P(1);
    nvn_prog *o = (nvn_prog *)P(0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('P', 'R', 'O', 'G');
    RET(b->data ? set_shaders(o, b->n, (const uint8_t *)gw(b->data, b->n > 0 && b->n <= 6 ? 16ull * (uint32_t)b->n : 0), 16) : 1);
}

/* command buffer modes the replay has no use for */
NV(nvnCommandBufferSetRecordingMode) {}
NV(nvnCommandBufferSetComputeOverlapMode) {}

/* Native images: the graphics system's display buffers (nn::gs), which the game's window textures are made from,
   and the images a system service hands out by handle (nn::chat's camera videos, nn_applet.c). The graphics system is
   not emulated (nn_gs.c: no handles), so a native image is a linear texture in memory of its own: of the size its
   service gave its handle (nvn_native_image_size), else of the display's, NVN_WINDOW_W x NVN_WINDOW_H; the replay
   presents a display one like any window texture. Builder: {device, format, permission, handle}. */
#define NVN_WINDOW_W 1920
#define NVN_WINDOW_H 1080
static struct { uint64_t handle; int32_t w, h; } image_sizes[256];
static int nimage_sizes;
static os_mutex image_sizes_lock = OS_MUTEX_INIT;
void nvn_native_image_size(uint64_t handle, int w, int h) {
    os_mutex_lock(&image_sizes_lock);
    int k = 0;
    while (k < nimage_sizes && image_sizes[k].handle != handle) k++;
    if (k == nimage_sizes) {
        if (k == (int)(sizeof image_sizes / sizeof image_sizes[0])) hos_fatal_lr(0, "nvn: more than %d sized native images", k);
        nimage_sizes++;
    }
    image_sizes[k].handle = handle, image_sizes[k].w = w, image_sizes[k].h = h;
    os_mutex_unlock(&image_sizes_lock);
}
typedef struct { void *dev; int32_t format, permission; uint64_t handle; } nvn_native_image_builder;
typedef struct { uint32_t magic; int32_t format, width, height; uint64_t storage, size; } nvn_native_image;
NV(nvnNativeImageBuilderSetDefaults) {
    nvn_native_image_builder *b = (nvn_native_image_builder *)P(0);
    void *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->format = NVN_FORMAT_RGBA8;
}
NV(nvnNativeImageBuilderSetDevice) { ((nvn_native_image_builder *)P(0))->dev = P(1); }
NV(nvnNativeImageBuilderSetFormat) { ((nvn_native_image_builder *)P(0))->format = I(1); }
NV(nvnNativeImageBuilderSetMemoryPermission) { ((nvn_native_image_builder *)P(0))->permission = I(1); }
NV(nvnNativeImageBuilderSetNativeImageHandle) { ((nvn_native_image_builder *)P(0))->handle = U64(1); }
NV(nvnNativeImageBuilderGetDevice) { RETP(((const nvn_native_image_builder *)P(0))->dev); }
NV(nvnNativeImageBuilderGetFormat) { RET((uint32_t)((const nvn_native_image_builder *)P(0))->format); }
NV(nvnNativeImageBuilderGetMemoryPermission) { RET((uint32_t)((const nvn_native_image_builder *)P(0))->permission); }
NV(nvnNativeImageBuilderGetNativeImageHandle) { RET(((const nvn_native_image_builder *)P(0))->handle); }
NV(nvnNativeImageInitialize) {
    const nvn_native_image_builder *b = (const nvn_native_image_builder *)P(1);
    nvn_native_image *o = (nvn_native_image *)P(0);
    uint64_t hos_region_alloc(uint64_t size, uint64_t align);   /* hos_mem.c */
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('N', 'I', 'M', 'G');
    o->format = b->format, o->width = NVN_WINDOW_W, o->height = NVN_WINDOW_H;
    os_mutex_lock(&image_sizes_lock);
    for (int k = 0; k < nimage_sizes; k++)
        if (image_sizes[k].handle == b->handle) o->width = image_sizes[k].w, o->height = image_sizes[k].h;
    os_mutex_unlock(&image_sizes_lock);
    o->size = (uint64_t)o->width * o->height * 4;
    o->storage = hos_region_alloc(o->size, 0x10000);
    if (o->storage) hos_gpu_track(o->storage, o->size, 1);
    hos_log("nvn: native image %dx%d format %#x (handle %#llx) at %#llx", o->width, o->height, o->format,
            (unsigned long long)b->handle, (unsigned long long)o->storage);
    RET(o->storage != 0);
}
NV(nvnNativeImageFinalize) { ((nvn_native_image *)P(0))->magic = 0; }
/* nvnTextureInitializeFromNativeImage(NVNtexture*, NVNnativeImage*): a pitch-linear 2D render target over it */
NV(nvnTextureInitializeFromNativeImage) {
    const nvn_native_image *img = (const nvn_native_image *)P(1);
    nvn_tex *o = TX(P(0));
    if (o->magic == NVN_MAGIC('T', 'E', 'X', 0) && o->serial) {
        live_del(o);
        rx_nvn_texture_released(o);
    }
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('T', 'E', 'X', 0);
    o->d.target = NVN_TEXTURE_TARGET_2D;
    o->d.format = img->format;
    o->d.flags = NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET;
    o->d.width = (uint32_t)img->width, o->d.height = (uint32_t)img->height, o->d.depth = o->d.levels = 1;
    o->d.stride = (uint32_t)img->width * 4;
    o->d.swizzle_r = 2, o->d.swizzle_g = 3, o->d.swizzle_b = 4, o->d.swizzle_a = 5;
    nvn_tex_layout(&o->d, &o->L);
    o->gpu = img->storage;
    o->serial = new_serial();
    live_put(o);
    rx_nvn_texture_created(o);
    RET(img->magic == NVN_MAGIC('N', 'I', 'M', 'G'));
}
/* nvnSyncInitializeFromNativeSyncHandle(NVNsync*, NVNdevice*, nn::gs::NativeSyncHandle): signaled */
NV(nvnSyncInitializeFromNativeSyncHandle) {
    nvn_sync *s = (nvn_sync *)P(0);
    s->magic = NVN_MAGIC('S', 'Y', 'N', 'C'), s->fenced = 3, s->fence = 0;
    RET(1);
}

/* nvnTextureGetSparseTileLayout(NVNtexture const*, NVNtextureSparseTileLayout* {int numTiledLevels, tileWidth,
   tileHeight, tileDepth, reserved[4]}): 64 KiB tiles, shaped by the format's bytes per block (128 x 128 texels at
   4 bytes); the levels at least one tile in size are tiled, the rest live in the mip tail. With nothing written the
   game sized its tile table 0 and read through a null table. */
NV(nvnTextureGetSparseTileLayout) {
    const nvn_tex *t = CTX(P(0));
    int bw = 1, bh = 1;
    const int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    int tw = 128, th = 128;   /* (in blocks: 64 KiB / bpb, as square as a power of two allows) */
    switch (bpb) {
    case 1: tw = 256, th = 256; break;
    case 2: tw = 256, th = 128; break;
    case 8: tw = 128, th = 64; break;
    case 16: tw = 64, th = 64; break;
    }
    tw *= bw, th *= bh;
    int levels = 0;
    for (uint32_t l = 0, w = t->d.width, h = t->d.height; l < t->d.levels; l++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
        if (w >= (uint32_t)tw && h >= (uint32_t)th) levels++;
    int32_t *out = (int32_t *)P(1);
    memset(out, 0, 32);
    out[0] = levels, out[1] = tw, out[2] = th, out[3] = 1;
}


/* queue and command buffer settings and debug annotations with no effect on the replay */
NV(nvnQueueBuilderSetPrimaryQueue) {}
NV(nvnQueueBuilderSetQueuePriority) {}
NV(nvnCommandBufferSetGraphicsWorkloadPriority) {}
NV(nvnCommandBufferSetComputeWorkloadPriority) {}
NV(nvnCommandBufferInsertDebugMarkerGraphics) {}
NV(nvnCommandBufferInsertDebugMarkerCompute) {}
NV(nvnCommandBufferPushDebugGroupGraphics) {}
NV(nvnCommandBufferPushDebugGroupCompute) {}
NV(nvnCommandBufferPopDebugGroupGraphics) {}
NV(nvnCommandBufferPopDebugGroupCompute) {}


NV(nvnTextureGetMemoryPool) { RETP(CTX(P(0))->pool); }
NV(nvnTextureBuilderGetMemoryPool) { RETP(CTB(P(0))->pool); }
NV(nvnBufferBuilderGetMemoryPool) { RETP(((const nvn_buffer_builder *)P(0))->pool); }
NV(nvnBufferBuilderGetMemoryOffset) { RET((uint64_t)((const nvn_buffer_builder *)P(0))->off); }
NV(nvnBufferBuilderGetSize) { RET(((const nvn_buffer_builder *)P(0))->size); }
NV(nvnTexturePoolGetMemoryPool) { RETP(((const nvn_dpool *)P(0))->pool); }
NV(nvnTexturePoolGetMemoryOffset) { RET((uint64_t)((const nvn_dpool *)P(0))->off); }
NV(nvnTexturePoolGetSize) { RET((uint32_t)((const nvn_dpool *)P(0))->n); }
NV(nvnSamplerPoolGetMemoryPool) { RETP(((const nvn_dpool *)P(0))->pool); }
NV(nvnSamplerPoolGetMemoryOffset) { RET((uint64_t)((const nvn_dpool *)P(0))->off); }
NV(nvnSamplerPoolGetSize) { RET((uint32_t)((const nvn_dpool *)P(0))->n); }
NV(nvnWindowGetNumTextures) { RET((uint32_t)((const nvn_win *)P(0))->ntex); }


/* GPU-driven draws (NVN2): the arguments are read at replay (rx_nvn.cpp), after the GPU's writes reached memory */
static void draw_indirect(Ctx *c, int indexed, int prim, int itype, uint64_t ibuf, uint64_t indirect, uint64_t count_addr,
                          int max, int stride) {
    nvn_p_draw_indirect *r = REC(CB(P(0)), NOP_DRAW_INDIRECT, nvn_p_draw_indirect);
    r->indexed = indexed, r->prim = prim, r->itype = itype, r->max = max, r->stride = stride;
    r->ibuf = ibuf, r->indirect = indirect, r->count_addr = count_addr;
}
/* DrawArraysIndirect(cmd, prim, indirect); DrawElementsIndirect(cmd, prim, index type, index buffer, indirect) */
NV(nvnCommandBufferDrawArraysIndirect) { draw_indirect(c, 0, I(1), 0, 0, U64(2), 0, 1, 16); }
NV(nvnCommandBufferDrawElementsIndirect) { draw_indirect(c, 1, I(1), I(2), U64(3), U64(4), 0, 1, 20); }
/* MultiDraw*IndirectCount(cmd, prim, [index type, index buffer,] indirect, count address, max draws, stride) */
NV(nvnCommandBufferMultiDrawArraysIndirectCount) { draw_indirect(c, 0, I(1), 0, 0, U64(2), U64(3), I(4), I(5)); }
NV(nvnCommandBufferMultiDrawElementsIndirectCount) { draw_indirect(c, 1, I(1), I(2), U64(3), U64(4), U64(5), I(6), I(7)); }
NV(nvnCommandBufferDispatchComputeIndirect) { REC(CB(P(0)), NOP_DISPATCH_INDIRECT, nvn_p_dispatch_indirect)->addr = U64(1); }
/* mesh shading: recorded for the Vulkan replay (VK_EXT_mesh_shader) */
static void draw_mesh(Ctx *c, int kind) {
    nvn_p_draw_mesh *r = REC(CB(P(0)), NOP_DRAW_MESH, nvn_p_draw_mesh);
    memset(r, 0, sizeof *r);
    r->kind = kind;
    if (kind == 0) r->x = I(1), r->y = I(2), r->z = I(3);
    else if (kind == 1) r->indirect = U64(1), r->max = 1;
    else r->indirect = U64(1), r->count_addr = U64(2), r->max = I(3), r->stride = I(4);
    if (kind && rx_direct()) game_indirect_mesh_draw(r->indirect, r->max, r->stride);
    static volatile int64_t n;
    const int64_t k = os_atomic_inc(&n);
    if (lognvn() && (k <= 3 || k % 20000 == 0))
        hos_log("nvn: DrawMeshTasks%s: %lld so far (x1 %#llx x2 %#llx x3 %#llx x4 %#llx)",
                kind == 0 ? "" : kind == 1 ? "Indirect" : " (multi, indirect count)", (long long)k,
                (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                (unsigned long long)c->x[4]);
}
NV(nvnCommandBufferDrawMeshTasks) { draw_mesh(c, 0); }
NV(nvnCommandBufferDrawMeshTasksIndirect) { draw_mesh(c, 1); }
NV(nvnCommandBufferMultiDrawMeshTasksIndirectCount) { draw_mesh(c, 2); }
/* state the replay does without: variable rate shading, the geometry pipeline's mode, stencil culling */
NV(nvnCommandBufferBindShadingRateImage) {}
NV(nvnCommandBufferShadingRateImagePalette) {}
NV(nvnCommandBufferSetGeometryProcessingMode) {}
NV(nvnCommandBufferSetStencilCullCriteria) {}


/* images share the texture pool's ids: the replay finds an image's texture as it finds a texture's */
NV(nvnTexturePoolRegisterImage) { rx_nvn_register_texture(I(1), (const nvn_tex *)P(2), (const nvn_view *)P(3)); }

/* ---- the procs ---- */
typedef struct { const char *name; void (*fn)(Ctx *, void *); } nvn_impl;
static const nvn_impl impls[] = {
/* table: every NV() above (regenerated from the NV() lines of this file) */
    {"nvnDeviceBuilderSetDefaults", n_nvnDeviceBuilderSetDefaults},
    {"nvnDeviceBuilderSetFlags", n_nvnDeviceBuilderSetFlags},
    {"nvnDeviceInitialize", n_nvnDeviceInitialize},
    {"nvnDeviceFinalize", n_nvnDeviceFinalize},
    {"nvnDeviceGetProcAddress", n_nvnDeviceGetProcAddress},
    {"nvnDeviceGetInteger", n_nvnDeviceGetInteger},
    {"nvnDeviceRegisterFastClearColor", n_nvnDeviceRegisterFastClearColor},
    {"nvnDeviceRegisterFastClearColori", n_nvnDeviceRegisterFastClearColori},
    {"nvnDeviceRegisterFastClearColorui", n_nvnDeviceRegisterFastClearColorui},
    {"nvnDeviceRegisterFastClearDepth", n_nvnDeviceRegisterFastClearDepth},
    {"nvnDeviceGetImageHandle", n_nvnDeviceGetImageHandle},
    {"nvnDeviceGetTextureHandle", n_nvnDeviceGetTextureHandle},
    {"nvnDeviceGetTexelFetchHandle", n_nvnDeviceGetTexelFetchHandle},
    {"nvnDeviceGetSeparateTextureHandle", n_nvnDeviceGetSeparateTextureHandle},
    {"nvnDeviceGetSeparateSamplerHandle", n_nvnDeviceGetSeparateSamplerHandle},
    {"nvnDeviceInstallDebugCallback", n_nvnDeviceInstallDebugCallback},
    {"nvnDeviceSetWindowOriginMode", n_nvnDeviceSetWindowOriginMode},
    {"nvnDeviceSetDepthMode", n_nvnDeviceSetDepthMode},
    {"nvnDeviceFinalizeCommandHandle", n_nvnDeviceFinalizeCommandHandle},
    {"nvnDeviceGetCurrentTimestampInNanoseconds", n_nvnDeviceGetCurrentTimestampInNanoseconds},
    {"nvnDeviceGetTimestampInNanoseconds", n_nvnDeviceGetTimestampInNanoseconds},
    {"nvnDeviceSetDebugLabel", n_nvnDeviceSetDebugLabel},
    {"nvnDeviceIsExternalDebuggerAttached", n_nvnDeviceIsExternalDebuggerAttached},
    {"nvnDeviceSetIntermediateShaderCache", n_nvnDeviceSetIntermediateShaderCache},
    {"nvnMemoryPoolBuilderSetDefaults", n_nvnMemoryPoolBuilderSetDefaults},
    {"nvnMemoryPoolBuilderSetDevice", n_nvnMemoryPoolBuilderSetDevice},
    {"nvnMemoryPoolBuilderSetStorage", n_nvnMemoryPoolBuilderSetStorage},
    {"nvnMemoryPoolBuilderSetFlags", n_nvnMemoryPoolBuilderSetFlags},
    {"nvnMemoryPoolInitialize", n_nvnMemoryPoolInitialize},
    {"nvnMemoryPoolFinalize", n_nvnMemoryPoolFinalize},
    {"nvnMemoryPoolGetFlags", n_nvnMemoryPoolGetFlags},
    {"nvnMemoryPoolGetSize", n_nvnMemoryPoolGetSize},
    {"nvnMemoryPoolMap", n_nvnMemoryPoolMap},
    {"nvnMemoryPoolGetBufferAddress", n_nvnMemoryPoolGetBufferAddress},
    {"nvnMemoryPoolFlushMappedRange", n_nvnMemoryPoolFlushMappedRange},
    {"nvnMemoryPoolInvalidateMappedRange", n_nvnMemoryPoolInvalidateMappedRange},
    {"nvnMemoryPoolSetDebugLabel", n_nvnMemoryPoolSetDebugLabel},
    {"nvnMemoryPoolMapVirtual", n_nvnMemoryPoolMapVirtual},
    {"nvnBufferBuilderSetDevice", n_nvnBufferBuilderSetDevice},
    {"nvnBufferBuilderSetDefaults", n_nvnBufferBuilderSetDefaults},
    {"nvnBufferBuilderSetStorage", n_nvnBufferBuilderSetStorage},
    {"nvnBufferInitialize", n_nvnBufferInitialize},
    {"nvnBufferFinalize", n_nvnBufferFinalize},
    {"nvnBufferMap", n_nvnBufferMap},
    {"nvnBufferGetAddress", n_nvnBufferGetAddress},
    {"nvnBufferGetMemoryPool", n_nvnBufferGetMemoryPool},
    {"nvnBufferGetMemoryOffset", n_nvnBufferGetMemoryOffset},
    {"nvnBufferGetSize", n_nvnBufferGetSize},
    {"nvnBufferFlushMappedRange", n_nvnBufferFlushMappedRange},
    {"nvnBufferInvalidateMappedRange", n_nvnBufferInvalidateMappedRange},
    {"nvnBufferSetDebugLabel", n_nvnBufferSetDebugLabel},
    {"nvnTexturePoolInitialize", n_nvnTexturePoolInitialize},
    {"nvnTexturePoolFinalize", n_nvnTexturePoolFinalize},
    {"nvnTexturePoolSetDebugLabel", n_nvnTexturePoolSetDebugLabel},
    {"nvnTexturePoolRegisterTexture", n_nvnTexturePoolRegisterTexture},
    {"nvnSamplerPoolInitialize", n_nvnSamplerPoolInitialize},
    {"nvnSamplerPoolFinalize", n_nvnSamplerPoolFinalize},
    {"nvnSamplerPoolSetDebugLabel", n_nvnSamplerPoolSetDebugLabel},
    {"nvnSamplerPoolRegisterSampler", n_nvnSamplerPoolRegisterSampler},
    {"nvnTextureBuilderSetDefaults", n_nvnTextureBuilderSetDefaults},
    {"nvnTextureBuilderSetDevice", n_nvnTextureBuilderSetDevice},
    {"nvnTextureBuilderSetFlags", n_nvnTextureBuilderSetFlags},
    {"nvnTextureBuilderSetTarget", n_nvnTextureBuilderSetTarget},
    {"nvnTextureBuilderSetWidth", n_nvnTextureBuilderSetWidth},
    {"nvnTextureBuilderSetHeight", n_nvnTextureBuilderSetHeight},
    {"nvnTextureBuilderSetDepth", n_nvnTextureBuilderSetDepth},
    {"nvnTextureBuilderSetSize1D", n_nvnTextureBuilderSetSize1D},
    {"nvnTextureBuilderSetSize2D", n_nvnTextureBuilderSetSize2D},
    {"nvnTextureBuilderSetSize3D", n_nvnTextureBuilderSetSize3D},
    {"nvnTextureBuilderSetLevels", n_nvnTextureBuilderSetLevels},
    {"nvnTextureBuilderSetFormat", n_nvnTextureBuilderSetFormat},
    {"nvnTextureBuilderSetDepthStencilMode", n_nvnTextureBuilderSetDepthStencilMode},
    {"nvnTextureBuilderSetSwizzle", n_nvnTextureBuilderSetSwizzle},
    {"nvnTextureBuilderSetSamples", n_nvnTextureBuilderSetSamples},
    {"nvnTextureBuilderSetStride", n_nvnTextureBuilderSetStride},
    {"nvnTextureBuilderSetStorage", n_nvnTextureBuilderSetStorage},
    {"nvnTextureBuilderSetPackagedTextureLayout", n_nvnTextureBuilderSetPackagedTextureLayout},
    {"nvnTextureBuilderSetPackagedTextureData", n_nvnTextureBuilderSetPackagedTextureData},
    {"nvnTextureBuilderGetPackagedTextureData", n_nvnTextureBuilderGetPackagedTextureData},
    {"nvnTextureBuilderGetStorageClass", n_nvnTextureBuilderGetStorageClass},
    {"nvnTextureBuilderGetFlags", n_nvnTextureBuilderGetFlags},
    {"nvnTextureBuilderGetStorageSize", n_nvnTextureBuilderGetStorageSize},
    {"nvnTextureBuilderGetStorageAlignment", n_nvnTextureBuilderGetStorageAlignment},
    {"nvnTextureBuilderGetZCullStorageSize", n_nvnTextureBuilderGetZCullStorageSize},
    {"nvnTextureBuilderGetTarget", n_nvnTextureBuilderGetTarget},
    {"nvnTextureBuilderGetWidth", n_nvnTextureBuilderGetWidth},
    {"nvnTextureBuilderGetHeight", n_nvnTextureBuilderGetHeight},
    {"nvnTextureBuilderGetDepth", n_nvnTextureBuilderGetDepth},
    {"nvnTextureBuilderGetLevels", n_nvnTextureBuilderGetLevels},
    {"nvnTextureBuilderGetFormat", n_nvnTextureBuilderGetFormat},
    {"nvnTextureBuilderGetSamples", n_nvnTextureBuilderGetSamples},
    {"nvnTextureBuilderGetStride", n_nvnTextureBuilderGetStride},
    {"nvnTextureBuilderGetMemoryOffset", n_nvnTextureBuilderGetMemoryOffset},
    {"nvnTextureBuilderGetViewOffset", n_nvnTextureBuilderGetViewOffset},
    {"nvnTextureInitialize", n_nvnTextureInitialize},
    {"nvnTextureFinalize", n_nvnTextureFinalize},
    {"nvnTextureGetStorageClass", n_nvnTextureGetStorageClass},
    {"nvnTextureGetFlags", n_nvnTextureGetFlags},
    {"nvnTextureGetWidth", n_nvnTextureGetWidth},
    {"nvnTextureGetHeight", n_nvnTextureGetHeight},
    {"nvnTextureGetDepth", n_nvnTextureGetDepth},
    {"nvnTextureGetLevels", n_nvnTextureGetLevels},
    {"nvnTextureGetFormat", n_nvnTextureGetFormat},
    {"nvnTextureGetSamples", n_nvnTextureGetSamples},
    {"nvnTextureGetTarget", n_nvnTextureGetTarget},
    {"nvnTextureGetStride", n_nvnTextureGetStride},
    {"nvnTextureGetStorageSize", n_nvnTextureGetStorageSize},
    {"nvnTextureGetZCullStorageSize", n_nvnTextureGetZCullStorageSize},
    {"nvnTextureGetTextureAddress", n_nvnTextureGetTextureAddress},
    {"nvnTextureGetMemoryOffset", n_nvnTextureGetMemoryOffset},
    {"nvnTextureGetDebugID", n_nvnTextureGetDebugID},
    {"nvnTextureSetDebugLabel", n_nvnTextureSetDebugLabel},
    {"nvnTextureGetSwizzle", n_nvnTextureGetSwizzle},
    {"nvnTextureCompare", n_nvnTextureCompare},
    {"nvnTextureGetRawStorageClass", n_nvnTextureGetRawStorageClass},
    {"nvnTextureViewSetDefaults", n_nvnTextureViewSetDefaults},
    {"nvnTextureViewSetLevels", n_nvnTextureViewSetLevels},
    {"nvnTextureViewSetLayers", n_nvnTextureViewSetLayers},
    {"nvnTextureViewSetFormat", n_nvnTextureViewSetFormat},
    {"nvnTextureViewSetTarget", n_nvnTextureViewSetTarget},
    {"nvnTextureViewSetSwizzle", n_nvnTextureViewSetSwizzle},
    {"nvnTextureViewSetDepthStencilMode", n_nvnTextureViewSetDepthStencilMode},
    {"nvnTextureViewGetLevels", n_nvnTextureViewGetLevels},
    {"nvnTextureViewGetLayers", n_nvnTextureViewGetLayers},
    {"nvnTextureViewGetFormat", n_nvnTextureViewGetFormat},
    {"nvnTextureViewGetTarget", n_nvnTextureViewGetTarget},
    {"nvnTextureGetViewOffset", n_nvnTextureGetViewOffset},
    {"nvnTextureReadTexels", n_nvnTextureReadTexels},
    {"nvnTextureReadTexelsStrided", n_nvnTextureReadTexelsStrided},
    {"nvnTextureWriteTexels", n_nvnTextureWriteTexels},
    {"nvnTextureWriteTexelsStrided", n_nvnTextureWriteTexelsStrided},
    {"nvnTextureFlushTexels", n_nvnTextureFlushTexels},
    {"nvnTextureInvalidateTexels", n_nvnTextureInvalidateTexels},
    {"nvnSamplerBuilderSetDefaults", n_nvnSamplerBuilderSetDefaults},
    {"nvnSamplerBuilderSetDevice", n_nvnSamplerBuilderSetDevice},
    {"nvnSamplerBuilderSetMinMagFilter", n_nvnSamplerBuilderSetMinMagFilter},
    {"nvnSamplerBuilderSetWrapMode", n_nvnSamplerBuilderSetWrapMode},
    {"nvnSamplerBuilderSetLodClamp", n_nvnSamplerBuilderSetLodClamp},
    {"nvnSamplerBuilderSetLodBias", n_nvnSamplerBuilderSetLodBias},
    {"nvnSamplerBuilderSetCompare", n_nvnSamplerBuilderSetCompare},
    {"nvnSamplerBuilderSetBorderColor", n_nvnSamplerBuilderSetBorderColor},
    {"nvnSamplerBuilderSetBorderColori", n_nvnSamplerBuilderSetBorderColori},
    {"nvnSamplerBuilderSetBorderColorui", n_nvnSamplerBuilderSetBorderColorui},
    {"nvnSamplerBuilderSetMaxAnisotropy", n_nvnSamplerBuilderSetMaxAnisotropy},
    {"nvnSamplerBuilderSetLodSnap", n_nvnSamplerBuilderSetLodSnap},
    {"nvnSamplerBuilderSetReductionFilter", n_nvnSamplerBuilderSetReductionFilter},
    {"nvnSamplerBuilderGetMinMagFilter", n_nvnSamplerBuilderGetMinMagFilter},
    {"nvnSamplerBuilderGetWrapMode", n_nvnSamplerBuilderGetWrapMode},
    {"nvnSamplerBuilderGetReductionFilter", n_nvnSamplerBuilderGetReductionFilter},
    {"nvnSamplerInitialize", n_nvnSamplerInitialize},
    {"nvnSamplerFinalize", n_nvnSamplerFinalize},
    {"nvnSamplerSetDebugLabel", n_nvnSamplerSetDebugLabel},
    {"nvnSamplerGetMinMagFilter", n_nvnSamplerGetMinMagFilter},
    {"nvnSamplerGetWrapMode", n_nvnSamplerGetWrapMode},
    {"nvnSamplerGetLodClamp", n_nvnSamplerGetLodClamp},
    {"nvnSamplerGetLodBias", n_nvnSamplerGetLodBias},
    {"nvnSamplerGetCompare", n_nvnSamplerGetCompare},
    {"nvnSamplerGetBorderColor", n_nvnSamplerGetBorderColor},
    {"nvnSamplerGetMaxAnisotropy", n_nvnSamplerGetMaxAnisotropy},
    {"nvnSamplerGetReductionFilter", n_nvnSamplerGetReductionFilter},
    {"nvnSamplerGetDebugID", n_nvnSamplerGetDebugID},
    {"nvnProgramInitialize", n_nvnProgramInitialize},
    {"nvnProgramFinalize", n_nvnProgramFinalize},
    {"nvnProgramSetShaders", n_nvnProgramSetShaders},
    {"nvnProgramSetShadersExt", n_nvnProgramSetShadersExt},
    {"nvnProgramSetDebugLabel", n_nvnProgramSetDebugLabel},
    {"nvnSyncInitialize", n_nvnSyncInitialize},
    {"nvnSyncFinalize", n_nvnSyncFinalize},
    {"nvnSyncSetDebugLabel", n_nvnSyncSetDebugLabel},
    {"nvnQueueFenceSync", n_nvnQueueFenceSync},
    {"nvnSyncWait", n_nvnSyncWait},
    {"nvnEventBuilderSetDefaults", n_nvnEventBuilderSetDefaults},
    {"nvnEventBuilderSetStorage", n_nvnEventBuilderSetStorage},
    {"nvnEventInitialize", n_nvnEventInitialize},
    {"nvnEventFinalize", n_nvnEventFinalize},
    {"nvnEventGetValue", n_nvnEventGetValue},
    {"nvnEventSignal", n_nvnEventSignal},
    {"nvnQueueBuilderSetDefaults", n_nvnQueueBuilderSetDefaults},
    {"nvnQueueBuilderSetDevice", n_nvnQueueBuilderSetDevice},
    {"nvnQueueBuilderSetFlags", n_nvnQueueBuilderSetFlags},
    {"nvnQueueBuilderSetCommandMemorySize", n_nvnQueueBuilderSetCommandMemorySize},
    {"nvnQueueBuilderSetComputeMemorySize", n_nvnQueueBuilderSetComputeMemorySize},
    {"nvnQueueBuilderSetControlMemorySize", n_nvnQueueBuilderSetControlMemorySize},
    {"nvnQueueBuilderSetCommandFlushThreshold", n_nvnQueueBuilderSetCommandFlushThreshold},
    {"nvnQueueBuilderGetQueueMemorySize", n_nvnQueueBuilderGetQueueMemorySize},
    {"nvnQueueBuilderSetQueueMemory", n_nvnQueueBuilderSetQueueMemory},
    {"nvnQueueInitialize", n_nvnQueueInitialize},
    {"nvnQueueFinalize", n_nvnQueueFinalize},
    {"nvnQueueGetError", n_nvnQueueGetError},
    {"nvnQueueFlush", n_nvnQueueFlush},
    {"nvnQueueFinish", n_nvnQueueFinish},
    {"nvnQueueSetDebugLabel", n_nvnQueueSetDebugLabel},
    {"nvnCommandBufferInitialize", n_nvnCommandBufferInitialize},
    {"nvnCommandBufferFinalize", n_nvnCommandBufferFinalize},
    {"nvnCommandBufferSetMemoryCallback", n_nvnCommandBufferSetMemoryCallback},
    {"nvnCommandBufferSetMemoryCallbackData", n_nvnCommandBufferSetMemoryCallbackData},
    {"nvnCommandBufferGetMemoryCallback", n_nvnCommandBufferGetMemoryCallback},
    {"nvnCommandBufferGetMemoryCallbackData", n_nvnCommandBufferGetMemoryCallbackData},
    {"nvnCommandBufferSetCommandMemoryCallbackEnabled", n_nvnCommandBufferSetCommandMemoryCallbackEnabled},
    {"nvnCommandBufferSetControlMemoryCallbackEnabled", n_nvnCommandBufferSetControlMemoryCallbackEnabled},
    {"nvnCommandBufferSetDebugLabel", n_nvnCommandBufferSetDebugLabel},
    {"nvnCommandBufferAddCommandMemory", n_nvnCommandBufferAddCommandMemory},
    {"nvnCommandBufferAddControlMemory", n_nvnCommandBufferAddControlMemory},
    {"nvnCommandBufferGetControlMemorySize", n_nvnCommandBufferGetControlMemorySize},
    {"nvnCommandBufferGetControlMemoryUsed", n_nvnCommandBufferGetControlMemoryUsed},
    {"nvnCommandBufferGetControlMemoryFree", n_nvnCommandBufferGetControlMemoryFree},
    {"nvnCommandBufferGetCommandMemorySize", n_nvnCommandBufferGetCommandMemorySize},
    {"nvnCommandBufferGetCommandMemoryUsed", n_nvnCommandBufferGetCommandMemoryUsed},
    {"nvnCommandBufferGetCommandMemoryFree", n_nvnCommandBufferGetCommandMemoryFree},
    {"nvnCommandBufferBeginRecording", n_nvnCommandBufferBeginRecording},
    {"nvnCommandBufferEndRecording", n_nvnCommandBufferEndRecording},
    {"nvnCommandBufferIsRecording", n_nvnCommandBufferIsRecording},
    {"nvnCommandBufferCallCommands", n_nvnCommandBufferCallCommands},
    {"nvnCommandBufferCopyCommands", n_nvnCommandBufferCopyCommands},
    {"nvnCommandBufferSignalEvent", n_nvnCommandBufferSignalEvent},
    {"nvnCommandBufferWaitEvent", n_nvnCommandBufferWaitEvent},
    {"nvnCommandBufferWaitSync", n_nvnCommandBufferWaitSync},
    {"nvnQueueWaitSync", n_nvnQueueWaitSync},
    {"nvnCommandBufferPushComputeConstants", n_nvnCommandBufferPushComputeConstants},
    {"nvnCommandBufferPushGraphicsConstants", n_nvnCommandBufferPushGraphicsConstants},
    {"nvnCommandBufferResetCounter", n_nvnCommandBufferResetCounter},
    {"nvnCommandBufferReportCounter", n_nvnCommandBufferReportCounter},
    {"nvnCommandBufferCopyBufferToBuffer", n_nvnCommandBufferCopyBufferToBuffer},
    {"nvnCommandBufferClearBuffer", n_nvnCommandBufferClearBuffer},
    {"nvnCommandBufferUpdateUniformBuffer", n_nvnCommandBufferUpdateUniformBuffer},
    {"nvnCommandBufferSetRenderTargets", n_nvnCommandBufferSetRenderTargets},
    {"nvnCommandBufferClearColor", n_nvnCommandBufferClearColor},
    {"nvnCommandBufferClearDepthStencil", n_nvnCommandBufferClearDepthStencil},
    {"nvnCommandBufferSetViewports", n_nvnCommandBufferSetViewports},
    {"nvnCommandBufferSetViewport", n_nvnCommandBufferSetViewport},
    {"nvnCommandBufferSetScissor", n_nvnCommandBufferSetScissor},
    {"nvnCommandBufferSetScissors", n_nvnCommandBufferSetScissors},
    {"nvnCommandBufferSetDepthRange", n_nvnCommandBufferSetDepthRange},
    {"nvnCommandBufferSetDepthRanges", n_nvnCommandBufferSetDepthRanges},
    {"nvnCommandBufferSetDepthClamp", n_nvnCommandBufferSetDepthClamp},
    {"nvnCommandBufferSetRenderEnable", n_nvnCommandBufferSetRenderEnable},
    {"nvnCommandBufferSetRenderEnableConditional", n_nvnCommandBufferSetRenderEnableConditional},
    {"nvnCommandBufferSetRasterizerDiscard", n_nvnCommandBufferSetRasterizerDiscard},
    {"nvnCommandBufferBindProgram", n_nvnCommandBufferBindProgram},
    {"nvnCommandBufferBindBlendState", n_nvnCommandBufferBindBlendState},
    {"nvnCommandBufferBindColorState", n_nvnCommandBufferBindColorState},
    {"nvnCommandBufferBindChannelMaskState", n_nvnCommandBufferBindChannelMaskState},
    {"nvnCommandBufferBindMultisampleState", n_nvnCommandBufferBindMultisampleState},
    {"nvnCommandBufferBindPolygonState", n_nvnCommandBufferBindPolygonState},
    {"nvnCommandBufferBindDepthStencilState", n_nvnCommandBufferBindDepthStencilState},
    {"nvnCommandBufferBindVertexAttribState", n_nvnCommandBufferBindVertexAttribState},
    {"nvnCommandBufferBindVertexStreamState", n_nvnCommandBufferBindVertexStreamState},
    {"nvnCommandBufferBindVertexBuffers", n_nvnCommandBufferBindVertexBuffers},
    {"nvnCommandBufferBindVertexBuffer", n_nvnCommandBufferBindVertexBuffer},
    {"nvnCommandBufferBindUniformBuffer", n_nvnCommandBufferBindUniformBuffer},
    {"nvnCommandBufferBindStorageBuffer", n_nvnCommandBufferBindStorageBuffer},
    {"nvnCommandBufferBindUniformBuffers", n_nvnCommandBufferBindUniformBuffers},
    {"nvnCommandBufferBindStorageBuffers", n_nvnCommandBufferBindStorageBuffers},
    {"nvnCommandBufferBindUniformBuffersDeferred", n_nvnCommandBufferBindUniformBuffersDeferred},
    {"nvnCommandBufferBindStorageBuffersDeferred", n_nvnCommandBufferBindStorageBuffersDeferred},
    {"nvnCommandBufferBindTexture", n_nvnCommandBufferBindTexture},
    {"nvnCommandBufferBindTextures", n_nvnCommandBufferBindTextures},
    {"nvnCommandBufferBindImage", n_nvnCommandBufferBindImage},
    {"nvnCommandBufferBindImages", n_nvnCommandBufferBindImages},
    {"nvnCommandBufferBindSeparateTexture", n_nvnCommandBufferBindSeparateTexture},
    {"nvnCommandBufferBindSeparateTextures", n_nvnCommandBufferBindSeparateTextures},
    {"nvnCommandBufferBindSeparateSampler", n_nvnCommandBufferBindSeparateSampler},
    {"nvnCommandBufferBindSeparateSamplers", n_nvnCommandBufferBindSeparateSamplers},
    {"nvnCommandBufferBindTexturesDeferred", n_nvnCommandBufferBindTexturesDeferred},
    {"nvnCommandBufferBindImagesDeferred", n_nvnCommandBufferBindImagesDeferred},
    {"nvnCommandBufferBindSeparateTexturesDeferred", n_nvnCommandBufferBindSeparateTexturesDeferred},
    {"nvnCommandBufferBindSeparateSamplersDeferred", n_nvnCommandBufferBindSeparateSamplersDeferred},
    {"nvnCommandBufferSetStencilValueMask", n_nvnCommandBufferSetStencilValueMask},
    {"nvnCommandBufferSetStencilMask", n_nvnCommandBufferSetStencilMask},
    {"nvnCommandBufferSetStencilRef", n_nvnCommandBufferSetStencilRef},
    {"nvnCommandBufferSetPolygonOffsetClamp", n_nvnCommandBufferSetPolygonOffsetClamp},
    {"nvnCommandBufferSetPrimitiveRestart", n_nvnCommandBufferSetPrimitiveRestart},
    {"nvnCommandBufferDrawArrays", n_nvnCommandBufferDrawArrays},
    {"nvnCommandBufferDrawArraysInstanced", n_nvnCommandBufferDrawArraysInstanced},
    {"nvnCommandBufferDrawElements", n_nvnCommandBufferDrawElements},
    {"nvnCommandBufferDrawElementsBaseVertex", n_nvnCommandBufferDrawElementsBaseVertex},
    {"nvnCommandBufferDrawElementsInstanced", n_nvnCommandBufferDrawElementsInstanced},
    {"nvnCommandBufferDispatchCompute", n_nvnCommandBufferDispatchCompute},
    {"nvnCommandBufferBarrier", n_nvnCommandBufferBarrier},
    {"nvnCommandBufferSetTexturePool", n_nvnCommandBufferSetTexturePool},
    {"nvnCommandBufferSetSamplerPool", n_nvnCommandBufferSetSamplerPool},
    {"nvnCommandBufferSetShaderScratchMemory", n_nvnCommandBufferSetShaderScratchMemory},
    {"nvnCommandBufferCopyBufferToTexture", n_nvnCommandBufferCopyBufferToTexture},
    {"nvnCommandBufferCopyTextureToBuffer", n_nvnCommandBufferCopyTextureToBuffer},
    {"nvnCommandBufferCopyTextureToTexture", n_nvnCommandBufferCopyTextureToTexture},
    {"nvnCommandBufferClearTexture", n_nvnCommandBufferClearTexture},
    {"nvnCommandBufferClearTextureui", n_nvnCommandBufferClearTextureui},
    {"nvnCommandBufferSetCopyRowStride", n_nvnCommandBufferSetCopyRowStride},
    {"nvnCommandBufferSetCopyImageStride", n_nvnCommandBufferSetCopyImageStride},
    {"nvnCommandBufferSetBlendColor", n_nvnCommandBufferSetBlendColor},
    {"nvnCommandBufferSetLineWidth", n_nvnCommandBufferSetLineWidth},
    {"nvnCommandBufferSetPointSize", n_nvnCommandBufferSetPointSize},
    {"nvnCommandBufferSetDepthBounds", n_nvnCommandBufferSetDepthBounds},
    {"nvnCommandBufferSetTiledCacheAction", n_nvnCommandBufferSetTiledCacheAction},
    {"nvnCommandBufferSetTiledCacheTileSize", n_nvnCommandBufferSetTiledCacheTileSize},
    {"nvnCommandBufferSetSubpixelPrecisionBias", n_nvnCommandBufferSetSubpixelPrecisionBias},
    {"nvnCommandBufferDiscardColor", n_nvnCommandBufferDiscardColor},
    {"nvnCommandBufferDiscardDepthStencil", n_nvnCommandBufferDiscardDepthStencil},
    {"nvnCommandBufferSaveZCullData", n_nvnCommandBufferSaveZCullData},
    {"nvnCommandBufferRestoreZCullData", n_nvnCommandBufferRestoreZCullData},
    {"nvnCommandBufferSetAlphaRef", n_nvnCommandBufferSetAlphaRef},
    {"nvnCommandBufferSetSampleMask", n_nvnCommandBufferSetSampleMask},
    {"nvnCommandBufferSetConservativeRasterEnable", n_nvnCommandBufferSetConservativeRasterEnable},
    {"nvnCommandBufferSetConservativeRasterDilate", n_nvnCommandBufferSetConservativeRasterDilate},
    {"nvnCommandBufferSetPatchSize", n_nvnCommandBufferSetPatchSize},
    {"nvnCommandBufferPushDebugGroupStatic", n_nvnCommandBufferPushDebugGroupStatic},
    {"nvnCommandBufferPushDebugGroupDynamic", n_nvnCommandBufferPushDebugGroupDynamic},
    {"nvnCommandBufferPushDebugGroup", n_nvnCommandBufferPushDebugGroup},
    {"nvnCommandBufferPopDebugGroup", n_nvnCommandBufferPopDebugGroup},
    {"nvnCommandBufferPopDebugGroupId", n_nvnCommandBufferPopDebugGroupId},
    {"nvnCommandBufferInsertDebugMarkerStatic", n_nvnCommandBufferInsertDebugMarkerStatic},
    {"nvnCommandBufferInsertDebugMarkerDynamic", n_nvnCommandBufferInsertDebugMarkerDynamic},
    {"nvnCommandBufferInsertDebugMarker", n_nvnCommandBufferInsertDebugMarker},
    {"nvnBlendStateSetDefaults", n_nvnBlendStateSetDefaults},
    {"nvnBlendStateSetBlendTarget", n_nvnBlendStateSetBlendTarget},
    {"nvnBlendStateSetBlendFunc", n_nvnBlendStateSetBlendFunc},
    {"nvnBlendStateSetBlendEquation", n_nvnBlendStateSetBlendEquation},
    {"nvnBlendStateSetAdvancedMode", n_nvnBlendStateSetAdvancedMode},
    {"nvnBlendStateSetAdvancedOverlap", n_nvnBlendStateSetAdvancedOverlap},
    {"nvnBlendStateSetAdvancedPremultipliedSrc", n_nvnBlendStateSetAdvancedPremultipliedSrc},
    {"nvnBlendStateSetAdvancedNormalizedDst", n_nvnBlendStateSetAdvancedNormalizedDst},
    {"nvnColorStateSetDefaults", n_nvnColorStateSetDefaults},
    {"nvnColorStateSetBlendEnable", n_nvnColorStateSetBlendEnable},
    {"nvnColorStateSetLogicOp", n_nvnColorStateSetLogicOp},
    {"nvnColorStateSetAlphaTest", n_nvnColorStateSetAlphaTest},
    {"nvnChannelMaskStateSetDefaults", n_nvnChannelMaskStateSetDefaults},
    {"nvnChannelMaskStateSetChannelMask", n_nvnChannelMaskStateSetChannelMask},
    {"nvnMultisampleStateSetDefaults", n_nvnMultisampleStateSetDefaults},
    {"nvnMultisampleStateSetMultisampleEnable", n_nvnMultisampleStateSetMultisampleEnable},
    {"nvnMultisampleStateSetSamples", n_nvnMultisampleStateSetSamples},
    {"nvnMultisampleStateSetAlphaToCoverageEnable", n_nvnMultisampleStateSetAlphaToCoverageEnable},
    {"nvnMultisampleStateSetAlphaToCoverageDither", n_nvnMultisampleStateSetAlphaToCoverageDither},
    {"nvnMultisampleStateSetRasterSamples", n_nvnMultisampleStateSetRasterSamples},
    {"nvnMultisampleStateSetSampleLocationsEnable", n_nvnMultisampleStateSetSampleLocationsEnable},
    {"nvnMultisampleStateSetSampleLocations", n_nvnMultisampleStateSetSampleLocations},
    {"nvnPolygonStateSetDefaults", n_nvnPolygonStateSetDefaults},
    {"nvnPolygonStateSetCullFace", n_nvnPolygonStateSetCullFace},
    {"nvnPolygonStateSetFrontFace", n_nvnPolygonStateSetFrontFace},
    {"nvnPolygonStateSetPolygonMode", n_nvnPolygonStateSetPolygonMode},
    {"nvnPolygonStateSetPolygonOffsetEnables", n_nvnPolygonStateSetPolygonOffsetEnables},
    {"nvnDepthStencilStateSetDefaults", n_nvnDepthStencilStateSetDefaults},
    {"nvnDepthStencilStateSetDepthTestEnable", n_nvnDepthStencilStateSetDepthTestEnable},
    {"nvnDepthStencilStateSetDepthWriteEnable", n_nvnDepthStencilStateSetDepthWriteEnable},
    {"nvnDepthStencilStateSetDepthFunc", n_nvnDepthStencilStateSetDepthFunc},
    {"nvnDepthStencilStateSetStencilTestEnable", n_nvnDepthStencilStateSetStencilTestEnable},
    {"nvnDepthStencilStateSetStencilFunc", n_nvnDepthStencilStateSetStencilFunc},
    {"nvnDepthStencilStateSetStencilOp", n_nvnDepthStencilStateSetStencilOp},
    {"nvnVertexAttribStateSetDefaults", n_nvnVertexAttribStateSetDefaults},
    {"nvnVertexAttribStateSetFormat", n_nvnVertexAttribStateSetFormat},
    {"nvnVertexAttribStateSetStreamIndex", n_nvnVertexAttribStateSetStreamIndex},
    {"nvnVertexStreamStateSetDefaults", n_nvnVertexStreamStateSetDefaults},
    {"nvnVertexStreamStateSetStride", n_nvnVertexStreamStateSetStride},
    {"nvnVertexStreamStateSetDivisor", n_nvnVertexStreamStateSetDivisor},
    {"nvnQueueSubmitCommands", n_nvnQueueSubmitCommands},
    {"nvnWindowBuilderSetDefaults", n_nvnWindowBuilderSetDefaults},
    {"nvnWindowBuilderSetDevice", n_nvnWindowBuilderSetDevice},
    {"nvnWindowBuilderSetNativeWindow", n_nvnWindowBuilderSetNativeWindow},
    {"nvnWindowBuilderSetTextures", n_nvnWindowBuilderSetTextures},
    {"nvnWindowBuilderSetNumActiveTextures", n_nvnWindowBuilderSetNumActiveTextures},
    {"nvnWindowBuilderSetPresentInterval", n_nvnWindowBuilderSetPresentInterval},
    {"nvnWindowBuilderGetNativeWindow", n_nvnWindowBuilderGetNativeWindow},
    {"nvnWindowBuilderGetPresentInterval", n_nvnWindowBuilderGetPresentInterval},
    {"nvnWindowInitialize", n_nvnWindowInitialize},
    {"nvnWindowFinalize", n_nvnWindowFinalize},
    {"nvnWindowSetDebugLabel", n_nvnWindowSetDebugLabel},
    {"nvnWindowSetNumActiveTextures", n_nvnWindowSetNumActiveTextures},
    {"nvnWindowGetNumActiveTextures", n_nvnWindowGetNumActiveTextures},
    {"nvnWindowGetPresentInterval", n_nvnWindowGetPresentInterval},
    {"nvnWindowSetPresentInterval", n_nvnWindowSetPresentInterval},
    {"nvnWindowSetCrop", n_nvnWindowSetCrop},
    {"nvnWindowGetCrop", n_nvnWindowGetCrop},
    {"nvnWindowAcquireTexture", n_nvnWindowAcquireTexture},
    {"nvnQueuePresentTexture", n_nvnQueuePresentTexture},
    {"nvnProgramBuilderSetDefaults", n_nvnProgramBuilderSetDefaults},
    {"nvnProgramBuilderSetDevice", n_nvnProgramBuilderSetDevice},
    {"nvnProgramBuilderSetShaders", n_nvnProgramBuilderSetShaders},
    {"nvnProgramInitializeFromBuilder", n_nvnProgramInitializeFromBuilder},
    {"nvnCommandBufferSetRecordingMode", n_nvnCommandBufferSetRecordingMode},
    {"nvnCommandBufferSetComputeOverlapMode", n_nvnCommandBufferSetComputeOverlapMode},
    {"nvnNativeImageBuilderSetDefaults", n_nvnNativeImageBuilderSetDefaults},
    {"nvnNativeImageBuilderSetDevice", n_nvnNativeImageBuilderSetDevice},
    {"nvnNativeImageBuilderSetFormat", n_nvnNativeImageBuilderSetFormat},
    {"nvnNativeImageBuilderSetMemoryPermission", n_nvnNativeImageBuilderSetMemoryPermission},
    {"nvnNativeImageBuilderSetNativeImageHandle", n_nvnNativeImageBuilderSetNativeImageHandle},
    {"nvnNativeImageBuilderGetDevice", n_nvnNativeImageBuilderGetDevice},
    {"nvnNativeImageBuilderGetFormat", n_nvnNativeImageBuilderGetFormat},
    {"nvnNativeImageBuilderGetMemoryPermission", n_nvnNativeImageBuilderGetMemoryPermission},
    {"nvnNativeImageBuilderGetNativeImageHandle", n_nvnNativeImageBuilderGetNativeImageHandle},
    {"nvnNativeImageInitialize", n_nvnNativeImageInitialize},
    {"nvnNativeImageFinalize", n_nvnNativeImageFinalize},
    {"nvnTextureInitializeFromNativeImage", n_nvnTextureInitializeFromNativeImage},
    {"nvnSyncInitializeFromNativeSyncHandle", n_nvnSyncInitializeFromNativeSyncHandle},
    {"nvnTextureGetSparseTileLayout", n_nvnTextureGetSparseTileLayout},
    {"nvnQueueBuilderSetPrimaryQueue", n_nvnQueueBuilderSetPrimaryQueue},
    {"nvnQueueBuilderSetQueuePriority", n_nvnQueueBuilderSetQueuePriority},
    {"nvnCommandBufferSetGraphicsWorkloadPriority", n_nvnCommandBufferSetGraphicsWorkloadPriority},
    {"nvnCommandBufferSetComputeWorkloadPriority", n_nvnCommandBufferSetComputeWorkloadPriority},
    {"nvnCommandBufferInsertDebugMarkerGraphics", n_nvnCommandBufferInsertDebugMarkerGraphics},
    {"nvnCommandBufferInsertDebugMarkerCompute", n_nvnCommandBufferInsertDebugMarkerCompute},
    {"nvnCommandBufferPushDebugGroupGraphics", n_nvnCommandBufferPushDebugGroupGraphics},
    {"nvnCommandBufferPushDebugGroupCompute", n_nvnCommandBufferPushDebugGroupCompute},
    {"nvnCommandBufferPopDebugGroupGraphics", n_nvnCommandBufferPopDebugGroupGraphics},
    {"nvnCommandBufferPopDebugGroupCompute", n_nvnCommandBufferPopDebugGroupCompute},
    {"nvnTextureGetMemoryPool", n_nvnTextureGetMemoryPool},
    {"nvnTextureBuilderGetMemoryPool", n_nvnTextureBuilderGetMemoryPool},
    {"nvnBufferBuilderGetMemoryPool", n_nvnBufferBuilderGetMemoryPool},
    {"nvnBufferBuilderGetMemoryOffset", n_nvnBufferBuilderGetMemoryOffset},
    {"nvnBufferBuilderGetSize", n_nvnBufferBuilderGetSize},
    {"nvnTexturePoolGetMemoryPool", n_nvnTexturePoolGetMemoryPool},
    {"nvnTexturePoolGetMemoryOffset", n_nvnTexturePoolGetMemoryOffset},
    {"nvnTexturePoolGetSize", n_nvnTexturePoolGetSize},
    {"nvnSamplerPoolGetMemoryPool", n_nvnSamplerPoolGetMemoryPool},
    {"nvnSamplerPoolGetMemoryOffset", n_nvnSamplerPoolGetMemoryOffset},
    {"nvnSamplerPoolGetSize", n_nvnSamplerPoolGetSize},
    {"nvnWindowGetNumTextures", n_nvnWindowGetNumTextures},
    {"nvnCommandBufferDrawArraysIndirect", n_nvnCommandBufferDrawArraysIndirect},
    {"nvnCommandBufferDrawElementsIndirect", n_nvnCommandBufferDrawElementsIndirect},
    {"nvnCommandBufferMultiDrawArraysIndirectCount", n_nvnCommandBufferMultiDrawArraysIndirectCount},
    {"nvnCommandBufferMultiDrawElementsIndirectCount", n_nvnCommandBufferMultiDrawElementsIndirectCount},
    {"nvnCommandBufferDispatchComputeIndirect", n_nvnCommandBufferDispatchComputeIndirect},
    {"nvnCommandBufferDrawMeshTasks", n_nvnCommandBufferDrawMeshTasks},
    {"nvnCommandBufferDrawMeshTasksIndirect", n_nvnCommandBufferDrawMeshTasksIndirect},
    {"nvnCommandBufferMultiDrawMeshTasksIndirectCount", n_nvnCommandBufferMultiDrawMeshTasksIndirectCount},
    {"nvnCommandBufferBindShadingRateImage", n_nvnCommandBufferBindShadingRateImage},
    {"nvnCommandBufferShadingRateImagePalette", n_nvnCommandBufferShadingRateImagePalette},
    {"nvnCommandBufferSetGeometryProcessingMode", n_nvnCommandBufferSetGeometryProcessingMode},
    {"nvnCommandBufferSetStencilCullCriteria", n_nvnCommandBufferSetStencilCullCriteria},
    {"nvnTexturePoolRegisterImage", n_nvnTexturePoolRegisterImage},
/* end of table */
};

/* An NVN function the SDK exports with no NV(...) here (nvnInstallGlobalDebugCallback, nvnBootstrapLoaderInternal,
   nvnDevtoolsBootstrap, the external memory pool builder, nvndetailUseMiddleWare): a call goes through nvn2_dispatch to
   unimplemented below, which stops the game; a direct call of one is lifted as a noreturn call
   (ounce/lift stops) */

/* A proc not implemented here: the game may ask for it (nvnDeviceGetProcAddress hands out every name it asks for), a
   call of it stops the game with its name and caller. A proc the game needs is an NV(...) above. */
static void unimplemented(Ctx *c, void *name) {
    die("the game called NVN %s, which is not implemented (x0 %#llx x1 %#llx x2 %#llx)", (const char *)name,
        (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
}
static int cmp_impl(const void *a, const void *b) { return strcmp(((const nvn_impl *)a)->name, ((const nvn_impl *)b)->name); }
uint64_t nvn_native_proc(const char *name) {
    static nvn_impl *sorted;
    static uint64_t *addr;
    static os_mutex lock = OS_MUTEX_INIT;
    enum { N = sizeof impls / sizeof *impls };
    os_mutex_lock(&lock);
    if (!sorted) {
        sorted = (nvn_impl *)malloc(sizeof impls);
        memcpy(sorted, impls, sizeof impls);
        qsort(sorted, N, sizeof *sorted, cmp_impl);
        addr = (uint64_t *)calloc(N, sizeof *addr);
        hos_log("nvn: native NVN layer: %d procs", (int)N);
    }
    const nvn_impl key = {name, NULL};
    const nvn_impl *f = (const nvn_impl *)bsearch(&key, sorted, N, sizeof *sorted, cmp_impl);
    uint64_t a;
    if (f) {
        const size_t i = (size_t)(f - sorted);
        if (!addr[i]) addr[i] = hos_hostcall(f->name, f->fn, NULL);
        a = addr[i];
    } else {   /* asked for, maybe never called: it stops the game only when called */
        static struct { char *name; uint64_t a; } *un;
        static size_t nun;
        size_t k;
        for (k = 0; k < nun && strcmp(un[k].name, name); k++) {}
        if (k == nun) {
            un = realloc(un, (nun + 1) * sizeof *un);
            un[nun].name = strdup(name);
            un[nun].a = hos_hostcall(un[nun].name, unimplemented, un[nun].name);
            nun++;
        }
        a = un[k].a;
    }
    os_mutex_unlock(&lock);
    return a;
}
/* The native NVN layer is the default on PC since 2026-10-20 (faster than the emulated GPU: castle 70.6 vs 60.4 fps;
   checked on the title, loading screens, prologue, sky island cave, rain open world, castle) and on the web since
   2026-10-23 (castle, shown fps: 39.3/40.4 against the emulated GPU's ~33; threaded build 43.9/44.6; rain 52, sky island
   cave 59; it drew the castle black there until NVN programs shared samplers past the browser's 16 a stage).
   HOS_NVN_NATIVE=0: the lifted NVN driver over the emulated Maxwell GPU (hos/gpu.c). */
int nvn_native_on(void) { return 1; }   /* (mkw-recomp: there is no emulated GPU to fall back to) */

/* An NVN function the SDK exports, called directly (gen/nvn2_exports.c: lift.py --standalone hooks every one, so
   the console driver in the SDK never runs): the same function the proc table hands out */
void nvn2_dispatch(Ctx *c, const char *name, void **cache) {
    if (!cache[0]) {
        uint64_t a = nvn_native_proc(name);
        uint64_t hos_hostcall_target(uint64_t addr, void (**fn)(Ctx *, void *), void **user);   /* hos_hostcall.c */
        void (*fn)(Ctx *, void *) = NULL;
        void *user = NULL;
        hos_hostcall_target(a, &fn, &user);
        cache[1] = user;
        __atomic_store_n(&cache[0], (void *)fn, __ATOMIC_RELEASE);
    }
    ((void (*)(Ctx *, void *))cache[0])(c, cache[1]);
}

/* nvnBootstrapLoader(char const* name) -> the proc */
HLE(nvnBootstrapLoader) {
    const char *name = ARG(0) ? (const char *)GP(ARG(0)) : "";
    RET(nvn_native_proc(name));
}
