"""nx2/entry_hooks.py splices host/*.c's ENTRY_HOOKs into the lifted C (native-vk; host/nn_libc.c and host/nn_zstd.c).

A small lifted file with three functions and a host file with an ENTRY_HOOK, an ENTRY_HOOK_SYM (resolved through
image.json's exports) and a hook that declines are spliced, compiled and run:
- a hook that returns 1 replaces the function (its x0 is the result), one that returns 0 lets the lifted body run;
- the call goes before the function's first label, so a branch back to the start does not run the hook again;
- a second splice changes nothing; a name the exports lack is reported, not spliced.

python -I -B tests/test_entry_hooks.py [--cc clang]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import entry_hooks  # noqa: E402

A64_H = r'''
#pragma once
#include <stdint.h>
typedef struct { uint64_t x[32]; } Ctx;
#define A64_NOESC
'''
LIFTED = r'''#include "a64.h"
int body_runs;
void f_80001000(Ctx *__restrict A64_NOESC c) {
L_80001000:;
    body_runs++;
    c->x[0] = 111;
    if (c->x[1]-- > 1) goto L_80001000;
return;
}
void f_80002000(Ctx *__restrict A64_NOESC c) {
    body_runs++;
    c->x[0] = 222;
return;
}
void f_80003000(Ctx *__restrict A64_NOESC c) {
    body_runs++;
    c->x[0] = 333;
return;
}
'''
HOST = r'''#include <stdint.h>
typedef struct { uint64_t x[32]; } Ctx;
#define ENTRY_HOOK(addr) int entry_##addr(Ctx *c)
#define ENTRY_HOOK_SYM(sym) int entry_sym_##sym(Ctx *c)
int hook_runs;
ENTRY_HOOK(0x80001000) { hook_runs++; return 0; }
ENTRY_HOOK_SYM(the_copy) { hook_runs++; c->x[0] = 999; return 1; }
ENTRY_HOOK_SYM(not_exported) { return 1; }
'''
MAIN = r'''#include <stdio.h>
#include <stdint.h>
typedef struct { uint64_t x[32]; } Ctx;
void f_80001000(Ctx *c); void f_80002000(Ctx *c); void f_80003000(Ctx *c);
extern int body_runs, hook_runs;
#define CHECK(x) do { if (!(x)) { printf("FAIL: %s\n", #x); return 1; } } while (0)
int main(void) {
    Ctx c = {0};
    c.x[1] = 3; f_80001000(&c);   /* declined: the body runs, its loop back to the start runs no hook */
    CHECK(c.x[0] == 111 && body_runs == 3 && hook_runs == 1);
    f_80002000(&c);                /* replaced by the symbol's hook */
    CHECK(c.x[0] == 999 && body_runs == 3 && hook_runs == 2);
    f_80003000(&c);                /* no hook */
    CHECK(c.x[0] == 333 && body_runs == 4 && hook_runs == 2);
    puts("ok");
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=os.environ.get("CC", "clang"))
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        gen, host, image = d / "sa" / "gen", d / "host", d / "sa" / "image"
        for p in (gen, host, image):
            p.mkdir(parents=True)
        (gen / "a64.h").write_text(A64_H)
        (gen / "code_000.c").write_text(LIFTED)
        (host / "hooks.c").write_text(HOST)
        (image / "image.json").write_text(json.dumps({"exports": {"the_copy": 0x80002000}}))
        hooks, missing = entry_hooks.wanted(str(gen), str(host))
        assert hooks == {0x80001000: "entry_0x80001000", 0x80002000: "entry_sym_the_copy"}, hooks
        assert missing == ["not_exported"], missing
        assert entry_hooks.apply(str(gen), quiet=True, host=str(host)) == (2, 1)
        once = (gen / "code_000.c").read_text()
        assert entry_hooks.apply(str(gen), quiet=True, host=str(host)) == (2, 0), "a second splice changed the file"
        assert (gen / "code_000.c").read_text() == once
        head = once.index("void f_80001000(")
        assert once.index("entry_0x80001000(c)", head) < once.index("L_80001000:;", head), "the hook after the first label"
        (d / "main.c").write_text(MAIN)
        exe = d / "t"
        subprocess.run([a.cc, "-O1", "-o", str(exe), str(gen / "code_000.c"), str(host / "hooks.c"), str(d / "main.c")],
                       check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        if out.returncode or out.stdout.strip() != "ok":
            raise SystemExit("FAIL: " + out.stdout + out.stderr)
    print("PASS: entry hooks spliced once, before the first label; a declining hook runs the lifted body")


if __name__ == "__main__":
    main()
