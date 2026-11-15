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
import gamedesc  # noqa: E402
gamedesc.use(os.path.join(ROOT, "games", "oot"))   # (the game whose host files and patches these read)
from lift2 import build_ir, emit_llvm, enc, llvm, probes  # noqa: E402
from lift2.build_ir import TERMINATORS  # noqa: E402

HOST = os.path.join(ROOT, "host", "oot_probes.c")


def have(tool):
    try:
        llvm.check(tool)
        return True
    except SystemExit:
        return False


class Table(unittest.TestCase):
    def test_rows(self):
        """the rows the host declares (host/patch.h, nx2/lift2/patch_scan.py): each a site the lifter can take"""
        self.assertGreater(len(probes.AT), 100)
        for p in probes.AT:
            i = enc.decode(p.word)
            self.assertIsNotNone(i, hex(p.pc))
            self.assertTrue(p.before or i.entry.template not in TERMINATORS or probes.is_call(p.word)
                            or p.replace and probes.is_tail(p.word), "%#x is a branch" % p.pc)
            self.assertTrue(not p.replace or probes.is_call(p.word) or probes.is_tail(p.word),
                            "%#x replaces no call or tail call" % p.pc)
            for a in p.args:
                self.assertRegex(a, probes.ARG)
            self.assertTrue(p.result is None or probes.RESULT.fullmatch(p.result), p.result)
            self.assertTrue(p.where[0].startswith("host/") and p.where[1] > 0, p.where)

    def test_word_checked(self):
        first = next(q for q in probes.AT if not q.before and not q.replace)
        rows = [probes.At(0x80000000, q.pc, q.word, q.fn, q.args, q.result, q.before, q.io, q.guest, q.replace)
                for q in probes.AT if q.fn == first.fn and not q.before and not q.replace]
        words = {q.pc: q.word for q in rows}
        real = probes.use([], rows)   # (their function: one of the test's own)
        try:
            self.assertEqual(sorted(probes.at(0x80000000, sorted(words), words.__getitem__)), sorted(words))
            with self.assertRaises(SystemExit):   # (a word that differs: another game version)
                probes.at(0x80000000, sorted(words), lambda pc: words[pc] ^ (pc == rows[0].pc))
            with self.assertRaises(SystemExit):   # (the instruction is not in the function)
                probes.at(0x80000000, sorted(words)[1:], words.__getitem__)
            self.assertEqual(probes.at(0x80001000, sorted(words), words.__getitem__), {})
        finally:
            probes.use(*real)


class ValueRows(unittest.TestCase):
    def test_word_checked(self):
        """a Value probe names its instruction and word: a differing word, or the instruction missing, stops the lift"""
        self.assertTrue(probes.PROBES)
        for q in probes.PROBES:
            p = probes.Value(q.pc, q.pc, q.word, q.fn, q.args, q.result)   # (its function: one of the test's own)
            real = probes.use([p], [])
            try:
                self.assertEqual(list(probes.find(p.func, [p.pc], lambda pc: p.word)), [p.pc])
                with self.assertRaises(SystemExit):
                    probes.find(p.func, [p.pc], lambda pc: p.word ^ 1)
                with self.assertRaises(SystemExit):
                    probes.find(p.func, [p.pc + 4], lambda pc: p.word)
            finally:
                probes.use(*real)


class Before(unittest.TestCase):
    def test_branch_target_refused(self):
        """an At probe before an instruction runs on the edge into it alone: a branch target there is refused"""
        base = 0x80003000
        words = [0x34000040, 0xd503201f, 0xd65f03c0]   # cbz w0, +8; nop; ret
        word = lambda pc: words[(pc - base) // 4]
        pcs = [base, base + 4, base + 8]
        real = probes.tables()
        try:
            probes.use([], [probes.At(base, base + 4, words[1], "t_none", ("x0",), before=True)])
            self.assertEqual(list(probes.at(base, pcs, word, before=True)), [base + 4])
            self.assertEqual(probes.at(base, pcs, word), {})
            probes.use([], [probes.At(base, base + 8, words[2], "t_none", ("x0",), before=True)])
            with self.assertRaises(SystemExit):
                probes.at(base, pcs, word, before=True)
        finally:
            probes.use(*real)

    def test_jump_table_and_entry_refused(self):
        """a jump table's target and another function's entry are edges into the instruction too: refused, with the
        declaration's file:line; the function's own entry is not"""
        base = 0x80004000
        words = [0xd61f0200, 0xd503201f, 0xd65f03c0]   # br x16; nop; ret
        word = lambda pc: words[(pc - base) // 4]
        pcs = [base, base + 4, base + 8]
        real = probes.tables()
        try:
            row = probes.At(base, base + 4, words[1], "t_none", ("x0",), before=True)
            row.where = ("host/t.c", 7)
            probes.use([], [row])
            self.assertEqual(list(probes.at(base, pcs, word, before=True, jt={}, entries={base})), [base + 4])
            with self.assertRaises(SystemExit) as e:
                probes.at(base, pcs, word, before=True, jt={base: [base + 4, base + 8]}, entries={base})
            self.assertIn("host/t.c:7: ", str(e.exception))
            self.assertIn("jump-table target", str(e.exception))
            with self.assertRaises(SystemExit) as e:
                probes.at(base, pcs, word, before=True, jt={}, entries={base, base + 4})
            self.assertIn("another function's entry", str(e.exception))
            probes.use([], [probes.At(base, base, words[0], "t_none", ("x0",), before=True)])
            self.assertEqual(list(probes.at(base, pcs, word, before=True, jt={}, entries={base})), [base])
        finally:
            probes.use(*real)


class ValueResult(unittest.TestCase):
    def test_destination_asserted(self):
        """a Value probe's result is its instruction's Rd at the instruction's width; an instruction without one is
        refused (dest)"""
        self.assertEqual(probes.dest(0x1e38000c), (12, 32))   # fcvtzs w12, s0
        self.assertEqual(probes.dest(0x9e38000c), (12, 64))   # fcvtzs x12, s0
        self.assertEqual(probes.dest(0xd280054d), (13, 64))   # mov x13, #42
        self.assertEqual(probes.dest(0x5280054d), (13, 32))   # mov w13, #42
        self.assertEqual(probes.dest(0x8b020020), (0, 64))    # add x0, x1, x2
        self.assertIsNone(probes.dest(0xf9000020))            # str x0, [x1]: no destination register
        self.assertIsNone(probes.dest(0xd65f03c0))            # ret
        self.assertIsNone(probes.dest(0xab020020))            # adds x0, x1, x2: the flags too
        word = lambda pc: 0x1e38000c
        for result, ok in (("w12", True), ("x12", False), ("w13", False)):
            p = probes.Value(0x1000, 0x1000, 0x1e38000c, "t_v", ("s0",), result)
            self.assertEqual(probes.errors(p, [0x1000], word) == [], ok, (result, probes.errors(p, [0x1000], word)))
        p = probes.Value(0x1000, 0x1000, 0x1e38000c, "t_v", ("w1",), "w12")
        self.assertIn("operands", probes.errors(p, [0x1000], word)[0])
        p = probes.Value(0x1000, 0x1000, 0xf9000020, "t_v", ("x1",), "w0")
        self.assertIn("no single general register", probes.errors(p, [0x1000], lambda pc: 0xf9000020)[0])


LOWER_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint64_t x[32]; uint64_t pc; uint8_t n, z, c, v; int halted; uint64_t vreg[32][2];
                 uint64_t tpidr, tpidrro, fpcr, fpsr; } Ctx;
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
        real = probes.tables()
        try:
            probes.use([], rows)
            prog = build_ir.Program()
            prog.after = probes.merge(probes.at(base, pcs, word))
        finally:
            probes.use(*real)
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

    def test_declared_kinds(self):
        """the row kinds beyond one plain result: a vector in and out (q3 := t_q(q3, x2), lane 1 + x2), registers
        rewritten through a struct (t_io: x1 += x2, x4 *= 2), a guest load and SP as values (x6 := t_load([sp+8], sp),
        the guest window mapped at its address for the run), and the BL replaced by a host function with a GuestCall (x0 := t_replace(ctx, x0):
        the callee g_80004000 is not called)"""
        base = 0x80003000
        # nop; nop; bl 0x80004000; add x9, x0, #1; ret
        words = [0xd503201f, 0xd503201f, 0x940003fe, 0x91000409, 0xd65f03c0]
        word = lambda pc: words[(pc - base) // 4]
        pcs = [base + 4 * k for k in range(len(words))]
        rows = [probes.At(base, base, words[0], "t_q", ("q3", "x2"), "q3"),
                probes.At(base, base, words[0], "t_io", ("x2",), io=("x1", "x4")),
                probes.At(base, base + 4, words[1], "t_load", ("[sp+8]", "sp"), "x6"),
                probes.At(base, base + 8, words[2], "t_replace", ("x0",), "x0", replace=True, guest="computed")]
        real = probes.tables()
        try:
            probes.use([], rows)
            prog = build_ir.Program()
            prog.after = probes.merge(probes.at(base, pcs, word))
            prog.replace = probes.at(base, pcs, word, replace=True)
            self.assertEqual(list(prog.replace), [base + 8])
            self.assertEqual(rows[1].effects(), ({"x2", "x1", "x4"}, {"x1", "x4"}))
            self.assertEqual(rows[2].effects(), ({"sp"}, {"x6"}))
        finally:
            probes.use(*real)
        fn = build_ir.build(base, pcs, word, prog)
        self.assertIn("load", {v.op for blk in fn.blocks.values() for v in blk.ops})
        m = emit_llvm.Module()
        m.add(fn)
        text = m.text()
        self.assertNotIn("@g_80004000", text)
        with tempfile.TemporaryDirectory() as d:
            ll, c, exe = (os.path.join(d, x) for x in ("f.ll", "h.c", "h"))
            # (the guest load at [sp + 8] reads the window: t_load is given a no-op body on the host and sp 0)
            with open(ll, "w") as fo:
                fo.write(text)
            with open(c, "w") as fo:
                fo.write(KINDS_HARNESS)
            subprocess.run([llvm.check("clang"), "-O1", "-w", c, ll, "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()
        if out != ["skip"]:
            # (q3: lane 1 2 + 5; x1 10 + 5, x4 6 * 2; x6 the word at [sp + 8], t_load saw sp; x0 3 * 100, not 999; x9 x0 + 1)
            self.assertEqual(out, ["1", "7", "15", "12", "85", "256", "300", "301"])

    def test_after_a_branch_refused(self):
        base = 0x80003000
        words = [0x14000002, 0xd503201f, 0xd65f03c0]   # b +8; nop; ret
        word = lambda pc: words[(pc - base) // 4]
        prog = build_ir.Program()
        prog.after = {base: [lambda b: None]}
        with self.assertRaises(ValueError):
            build_ir.build(base, [base, base + 4, base + 8], word, prog)


KINDS_HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
typedef struct { uint64_t x[32]; uint64_t pc; uint8_t n, z, c, v; int halted; uint64_t vreg[32][2];
                 uint64_t tpidr, tpidrro, fpcr, fpsr; } Ctx;
typedef uint64_t q __attribute__((vector_size(16)));
void g_80003000(Ctx *);
void g_80004000(Ctx *c) { c->x[0] = 999; }
static uint64_t seen_sp = 7;
q t_q(q a, uint64_t b) { a[1] += b; return a; }
void t_io(uint64_t k, uint64_t *io) { io[0] += k, io[1] *= 2; }
uint64_t t_load(uint64_t v, uint64_t sp) { seen_sp = sp; return v; }
uint64_t t_replace(Ctx *g, uint64_t x0) { return g ? x0 * 100 : 0; }
int main(void) {
    uint8_t *w = mmap((void *)0x100000000000ULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (w == MAP_FAILED) { puts("skip"); return 0; }
    const uint64_t v = 0x55; memcpy(w + 0x108, &v, 8);   /* ([sp + 8], sp 0x100) */
    Ctx c; memset(&c, 0, sizeof c);
    c.x[0] = 3, c.x[1] = 10, c.x[2] = 5, c.x[4] = 6, c.x[31] = 0x100, c.vreg[3][0] = 1, c.vreg[3][1] = 2;
    g_80003000(&c);
    printf("%llu %llu %llu %llu %llu %llu %llu %llu\n", (unsigned long long)c.vreg[3][0], (unsigned long long)c.vreg[3][1],
           (unsigned long long)c.x[1], (unsigned long long)c.x[4], (unsigned long long)c.x[6], (unsigned long long)seen_sp,
           (unsigned long long)c.x[0], (unsigned long long)c.x[9]);
    return 0;
}
"""

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
static int seen_loading = -1, seen_event = -1;
void hos_bench_player(int loading, int event_playing) { seen_loading = loading, seen_event = event_playing; }
volatile uint64_t hos_wide_out, hos_wide_scene;
volatile float hos_wide_ratio, hos_wide_margin, hos_wide_squeeze;
volatile int hos_occlusion_off;
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
    /* the player's update for HOS_BENCH: Link's actor chain, the loading bool by its hash, the event manager */
    {
        const uint64_t actor = 0x8000, comp = 0x8400, mv = 0xad50, res = 0xa000;   /* (Link's life actor at mv - 0x1d50) */
        G64(actor) = 0x84b45e18ULL; G64(actor + 624) = comp; G64(comp + 800) = mv; G64(mv + 24) = actor;
        G64(mv - 0x1d50) = 0x84b6c460ULL; G64(mv - 0x1d50 + 16) = res; memcpy(mem + res + 88, "Player", 7);
        G64(0xe180) = 0xb000; G64(0xb000) = 0xb100;   /* (GameDataMgr: slot -> holder -> manager) */
        G32(0xb100 + 16) = 4; G64(0xb100 + 24) = 0xb200; G64(0xb100 + 32) = 0xb300; G32(0xb100 + 44) = 8;
        G32(0xb300 + (0xef2b60c2u % 8) * 8) = 0xef2b60c2u; G32(0xb300 + (0xef2b60c2u % 8) * 8 + 4) = 2;
        mem[0xb200 + 2 * 24 + 22] = 1;
        G64(0xe090) = 0xb400; G64(0xb400) = 0xb500; G64(0xb500 + 104) = 0;
        hos_oot_player_update(actor); CHECK(seen_loading == -1);   /* (no bench: nothing) */
        bench_on = 1; hos_oot_player_update(actor); CHECK(seen_loading == 1 && seen_event == 0);
        mem[0xb200 + 2 * 24 + 22] = 0; G64(0xb500 + 104) = 1; hos_oot_player_update(actor); CHECK(seen_loading == 0 && seen_event == 1);
        seen_loading = -1; memcpy(mem + res + 88, "Navi\0\0", 7); hos_oot_player_update(actor); CHECK(seen_loading == -1);
    }
    /* widescreen: the game's values while the window is 16:9 (the globals 0), the picture's otherwise */
    CHECK(hos_oot_wide_out(0x5a000000a00ULL) == 0x5a000000a00ULL && hos_oot_wide_ratio(16.0f / 9) == 16.0f / 9);
    hos_wide_out = 0x5a000000d70ULL; hos_wide_ratio = 2.3889f; hos_wide_scene = 0x43800000a18ULL;
    CHECK(hos_oot_wide_out(0x5a000000a00ULL) == 0x5a000000d70ULL && hos_oot_wide_scene(1) == 0x43800000a18ULL);
    CHECK(hos_oot_wide_ratio(16.0f / 9) == 2.3889f && hos_oot_wide_ratio(1.0f) == 1.0f);
    CHECK(hos_oot_wide_projection_ratio(16.0f / 9, 0x6000) == 2.3889f && G32(0x6000 + 0xb0) == bits(2.3889f));
    G32(0x6000 + 0xb0) = 0; CHECK(hos_oot_wide_projection_ratio(1.0f, 0x6000) == 1.0f && G32(0x6000 + 0xb0) == 0);
    G32(0x7000 - 40 + 8) = bits(0.0f), G32(0x7000 - 40 + 16) = bits(100.0f);
    hos_oot_wide_movie(0x7000); CHECK(G32(0x7000 - 40 + 8) == bits(0.0f));   /* (no margin: as it was) */
    hos_wide_margin = 0.125f; hos_oot_wide_movie(0x7000);
    CHECK(G32(0x7000 - 40 + 8) == bits(12.5f) && G32(0x7000 - 40 + 16) == bits(87.5f));
    /* the HUD: the UI's orthographic rows squeezed into the middle 16:9, other projections and the kept screens as they are */
    {
        const float r0[4] = {2.0f / 3840.0f, 0, 0, -1}, r1[4] = {0, 2.0f / 2160.0f, 0, 1};
        hos_q q1, q4, out;
        memcpy(&q1, r0, 16), memcpy(&q4, r1, 16);
        G64(0x8000 - 0x1b8) = 0x1234;   /* (the canvas's screen: an ordinary one) */
        out = hos_oot_wide_hud(0x8000, q1, q4); CHECK(!memcmp(&out, &q1, 16));   /* (16:9: squeeze 0) */
        hos_wide_squeeze = 0.5f;
        out = hos_oot_wide_hud(0x8000, q1, q4);
        float o[4]; memcpy(o, &out, 16); CHECK(o[0] == r0[0] * 0.5f && o[3] == -0.5f);
        G64(0x8000 - 0x1b8) = OOT_MAIN + 0x4bed098; out = hos_oot_wide_hud(0x8000, q1, q4); CHECK(!memcmp(&out, &q1, 16));
        hos_wide_squeeze = 0;
    }
    /* occlusion off: the byte reads 0 */
    CHECK(hos_oot_occlusion_flag(1) == 1); hos_occlusion_off = 1; CHECK(hos_oot_occlusion_flag(1) == 0);
    /* the per-core rings: the first writer owns a ring; another thread is logged once, the owner never */
    {
        G64(0xe890) = 0xc000; G64(0xc000) = 0xc100;   /* (slot -> holder -> the singleton) */
        const uint64_t r2 = 0xc100 + 0x20 + 2 * 0x30;
        cur_thread = (HosThread *)"BaseProcExecutor2"; logged = 0;
        hos_oot_ring_write(r2, 0, 0x800b4228); hos_oot_ring_write(r2, 1, 0x800b4468);
        CHECK(logged == 1 && rings[2].owner == cur_thread && rings[2].pushes == 1 && rings[2].pops == 1);
        cur_thread = (HosThread *)"ModuleSystemWorker2";
        hos_oot_ring_write(r2, 0, 0x8004c820); hos_oot_ring_write(r2, 0, 0x8004c820);
        CHECK(logged == 2 && ring_conflicts == 1 && rings[2].pushes == 3);
        hos_oot_ring_write(0xc100 + 0x20 + 7 * 0x30, 0, 0); CHECK(logged == 2);   /* (past the seven rings: ignored) */
        hos_present_count = 3; hos_oot_ring_report(); CHECK(logged == 3);
    }
    if (!bad) printf("ok\n");
    return bad;
}
"""
STUB_HOS = """#pragma once
#include <stdint.h>
extern uint8_t mem[1 << 16];
#define A64_PTR(a) (mem + ((uint64_t)(a) & 0xffff))   /* (guest addresses folded into the small memory) */
static inline void *hos_guest_range(uint64_t a, uint64_t n) { return a && (a & 0xffff) + n <= sizeof mem ? A64_PTR(a) : 0; }
static int bench_on;
static int hos_bench_on(void) { return bench_on; }
#define GP(a) ((void *)A64_PTR(a))
typedef uint64_t hos_q __attribute__((vector_size(16)));
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
static int logged;
static void hos_log(const char *f, ...) { (void)f; logged++; }
typedef struct HosThread HosThread;
static HosThread *cur_thread;
static HosThread *hos_self(void) { return cur_thread; }
static const char *hos_thread_name(HosThread *t) { return (const char *)t; }
volatile int64_t hos_present_count;
#include "patch.h"
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
            shutil.copy(os.path.join(ROOT, "host", "patch.h"), d)   # (the declarations: PATCH_AT)
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
