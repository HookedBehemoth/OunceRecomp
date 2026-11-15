"""Jump tables as switches on the table's index (nx2/lift2/discover.py index_tables, build_ir.py), without the dump.

A synthetic image of six functions, each `x0 = case(x0)` through a jump table: a byte table, a halfword table, a
signed word table of offsets from the table, a byte table whose entries share targets, a byte table whose index register
is overwritten between the load and the BR, and a halfword table indexed by a byte offset (`ldrh w10, [x8, x11]`), which
must stay on the target's offset key. Checks: the switches of the first five are on the index with cases 0..n-1, the
sixth is on the offset with its reason; the program, lifted, compiled and run, returns for every index (in range and
out) what the guest code returns (the reference: the table read in Python, the bound check's default 99).

python -I -B tests/test_lift2_jumptable.py
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import build_ir, discover, emit_llvm, llvm, tables  # noqa: E402
import rtgen  # noqa: E402

BASE = 0x80000000
# (clang --target=aarch64-linux-gnu, ld.lld -Ttext=0x80000000; the source per function below)
#  f1 0x000: cmp w0, #3; b.hi def; adr x9, c0; adrp x8, tb1; add x8, x8, :lo12:tb1; ldrb w10, [x8, w0, uxtw];
#            add x9, x9, x10, lsl #2; br x9; c0..c3: mov x0, #10..#13; ret (a nop before c3); def: mov x0, #99; ret
#  f2 0x04c: the same with cmp x0, #3 and ldrh w10, [x8, x0, lsl #1]; cases 20..23, two nops before 22
#  f3 0x09c: cmp w0, #2; b.hi def; adrp x8, tb3; add x8, ...; ldrsw x9, [x8, w0, uxtw #2]; add x8, x8, x9; br x8;
#            cases 30..32 (entries: negative offsets from tb3)
#  f4 0x0d8: a byte table of five entries over three cases, add x0, x1, #40..#42 (x1 0: a phi in each case block,
#            one entry per switch edge)
#  f5 0x118: a byte table with `mov w0, #1` between the ldrb and the add; cases 50..53
#  f6 0x164: lsl x11, x0, #1; ldrh w10, [x8, x11]: the index a byte offset; cases 60..63
CODE = [
    0x71000c1f, 0x54000208, 0x100000c9, 0xb0000008, 0x91000108, 0x3860490a, 0x8b0a0929, 0xd61f0120,
    0xd2800140, 0xd65f03c0, 0xd2800160, 0xd65f03c0, 0xd2800180, 0xd65f03c0, 0xd503201f, 0xd28001a0,
    0xd65f03c0, 0xd2800c60, 0xd65f03c0, 0xf1000c1f, 0x54000228, 0x100000c9, 0xb0000008, 0x91001108,
    0x7860790a, 0x8b0a0929, 0xd61f0120, 0xd2800280, 0xd65f03c0, 0xd28002a0, 0xd65f03c0, 0xd503201f,
    0xd503201f, 0xd28002c0, 0xd65f03c0, 0xd28002e0, 0xd65f03c0, 0xd2800c60, 0xd65f03c0, 0x7100081f,
    0x54000188, 0xb0000008, 0x91003108, 0xb8a05909, 0x8b090108, 0xd61f0100, 0xd28003c0, 0xd65f03c0,
    0xd28003e0, 0xd65f03c0, 0xd2800400, 0xd65f03c0, 0xd2800c60, 0xd65f03c0, 0x7100101f, 0x540001a8,
    0x100000c9, 0xb0000008, 0x91006108, 0x3860490a, 0x8b0a0929, 0xd61f0120, 0x9100a020, 0xd65f03c0,
    0x9100a420, 0xd65f03c0, 0x9100a820, 0xd65f03c0, 0xd2800c60, 0xd65f03c0, 0x71000c1f, 0x54000208,
    0x100000e9, 0xb0000008, 0x91007508, 0x3860490a, 0x52800020, 0x8b0a0929, 0xd61f0120, 0xd2800640,
    0xd65f03c0, 0xd2800660, 0xd65f03c0, 0xd2800680, 0xd65f03c0, 0xd28006a0, 0xd65f03c0, 0xd2800c60,
    0xd65f03c0, 0x71000c1f, 0x54000208, 0x100000e9, 0xb0000008, 0x91008908, 0xd37ff80b, 0x786b690a,
    0x8b0a0929, 0xd61f0120, 0xd2800780, 0xd65f03c0, 0xd28007a0, 0xd65f03c0, 0xd28007c0, 0xd65f03c0,
    0xd28007e0, 0xd65f03c0, 0xd2800c60, 0xd65f03c0]
# tb1 @0x1000 bytes 0 2 4 7; tb2 @0x1004 halfwords 0 2 6 8; tb3 @0x100c words c0-tb3, c1-tb3, c2-tb3; tb4 @0x1018 bytes
# 0 2 0 4 2; tb5 @0x101d bytes 0 2 4 6; tb6 @0x1022 halfwords 0 2 4 6
TABLES = bytes.fromhex("000204070000020006000800acf0ffffb4f0ffffbcf0ffff000200040200020406000000020004000600")
# function: (entry offset, BR offset, cases' results per index, table entries (None: stays on the offset))
FUNCS = {"f1": (0x000, 0x01c, [10, 11, 12, 13], 4), "f2": (0x04c, 0x068, [20, 21, 22, 23], 4),
         "f3": (0x09c, 0x0b4, [30, 31, 32], 3), "f4": (0x0d8, 0x0f4, [40, 41, 40, 42, 41], 5),
         "f5": (0x118, 0x138, [50, 51, 52, 53], 4), "f6": (0x164, 0x184, [60, 61, 62, 63], None)}


class Image:
    def __init__(self):
        self.mem = bytearray(0x1100)
        for k, w in enumerate(CODE):
            struct.pack_into("<I", self.mem, 4 * k, w)
        self.mem[0x1000:0x1000 + len(TABLES)] = TABLES
        self.entries = {BASE + e for e, _, _, _ in FUNCS.values()}
        self.hooks, self.names = {}, {}
        self.eh_starts = sorted(self.entries)

    def in_code(self, a):
        return BASE <= a < BASE + 4 * len(CODE) and not a & 3

    def read_only(self, a, n):
        return BASE <= a and a + n <= BASE + len(self.mem)

    def word(self, a):
        return struct.unpack_from("<I", self.mem, a - BASE)[0]

    def read(self, a, n):
        if not BASE <= a <= BASE + len(self.mem) - n:
            raise ValueError(a)
        return bytes(self.mem[a - BASE:a - BASE + n])


HARNESS = r"""
#include "a64.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void) {
    static const unsigned char mem[] = { %(mem)s };
    static const unsigned long long fn[] = { %(fns)s };
    if (mmap(A64_PTR(%(base)#x), 0x2000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1, 0) == MAP_FAILED) return 2;
    memcpy(A64_PTR(%(base)#x), mem, sizeof mem);
    static GuestContext ctx;
    for (int f = 0; f < (int)(sizeof fn / sizeof fn[0]); f++)
        for (int k = 0; k < 7; k++) {
            memset(&ctx, 0, sizeof ctx);
            ctx.x[31] = 0x90008000; ctx.x[0] = k;
            a64_icall(&ctx, fn[f]);
            printf("%%d %%d %%llu\n", f, k, (unsigned long long)ctx.x[0]);
        }
    return 0;
}
"""


class JumpTables(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.img = Image()
        cls.D = discover.Discovery(cls.img)
        cls.D.discover()
        cls.prog = build_ir.Program(entries=cls.D.entries, jt=cls.D.jt, callee=cls.D.callee, plt=cls.D.plt,
                                    symbol=lambda e: "g_%08x" % e, jt_index=cls.D.jt_index)
        cls.fns = {e: build_ir.build(e, sorted(cls.D.funcs[e][0]), cls.img.word, cls.prog) for e in sorted(cls.D.funcs)}

    def test_switch_keys(self):
        for name, (e, br, results, n) in FUNCS.items():
            sw = [blk.term for blk in self.fns[BASE + e].blocks.values() if blk.term and blk.term[0] == "switch"]
            self.assertEqual(len(sw), 1, name)
            keys = [k for k, _ in sw[0][2]]
            if n is None:
                self.assertEqual(self.D.jt_offset.get(BASE + br), "index not scaled", name)
                self.assertNotEqual(keys, list(range(len(keys))), name)
            else:
                self.assertIn(BASE + br, self.D.jt_index, (name, self.D.jt_offset.get(BASE + br)))
                self.assertEqual(keys, list(range(n)), name)
                self.assertEqual([b.pc for _, b in sw[0][2]], self.D.jt_index[BASE + br][3], name)

    def test_runs(self):
        m = emit_llvm.Module()
        extra = []
        for e, fn in self.fns.items():
            m.add(fn)
            extra.append("define void @f_%08x(ptr %%ctx) {\n  musttail call void @g_%08x(ptr %%ctx)\n  ret void\n}"
                         % (e, e))
        names = list(FUNCS)
        with tempfile.TemporaryDirectory() as d:
            rt = rtgen.write(os.path.join(d, "rt64"))
            tables.write(d, self.D.funcs, {})
            with open(os.path.join(d, "code.ll"), "w") as f:
                f.write(m.text() + "\n".join(extra) + "\n")
            with open(os.path.join(d, "main.c"), "w") as f:
                f.write(HARNESS % {"mem": ",".join(str(b) for b in self.img.mem), "base": BASE,
                                   "fns": ", ".join("%#x" % (BASE + FUNCS[k][0]) for k in names)})
            exe = os.path.join(d, "prog")
            cc = [llvm.check("clang"), "-O1", "-w", "-march=x86-64-v3", "-I" + rt, "-I" + d]
            subprocess.run(cc + [os.path.join(d, "main.c"), os.path.join(rt, "a64_rt.c"),
                                 os.path.join(d, "a64_table.c"), os.path.join(d, "code.ll"), "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
        got = {(int(out[i]), int(out[i + 1])): int(out[i + 2]) for i in range(0, len(out), 3)}
        for f, name in enumerate(names):
            results = FUNCS[name][2]
            for k in range(7):
                want = results[k] if k < len(results) else 99
                self.assertEqual(got[(f, k)], want, "%s(%d)" % (name, k))


if __name__ == "__main__":
    unittest.main()
