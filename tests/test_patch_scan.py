"""The host's patch declarations (host/patch.h) and the lifter's scan of them (nx2/lift2/patch_scan.py), without the game.

- The scan: comments, strings, line continuations and nested parentheses; the errors (inside #if, inside a function,
  made by another macro, a bare word, an unknown mode / result / operand, a second PATCH_AT, a further site of another
  type, a duplicate site), each with file:line. The replaced functions: PATCH_FUNC / PATCH_FUNC_SYM (orig, guest,
  operands, result, their errors, their checks against a plan: word, body hash, export, an orig body's argument
  registers) and HLE, NV, HLE_EFFECTS, PATCH_ANSWER with their effects (reads, clobbers).
- The macros: a declaration compiles to its function's prototype (a body written against it compiles; a further site
  whose operands are other types does not) and to a record; a PATCH_FUNC's adapter reads its operands from the context,
  writes its result there, and its orig runs the lifted body with the arguments given; the build's cross-check passes for the same text and stops
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
import gamedesc  # noqa: E402
gamedesc.use(os.path.join(ROOT, "games", "oot"))   # (the game whose host files and patches these read)
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


FUNCS = r'''PATCH_FUNC(0x80001000, word(0xd503237f), body(0x1234), aapcs, x0, t_alloc,
           orig, guest(0x80003000, 0x80004000), as(x0, dta), as(w3, flags), as(x30, lr), sp) {
    (void)g; (void)lr; (void)sp;
    return flags ? dta : guest_orig(orig, dta, 0, 0, flags);
}
PATCH_FUNC(0x80002000, word(0xd503237f), body(0x5678), aapcs, none, t_watch, x1) { (void)x1; }
PATCH_FUNC_SYM(strlen, aapcs, x0, t_strlen, as(x0, s)) { return s + 1; }
HLE(nn_a) { c->x[0] = 1; }
HLE(nn_b, clobbers("a context switch")) {}
NV(nvnFoo, reads(x29, x19, "a log label")) { (void)c; (void)u_; }
HLE_EFFECTS(nvnBar, clobbers("not audited"));
PATCH_ANSWER(nn_c, 0x2a, "the answer the game needs");
'''

# (a program running FUNCS' adapters on a context of its own, with stand-ins for the lifted bodies)
ADAPTER_MAIN = r'''
static uint64_t seen[3];
void hos_orig_0x80001000(Ctx *c) { seen[0] = c->x[0], seen[1] = c->x[3], seen[2] = c->x[5]; c->x[0] = 77; }
void hos_orig_0x80002000(Ctx *c) { (void)c; }
void hos_orig_sym_strlen(Ctx *c) { (void)c; }
int main(void) {
    static Ctx c;
    c.x[0] = 5, c.x[3] = 1ull << 32, c.x[5] = 9, c.x[31] = 0x1000;
    hos_func_0x80001000(&c);   /* (flags, w3, is 0: orig runs with x0 = 5, x3 = 0, x5 the call's) */
    if (c.x[0] != 77 || seen[0] != 5 || seen[1] != 0 || seen[2] != 9) return 1;
    c.x[0] = 5, c.x[3] = 1;
    hos_func_0x80001000(&c);   /* (flags 1: x0 = dta, no orig) */
    if (c.x[0] != 5) return 2;
    c.x[0] = 41;
    hos_func_sym_strlen(&c);
    if (c.x[0] != 42) return 3;
    c.x[0] = 3;
    hos_func_0x80002000(&c);   /* (result none: x0 as it was) */
    return c.x[0] == 3 ? 0 : 4;
}
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
            self.assertEqual(len(patch_scan.scan(ok).sites), 2)
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

    def test_funcs_and_replaced(self):
        ps = patch_scan.scan_text(FUNCS)
        f = {x.fn: x for x in ps.funcs}
        self.assertEqual(sorted(f), ["t_alloc", "t_strlen", "t_watch"])
        a = f["t_alloc"]
        self.assertEqual((a.addr, a.word, a.body, a.orig, a.guest, a.args, a.result, a.name, a.orig_name, a.where),
                         (0x80001000, 0xd503237f, 0x1234, True, (0x80003000, 0x80004000), ["x0", "w3", "x30", "sp"], "x0",
                          "hos_func_0x80001000", "hos_orig_0x80001000", ("t.c", 1)))
        self.assertEqual(a.reads(), {"x0", "x3", "x30", "sp"})
        self.assertEqual((f["t_watch"].orig, f["t_watch"].result), (False, None))
        self.assertEqual((f["t_strlen"].symbol, f["t_strlen"].addr, f["t_strlen"].name),
                         ("strlen", None, "hos_func_sym_strlen"))
        self.assertEqual(sorted(ps.hle_symbols()), ["nn_a", "nn_b", "nn_c"])
        self.assertEqual((ps.answers["nn_c"].value, ps.answers["nn_c"].why, ps.answers["nn_c"].where),
                         (0x2a, '"the answer the game needs"', ("t.c", 12)))
        self.assertIsNone(ps.effects("nn_c"))
        self.assertEqual((ps.effects("nn_a").reads, ps.effects("nn_a").clobbers), ((), None))
        self.assertEqual(ps.effects("nn_b").clobbers, '"a context switch"')
        self.assertEqual(ps.effects("nvnFoo").reads, ("x29", "x19"))
        self.assertEqual(ps.effects("nvnBar").clobbers, '"not audited"')
        self.assertIsNone(ps.effects("nn_none"))

    def test_func_errors(self):
        F = "PATCH_FUNC(0x10, word(1), body(2), aapcs, %s) {}"
        for text, what in (
                ("PATCH_FUNC(0x10, 0xd503237f, body(1), aapcs, x0, f) {}", "first word is written word(0x...)"),
                ("PATCH_FUNC(0x10, word(1), 0x55, aapcs, x0, f) {}", "body hash is written body(0x...)"),
                ("PATCH_FUNC(0x10, word(1), body(2), ctx, x0, f) {}", "abi: aapcs, not ctx"),
                (F % "x1, f", "a function's result: none, x0, w0, s0, q0, not x1"),
                (F % "x0, f, x9", "a function's operand: x0-x8, w0-w8, s0-s7, q0-q7, x30 or sp, not x9"),
                (F % "x0, f, ld(sp, 8)", "not [sp+8]"),
                (F % "x0, f, x0, orig", "t.c:1: orig comes first"),
                (F % "x0, f, x0, guest(0x20), sp", "guest(...) comes first"),
                (F % "x0, f, guest(0x20), x0", "sp is an operand"),
                (F % "x0, f, guest(computed), sp", "names the guest functions it calls"),
                (F % "x0, f, io(x1)", "io(...) is a PATCH_AT's"),
                ("PATCH_FUNC_SYM(memcpy) {}", "PATCH_FUNC_SYM(symbol, aapcs, result, fn, operand...)"),
                ((F % "x0, f") + "\n" + (F % "x0, g"), "t.c:2: a second declaration of hos_func_0x10 (the first at t.c:1)"),
                ((F % "x0, f") + "\nPATCH_FUNC(0x20, word(1), body(2), aapcs, x0, f) {}",
                 "t.c:2: f replaces a second function (the first at t.c:1)"),
                ("HLE(f, guest(1)) {}", "an effect: reads(...) or clobbers(...)"),
                ("HLE(f, clobbers(why)) {}", 'clobbers("why")'),
                ("HLE(f) {}\nHLE(f) {}", "t.c:2: f declared again (the first at t.c:1)"),
                ("HLE(f) {}\nHLE_EFFECTS(f, clobbers(\"w\"));", "t.c:2: HLE_EFFECTS of f, which HLE declares at t.c:1"),
                ("HLE_EFFECTS(f);", "HLE_EFFECTS without an effect"),
                ("PATCH_ANSWER(f, 0);", 'PATCH_ANSWER(symbol, value, "why")'),
                ("PATCH_ANSWER(f, x, \"w\");", "the answer: a number"),
                ("PATCH_ANSWER(f, 0, why);", "the reason as a string"),
                ("PATCH_ANSWER(f, 0, \"\");", "the reason as a string"),
                ("PATCH_ANSWER(f, 0, \"w\");\nPATCH_ANSWER(f, 1, \"w\");", "t.c:2: PATCH_ANSWER of f again"),
                ("HLE(f) {}\nPATCH_ANSWER(f, 0, \"w\");", "t.c:2: PATCH_ANSWER of f, which HLE declares at t.c:1"),
                ("void g(void) { HLE(f); }", "t.c:1: HLE inside a function"),
                ("#if X\nNV(f) {}\n#endif\n", "t.c:2: NV inside conditional"),
                ("#define M(n) HLE(n) {}\n", "another macro")):
            with self.assertRaises(SystemExit) as e:
                patch_scan.scan_text(text)
            self.assertIn(what, str(e.exception), text)

    def test_funcs_in_a_plan(self):
        import json
        from array import array
        words = [0xd503237f, 0xd65f03c0]
        h = "body(%#x)" % patch_scan.body_hash(words)
        ps = patch_scan.scan_text(FUNCS.replace("body(0x1234)", h).replace("body(0x5678)", h))
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "image"))
            with open(os.path.join(d, "image", "image.json"), "w") as f:
                json.dump({"exports": {"strlen": 0x80005000}}, f)
            gen = os.path.join(d, "gen")
            plan = {"bodies": {a: (array("Q", [a, a + 4]), array("I", words)) for a in
                               (0x80001000, 0x80002000, 0x80005000)}}
            got = patch_scan.check_funcs(plan, gen, ps)
            self.assertEqual({a: f.fn for a, f in got.items()}, {0x80001000: "t_alloc", 0x80002000: "t_watch",
                                                                 0x80005000: "t_strlen"})
            plan["bodies"][0x80002000][1][1] = 0xd503201f
            with self.assertRaises(SystemExit) as e:
                patch_scan.check_funcs(plan, gen, ps)
            self.assertIn("t.c:6: PATCH_FUNC at 0x80002000: the function's body is body(", str(e.exception))
            plan["bodies"][0x80002000][1][0] = 0xd503201f
            with self.assertRaises(SystemExit) as e:
                patch_scan.check_funcs(plan, gen, ps)
            self.assertIn("t.c:6: PATCH_FUNC at 0x80002000: the function's first word is 0xd503201f, not 0xd503237f",
                          str(e.exception))
            del plan["bodies"][0x80002000]
            with self.assertRaises(SystemExit) as e:
                patch_scan.check_funcs(plan, gen, ps)
            self.assertIn("t.c:6: PATCH_FUNC at 0x80002000: no lifted function there", str(e.exception))
            with open(os.path.join(d, "image", "image.json"), "w") as f:
                json.dump({"exports": {}}, f)
            with self.assertRaises(SystemExit) as e:
                patch_scan.check_funcs(plan, gen, ps)
            self.assertIn("t.c:7: PATCH_FUNC_SYM: the image exports no strlen", str(e.exception))

    def test_containing(self):
        from array import array
        plan = {"bodies": {0x100: (array("Q", [0x100, 0x104, 0x108]), None), 0x200: (array("Q", [0x200, 0x104]), None),
                           0x300: (array("Q", [0x300, 0x304]), None)}}
        plan["bodies"][0x200] = (array("Q", [0x104, 0x200]), None)
        self.assertEqual(patch_scan.containing(plan, [0x108, 0x104, 0x400]), {0x108: [0x100], 0x104: [0x100, 0x200],
                                                                             0x400: []})


# (what host/patch.h takes from host/hos.h and rt/a64.h)
CTX_FORM = r'''typedef struct GuestContext { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
#define A64_NOESC
'''


@unittest.skipUnless(shutil.which("clang"), "clang")
class Macros(unittest.TestCase):
    def compile(self, d, text, name="t.c"):
        src = os.path.join(d, name)
        with open(src, "w") as f:
            f.write('#include <stdint.h>\ntypedef struct GuestCall GuestCall;\n'
                    'typedef uint64_t hos_q __attribute__((vector_size(16)));\n' + CTX_FORM +
                    '#include "patch.h"\n' + text)
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

    def test_funcs_and_replaced(self):
        with tempfile.TemporaryDirectory() as d:
            r, obj, src = self.compile(d, FUNCS + "void use(Ctx *c) { n_nvnFoo(c, 0); }\n")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("declarations: 8,", patch_scan.check_records([obj], [src]))
            nm = subprocess.run(["nm", obj], capture_output=True, text=True).stdout.split()
            for name in ("hos_func_0x80001000", "hos_func_0x80002000", "hos_func_sym_strlen", "imp_nn_a", "imp_nn_b"):
                self.assertIn(name, nm)

    def test_adapter(self):
        """the adapter reads the operands from the context and writes the result; orig runs the body with the
        arguments given in x0.. and the call's other registers"""
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "a.c")
            with open(src, "w") as f:
                f.write('#include <stdint.h>\ntypedef struct GuestCall GuestCall;\n'
                        'typedef uint64_t hos_q __attribute__((vector_size(16)));\n' + CTX_FORM +
                        '#include "patch.h"\n' + FUNCS.split("HLE(")[0] + ADAPTER_MAIN)
            exe = os.path.join(d, "a")
            r = subprocess.run(["clang", "-O1", "-Wall", "-Werror", "-I", os.path.join(ROOT, "host"), src, "-o", exe],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(subprocess.run([exe]).returncode, 0)


class Host(unittest.TestCase):
    def test_funcs_and_replaced(self):
        ps = patch_scan.patches()
        self.assertGreater(len(ps.funcs), 20)
        self.assertGreater(len(ps.hle), 400)
        self.assertGreater(len(ps.nv), 400)
        for f in ps.funcs:
            self.assertEqual(f.addr is None, f.word is None, f.where)
            self.assertEqual(f.addr is None, f.body is None, f.where)

    def test_declarations(self):
        values, ats = probes.tables()
        self.assertGreater(len(ats), 100)
        self.assertTrue(values)
        for p in values + ats:
            self.assertTrue(p.where[0].startswith("host/"), p.where)


if __name__ == "__main__":
    unittest.main()
