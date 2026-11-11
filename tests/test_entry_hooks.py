"""nx2/entry_hooks.py splices host/*.c's ENTRY_HOOKs into the lifted C (native-vk; host/nn_libc.c and host/nn_zstd.c).

A small lifted file with three functions and a host file with an ENTRY_HOOK, an ENTRY_HOOK_SYM (resolved through
image.json's exports) and a hook that declines are spliced, compiled and run:
- a hook that returns 1 replaces the function (its x0 is the result), one that returns 0 lets the lifted body run;
- the call goes before the function's first label, so a branch back to the start does not run the hook again;
- a second splice changes nothing; a name the exports lack is reported, not spliced;
- SPLICE_AFTER runs after the first line with its text, SPLICE_AFTER_EACH after every one, with the registers of that
  point (the splice changes what the next lifted line computes); the hook sees the caller's lr in x30; an anchor the
  function lacks stops the splice, a function the lift lacks is left out.

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
#define recomp_store64(c, a, v) ((void)(a), (void)(v))
#define recomp_store64_sp(c, a, v) ((void)(a), (void)(v))
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
void f_80004000(Ctx *__restrict A64_NOESC c) {
    c->x[10] = 5;
    recomp_store64(c,c->x[23] + 56,c->x[10]);
    c->x[0] = c->x[10];
    c->x[10] = 7;
    recomp_store64_sp(c,c->x[23] + 56,c->x[10]);
    c->x[0] += c->x[10] * 100;
return;
}
void f_80005000(Ctx *__restrict A64_NOESC c) {
    c->x[1] = 1;
    c->x[0] = c->x[1];
    c->x[1] = 1;
    c->x[0] += c->x[1] * 10;
return;
}
void f_80006000(Ctx *__restrict A64_NOESC c) {
    c->x[0] = 3;
    c->x[0] = 4;
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
#define SPLICE_AFTER(addr, anchor) void splice_##addr(Ctx *c)
#define SPLICE_AFTER_EACH(addr, anchor) void splice_##addr(Ctx *c)
uint64_t seen_lr;
ENTRY_HOOK(0x80004000) { seen_lr = c->x[30]; return 0; }
SPLICE_AFTER_EACH(0x80004000, "c->x[23] + 56,c->x[10]);") { c->x[10] += 1; }
SPLICE_AFTER(0x80005000, "c->x[1] = 1;") { c->x[1] = 2; }
SPLICE_AFTER(0x80009000, "gone") { }
'''
MAIN = r'''#include <stdio.h>
#include <stdint.h>
typedef struct { uint64_t x[32]; } Ctx;
void f_80001000(Ctx *c); void f_80002000(Ctx *c); void f_80003000(Ctx *c); void f_80004000(Ctx *c);
void f_80005000(Ctx *c); void f_80006000(Ctx *c);
extern int body_runs, hook_runs;
extern uint64_t seen_lr;
#define CHECK(x) do { if (!(x)) { printf("FAIL: %s\n", #x); return 1; } } while (0)
int main(void) {
    Ctx c = {0};
    c.x[1] = 3; f_80001000(&c);   /* declined: the body runs, its loop back to the start runs no hook */
    CHECK(c.x[0] == 111 && body_runs == 3 && hook_runs == 1);
    f_80002000(&c);                /* replaced by the symbol's hook */
    CHECK(c.x[0] == 999 && body_runs == 3 && hook_runs == 2);
    f_80003000(&c);                /* no hook */
    CHECK(c.x[0] == 333 && body_runs == 4 && hook_runs == 2);
    c.x[30] = 0x80000abc; f_80004000(&c);   /* spliced after both stores (either spelling): 6 + 8 * 100 */
    CHECK(c.x[0] == 806 && seen_lr == 0x80000abc);
    f_80005000(&c);                /* spliced after the first line with its text only */
    CHECK(c.x[0] == 12);
    f_80006000(&c);                /* not spliced */
    CHECK(c.x[0] == 4);
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
        assert hooks == {0x80001000: "entry_0x80001000", 0x80002000: "entry_sym_the_copy",
                         0x80004000: "entry_0x80004000"}, hooks
        assert missing == ["not_exported"], missing
        assert entry_hooks.apply(str(gen), quiet=True, host=str(host)) == (3, 1)
        once = (gen / "code_000.c").read_text()
        assert entry_hooks.apply(str(gen), quiet=True, host=str(host)) == (3, 0), "a second splice changed the file"
        assert (gen / "code_000.c").read_text() == once
        assert once.count("splice_0x80004000(c);") == 2 and once.count("splice_0x80005000(c);") == 1, once
        assert "splice_0x80006000" not in once and "splice_0x80009000" not in once
        assert entry_hooks.wanted_splices(str(host))[0x80004000] == ("c->x[23] + 56,c->x[10]);", True)
        head = once.index("void f_80001000(")
        assert once.index("entry_0x80001000(c)", head) < once.index("L_80001000:;", head), "the hook after the first label"
        (host / "esc.c").write_text('SPLICE_AFTER(0x80006000, "say \\"a\\\\b\\"") {}\n')
        assert entry_hooks.wanted_splices(str(host))[0x80006000] == ('say "a\\b"', False)
        try:   # (an anchor the function lacks stops the splice)
            entry_hooks.apply(str(gen), quiet=True, host=str(host))
            raise AssertionError("a missing anchor was spliced")
        except SystemExit as e:
            assert "is not in f_80006000" in str(e), e
        (host / "esc.c").unlink()
        assert (gen / "code_000.c").read_text() == once
        (d / "main.c").write_text(MAIN)
        exe = d / "t"
        subprocess.run([a.cc, "-O1", "-o", str(exe), str(gen / "code_000.c"), str(host / "hooks.c"), str(d / "main.c")],
                       check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        if out.returncode or out.stdout.strip() != "ok":
            raise SystemExit("FAIL: " + out.stdout + out.stderr)
    print("PASS: entry hooks spliced once, before the first label; a declining hook runs the lifted body; splices after "
          "their anchors")


if __name__ == "__main__":
    main()
