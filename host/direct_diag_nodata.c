/* HOS_DBG=nodata[:<name part>] (diagnosis only, either back end): the textures agl::TextureData
   loadImage_ (f_80798690) makes with no data (src 0: no copy into their slot). Each is logged once with its name, its
   TextureData, its slot in the texture heap ([td+0x18]+0x10, where a load with data copies its bytes) and the guest
   call chain; with a name part, the slots of the textures whose name holds it are page-watched (nvn_native.c's
   hos_watch_guest_writes: the first writes, with their call stacks). Every 300 presents a line per slot (the first 64)
   says how many of its first 64 KB are not zero, so a slot that something fills is seen in either back end
. With HOS_DBG unset nothing here runs. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

extern volatile LONG64 hos_present_count;
void hos_backtrace(void);
void hos_watch_guest_writes(const uint8_t *data, uint64_t size);

static char want[128];
static int nodata_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *d = getenv("HOS_DBG");
        const char *p = d ? strstr(d, "nodata") : NULL;
        on = p && (p == d || p[-1] == ',') && (p[6] == 0 || p[6] == ',' || p[6] == ':');
        if (on && p[6] == ':') {
            const char *v = p + 7, *e = strchr(v, ',');
            snprintf(want, sizeof want, "%.*s", (int)(e ? (size_t)(e - v) : strlen(v)), v);
        }
    }
    return on;
}

#define NSLOT 256
static struct { uint64_t td, slot; uint32_t size; char name[64]; } slots[NSLOT];
static volatile LONG nslots;
static LONG64 last_report = -1;
static SRWLOCK lock = SRWLOCK_INIT;

static void report(void) {
    const LONG n = nslots < NSLOT ? nslots : NSLOT;
    for (LONG i = 0; i < n && i < 64; i++) {
        const uint64_t s = slots[i].slot;
        const uint32_t k = slots[i].size < 65536 ? slots[i].size : 65536;
        uint32_t nz = 0;
        uint64_t first = 0;
        if (s && hos_guest_range(s, k))
            for (uint32_t j = 0; j < k; j++)
                if (((const uint8_t *)GP(s))[j]) { if (!nz++) first = j; }
        hos_log("nodata: present %lld '%s' slot %#llx: %u of its first %u bytes not zero (first at +%#llx)%s",
                (long long)hos_present_count, slots[i].name, (unsigned long long)s, nz, k, (unsigned long long)first,
                nz ? "" : "");
    }
}

/* from direct_assets.c's hook of f_80798690 (x0 td, x2 src), before anything else */
void direct_diag_nodata_load(Ctx *c) {
    if (!nodata_on()) return;
    const LONG64 p = hos_present_count;
    if (p / 300 != last_report / 300) {
        AcquireSRWLockExclusive(&lock);
        if (p / 300 != last_report / 300) last_report = p, report();
        ReleaseSRWLockExclusive(&lock);
    }
    const uint64_t td = c->x[0], src = c->x[2];
    if (!hos_guest_range(td, 0x100)) return;
    const uint64_t brti = td + 0x58, np = G64(brti + 0x60);
    char name[64] = "";
    if (np && hos_guest_range(np, 2)) {
        const uint16_t l = *(const uint16_t *)GP(np);
        if (hos_guest_range(np + 2, l)) snprintf(name, sizeof name, "%.*s", (int)l, (const char *)GP(np + 2));
    }
    const uint64_t base = G64(td + 0x18), slot = base && hos_guest_range(base, 0x10) ? base + 0x10 : 0;
    const uint32_t size = G32(td + 0xa8);
    {   /* (every load's slot, with data or not: a slot shared by a load with data and one without is named) */
        static struct { uint64_t slot; uint32_t size; int data; char name[64]; } all[1 << 14];
        static SRWLOCK all_lock = SRWLOCK_INIT;
        AcquireSRWLockExclusive(&all_lock);
        for (uint32_t i = (uint32_t)((slot >> 6) * 2654435761u) & ((1 << 14) - 1), n = 0; n < (1 << 14); n++, i = (i + 1) & ((1 << 14) - 1)) {
            if (all[i].slot == slot) {
                if (all[i].data != !!src || strcmp(all[i].name, name))
                    hos_log("nodata: slot %#llx: '%s' (%s, %u bytes) at present %lld; before it '%s' (%s, %u bytes)", (unsigned long long)slot,
                            name, src ? "with data" : "no data", size, (long long)p, all[i].name, all[i].data ? "with data" : "no data",
                            all[i].size);
                all[i].data = !!src, all[i].size = size;
                snprintf(all[i].name, sizeof all[i].name, "%s", name);
                break;
            }
            if (!all[i].slot) {
                all[i].slot = slot, all[i].data = !!src, all[i].size = size;
                snprintf(all[i].name, sizeof all[i].name, "%s", name);
                break;
            }
        }
        ReleaseSRWLockExclusive(&all_lock);
    }
    if (src) return;
    AcquireSRWLockExclusive(&lock);
    const LONG i = nslots;
    int dup = 0;
    for (LONG j = 0; j < i && j < NSLOT; j++)
        if (slots[j].td == td && slots[j].slot == slot) dup = 1;
    if (!dup && i < NSLOT) {
        slots[i].td = td, slots[i].slot = slot, slots[i].size = size;
        snprintf(slots[i].name, sizeof slots[i].name, "%s", name);
        nslots = i + 1;
    }
    ReleaseSRWLockExclusive(&lock);
    if (dup) return;
    hos_log("nodata: '%s' td %#llx slot %#llx size %u (format %#x %ux%u, %u mips) lr %#llx at present %lld", name,
            (unsigned long long)td, (unsigned long long)slot, size, G32(brti + 0x1c), G32(brti + 0x24), G32(brti + 0x28),
            *(const uint16_t *)GP(brti + 0x16), (unsigned long long)c->x[30], (long long)p);
    if (want[0] && strstr(name, want)) {
        hos_backtrace();
        if (slot && size) hos_watch_guest_writes(GP(slot), size);
    }
}
