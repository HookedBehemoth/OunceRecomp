"""Call boundaries from the whole-program summaries (nx2/lift2/bounds.py call_bounds, build_ir.py BL), without the dump.

Small functions: c1 keeps x19 and x29 in a frame and calls `keep` (which writes no callee-saved register); c2 the same
calling `clob` (writes x19: its summary is demoted); c3 calls `keep` through BLR; c4 calls `die` (BRK: noreturn) when
x0 is not 0. Checks on the IR: after c1's call no load of x19, x29 or SP from the context; after c2's and c3's the
reload of x19; with `keep` a setjmp owner, or `clob` replaced with orig (its body writes x19), the reload too; with
`keep` and `clob` HLE'd (nx2/lift2/host_effects.py: clean unless declared) or replaced (PATCH_FUNC, with or without
orig on `keep`, calling `clob` as guest code: guest_call puts every register back), no reload; with `keep` declared
clobbering, the reload; an HLE_EFFECTS of a function not replaced stops the build. The summaries count a replaced
function's operands, result and guest callees, and its body's facts with orig. The declarations are C text, scanned
(nx2/lift2/patch_scan.py), as the host's are. After c4's call
of `die` a trap and `unreachable`. The program, lifted, compiled and run, returns what the guest code returns.

python -I -B tests/test_lift2_calls.py
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
import gamedesc  # noqa: E402
gamedesc.use(os.path.join(ROOT, "games", "oot"))   # (the game whose host files and patches these read)
from lift2 import (bounds, build_ir, emit_llvm, game, host_effects, llvm, patch_scan, probes,  # noqa: E402
                   summary, tables)
import rtgen  # noqa: E402

BASE = 0x80000000
# (clang --target=aarch64-linux-gnu, ld.lld -Ttext=0x80000000)
#  c1 0x00: stp x29, x30, [sp, #-32]!; stp x19, x20, [sp, #16]; mov x29, sp; mov x19, x0; bl keep; add x0, x0, x19;
#           mov x2, sp; sub x1, x29, x2; add x0, x0, x1; ldp x19, x20, [sp, #16]; ldp x29, x30, [sp], #32; ret
#  c2 0x30: the same frame; mov x19, x0; bl clob; add x0, x0, x19; epilogue
#  c3 0x54: the same frame; mov x19, x0; adr x8, keep; blr x8; add x0, x0, x19; epilogue
#  c4 0x7c: stp x29, x30, [sp, #-16]!; mov x29, sp; cbz x0, 1f; bl die; 1: mov x0, #7; ldp x29, x30, [sp], #16; ret
#  keep 0x98: add x0, x0, #1; ret      clob 0xa0: mov x19, #5; add x0, x0, #1; ret      die 0xac: brk #1
CODE = [
    0xa9be7bfd, 0xa90153f3, 0x910003fd, 0xaa0003f3, 0x94000022, 0x8b130000, 0x910003e2, 0xcb0203a1,
    0x8b010000, 0xa94153f3, 0xa8c27bfd, 0xd65f03c0, 0xa9be7bfd, 0xa90153f3, 0x910003fd, 0xaa0003f3,
    0x94000018, 0x8b130000, 0xa94153f3, 0xa8c27bfd, 0xd65f03c0, 0xa9be7bfd, 0xa90153f3, 0x910003fd,
    0xaa0003f3, 0x100001a8, 0xd63f0100, 0x8b130000, 0xa94153f3, 0xa8c27bfd, 0xd65f03c0, 0xa9bf7bfd,
    0x910003fd, 0xb4000040, 0x94000009, 0xd28000e0, 0xa8c17bfd, 0xd65f03c0, 0x91000400, 0xd65f03c0,
    0xd28000b3, 0x91000400, 0xd65f03c0, 0xd4200020]
F = {"c1": 0x00, "c2": 0x30, "c3": 0x54, "c4": 0x7c, "keep": 0x98, "clob": 0xa0, "die": 0xac}
A = {k: BASE + v for k, v in F.items()}


# (x9 stored, a call, x9 stored, a guest load, x9 stored again, a call: the second store is overwritten before
# anything can read the context)
OVERWRITTEN = """target triple = "x86_64-pc-linux-gnu"
declare void @g(ptr)
define void @f(ptr noalias nocapture %ctx, i64 %a) {
  %cx = getelementptr i8, ptr %ctx, i64 72
  store i64 1, ptr %cx, align 8
  call void @g(ptr %ctx)
  store i64 2, ptr %cx, align 8
  %p = getelementptr i8, ptr inttoptr (i64 17592186044416 to ptr), i64 %a
  %v = load i64, ptr %p, align 1
  store i64 %v, ptr %cx, align 8
  call void @g(ptr %ctx)
  ret void
}
"""


def word(pc):
    return CODE[(pc - BASE) // 4]


class declared:
    """the patch set of a C text in place of the host's (with ... as ps)"""
    def __init__(self, text):
        self.ps = patch_scan.scan_text(text)

    def __enter__(self):
        self.before = patch_scan.use(self.ps)
        return self.ps

    def __exit__(self, *a):
        patch_scan.use(self.before)


def func_decl(e, orig=False, guest=(), ops="x0"):
    """a PATCH_FUNC of the test's function at e (its word and body hash as the plan has them)"""
    words = [word(p) for p in sorted(summary.walk(set(A.values()), word)[e])]
    return "PATCH_FUNC(0x%08x, word(0x%08x), body(%#x), aapcs, x0, t_%08x%s%s, %s) { return 0; }\n" % (
        e, word(e), patch_scan.body_hash(words), e, ", orig" if orig else "",
        ", guest(%s)" % ", ".join("0x%08x" % c for c in guest) if guest else "", ops + (", sp" if guest else ""))


def lift(replaced=(), sj=(), hle=None, orig=(), calls=None, text=""):
    """{name: IR function}, the Program and the bodies, with the summaries as a game build has them; replaced: functions
    replaced (PATCH_FUNC; those in orig with orig), hle: {address: symbol} of HLE'd ones, calls: {replaced function:
    guest functions it calls}; text: further declarations"""
    entries = set(A.values())
    funcs = summary.walk(entries, word)
    prog = build_ir.Program(entries=entries)
    sums, _, _ = summary.summarise(funcs, prog, set(), word)
    hle = hle or {}
    plan = {"summaries": {e: summary.pack(s) for e, s in sums.items()}, "plt": {}, "hooks": sorted(hle),
            "sj_rets": {e: (0, 0, 0, 0) for e in sj},
            "bodies": {e: (sorted(pcs), [word(p) for p in sorted(pcs)]) for e, pcs in funcs.items()}}
    for e in replaced:
        text += func_decl(e, e in orig, (calls or {}).get(e, ()))
    with declared(text) as ps:
        bound = bounds.call_bounds(plan, hle, False, {f.addr: f for f in ps.funcs})
        bound(A["keep"])   # (memoised: computed while the test's declarations hold)
    prog = build_ir.Program(entries=entries, bound=lambda e: bound(e)._replace(symbol="g_%08x" % e))
    fns = {n: build_ir.build(a, sorted(funcs[a]), word, prog) for n, a in A.items()}
    return fns, funcs


def loads_after_call(fn):
    """the registers the blocks with a call load from the context after it"""
    out = set()
    for blk in fn.blocks.values():
        seen = False
        for v in blk.ops:
            if v.op in ("gcall", "icall"):
                seen = True
            elif seen and v.op == "regload":
                out.add(v.attrs["var"])
    # (and the blocks after them: a successor's parameter of a called block is a load in it, ir.finish)
    return out


# (rows beside their instruction in the summaries: f 0x1000: bl g; add x0, x0, #1; ret  g 0x100c: mov x5, #1; ret)
ROWS_CODE = {0x1000: 0x94000003, 0x1004: 0x91000400, 0x1008: 0xd65f03c0, 0x100c: 0xd2800025, 0x1010: 0xd65f03c0}


class RowsInSummaries(unittest.TestCase):
    """a row's reads and writes enter the summaries when they happen: an after row's reads after its instruction's
    writes (after a BL, after the callee's must-writes), a before row's writes before the instruction reads, an
    instead row in place of the call"""
    def sums(self, *rows):
        real = probes.use([], list(rows))
        try:
            funcs = {0x1000: [0x1000, 0x1004, 0x1008], 0x100c: [0x100c, 0x1010]}
            s, _, _ = summary.summarise(funcs, build_ir.Program(entries=set(funcs)), set(), ROWS_CODE.__getitem__)
            return s
        finally:
            probes.use(*real)
            summary._ROWS = None

    def row(self, pc, args, result=None, **kw):
        return probes.At(0x1000 if pc < 0x100c else 0x100c, pc, ROWS_CODE[pc], "t_row", args, result, **kw)

    def test_after_reads_the_destination(self):
        x1, x5 = summary.BIT["x1"], summary.BIT["x5"]
        s = self.sums(self.row(0x100c, ("w5",)))   # (after mov x5, #1: x5 is the instruction's)
        self.assertFalse(s[0x100c].inputs & x5)
        s = self.sums(self.row(0x100c, ("x5",), before=True))   # (before it: the caller's x5)
        self.assertTrue(s[0x100c].inputs & x5)
        s = self.sums(self.row(0x1004, ("x0", "x1")))   # (after add x0, x0, #1: x1 the caller's)
        self.assertTrue(s[0x1000].inputs & x1)

    def test_after_a_call(self):
        x5 = summary.BIT["x5"]
        s = self.sums(self.row(0x1000, ("x5",)))   # (after bl g: g must-writes x5)
        self.assertFalse(s[0x1000].inputs & x5)
        self.assertTrue(s[0x100c].results & x5)   # (the row reads g's x5: one of g's results)

    def test_before_writes(self):
        x0, x2 = summary.BIT["x0"], summary.BIT["x2"]
        s = self.sums(self.row(0x1004, ("x2",), "x0", before=True))   # (x0 := row(x2), then add x0, x0, #1)
        self.assertEqual(s[0x1000].inputs & (x0 | x2), x2)
        s = self.sums(self.row(0x1004, ("x2",), "x1"))   # (an after row's result: written there)
        self.assertTrue(s[0x1000].mustdef & summary.BIT["x1"])

    def test_instead(self):
        x1, x5 = summary.BIT["x1"], summary.BIT["x5"]
        s = self.sums(self.row(0x1000, ("x1",), "x0", replace=True))   # (no call: g's x5 is not written)
        self.assertTrue(s[0x1000].inputs & x1)
        self.assertFalse(s[0x1000].clobbers & x5)


class Calls(unittest.TestCase):
    def test_kept(self):
        fns, _ = lift()
        self.assertFalse(loads_after_call(fns["c1"]) & {"x19", "x29", "sp"}, loads_after_call(fns["c1"]))
        self.assertIn("x19", loads_after_call(fns["c2"]))       # (clob writes x19)
        self.assertIn("x19", loads_after_call(fns["c3"]))       # (BLR)

    def test_host_edited(self):
        fns, _ = lift(sj=[A["keep"]])
        self.assertIn("x19", loads_after_call(fns["c1"]))      # (a setjmp owner)
        fns, _ = lift(replaced=[A["clob"]], orig=[A["clob"]])
        self.assertIn("x19", loads_after_call(fns["c2"]))      # (orig: a body that writes x19 may run)

    def test_host_clean(self):
        # (an HLE'd callee, declared clean by default: x19, x29 and SP stay; even in place of clob, whose guest body
        # the host replaces)
        fns, _ = lift(hle={A["keep"]: "nn_keep", A["clob"]: "nn_clob"})
        for f in ("c1", "c2"):
            self.assertFalse(loads_after_call(fns[f]) & {"x19", "x29", "sp"}, (f, loads_after_call(fns[f])))
        # (a replaced function, by its declaration: clob's body is not run; keep's with orig preserves them; guest
        # code it calls through guest_call has every register put back)
        for f, kw in (("c2", {"replaced": [A["clob"]]}), ("c1", {"replaced": [A["keep"]], "orig": [A["keep"]]}),
                      ("c1", {"replaced": [A["keep"]], "calls": {A["keep"]: (A["clob"],)}})):
            fns, _ = lift(**kw)
            self.assertFalse(loads_after_call(fns[f]) & {"x19", "x29", "sp"}, (kw, f, loads_after_call(fns[f])))

    def test_row_callee(self):
        """a callee with an At row is not host-edited: a call of it keeps what its summary preserves, and the summary
        counts the row's declared reads; a row writing a callee-saved register its instruction does not stops the lift"""
        real = probes.tables()
        try:
            probes.use([], [probes.At(A["keep"], A["keep"], word(A["keep"]), "t_row", ("x0", "x5"), "x0")])
            fns, funcs = lift()
            self.assertFalse(loads_after_call(fns["c1"]) & {"x19", "x29", "sp"}, loads_after_call(fns["c1"]))
            sums, _, _ = summary.summarise(funcs, build_ir.Program(entries=set(A.values())), set(), word)
            self.assertTrue(sums[A["keep"]].inputs & summary.BIT["x5"])   # (the row's argument)
            pcs = sorted(funcs[A["keep"]])
            self.assertEqual(list(probes.at(A["keep"], pcs, word)), [A["keep"]])
            probes.use([], [probes.At(A["keep"], A["keep"], word(A["keep"]), "t_row", ("x0",), "x19")])
            with self.assertRaises(SystemExit):
                probes.at(A["keep"], pcs, word)
        finally:
            probes.use(*real)

    def test_patches_in_summaries(self):
        """the host's patches enter the summaries: a replaced callee's operands and result (and with orig its body's
        facts), an HLE'd callee's declared reads (all for one declared clobbering), a row's guest
        calls (their inputs; "computed": every register), a row in place of a BL (no call there)"""
        funcs = summary.walk(set(A.values()), word)
        prog = build_ir.Program(entries=set(A.values()))
        bit = summary.BIT
        x1_7 = sum(bit["x%d" % r] for r in range(1, 8))
        sums, _, _ = summary.summarise(funcs, prog, set(), word)
        self.assertFalse(sums[A["c1"]].inputs & x1_7)
        self.assertTrue(sums[A["keep"]].mustdef & bit["x0"])
        for o, g in ((False, ()), (True, ()), (False, (A["c4"],))):
            with declared(func_decl(A["keep"], o, g, "x0, x5")) as ps:
                rep = summary.func_facts({f.addr: f for f in ps.funcs})
                sums, _, _ = summary.summarise(funcs, prog, set(), word, replaced=rep)
                fk = summary.func_summary(sums, A["keep"], {})
            self.assertEqual(sums[A["c1"]].inputs & x1_7, bit["x5"], (o, g))   # (the operands, through the call)
            self.assertEqual(sums[A["keep"]].inputs & x1_7, 0)                 # (its body's own summary as it is)
            self.assertTrue(sums[A["c1"]].inputs & bit["x29"])                  # (c1's own frame)
            # (c1 adds x19 to keep's x0 after the call: keep's result, written on every return)
            self.assertTrue(fk.mustdef & bit["x0"])
            self.assertFalse(fk.clobbers & bit["x19"])
        with declared(func_decl(A["clob"], True)) as ps:
            rep = summary.func_facts({f.addr: f for f in ps.funcs})
            sums, _, _ = summary.summarise(funcs, prog, set(), word, replaced=rep)
            self.assertTrue(summary.func_summary(sums, A["clob"], {}).clobbers & bit["x19"])   # (orig: the body's)
        with declared(func_decl(A["die"])) as ps:
            rep = summary.func_facts({f.addr: f for f in ps.funcs})
            sums, _, _ = summary.summarise(funcs, prog, set(), word, replaced=rep)
            self.assertTrue(summary.func_summary(sums, A["die"], {}).returns)   # (the host function returns)
        with declared('HLE(nn_keep, reads(x25, "test")) {}'):
            hle = summary.host_summaries({A["keep"]: "nn_keep"})
            sums, _, _ = summary.summarise(funcs, prog, hle, word)
            self.assertTrue(sums[A["c1"]].inputs & bit["x25"])         # (the HLE'd callee's declared read)
            hle = summary.host_summaries({A["keep"]: None})            # (not known: every register)
            sums, _, _ = summary.summarise(funcs, prog, hle, word)
            self.assertTrue(sums[A["c1"]].inputs & bit["x9"])
        real = probes.tables()
        try:
            for guest, reg in (((A["c4"],), None), ("computed", "x12")):
                probes.use([], [probes.At(A["keep"], A["keep"], word(A["keep"]), "t_row", ("x0",), "x0", guest=guest)])
                sums, _, _ = summary.summarise(funcs, prog, set(), word)
                if reg:
                    self.assertTrue(sums[A["keep"]].inputs & bit[reg], guest)
                else:   # (c4 reads x0 and saves x29, x30: its inputs)
                    self.assertTrue(sums[A["keep"]].inputs & bit["x29"], guest)
            bl = A["c1"] + 0x10
            probes.use([], [probes.At(A["c1"], bl, word(bl), "t_row", ("x0",), "x0", replace=True)])
            fn = summary.Func(A["c1"], sorted(funcs[A["c1"]]), word, prog, {})
            k = sorted(funcs[A["c1"]]).index(bl)
            self.assertEqual(fn.kind[k], "goto")
            self.assertEqual(fn.dfn[k], bit["x0"] | bit["x30"])
        finally:
            probes.use(*real)

    def test_declared_clobbering(self):
        fns, _ = lift(hle={A["keep"]: "nn_keep"}, text='HLE(nn_keep, clobbers("test")) {}')
        self.assertIn("x19", loads_after_call(fns["c1"]))
        fns, _ = lift(hle={A["keep"]: "nn_keep"}, text='HLE_EFFECTS(nn_keep, clobbers("test"));')
        self.assertIn("x19", loads_after_call(fns["c1"]))

    def test_undeclared_name(self):
        names = {e: "nn_" + n for n, e in A.items()}
        with declared('HLE_EFFECTS(nn_clob, clobbers("test"));'):
            host_effects.check(names)   # (it exists)
        with declared('HLE_EFFECTS(nn_clob, clobbers("test"));\nHLE_EFFECTS(nn_none, clobbers("test"));'):
            with self.assertRaises(SystemExit) as e:
                host_effects.check(names)
            self.assertIn("t.c:2: HLE_EFFECTS of nn_none", str(e.exception))

    def test_context_not_captured(self):
        # (every declaration taking the context says it keeps no copy: otherwise a guest access after a call may read
        # the context, and LLVM keeps the context stores a later store overwrites)
        fns, _ = lift()
        m = emit_llvm.Module()
        m.add(fns["c1"])
        decls = [l for l in m.text().splitlines() if l.startswith("declare") and "(ptr" in l]
        self.assertTrue(decls)
        for d in decls:
            self.assertIn("(ptr captures(none)", d)
        # (x9 stored, a guest load, x9 stored again, a call: the first store goes at -O2 with the build's flags)
        ir_text = emit_llvm.no_capture(OVERWRITTEN)
        out = subprocess.run([llvm.check("clang"), "-O2", "-x", "ir", "-", "-S", "-emit-llvm", "-o", "-"] + game.DSE_FLAGS,
                             input=ir_text, capture_output=True, text=True, check=True).stdout
        self.assertEqual(len(re.findall(r"store i64 [^\n]*ptr %cx", out)), 2, out)   # (1 before the call, %v)

    def test_cold_partition_dse(self):
        # (clang's -O1 pipeline has no DSE: a cold partition goes through opt with clang's -O1 pipeline plus DSE,
        # game.opt_argv, then clang's code generation; the overwritten store goes there too. -O2 has DSE itself)
        clang = llvm.check("clang")
        march = ["-march=x86-64-v3"]
        pre = game.opt_argv(clang, "O1", march)
        self.assertIsNotNone(pre)
        self.assertIn("memcpyopt,dse,", pre[2])
        self.assertIsNone(game.opt_argv(clang, "O2", march))
        fl = ["-O1"] + march + game.DSE_FLAGS
        r = game.compile_ir(clang, emit_llvm.no_capture(OVERWRITTEN), "-", fl, pre, ("-S", "-emit-llvm"))
        self.assertEqual(r.returncode, 0, r.stderr)
        out = r.stdout.decode()
        self.assertEqual(len(re.findall(r"store i64 [^\n]*ptr %cx", out)), 2, out)
        plain = game.compile_ir(clang, emit_llvm.no_capture(OVERWRITTEN), "-", fl, None, ("-S", "-emit-llvm"))
        self.assertEqual(len(re.findall(r"store i64 [^\n]*ptr %cx", plain.stdout.decode())), 3)   # (without: kept)

    def test_noreturn(self):
        fns, _ = lift()
        self.assertTrue(any(b.term[0] == "noreturn" for b in fns["c4"].blocks.values()))
        m = emit_llvm.Module()
        m.add(fns["c4"])
        text = m.text()
        self.assertRegex(text, r"call void @a64_trap\(ptr %%ctx, i64 %d, i32 %d\)\n  unreachable" % (A["c4"] + 0xc,
                                                                                                   CODE[0x88 // 4]))

    def test_runs(self):
        fns, funcs = lift()
        m = emit_llvm.Module()
        extra = []
        for n, fn in fns.items():
            m.add(fn)
            extra.append("define void @f_%08x(ptr %%ctx) {\n  musttail call void @g_%08x(ptr %%ctx)\n  ret void\n}"
                         % (A[n], A[n]))
        harness = r"""
#include "a64.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void) {
    static const unsigned int code[] = { %(code)s };
    static const unsigned long long fn[] = { %(fns)s };
    if (mmap(A64_PTR(%(base)#x), 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1, 0) == MAP_FAILED) return 2;
    if (mmap(A64_PTR(0x90000000), 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1, 0) == MAP_FAILED) return 3;
    memcpy(A64_PTR(%(base)#x), code, sizeof code);
    static GuestContext ctx;
    for (int f = 0; f < (int)(sizeof fn / sizeof fn[0]); f++)
        for (int k = 0; k < 3; k++) {
            memset(&ctx, 0, sizeof ctx);
            ctx.x[31] = 0x90008000; ctx.x[0] = f == 3 ? 0 : k; ctx.x[19] = 100;
            a64_icall(&ctx, fn[f]);
            printf("%%d %%d %%llu %%llu %%llu\n", f, k, (unsigned long long)ctx.x[0], (unsigned long long)ctx.x[19],
                   (unsigned long long)ctx.x[31]);
        }
    return 0;
}
"""
        names = ["c1", "c2", "c3", "c4"]
        with tempfile.TemporaryDirectory() as d:
            rt = rtgen.write(os.path.join(d, "rt64"))
            tables.write(d, {A[n]: (funcs[A[n]], None) for n in A}, {})
            with open(os.path.join(d, "code.ll"), "w") as f:
                f.write(m.text() + "\n".join(extra) + "\n")
            with open(os.path.join(d, "main.c"), "w") as f:
                f.write(harness % {"code": ",".join("%#x" % w for w in CODE), "base": BASE,
                                   "fns": ", ".join("%#x" % A[n] for n in names)})
            exe = os.path.join(d, "prog")
            cc = [llvm.check("clang"), "-O1", "-w", "-march=x86-64-v3", "-I" + rt, "-I" + d]
            subprocess.run(cc + [os.path.join(d, "main.c"), os.path.join(rt, "a64_rt.c"),
                                 os.path.join(d, "a64_table.c"), os.path.join(d, "code.ll"), "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
        got = {(int(out[i]), int(out[i + 1])): tuple(int(x) for x in out[i + 2:i + 5]) for i in range(0, len(out), 5)}
        for k in range(3):
            # (x0; x19 back at its entry value 100; SP back at its entry value)
            self.assertEqual(got[(0, k)], (2 * k + 1, 100, 0x90008000), "c1(%d)" % k)
            self.assertEqual(got[(1, k)], (k + 6, 100, 0x90008000), "c2(%d)" % k)
            self.assertEqual(got[(2, k)], (2 * k + 1, 100, 0x90008000), "c3(%d)" % k)
            self.assertEqual(got[(3, k)], (7, 100, 0x90008000), "c4(0)")


if __name__ == "__main__":
    unittest.main()
