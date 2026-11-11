"""host/rt_slots.c's slot allocator (HOS_NATIVE_TEXALLOC and the default): compiles its typedefs, place / take / steal
and list_bad from the source as it is, with guest memory as host memory, and checks

- slots are 64 KB aligned, inside [lo, hi), never overlapping; a freed slot goes back to its family;
- a full slot range gives no slot by default; natively it takes the smallest parked entry's slot that fits, and that
  entry leaves the parked list for the release queue;
- list_bad: a sound circular list passes, a wrong count, a broken previous link and a cycle are each found.

python -I -B tests/test_rt_slots_native.py [--cc cc]
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def until(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i) + len(end)]


HARNESS = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GP(a) ((void *)(uintptr_t)(a))
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
static int hos_guest_range(uint64_t a, uint64_t n) { (void)n; return a != 0; }
static void hos_log(const char *f, ...) { (void)f; }
static int native;
static int native_on(void) { return native; }
static int log_slots(void) { return 0; }
#include "cut.inc"
static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
static void reset(void) { nslots = 0, nparked = 0, nsteal_q = 0, used = peak = 0; frame = 100; memset(&n, 0, sizeof n); }
static Slot *alloc(uint64_t lr, uint32_t w, uint64_t need, uint64_t block) {
    memset(&cur, 0, sizeof cur);
    cur.lr = lr, cur.w = w, cur.h = w, cur.valid = 1;
    return take(fam_of(&cur), need, 0x200, block);
}
int main(void) {
    lo = 0xc000000, hi = 0xc000000 + (8ull << 20);   /* an 8 MB slot range */
    reset();
    Slot *a = alloc(1, 64, 3 << 20, 0x1000), *b = alloc(2, 64, 3 << 20, 0x2000);
    CHECK(a && b);
    if (a && b) {
        CHECK(!(a->off & 0xffff) && !(b->off & 0xffff));
        CHECK(a->off >= lo && a->off + a->cap <= hi && b->off >= lo && b->off + b->cap <= hi);
        CHECK(a->off + a->cap <= b->off || b->off + b->cap <= a->off);
        const uint64_t at = a->off;
        a->block = 0;                                   /* (the free hook) */
        Slot *again = alloc(1, 64, 3 << 20, 0x3000);
        CHECK(again && again->off == at);
    }
    CHECK(alloc(3, 64, 3 << 20, 0x4000) == NULL);      /* full: none by default */
    native = 1;
    parked[nparked++] = (Parked){.entry = 0x900, .block = 0x3000, .frame = 99};   /* slot 1's block is parked */
    Slot *s = alloc(3, 64, 3 << 20, 0x4000);
    CHECK(s && s->block == 0x4000 && nparked == 0 && nsteal_q == 1 && steal_q[0] == 0x900 && n.stolen == 1);
    CHECK(alloc(4, 64, 3 << 20, 0x5000) == NULL);      /* nothing parked left */
    /* list_bad over a circular list: sentinel and 3 nodes (+0 previous, +8 next, count at sentinel +0x10) */
    uint64_t m[4][3] = {{0}};
    #define L(i) ((uint64_t)(uintptr_t)m[i])
    for (int i = 0; i < 4; i++) m[i][0] = L((i + 3) % 4), m[i][1] = L((i + 1) % 4);
    m[0][2] = 3;
    CHECK(list_bad(L(0)) == 0);
    m[0][2] = 2;
    CHECK(list_bad(L(0)) >= 2000 && list_bad(L(0)) < 3000);
    m[0][2] = 3;
    m[2][0] = L(3);
    CHECK(list_bad(L(0)) == 2);
    m[2][0] = L(1);
    m[3][1] = L(1);                                     /* the last node leads back into the list: a cycle */
    CHECK(list_bad(L(0)) != 0);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="cc")
    a = ap.parse_args()
    cc = shutil.which(a.cc) or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        raise SystemExit("no C compiler")
    src = (ROOT / "host/rt_slots.c").read_text(encoding="utf-8")
    parts = [
        until(src, "typedef struct {\n    uint64_t lr, name, dta;", "} Cur;"),
        "static Cur cur;",
        until(src, "typedef struct {\n    uint64_t fam, off, cap, block;", "} Slot;"),
        until(src, "#define MAX_SLOTS", "static volatile uint64_t frame;"),
        until(src, "static struct {   /* counters", "} n;"),
        block(src, "static uint64_t fam_of(const Cur *k) {"),
        until(src, "typedef struct { Cur key; uint64_t entry, block, frame; } Parked;", "static int nsteal_q;"),
        until(src, "#define MAX_PARKED", "static int nparked;"),
        block(src, "static int list_bad(uint64_t sentinel) {"),
        block(src, "static int by_off(const void *a, const void *b) {"),
        block(src, "static uint64_t place(uint64_t cap, uint64_t align) {"),
        block(src, "static Slot *steal(uint64_t need, uint64_t align, uint64_t block) {"),
        block(src, "static Slot *take(uint64_t fam, uint64_t need, uint64_t align, uint64_t block) {"),
    ]
    cut = "\n".join(parts)
    cut = re.sub(r"static int check_on\(void\);[^\n]*\n|static void check_arenas\([^\n]*\n", "", cut)
    with tempfile.TemporaryDirectory(prefix="rt-slots-") as tmp:
        d = Path(tmp)
        (d / "cut.inc").write_text(cut, encoding="utf-8")
        (d / "t.c").write_text(HARNESS, encoding="utf-8")
        subprocess.run([cc, "-std=c11", "-Wall", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-I" + str(d),
                        str(d / "t.c"), "-o", str(d / "t")], check=True)
        r = subprocess.run([str(d / "t")], capture_output=True, text=True)
        print(r.stdout, end="")
        sys.exit(r.returncode)


if __name__ == "__main__":
    main()
