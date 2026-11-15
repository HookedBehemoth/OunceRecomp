"""The host's patch declarations (host/patch.h) and the lifter's scan of them (nx2/lift2/patch_scan.py), without the game.

- The scan: comments, strings, line continuations and nested parentheses; the errors (inside #if, inside a function,
  made by another macro, a bare word, an unknown mode / result / operand, a second PATCH_AT, a further site of another
  type, a duplicate site), each with file:line.
- The macros: a declaration compiles to its function's prototype (a body written against it compiles; a further site
  whose operands are other types does not) and to a record; the build's cross-check passes for the same text and stops
  for a record the scan did not see.
- The host's own declarations: scanned without an error; the rows they make.
No game is used.

python -I -B tests/test_patch_scan.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import patch_scan, probes  # noqa: E402

GOOD = r'''/* PATCH_AT(0, word(0), after, none, in_a_comment) */
static const char *s = "PATCH_AT(0, word(0), after, none, in_a_string)";
#define NOTHING(x) x
PATCH_AT(0x1004, word(0xd503201f), after, x0, t_fn, as(x1, a), ld(sp, 0x10), \
         k(3)) { int c = ')'; (void)c; return a + ld_sp_0x10 + k_3; }
PATCH_AT_SITE(0x2004, word(0xd503201f), before, x8, t_fn, x2, ld(x3, 8), k(4));
PATCH_AT(0x3004, word(0x94000000), instead, none, t_call, guest(0x1000, 0x2000), sp) { (void)g; (void)sp; }
PATCH_AT(0x4004, word(0xd503201f), after, none, t_io, io(w1, x2, q3)) { io->w1 = io->x2; }
'''


def scan_text(text, name="t.c"):
    return [patch_scan.Site(d) for d in patch_scan.declarations(text, name)]


class Scan(unittest.TestCase):
    def test_good(self):
        s = scan_text(GOOD)
        self.assertEqual([(x.kind, x.pc, x.fn) for x in s],
                         [("PATCH_AT", 0x1004, "t_fn"), ("PATCH_AT_SITE", 0x2004, "t_fn"), ("PATCH_AT", 0x3004, "t_call"),
                          ("PATCH_AT", 0x4004, "t_io")])
        self.assertEqual(s[0].args, ["x1", "[sp+0x10]", "#3"])
        self.assertEqual(s[0].where, ("t.c", 4))
        self.assertEqual((s[2].mode, s[2].guest, s[2].args), ("instead", (0x1000, 0x2000), ["sp"]))
        self.assertEqual(s[3].io, ("w1", "x2", "q3"))

    def test_errors(self):
        for text, what in (
                ("#if X\nPATCH_AT(1, word(2), after, none, f) {}\n#endif\n", "t.c:2: PATCH_AT inside conditional"),
                ("void g(void) { PATCH_AT(1, word(2), after, none, f); }", "t.c:1: PATCH_AT inside a function"),
                ("#define M PATCH_AT(1, word(2), after, none, f)\n", "another macro"),
                ("PATCH_AT(1, 2, after, none, f) {}", "word(0x...)"),
                ("PATCH_AT(1, word(2), aft, none, f) {}", "mode"),
                ("PATCH_AT(1, word(2), after, x31, f) {}", "unknown result x31"),
                ("PATCH_AT(1, word(2), after, none, f, y3) {}", "unknown operand y3"),
                ("PATCH_AT(1, word(2), after, none, f, x1, guest(3)) {}", "guest(...) comes first"),
                ("PATCH_AT(1, word(2), after, none, f, io(w1), x1) {}", "io(...) comes last")):
            with self.assertRaises(SystemExit) as e:
                scan_text(text)
            self.assertIn(what, str(e.exception), text)

    def test_across_files(self):
        with tempfile.TemporaryDirectory() as d:
            def files(*texts):
                out = []
                for k, t in enumerate(texts):
                    p = os.path.join(d, "f%d.c" % k)
                    with open(p, "w") as f:
                        f.write(t)
                    out.append(p)
                return out
            ok = files("PATCH_AT(0x10, word(0x1), after, x0, f, x1) {}\n",
                       "PATCH_AT_SITE(0x20, word(0x1), after, x9, f, x3);\n")
            self.assertEqual(len(patch_scan.scan(ok)), 2)
            for texts, what in (
                    (("PATCH_AT(0x10, word(0x1), after, x0, f, x1) {}\nPATCH_AT(0x20, word(0x1), after, x0, f, x1) {}\n",),
                     "a second PATCH_AT of f"),
                    (("PATCH_AT(0x10, word(0x1), after, x0, f, x1) {}\nPATCH_AT_SITE(0x20, word(0x1), after, x0, f, w1);\n",),
                     "differ"),
                    (("PATCH_AT_SITE(0x20, word(0x1), after, x0, f, x1);\nPATCH_AT(0x10, word(0x1), after, x0, f, x1) {}\n",),
                     "before or without"),
                    (("PATCH_AT(0x10, word(0x1), after, x0, f, x1) {}\nPATCH_AT_SITE(0x10, word(0x1), after, x0, f, x1);\n",),
                     "the same site")):
                with self.assertRaises(SystemExit) as e:
                    patch_scan.scan(files(*texts))
                self.assertIn(what, str(e.exception))

    def test_containing(self):
        from array import array
        plan = {"bodies": {0x100: (array("Q", [0x100, 0x104, 0x108]), None), 0x200: (array("Q", [0x200, 0x104]), None),
                           0x300: (array("Q", [0x300, 0x304]), None)}}
        plan["bodies"][0x200] = (array("Q", [0x104, 0x200]), None)
        self.assertEqual(patch_scan.containing(plan, [0x108, 0x104, 0x400]), {0x108: [0x100], 0x104: [0x100, 0x200],
                                                                             0x400: []})


@unittest.skipUnless(shutil.which("clang"), "clang")
class Macros(unittest.TestCase):
    def compile(self, d, text, name="t.c"):
        src = os.path.join(d, name)
        with open(src, "w") as f:
            f.write('#include <stdint.h>\ntypedef struct GuestCall GuestCall;\n'
                    'typedef uint64_t hos_q __attribute__((vector_size(16)));\n#include "patch.h"\n' + text)
        obj = src[:-2] + ".o"
        r = subprocess.run(["clang", "-c", "-O1", "-Wall", "-Werror", "-Wno-unused-variable", "-I",
                            os.path.join(ROOT, "host"), src, "-o", obj], capture_output=True, text=True)
        return r, obj, src

    def test_prototypes_and_records(self):
        with tempfile.TemporaryDirectory() as d:
            r, obj, src = self.compile(d, GOOD)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("4", patch_scan.check_records([obj], [src]).split()[2])
            # (a further site of another type: a compile error)
            r, _, _ = self.compile(d, GOOD.replace("x2, ld(x3, 8), k(4)", "x2, ld(x3, 8), s4"), "u.c")
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("operands differ", r.stderr)
            # (a declaration the compiler saw and the scan did not: here, a file the scan was not given)
            r, obj2, src2 = self.compile(d, "PATCH_AT(0x5004, word(0x1), after, none, t_more) {}\n", "v.c")
            self.assertEqual(r.returncode, 0, r.stderr)
            with self.assertRaises(SystemExit) as e:
                patch_scan.check_records([obj, obj2], [src])
            self.assertIn("compiled only", str(e.exception))


class Host(unittest.TestCase):
    def test_declarations(self):
        values, ats = probes.tables()
        self.assertGreater(len(ats), 100)
        self.assertTrue(values)
        for p in values + ats:
            self.assertTrue(p.where[0].startswith("host/"), p.where)


if __name__ == "__main__":
    unittest.main()
