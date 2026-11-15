"""a64_lift.mark_groups: the page marks of a straight-line run of stores through one base register shared by one A64_MARK
after the group's last store, every case where a store must keep its own marks, and its place in the lift: after
nx2/oot_patches.py, whose exact-text edits a group would break.

python -I -B tests/test_mark_groups.py      (the compile-and-run case needs emsdk's clang: EMSDK, else it is skipped)
"""
import contextlib
import glob
import importlib.util
import inspect
import io
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from collections import Counter
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = [os.path.join(ROOT, "third_party", "a64-kit", d) for d in ("lift", "tools")] + [os.path.join(ROOT, "nx2")]
import a64_lift  # noqa: E402
import chunks  # noqa: E402

HEAD = "void f_80000000(Ctx *__restrict A64_NOESC c) {"


def mark(reg, lo, n):
    return "    A64_MARK((c->x[%d] + (uint64_t)(int64_t)(%d)), %d);" % (reg, lo, n)


def run(*body, stats=None):
    return a64_lift.mark_groups("\n".join((HEAD,) + body + ("}",)), stats).split("\n")[1:-1]


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def quiet(fn, *a, **k):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*a, **k)


class MarkGroupsTests(unittest.TestCase):
    def test_a_group_shares_one_mark_before_the_call(self):
        out = run("    recomp_store64(c,c->x[19] + 8,c->x[1]);",
                  "    recomp_store32(c,c->x[19] + 16,c->x[2]);",
                  "    { uint64_t _b=c->x[19]; int64_t _o=24; recomp_stp64(c,(_b+_o),c->x[3],c->x[4]); }",
                  "    c->x[0]=recomp_load32(c,c->x[20] + 0);",
                  "    TAILCALL(f_80000100);")
        self.assertEqual(out, ["    recomp_store64_sp(c,c->x[19] + 8,c->x[1]);",
                               "    recomp_store32_sp(c,c->x[19] + 16,c->x[2]);",
                               "    { uint64_t _b=c->x[19]; int64_t _o=24; recomp_stp64_sp(c,(_b+_o),c->x[3],c->x[4]); }",
                               "    c->x[0]=recomp_load32(c,c->x[20] + 0);",
                               mark(19, 8, 32),   # [8, 40): every byte the group wrote
                               "    TAILCALL(f_80000100);"])

    def test_a_write_of_the_base_marks_before_it(self):
        for write in ("    c->x[8]=c->x[8] + 16;", "    c->x[8] += 16;", "    c->x[8]++;", "    --c->x[8];",
                      "    { uint64_t _p0,_p1; recomp_ldp64(c,c->x[9] + 0,&c->x[8],&_p1); }"):
            out = run("    recomp_store64(c,c->x[8] + 0,c->x[1]);", "    recomp_store64(c,c->x[8] + 8,c->x[2]);", write,
                      "    recomp_store64(c,c->x[8] + 0,c->x[3]);", "    a64_trap(c, 1);")
            self.assertEqual(out[2], mark(8, 0, 16), write)
            self.assertEqual(out[3], write)
            self.assertEqual(out[4], "    recomp_store64(c,c->x[8] + 0,c->x[3]);", write)   # (alone after it: its own marks)

    def test_every_run_end_marks_before_it(self):
        for end in ("L_80000010:;", "L_80000010:; A64_POLL(c);", "if (!c->z) { goto L_80000010; }", "    recomp_barrier();",
                    "if (!c->z) { { A64_POLL(c); goto L_80000010; } }",   # (a backward goto with its interrupt poll: nx2/poll.py)
                    "    a64_svc(c, 0x60);", "    A64_ICALL(c, c->x[16]);", "    A64_IJUMP(c, c->x[16]); return;",
                    "    __atomic_fetch_add((uint32_t *)A64_PTR(c->x[2]), 1, __ATOMIC_SEQ_CST);",
                    "    c->x[30] = 0x80000020ULL; f_80001000(c);", "    return;",
                    # (store-release and load-acquire as suyu writes them, without nx2/insn.py: a plain store / load at
                    # the bare register)
                    "    recomp_store32(c,c->x[3],c->x[4]);", "    c->x[5] = recomp_load32(c,c->x[3]);",
                    # (host calls the patches put in, and any helper the pass does not know)
                    "    c->x[8] = hos_output_size(0) | ((uint64_t)hos_output_size(1) << 32);",
                    "    hos_oot_lift_player(c, c->x[19]);", "    a64_new_helper(c);", "    memcpy(A64_PTR(c->x[2]), &c->x[3], 8);",
                    # (the register file as a whole; a block opening or closing over several lines)
                    "    memset(c->x, 0, sizeof c->x);", "    {", "    }", "    if (c->z) {", "    } else {"):
            out = run("    recomp_store32(c,c->x[21] + 0,c->x[1]);", "    recomp_store32(c,c->x[21] + 4,c->x[2]);", end,
                      "    recomp_store32(c,c->x[21] + 8,c->x[3]);")
            self.assertEqual(out[2], mark(21, 0, 8), end)
            self.assertEqual(out[3], end)

    def test_plain_lines_do_not_end_a_run(self):
        for plain in ("    c->x[0]=recomp_load64(c,c->x[20] + 8);",
                      "    { uint64_t _p0,_p1; recomp_ldp64(c,(c->x[20]+16ULL),&_p0,&_p1); c->x[0]=_p0; c->x[1]=_p1; }",
                      "    { uint64_t _v=0; memcpy(&_v,&c->vreg[3][0],8); a64_vset64(c->vreg[1],_v); }",
                      "    { extern volatile uint64_t hos_wide_out; const uint64_t _s = hos_wide_out; if (_s) c->x[8] = _s; }",
                      "    c->x[9] = UINT64_C(1) << 3;", "    /* ic ivau: no self-modifying code */"):
            out = run("    recomp_store32(c,c->x[21] + 0,c->x[1]);", plain, "    recomp_store32(c,c->x[21] + 4,c->x[2]);",
                      "    return;")
            self.assertEqual(out[3], mark(21, 0, 8), plain)

    def test_stores_that_keep_their_own_marks(self):
        cases = [
            # (a conditional store: it may not run)
            ("    if (c->z) recomp_store32(c,c->x[19] + 0,c->x[1]);", "    recomp_store32(c,c->x[19] + 4,c->x[2]);"),
            ("    c->x[0] ? recomp_store32(c,c->x[19] + 0,c->x[1]) : (void)0;", "    recomp_store32(c,c->x[19] + 4,c->x[2]);"),
            # (more than 4 KB apart: more than two pages)
            ("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    recomp_store64(c,c->x[19] + 4089,c->x[2]);"),
            # (a line that also writes a register; pre- and post-index writeback)
            ("    recomp_store64(c,c->x[19] + 0,c->x[1]); c->x[1]=0;", "    recomp_store64(c,c->x[19] + 8,c->x[2]); c->x[2]=0;"),
            ("    { uint64_t _b=c->x[19]; int64_t _o=16; recomp_store64(c,(_b+_o),c->x[1]); c->x[19]=_b+_o; }",
             "    { uint64_t _b=c->x[19]; int64_t _o=16; recomp_store64(c,_b,c->x[2]); c->x[19]=_b+_o; }"),
            # (an address the pass does not follow: a register offset)
            ("    recomp_store64(c,c->x[19] + c->x[2],c->x[1]);", "    recomp_store64(c,c->x[19] + 8 * c->x[3],c->x[2]);"),
            # (the STLR form: it ends the run besides)
            ("    recomp_store32(c,c->x[19],c->x[1]);", "    recomp_store32(c,c->x[19],c->x[2]);"),
        ]
        for body in cases:
            out = run(*body, "    TAILCALL(f_80000100);")
            self.assertFalse(any("A64_MARK" in l or "_sp(" in l for l in out), body)

    def test_address_forms(self):
        q = "    { { uint64_t _v=0; memcpy(&_v,&c->vreg[3][0],8); recomp_store64(c,(c->x[0]+%dULL),_v); } }"
        cases = [
            # (Q / D stores at (c->x[N]+IMMULL))
            ((q % 16, q % 24), mark(0, 16, 16)),
            # (STP of two Q registers: the second at (_b+_o)+16)
            (("    { uint64_t _b=c->x[22]; int64_t _o=40; recomp_stp64(c,(_b+_o),c->vreg[2][0],c->vreg[2][1]); "
              "recomp_stp64(c,(_b+_o)+16,c->vreg[3][0],c->vreg[3][1]); }",), mark(22, 40, 32)),
            # (ST1 lanes at _a+IMM)
            (("    { uint64_t _a=c->x[12]; { uint64_t _v=0; memcpy(&_v,(const uint8_t*)c->vreg[0]+8,4); recomp_store32(c,_a+0,_v); } "
              "{ uint64_t _v=0; memcpy(&_v,(const uint8_t*)c->vreg[0]+12,4); recomp_store32(c,_a+4,_v); } }",), mark(12, 0, 8)),
            # (STUR: negative offsets, both spellings)
            (("    { uint64_t _b=c->x[8]; int64_t _o=-16; recomp_store64(c,(_b+_o),c->x[1]); }",
              "    recomp_store64(c,c->x[8] + -8,c->x[2]);"), mark(8, -16, 16)),
            # (exactly 4096 bytes: two pages)
            (("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    recomp_store64(c,c->x[19] + 4088,c->x[2]);"), mark(19, 0, 4096)),
            # (the nearest _b before a store is its own: x9's store stays alone, x7's two group)
            (("    { uint64_t _b=c->x[9]; int64_t _o=0; recomp_store64(c,(_b+_o),c->x[1]); } "
              "{ uint64_t _b=c->x[7]; int64_t _o=8; recomp_store64(c,(_b+_o),c->x[2]); }",
              "    recomp_store64(c,c->x[7] + 0,c->x[3]);"), mark(7, 0, 16)),
        ]
        for body, want in cases:
            out = run(*body, "    return;")
            self.assertEqual([l for l in out if "A64_MARK" in l], [want], body)
            self.assertEqual(out[-2], want, body)
            left = re.findall(r"recomp_(?:store|stp)\d+\(c,[^,]*", "\n".join(out))
            self.assertEqual(left, ["recomp_store64(c,(_b+_o)"] if "c->x[9]" in body[0] else [], body)

    def test_the_pages_marked_are_the_same(self):
        """random groups at random bases: the pages the marks left behind (and the group marks) mark are the pages a mark per
        store marks"""
        rng = random.Random(912)
        store = re.compile(r"recomp_(store8|store16|store32|store64|stp32|stp64)(_sp)?\(c,c->x\[(\d+)\] \+ (-?\d+),")
        own = re.compile(r"A64_MARK\(\(c->x\[(\d+)\] \+ \(uint64_t\)\(int64_t\)\((-?\d+)\)\), (\d+)\)")

        def pages(lines, base, each):
            got = set()
            for l in lines:
                for m in store.finditer(l):
                    if each or not m.group(2):
                        a, n = base[int(m.group(3))] + int(m.group(4)), a64_lift.MG_SIZE[m.group(1)]
                        got |= {a >> 12, (a + n - 1) >> 12}
                m = own.search(l)
                if m:
                    a, n = base[int(m.group(1))] + int(m.group(2)), int(m.group(3))
                    got |= {a >> 12, (a + n - 1) >> 12}
            return got
        grouped = 0
        for _ in range(3000):
            body = []
            for _ in range(rng.randint(2, 9)):
                kind = rng.choice(sorted(a64_lift.MG_SIZE))
                body.append("    recomp_%s(c,c->x[%d] + %d,c->x[1]%s);" % (
                    kind, rng.choice((19, 20)), rng.randint(-300, 4400), ",c->x[2]" if kind.startswith("stp") else ""))
            out = run(*body, "    return;")
            grouped += any("A64_MARK" in l for l in out)
            for _ in range(4):
                base = {19: rng.randrange(1 << 20, 1 << 30), 20: rng.choice((1 << 24, (1 << 24) - rng.randint(1, 4096)))}
                self.assertEqual(pages(out, base, False), pages(body, base, True), body)
        self.assertGreater(grouped, 1000)

    def test_multi_line_blocks_keep_their_marks_inside(self):
        out = run("    recomp_store32(c,c->x[19] + 0,c->x[1]);", "    recomp_store32(c,c->x[19] + 4,c->x[2]);",
                  "    if (c->z) {",
                  "        recomp_store32(c,c->x[19] + 8,c->x[3]);", "        recomp_store32(c,c->x[19] + 12,c->x[3]);",
                  "    }",
                  "    recomp_store32(c,c->x[19] + 16,c->x[4]);", "    return;")
        self.assertEqual(out, [
            "    recomp_store32_sp(c,c->x[19] + 0,c->x[1]);", "    recomp_store32_sp(c,c->x[19] + 4,c->x[2]);",
            mark(19, 0, 8),   # (before the block: the block may not run)
            "    if (c->z) {",
            "        recomp_store32_sp(c,c->x[19] + 8,c->x[3]);", "        recomp_store32_sp(c,c->x[19] + 12,c->x[3]);",
            mark(19, 8, 8),   # (inside it, with its stores)
            "    }",
            "    recomp_store32(c,c->x[19] + 16,c->x[4]);", "    return;"])

    def test_bases_group_apart_and_sp_is_left_alone(self):
        out = run("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    recomp_store64(c,c->x[20] + 0,c->x[2]);",
                  "    recomp_store64(c,c->x[19] + 8,c->x[3]);", "    recomp_store64(c,c->x[20] + 8,c->x[4]);",
                  "    recomp_store64(c,c->x[31] + 8,c->x[5]);", "    recomp_store64(c,c->x[31] + 16,c->x[5]);",
                  "    TAILCALL(f_80000100);")
        self.assertEqual(sorted(l for l in out if "A64_MARK" in l), [mark(19, 0, 16), mark(20, 0, 16)])
        self.assertIn("    recomp_store64(c,c->x[31] + 8,c->x[5]);", out)

    def test_no_group_leaves_the_text_as_it_was(self):
        for body in (("    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    TAILCALL(f_80000100);"),
                     # (a function with locals, A64_LOCAL_REGS: left alone)
                     ("    uint64_t x19_ = c->x[19];", "    recomp_store64(c,x19_ + 0,c->x[1]);",
                      "    recomp_store64(c,c->x[20] + 0,c->x[1]);", "    recomp_store64(c,c->x[20] + 8,c->x[1]);")):
            text = "\n".join((HEAD,) + body + ("}",))
            self.assertEqual(a64_lift.mark_groups(text), text)

    def test_twice_is_once(self):
        """its own marks are not run ends: a group the first pass found too wide is not split by them on a second pass"""
        text = "\n".join(('#include "a64.h"', "void f_80000000(Ctx *A64_NOESC);", HEAD,
                          "    recomp_store64(c,c->x[19] + 0,c->x[1]);", "    recomp_store64(c,c->x[19] + 8,c->x[1]);",
                          "    recomp_store64(c,c->x[20] + 0,c->x[1]);", "    recomp_store64(c,c->x[20] + 8,c->x[1]);",
                          "    c->x[20] = 0;", "    recomp_store64(c,c->x[19] + 5000,c->x[1]);",
                          "    { uint64_t _b=c->x[22]; int64_t _o=40; recomp_stp64(c,(_b+_o),c->x[1],c->x[2]); }",
                          "    recomp_store8(c,c->x[22] + 39,c->x[1]);", "    return;", "}",
                          "void f_80000100(Ctx *__restrict A64_NOESC c) {", "    recomp_store8(c,c->x[3] + 0,c->x[1]);",
                          "    recomp_store8(c,c->x[3] + 1,c->x[1]);", "}", ""))
        st = Counter()
        once = a64_lift.mark_groups(text, st)
        self.assertEqual(st["groups"], 3)   # (x20, x22, and x3 of the second function; x19's is 5008 bytes)
        self.assertEqual(a64_lift.mark_groups(once), once)
        self.assertIn("void f_80000000(Ctx *A64_NOESC);", once.split("\n"))

    def test_mark_each_and_the_stats(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "code_000.c")
            text = "\n".join((HEAD, "    recomp_store64(c,c->x[19] + 0,c->x[1]);",
                              "    recomp_store64(c,c->x[19] + 8,c->x[1]);", "    return;", "}", ""))
            with open(path, "w", newline="") as f:
                f.write(text)
            st = Counter()
            with mock.patch.dict(os.environ, {"A64_MARK_EACH": "1"}):
                self.assertEqual(a64_lift.group_marks([path], st), 0)
            self.assertEqual(read(path), text)
            self.assertEqual(st["groups"], 0)
            with mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if k != "A64_MARK_EACH"}, clear=True):
                self.assertEqual(a64_lift.group_marks([path], st), 1)
            self.assertEqual((st["stores"], st["groups"], st["grouped_stores"]), (2, 1, 2))
            self.assertIn(mark(19, 0, 16), read(path))

    def test_the_lift_groups_once_with_its_stats(self):
        """emit_function leaves the marks as they are; nx2/lift.py calls chunks.edit_c, which groups the written files once, with
        the lift's stats, before its report"""
        self.assertNotIn("mark_groups", inspect.getsource(a64_lift.Lifter.emit_function))
        src = read(os.path.join(ROOT, "nx2", "lift.py"))
        self.assertEqual(src.count("group_marks(") + read(os.path.join(ROOT, "nx2", "chunks.py")).count("group_marks("), 1)
        self.assertIn("group_marks(", inspect.getsource(chunks.edit_c))
        call = src.find("chunks.edit_c(gen, L.stats)")
        self.assertGreater(call, 0)
        self.assertLess(call, src.find("L.report()"))


if __name__ == "__main__":
    unittest.main()
