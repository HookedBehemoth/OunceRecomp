"""The frame-rate and resolution patches as lift 2 At probes (nx2/lift2/probes.py AT) and their host functions
(host/oot_probes.c), without the game.

- The table: every row's word decodes to an instruction a probe can follow (not a branch; a BL / BLR is followed when it
  returns), its arguments and result are registers the grammar knows, and a differing word stops the lift.
- The lowering: a synthetic function with At probes of every kind (W, X and S results, none, after a BL) lifted, compiled
  and run against host functions of plain values: the registers after it are what the host functions made of them.
- The host functions, compiled with a small guest memory: the dynamic path's flag, the fixed scale's recompute, the
  output size, the frame seconds' quotient, the render-target pool.

python -I -B tests/test_oot_probes.py      (the compile-and-run cases need Linux and clang of the pinned LLVM major)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import build_ir, emit_llvm, enc, llvm, probes  # noqa: E402
from lift2.build_ir import TERMINATORS  # noqa: E402

HOST = os.path.join(ROOT, "host", "oot_probes.c")
ARG = re.compile(r"[xws]([0-9]|[12][0-9]|30)|#\d+")


def have(tool):
    try:
        llvm.check(tool)
        return True
    except SystemExit:
        return False


class Table(unittest.TestCase):
    def test_rows(self):
        host = ""
        for name in (HOST, os.path.join(ROOT, "host", "mouse_native.c")):
            with open(name) as f:
                host += f.read()
        for p in probes.AT:
            i = enc.decode(p.word)
            self.assertIsNotNone(i, hex(p.pc))
            self.assertTrue(p.before or i.entry.template not in TERMINATORS or probes.is_call(p.word), "%#x is a branch" % p.pc)
            self.assertLessEqual(p.func, p.pc)
            for a in p.args:
                self.assertRegex(a, ARG)
            self.assertTrue(p.result is None or ARG.fullmatch(p.result) and p.result[0] != "#", p.result)
            self.assertIn(" %s(" % p.fn, host, "%s is not in host/oot_probes.c or host/mouse_native.c" % p.fn)
        self.assertEqual({p.func for p in probes.AT}, probes.AT_FUNCS)

    def test_word_checked(self):
        p = probes.AT[0]
        words = {q.pc: q.word for q in probes.AT if q.func == p.func}
        self.assertEqual(sorted(probes.at(p.func, sorted(words), words.__getitem__)), sorted(words))
        with self.assertRaises(SystemExit):   # (a word that differs: another game version)
            probes.at(p.func, sorted(words), lambda pc: words[pc] ^ (pc == p.pc))
        with self.assertRaises(SystemExit):   # (the instruction is not in the function)
            probes.at(p.func, sorted(words)[1:], words.__getitem__)
        self.assertEqual(probes.at(0x80000000, sorted(words), words.__getitem__), {})


class Before(unittest.TestCase):
    def test_branch_target_refused(self):
        """an At probe before an instruction runs on the edge into it alone: a branch target there is refused"""
        base = 0x80003000
        words = [0x34000040, 0xd503201f, 0xd65f03c0]   # cbz w0, +8; nop; ret
        word = lambda pc: words[(pc - base) // 4]
        pcs = [base, base + 4, base + 8]
        real = probes.AT
        try:
            probes.AT = [probes.At(base, base + 4, words[1], "t_none", ("x0",), before=True)]
            self.assertEqual(list(probes.at(base, pcs, word, before=True)), [base + 4])
            self.assertEqual(probes.at(base, pcs, word), {})
            probes.AT = [probes.At(base, base + 8, words[2], "t_none", ("x0",), before=True)]
            with self.assertRaises(SystemExit):
                probes.at(base, pcs, word, before=True)
        finally:
            probes.AT = real


LOWER_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t x[32]; uint64_t pc; uint8_t n, z, c, v; int halted; uint64_t vreg[32][2];
                 uint64_t tpidr, tpidrro, fpcr, fpsr; } Ctx;
unsigned char a64_dirty[1u << 27];
void a64_poll_cur(void) {}
void g_80003000(Ctx *);
void g_80004000(Ctx *c) { c->x[0] = 10; }
static uint64_t seen;
uint32_t t_w(uint32_t a, uint32_t k) { return a * 3 + k; }
uint64_t t_x(uint64_t a, uint64_t b) { return a + b; }
float t_s(float f) { return f * 2; }
void t_none(uint64_t a) { seen = a; }
uint64_t t_call(uint64_t r) { return r + 5; }
int main(void) {
    Ctx c; memset(&c, 0, sizeof c);
    float f = 1.5f; uint32_t u; memcpy(&u, &f, 4);
    c.x[1] = 0xffffffff00000007ULL; c.x[2] = 100; c.vreg[3][0] = u; c.vreg[3][1] = 9;
    g_80003000(&c);
    memcpy(&f, &c.vreg[3][0], 4);
    printf("%llx %llu %g %llu %llu %llu %llu\n", (unsigned long long)c.x[1], (unsigned long long)c.x[2], f,
           (unsigned long long)c.vreg[3][1], (unsigned long long)seen, (unsigned long long)c.x[0], (unsigned long long)c.x[9]);
    return 0;
}
"""


@unittest.skipUnless(sys.platform.startswith("linux") and have("clang"), "Linux and clang of the pinned LLVM major")
class Lowering(unittest.TestCase):
    def test_kinds(self):
        """w1 := t_w(w1, 2) (a W result zero-extends), x2 := t_x(x2, x1), s3 := t_s(s3) (the rest of V3 zero),
        t_none(x2), then after a BL x0 := t_call(x0), and the code after reads them (x9 = x0 + 1)"""
        base = 0x80003000
        # nop; nop; bl 0x80004000; add x9, x0, #1; ret
        words = [0xd503201f, 0xd503201f, 0x940003fe, 0x91000409, 0xd65f03c0]
        word = lambda pc: words[(pc - base) // 4]
        pcs = [base + 4 * k for k in range(len(words))]
        rows = [probes.At(base, base, words[0], "t_w", ("w1", "#2"), "w1"),
                probes.At(base, base, words[0], "t_x", ("x2", "x1"), "x2"),
                probes.At(base, base + 4, words[1], "t_s", ("s3",), "s3"),
                probes.At(base, base + 4, words[1], "t_none", ("x2",)),
                probes.At(base, base + 8, words[2], "t_call", ("x0",), "x0")]
        real = probes.AT
        try:
            probes.AT = rows
            prog = build_ir.Program()
            prog.after = probes.merge(probes.at(base, pcs, word))
        finally:
            probes.AT = real
        fn = build_ir.build(base, pcs, word, prog)
        m = emit_llvm.Module()
        m.add(fn)
        with tempfile.TemporaryDirectory() as d:
            ll, c, exe = (os.path.join(d, x) for x in ("f.ll", "h.c", "h"))
            with open(ll, "w") as fo:
                fo.write(m.text())
            with open(c, "w") as fo:
                fo.write(LOWER_HARNESS)
            subprocess.run([llvm.check("clang"), "-O1", "-w", c, ll, "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
        # (w1: 7 * 3 + 2 = 23, the upper half gone; x2: 100 + 23; s3: 3 with V3's upper half 0; t_none saw x2; x0: 15)
        self.assertEqual(out, ["17", "123", "3", "0", "123", "15", "16"])

    def test_after_a_branch_refused(self):
        base = 0x80003000
        words = [0x14000002, 0xd503201f, 0xd65f03c0]   # b +8; nop; ret
        word = lambda pc: words[(pc - base) // 4]
        prog = build_ir.Program()
        prog.after = {base: [lambda b: None]}
        with self.assertRaises(ValueError):
            build_ir.build(base, [base, base + 4, base + 8], word, prog)


HOST_HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "hos.h"
#include "oot_probes.c"
uint8_t mem[1 << 16];
volatile int hos_fixed_resolution, hos_dynres_runs, hos_native_scene, hos_fix_res;
volatile uint32_t hos_fixed_scale_bits = 0x3f400000, hos_frame_seconds_bits;
volatile uint64_t hos_res_module, hos_step_probe;
volatile float hos_text_scale = 1.0f;
static uint32_t out_w = 3840, out_h = 2160;
uint32_t hos_output_size(int height) { return height ? out_h : out_w; }
uint64_t hos_rt_pool_size(uint64_t game) { return game * out_w / 2560 * out_h / 1440; }
uint32_t hos_movie_frames(uint64_t player) { return player == 7 ? 2 : 1; }
static int bad;
#define CHECK(x) do { if (!(x)) { printf("FAIL:%d %s\n", __LINE__, #x); bad = 1; } } while (0)
static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
int main(void) {
    /* the dynamic path's flag: bit 0 cleared while the resolution is fixed; counted when the game takes that path */
    hos_fixed_resolution = 1; CHECK(hos_oot_dynres_flags(0x81) == 0x80 && hos_dynres_runs == 0);
    hos_fixed_resolution = 0; CHECK(hos_oot_dynres_flags(0x81) == 0x81 && hos_dynres_runs == 1);
    CHECK(hos_oot_fixed_scale() == 0x3f400000);
    /* the recompute bit while +0xe5c is not the chosen scale (0.75 * 1440 / 1440), not once it is */
    hos_fixed_resolution = 1;
    G32(0x1000 + 0xe50) = bits(1440), G32(0x1000 + 0xe58) = bits(1440), G32(0x1000 + 0xe5c) = bits(1.0f);
    CHECK(hos_oot_scale_recompute(0x1000, 0) == 8);
    G32(0x1000 + 0xe5c) = bits(0.75f); CHECK(hos_oot_scale_recompute(0x1000, 0) == 0);
    CHECK(hos_oot_scale_flags(0x1000, 0x1f) == 0x07 && hos_res_module == 0x1000);
    hos_fixed_resolution = 0; CHECK(hos_oot_scale_flags(0x1000, 0x1f) == 0x1f);
    /* output size */
    CHECK(hos_oot_output_size() == (3840 | (uint64_t)2160 << 32));
    CHECK(hos_oot_base_size(1920, 0) == 2880 && hos_oot_base_size(1080, 1) == 1620);
    CHECK(hos_oot_base_size_bits(0) == bits(2880.0f) && hos_oot_base_size_bits(1) == bits(1620.0f));
    G32(0x2000 + 8) = 2560; hos_oot_output_config(0x2000); CHECK(G32(0x2000 + 8) == 3840 && G32(0x2000 + 12) == 2160);
    /* frame seconds: base * the frame's seconds while the host gives them, the game's quotient otherwise */
    CHECK(hos_oot_frame_step_probe(60.0f, 1.0f, 0x5000) == 1.0f && hos_step_probe == 0x5000);
    hos_frame_seconds_bits = bits(1.0f / 120); CHECK(hos_oot_frame_step(60.0f, 1.0f) == 0.5f);
    /* native scene: the stock table's mode 1 made 1.0 and the mode compared unequal once; another table left alone */
    hos_native_scene = 1;
    { const float stock[6] = {1.0f, 1.0f, 0.8f, 0.8f, 2.0f / 3, 2.0f / 3};
      memcpy(mem + 0x5000 + 724, stock, sizeof stock); memcpy(mem + 0x5000 + 1244, stock, sizeof stock);
      ((float *)(mem + 0x5000 + 1244))[4] = 0.5f;
      CHECK(hos_oot_scene_mode(0x5000, 1) == 0xff);
      CHECK(G32(0x5000 + 724 + 8) == bits(1.0f) && G32(0x5000 + 724 + 12) == bits(1.0f) && G32(0x5000 + 1244 + 8) == bits(0.8f));
      CHECK(hos_oot_scene_mode(0x5000, 1) == 1); }
    /* HOS_FIX_RES: the dirty bit while the cached scale differs */
    hos_fixed_resolution = 1; hos_fix_res = 1; G32(0x1000 + 0xe2c) = 0; G32(0x1000 + 0xe5c) = bits(1.0f);
    hos_oot_scene_check(0x1000); CHECK(G32(0x1000 + 0xe2c) == 8);
    G32(0x1000 + 0xe2c) = 0; G32(0x1000 + 0xe5c) = bits(0.75f); hos_oot_scene_check(0x1000); CHECK(G32(0x1000 + 0xe2c) == 0);
    CHECK(hos_oot_text_time(0.5f) == 0.5f); hos_text_scale = 3.0f; CHECK(hos_oot_text_time(0.5f) == 1.5f);
    CHECK(hos_oot_movie_count(10, 7) == 11 && hos_oot_movie_count(10, 1) == 10);
    /* the render-target pool */
    G64(0x3000 + 1464) = 0x13100000; hos_oot_rt_pool_config(0x3000);
    CHECK(G64(0x3000 + 1464) == hos_rt_pool_size(0x13100000));
    const uint64_t g = hos_rt_pool_size(0x13100000) - 0x13100000;
    CHECK(hos_oot_rt_heap_size(0x34900000) == 0x34900000 + g && hos_oot_rt_heap_size(0x1000) == 0x1000);
    G64(0x4000 + 2 * 8) = 0x348ffe20; CHECK(hos_oot_rt_heap_use(g + 5, 0x4000, 2) == 5 && hos_oot_rt_heap_use(g + 5, 0x4000, 1) == g + 5);
    CHECK(hos_oot_rt_rest(g + 0x1b6cb48) == 0x1b6cb48);
    if (!bad) printf("ok\n");
    return bad;
}
"""
STUB_HOS = """#pragma once
#include <stdint.h>
extern uint8_t mem[1 << 16];
#define A64_PTR(a) (mem + (a))
#define GP(a) ((void *)A64_PTR(a))
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
static void hos_log(const char *f, ...) { (void)f; }
"""


def cc():
    return shutil.which("clang") or shutil.which("cc")


@unittest.skipUnless(cc(), "needs a C compiler")
class Host(unittest.TestCase):
    def test_functions(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "hos.h"), "w") as f:
                f.write(STUB_HOS)
            shutil.copy(HOST, d)   # (beside the stub hos.h: its #include "hos.h" looks in its own directory first)
            h = os.path.join(d, "h.c")
            with open(h, "w") as f:
                f.write(HOST_HARNESS)
            exe = os.path.join(d, "h")
            r = subprocess.run([cc(), "-O1", "-Wall", "-Werror", "-I", d, h, "-o", exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, "ok"), r.stdout)


if __name__ == "__main__":
    unittest.main()
