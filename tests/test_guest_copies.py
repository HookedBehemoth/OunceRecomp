"""Host copies the game asks for stay in guest memory (host/nn_libc.c, host/nn_fs.c, host/nn_video.c).

- The memcpy, memmove and memset replacements (PATCH_FUNC_SYM): inside the guest window the host copies; a range
  outside it is a fatal stop naming the function, the range and the caller (the game's own copy could not reach it
  either). Built from host/nn_libc.c and host/patch.h as they are, over a small window with a page after it.
- nn::fs reads and writes take guest buffers only, and GetFileSize answers 0 for a handle with no size.
- ReadDecoderOutputImageNv12 reads nothing when the pool's NV12 allocation failed (its NULL plus an offset was any host
  address, copied to the game).

python -I -B tests/test_guest_copies.py [--cc gcc]
"""
import argparse
import os
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


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


HOS_H = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct GuestContext { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
#define A64_NOESC
/* (a 64 KB guest window, then a page the test watches) */
extern uint8_t *window;
#define WINDOW 0x10000ull
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)(window + (a)))
#define HOS_WRITTEN(p, n) ((void)0)
typedef struct GuestCall GuestCall;
typedef uint64_t hos_q __attribute__((vector_size(16)));
#include "patch.h"
static inline void *hos_guest_range(uint64_t a, uint64_t n) { return a < WINDOW && n <= WINDOW - a ? GP(a) : (void *)0; }
void hos_fatal(Ctx *c, const char *fmt, ...);
'''

FIXTURE = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "hos.h"
uint8_t *window;
void hos_fatal(Ctx *c, const char *fmt, ...) {   /* (the stop: its message, exit 7) */
    (void)c;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    _exit(7);
}
#include "nn_libc.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    window = mmap(NULL, WINDOW + 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(window != MAP_FAILED);
    memset(window + WINDOW, 0xAB, 0x1000);
    Ctx c = {{0}};
    c.x[30] = 0x80001234;
    const int stop = argc > 1 ? atoi(argv[1]) : 0;
    if (stop) {   /* (a range outside the window: a fatal stop, the page after it untouched) */
        static const uint64_t cases[][3] = {{WINDOW - 8, 0, 64}, {WINDOW - 8, 0x100, 64}, {0x100, WINDOW + 0x10, 8},
                                            {~0ull - 4, 0, 16}};
        c.x[0] = cases[stop - 1][0], c.x[1] = cases[stop - 1][1], c.x[2] = cases[stop - 1][2];
        if (stop == 1 || stop == 4) hos_func_sym_memset(&c);
        else if (stop == 2) hos_func_sym_memmove(&c);
        else hos_func_sym_memcpy(&c);
        return 0;   /* (not reached) */
    }
    /* inside the window: the host copies, x0 the destination */
    memcpy(window + 0x100, "guest", 6);
    c.x[0] = 0x200, c.x[1] = 0x100, c.x[2] = 6;
    hos_func_sym_memcpy(&c);
    CHECK(c.x[0] == 0x200 && !memcmp(window + 0x200, "guest", 6));
    c.x[0] = 0x300, c.x[1] = 0x41, c.x[2] = 16;
    hos_func_sym_memset(&c);
    CHECK(c.x[0] == 0x300 && window[0x30f] == 0x41);
    c.x[0] = ~0ull - 4, c.x[1] = 0, c.x[2] = 0;   /* (nothing to copy: no range to check) */
    hos_func_sym_memcpy(&c);
    CHECK(c.x[0] == ~0ull - 4);
    printf("PASS: the libc replacements copy inside the guest window\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    fs = (ROOT / "host" / "nn_fs.c").read_text(encoding="utf-8")
    read_at = block(fs, "static uint64_t read_at(")
    check("if (!hos_guest_range(buf, size)) return 0;" in read_at, "read_at writes into a buffer it did not check")
    write = block(fs, "HLE(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE)")
    check("hos_guest_range(ARG(2), ARG(3))" in write, "WriteFile reads a buffer it did not check")
    size = block(fs, "HLE(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE)")
    check("LARGE_INTEGER sz = {0};" in size, "GetFileSize can answer with the host stack's bytes")
    video = (ROOT / "host" / "nn_video.c").read_text(encoding="utf-8")
    nv12 = block(video, "HLE(_ZN2nn5codec26ReadDecoderOutputImageNv12E")
    check("!o->nv12" in nv12, "ReadDecoderOutputImageNv12 reads a pool whose NV12 was not allocated")
    print("PASS: nn::fs and the NV12 reader check their buffers")

    if os.name == "nt" or sys.platform == "darwin":
        print("SKIP: the fixture uses mmap (Linux)")
        return
    with tempfile.TemporaryDirectory(prefix="guest-copies-") as tmp:
        tmp = Path(tmp)
        (tmp / "hos.h").write_text(HOS_H, encoding="utf-8")
        (tmp / "fixture.c").write_text(FIXTURE, encoding="utf-8")
        # (beside the stand-in hos.h: an include in quotes looks in the including file's folder first)
        (tmp / "nn_libc.c").write_text((ROOT / "host" / "nn_libc.c").read_text(encoding="utf-8"), encoding="utf-8")
        exe = tmp / "fixture"
        subprocess.run([a.cc, "-std=gnu11", "-Wall", "-Wextra", "-I", str(tmp), "-I", str(ROOT / "host"),
                        str(tmp / "fixture.c"), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
        for k, fn in ((1, "memset"), (2, "memmove"), (3, "memcpy"), (4, "memset")):
            r = subprocess.run([str(exe), str(k)], capture_output=True, text=True)
            check(r.returncode == 7 and r.stdout.startswith(fn + ": ") and "outside the guest window (lr 0x80001234)"
                  in r.stdout, "case %d: %r" % (k, r.stdout))
        print("PASS: a range outside the guest window stops the game, naming the function, the range and lr")


if __name__ == "__main__":
    main()
