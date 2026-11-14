"""Lifter 2 (nx2/lift2) without the game: the encoding table, the decoder against LLVM's disassembler on synthetic
words, and whole synthetic functions taken from encodings to IR, to LLVM IR, compiled, run, and compared with values
computed here from the Arm ARM's definitions.

python -I -B tests/test_lift2.py      (the llvm-mc and compile-and-run cases need LLVM of nx2/lift2/llvm.py's pinned
                                       major and Linux; without them they are skipped)
"""
import os
import random
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import enc, asm, build_ir, emit_llvm, llvm, selfcheck  # noqa: E402

M64, M32 = (1 << 64) - 1, (1 << 32) - 1


def have(tool):
    try:
        llvm.check(tool)
        return True
    except SystemExit:
        return False


# ---- reference values, from the Arm ARM's definitions
def s(x, w=64):
    x &= (1 << w) - 1
    return x - (1 << w) if x >> (w - 1) else x


def ror(x, n, w=64):
    n %= w
    x &= (1 << w) - 1
    return ((x >> n) | (x << (w - n))) & ((1 << w) - 1) if n else x


def bswap(x, nbytes):
    return int.from_bytes(x.to_bytes(nbytes, "little"), "big")


def clz(x, w):
    return w - x.bit_length()


def add_carry(x, y, c, w=64):
    """AddWithCarry: (result, N, Z, C, V)"""
    m = (1 << w) - 1
    x, y = x & m, y & m
    us = x + y + c
    ss = s(x, w) + s(y, w) + c
    r = us & m
    return r, r >> (w - 1), int(r == 0), int(r != us), int(s(r, w) != ss)


def nzcv(t):
    return t[1:]


A = 0x8123456789ABCDEF
B = 0xFEDCBA9876543210


def ref_bits():
    x1 = (A >> 4) & 0xFF
    x0_32 = A & M32
    out = {1: x1, 2: s((A >> 8) & 0xFF, 8) & M64, 3: (A & ~(0xFF << 32)) | (x1 << 32), 4: ror(A, 12),
           5: (A << 3) & M32, 6: (s(A) >> 60) & M64, 7: bswap(A, 8),
           8: int.from_bytes(bytes(b for pair in zip(x0_32.to_bytes(4, "little")[1::2], x0_32.to_bytes(4, "little")[0::2]) for b in pair), "little"),
           9: bswap(A >> 32, 4) << 32 | bswap(A & M32, 4), 10: clz(x1, 64),
           11: clz((x0_32 ^ (s(x0_32, 32) >> 31 & M32)) & M32, 32) - 1,
           12: int(format(A, "064b")[::-1], 2), 13: ((A << 48) | (x1 >> 16)) & M64, 14: A ^ 0xFF00FF00FF00FF00,
           15: A & ~(x1 << 4) & M64, 16: (x0_32 | ~ror(x1, 5, 32)) & M32, 17: (A ^ ~(s(x1) >> 3)) & M64,
           18: s(A & 0xFFFF, 16) & M64, 19: A & 0xFF, 20: x0_32 & 0x7FFFFFFE,
           21: (0x1111111111111111 & ~0xF) | (A >> 60), 22: (A << (x1 & 63)) & M64, 23: ror(x0_32, x1 & 31, 32),
           24: (s(A) >> (x1 & 63)) & M64}
    return out


def ref_mul():
    a32, b32 = A & M32, B & M32
    q = abs(s(a32, 32)) // abs(s(b32, 32))
    q = -q if (s(a32, 32) < 0) != (s(b32, 32) < 0) else q
    return {2: (A + A * B) & M64, 3: (B - A * B) & M64, 4: (A * B) >> 64, 5: ((s(A) * s(B)) >> 64) & M64, 6: 0,
            7: 1 << 63, 8: M64, 9: 1 << 63, 10: q & M32, 11: (A + s(a32, 32) * s(b32, 32)) & M64,
            12: (B - a32 * b32) & M64, 13: a32 // b32, 14: ~(0x1234 << 16) & M32, 15: 0xFFFF0000FFFF0000}


def cond(code, f):
    n, z, c, v = f
    r = [z, c, n, v, c and not z, n == v, not z and n == v][code >> 1]
    return int(bool(r) != bool(code & 1))


def ref_flags():
    out = {}
    t = add_carry(A, B, 0)
    out[2], f = t[0], nzcv(t)
    t = add_carry(A, B, f[2])
    out[3], f = t[0], nzcv(t)
    t = add_carry(A & M32, ~B & M32, f[2], 32)
    out[4], f = t[0], nzcv(t)
    out[5] = f[2]
    f = nzcv(add_carry(A, ~B, 1)) if cond(1, f) else (0, 1, 0, 0)
    out[6] = f[1]
    f = nzcv(add_carry(A & M32, 5, 0, 32)) if cond(11, f) else (0, 0, 1, 0)
    out[7] = M64 if cond(8, f) else 0
    f = nzcv(add_carry(A, ~B, 1))
    out[8] = A if cond(11, f) else B
    out[9] = A if cond(10, f) else ~B & M64
    out[10] = A if cond(12, f) else -B & M64
    out[11] = (A + 1) & M64 if cond(13, f) else A
    r = A & 0x80000000
    f = (r >> 31, int(r == 0), 0, 0)
    out[12] = cond(1, f)
    t = add_carry(0, ~B, 1)
    out[13], f = t[0], nzcv(t)
    out[14] = add_carry(0, ~A, f[2])[0]
    f = nzcv(add_carry(A & M32, ~B & M32, 1, 32))
    return out, f


BASE = 0x80001FF0
PROGRAMS = {
    # name: (words, inputs {reg: value} (31 = SP), expected {reg: value}, expected NZCV or None)
    "sum": ([0xd2800002, 0x8b000042, 0xf1000400, 0x54ffffc1, 0xaa0203e0, 0xd65f03c0],
            {0: 10}, {0: 55, 2: 55}, (0, 1, 1, 0)),
    "bits": ([0xd3442c01, 0x93483c02, 0xaa0003e3, 0xb3601c23, 0x93c03004, 0x531d7005, 0x937cfc06, 0xdac00c07,
              0x5ac00408, 0xdac00809, 0xdac0102a, 0x5ac0140b, 0xdac0000c, 0x93c1400d, 0xd2089c0e, 0x8a21100f,
              0x2ae11410, 0xcaa10c11, 0x93403c12, 0x53001c13, 0x121f7414, 0xb37cfc15, 0x9ac12016, 0x1ac12c17,
              0x9ac12818, 0xd65f03c0],
             {0: A, 21: 0x1111111111111111}, ref_bits(), None),
    "mem": ([0xd2824681, 0xf2aacf01, 0xf9000001, 0xb8008c01, 0x79800002, 0x39400403, 0xa9010801, 0x29421404,
             0xd2800066, 0xf8667807, 0x781f8401, 0xb8408008, 0xb980080a, 0xd100040b, 0x39c0056c, 0xf866d80d,
             0x29bf0402, 0x68c13c0e, 0xd65f03c0],
            {0: 0x10000},
            {0: 0x10000, 1: 0x56781234, 2: 0x1234, 3: 0x12, 4: 0x56781234, 5: 0, 6: 3, 7: 0x1234, 8: 0x56781234,
             10: 0x56781234, 11: 0xFFFF, 12: 0x34, 13: 0x56781234, 14: 0x1234, 15: 0x56781234}, None),
    "mul": ([0x9b010002, 0x9b018403, 0x9bc17c04, 0x9b417c05, 0x9adf0806, 0xd2f00007, 0x92800008, 0x9ac80ce9,
             0x1ac10c0a, 0x9b21000b, 0x9ba1840c, 0x1ac1080d, 0x12a2468e, 0xb2103fef, 0xd65f03c0],
            {0: A, 1: B}, ref_mul(), None),
    "flags": ([0xab010002, 0xba010003, 0x7a010004, 0x9a9f37e5, 0xfa411004, 0x9a9f17e6, 0x3a45b802, 0xda9f93e7,
               0xeb01001f, 0x9a81b008, 0xda81a009, 0xda81c40a, 0x9a80c40b, 0x7201001f, 0x1a9f07ec, 0xeb0103ed,
               0xda0003ee, 0x6b01001f, 0xd65f03c0],
              {0: A, 1: B}, ref_flags()[0], ref_flags()[1]),
    "branch": ([0xd2800001, 0xb40000a0, 0x36000040, 0x91000421, 0xd341fc00, 0x17fffffc, 0x10000002, 0x90000003,
                0x37f80041, 0x35000041, 0xd2800024, 0xaa0103e0, 0xd65f03c0],
               {0: 0xB6, 4: 77}, {0: 5, 1: 5, 2: BASE + 24, 3: (BASE + 28) & ~0xFFF, 4: 77}, None),
}

def crc_ref(acc, data, nbits, castagnoli):
    """CRC32 (via zlib, which inverts before and after) or CRC32C (bit by bit), the Arm ARM's CRC32 instructions"""
    if not castagnoli:
        import zlib
        return ~zlib.crc32((data & ((1 << nbits) - 1)).to_bytes(nbits // 8, "little"), ~acc & M32) & M32
    for _ in range(nbits):
        bit = (acc ^ data) & 1
        acc = (acc >> 1) ^ (0x82F63B78 if bit else 0)
        data >>= 1
    return acc


def nzcv_bits(f):
    return f[0] << 31 | f[1] << 30 | f[2] << 29 | f[3] << 28


PROGRAMS.update({
    # CRC32, MRS/MSR (TPIDR_EL0, TPIDRRO_EL0, NZCV, CTR_EL0), PAC as the identity, PACGA as 0
    "sys": ([0x1ac14002, 0x9ac15c03, 0x1ac14404, 0xd53bd045, 0xd51bd040, 0xd53bd046, 0xeb01001f, 0xd53b4207,
             0xd2ac0008, 0xd51b4208, 0x9a9f17e9, 0xd53b002a, 0xdac10020, 0xdac143e0, 0x9ac1300b, 0xd53bd06c,
             0xd65f03c0],
            {0: A, 1: B, "ctx": {0x310: 0x5555, 0x318: 0x7777}},
            {0: A, 2: crc_ref(A & M32, B, 8, False), 3: crc_ref(A & M32, B, 64, True), 4: crc_ref(A & M32, B, 16, False),
             5: 0x5555, 6: A, 7: nzcv_bits(nzcv(add_carry(A, ~B, 1))), 8: 0x60000000, 9: 1, 10: 0x8444C004, 11: 0,
             12: 0x7777}, (0, 1, 1, 0)),
    # BL through the callee's generic entry, then a tail call (B to another function)
    "calls": ([0xaa0003f3, 0x94000003, 0x8b130000, 0x14000003, 0x8b000400, 0xd65f03c0, 0x91019000, 0xd65f03c0],
              {0: A}, {0: (A * 4 + 100) & M64, 19: A, 30: None}, None, {"entries": [0, 4, 6]}),
    # a jump table: BR through the discovery's targets
    "jt": ([0x92400409, 0xd37ef529, 0x1000008a, 0x8b090149, 0xd61f0120, 0xd4200000, 0x14000004, 0x14000005,
            0x14000006, 0x14000007, 0xd2800140, 0xd65f03c0, 0xd2800160, 0xd65f03c0, 0xd2800180, 0xd65f03c0,
            0xd28001a0, 0xd65f03c0], {0: 6}, {0: 12}, None, {"jt": {4: [6, 7, 8, 9]}}),
})

HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
typedef struct { uint64_t x[32]; uint64_t pc; uint8_t n, z, c, v; uint8_t rest[0x300]; } Ctx;   /* (rt/a64.h) */
unsigned char a64_dirty[1u << 27];
void a64_poll_cur(void) {}
void a64_icall(Ctx *c, uint64_t t) { fprintf(stderr, "icall %%llx\n", (unsigned long long)t); }
void a64_ijump(Ctx *c, uint64_t t) { fprintf(stderr, "ijump %%llx\n", (unsigned long long)t); }
void a64_trap(Ctx *c, uint64_t pc, uint32_t w) { fprintf(stderr, "trap %%llx %%08x\n", (unsigned long long)pc, w); }
%(decls)s
int main(void) {
    void *mem = mmap((void *)(0x100000000000ULL + 0x8000), 0x18000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem == MAP_FAILED) return 2;
%(runs)s
    return 0;
}
"""
RUN = r"""    { Ctx c; memset(&c, 0, sizeof c); memset(mem, 0, 0x18000); %(init)s %(fn)s(&c);
      printf("%(name)s"); for (int i = 0; i < 32; i++) printf(" %%llx", (unsigned long long)c.x[i]);
      printf(" %%d %%d %%d %%d\n", c.n, c.z, c.c, c.v); }
"""


class Table(unittest.TestCase):
    def test_entries_do_not_overlap(self):
        rng = random.Random(7)
        for e in enc.ENTRIES:
            for _ in range(400):
                w = (rng.getrandbits(32) & ~e.mask) | e.value
                got = enc.matches(w)
                self.assertLessEqual(len(got), 1, "%08x matches %s" % (w, got))

    def test_templates_exist(self):
        from lift2 import sem
        for e in enc.ENTRIES:
            self.assertTrue(e.template in sem.TEMPLATES or e.template in build_ir.TERMINATORS, e.name)

    def test_known_words(self):
        for w, text in ((0x91060260, "add x0, x19, #384"), (0xaa0003f3, "orr x19, xzr, x0"),
                        (0xf946b108, "ldr x8, [x8, #3424]"), (0xa90257f6, "stp x22, x21, [sp, #32]"),
                        (0x7100069f, "subs wzr, w20, #1"), (0x93407c00, "sxtw x0, w0"),
                        (0xd65f0fff, "retab"), (0xd503237f, "hint #27"), (0xb8bfc008, "ldapr w8, [x0]"),
                        (0x1e202008, "fcmp s0, #0.0"), (0x4e083e00, "umov x0, v16.d[0]"),
                        (0x2f00e400, "movi d0, #0000000000000000"), (0x0d40c800, "ld1r { v0.2s }, [x0]")):
            i = enc.decode(w)
            self.assertEqual(asm.render(i) if i else None, text, "%08x" % w)


@unittest.skipUnless(have("llvm-mc"), "llvm-mc of the pinned LLVM major")
class AgainstLLVM(unittest.TestCase):
    def test_synthetic_words(self):
        words = sorted(set(selfcheck.synthetic_words(200, random.Random(3))))
        import io
        buf = io.StringIO()
        n, summary = selfcheck.check(words, show=10, out=buf)
        self.assertEqual(n, 0, buf.getvalue())
        self.assertGreater(summary["decoded"], 5000)


@unittest.skipUnless(sys.platform.startswith("linux") and have("clang"), "Linux and clang of the pinned LLVM major")
class EndToEnd(unittest.TestCase):
    def test_programs(self):
        m = emit_llvm.Module(flags_out=True)
        runs, decls = [], []
        for k, (name, spec) in enumerate(PROGRAMS.items()):
            words, inputs, expect, flags = spec[:4]
            extra = spec[4] if len(spec) > 4 else {}
            base = BASE if name == "branch" else BASE + 0x10000 * (k + 1)
            starts = [base + 4 * i for i in extra.get("entries", [0])]
            jt = {base + 4 * i: [base + 4 * t for t in ts] for i, ts in extra.get("jt", {}).items()}
            prog = build_ir.Program(entries=starts, jt=jt)

            def word(pc, words=words, base=base):
                return words[(pc - base) // 4]
            for n, e in enumerate(starts):
                end = starts[n + 1] if n + 1 < len(starts) else base + 4 * len(words)
                pcs = list(range(e, end, 4))
                self.assertIsNone(build_ir.scope(e, pcs, word, prog), name)
                fn = build_ir.build(e, pcs, word, prog)
                m.add(fn)
                decls.append("void %s(Ctx *);" % fn.name)
            init = " ".join("c.x[%d] = %#xULL;" % (r, v) for r, v in inputs.items() if r != "ctx")
            init += " ".join(" *(uint64_t *)((char *)&c + %d) = %#xULL;" % kv for kv in inputs.get("ctx", {}).items())
            runs.append(RUN % {"init": init, "fn": "g_%08x" % starts[0], "name": name})
        with tempfile.TemporaryDirectory() as d:
            ll, c, exe = (os.path.join(d, n) for n in ("f.ll", "h.c", "h"))
            with open(ll, "w") as fo:
                fo.write(m.text())
            with open(c, "w") as fo:
                fo.write(HARNESS % {"decls": "\n".join(sorted(set(decls))), "runs": "".join(runs)})
            subprocess.run([llvm.check("clang"), "-O1", "-w", c, ll, "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split("\n")
        got = {l.split()[0]: l.split()[1:] for l in out if l}
        for k, (name, spec) in enumerate(PROGRAMS.items()):
            expect, flags = spec[2], spec[3]
            base = BASE if name == "branch" else BASE + 0x10000 * (k + 1)
            regs = [int(v, 16) for v in got[name][:32]]
            for r, v in expect.items():
                if v is None and r == 30:
                    v = base + 8   # (the calls program: BL's return address)
                self.assertEqual(regs[r], v, "%s: x%d = %#x, expected %#x" % (name, r, regs[r], v))
            if flags is not None:
                self.assertEqual(tuple(int(v) for v in got[name][32:]), tuple(flags), "%s: NZCV" % name)


class VersionCheck(unittest.TestCase):
    def test_other_major_refused(self):
        from unittest import mock
        llvm._checked.pop("clang", None)
        with mock.patch.object(llvm, "tool", return_value="/x/clang"), \
                mock.patch.object(llvm, "major", return_value=llvm.PINNED_MAJOR + 1):
            with self.assertRaises(SystemExit) as e:
                llvm.check("clang")
        self.assertIn("LLVM %d" % llvm.PINNED_MAJOR, str(e.exception))
        llvm._checked.pop("clang", None)


if __name__ == "__main__":
    unittest.main()
