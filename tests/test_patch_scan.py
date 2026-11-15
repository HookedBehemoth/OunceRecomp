"""The host's patch declarations (host/patch.h) and the lifter's scan of them (nx2/lift2/patch_scan.py), without the game.

- The scan: comments, strings, line continuations and nested parentheses; the errors (inside #if, inside a function,
  made by another macro, a bare word, an unknown mode / result / operand, a second PATCH_AT, a further site of another
  type, a duplicate site), each with file:line. The hooks (PATCH_HOOK, PATCH_HOOK_SYM) and the replaced functions (HLE,
  NV, HLE_EFFECTS) with their effects (reads, guest, clobbers), their errors, and a hook's checks against a plan.
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


HOOKS = r'''PATCH_HOOK(0x80001000, word(0xd503237f), reads(sp, "hook_call_guest"), guest(0x80003000, 0x80004000)) {
    return 0;
}
PATCH_HOOK(0x80002000, word(0xd503237f)) { return hook_return(h, a->x[0]); }
PATCH_HOOK_SYM(memcpy) { (void)a; (void)h; return 0; }
HLE(nn_a) { c->x[0] = 1; }
HLE(nn_b, clobbers("a context switch")) {}
NV(nvnFoo, reads(x29, x19, "a log label")) { (void)c; (void)u_; }
HLE_EFFECTS(nvnBar, clobbers("not audited"));
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

    def test_hooks_and_replaced(self):
        ps = patch_scan.scan_text(HOOKS)
        h = {x.name: x for x in ps.hooks}
        self.assertEqual(sorted(h), ["entry_0x80001000", "entry_0x80002000", "entry_sym_memcpy"])
        self.assertEqual((h["entry_0x80001000"].addr, h["entry_0x80001000"].word, h["entry_0x80001000"].reads,
                          h["entry_0x80001000"].guest), (0x80001000, 0xd503237f, ("sp",), (0x80003000, 0x80004000)))
        self.assertEqual((h["entry_0x80002000"].reads, h["entry_0x80002000"].guest), ((), ()))
        self.assertEqual((h["entry_sym_memcpy"].symbol, h["entry_sym_memcpy"].addr), ("memcpy", None))
        self.assertEqual(h["entry_0x80001000"].where, ("t.c", 1))
        self.assertEqual(sorted(ps.hle_symbols()), ["nn_a", "nn_b"])
        self.assertEqual((ps.effects("nn_a").reads, ps.effects("nn_a").clobbers), ((), None))
        self.assertEqual(ps.effects("nn_b").clobbers, '"a context switch"')
        self.assertEqual(ps.effects("nvnFoo").reads, ("x29", "x19"))
        self.assertEqual(ps.effects("nvnBar").clobbers, '"not audited"')
        self.assertIsNone(ps.effects("nn_none"))

    def test_hook_errors(self):
        for text, what in (
                ("PATCH_HOOK(0x10, 0xd503237f) { return 0; }", "first word is written word(0x...)"),
                ("PATCH_HOOK(0x10, word(1), reads(sp)) { return 0; }", 'reads(reg..., "why")'),
                ("PATCH_HOOK(0x10, word(1), reads(y1, \"w\")) { return 0; }", "reads: xN, vN or sp, not y1"),
                ("PATCH_HOOK(0x10, word(1), clobbers(\"w\")) { return 0; }", "an effect: reads(...) or guest(...)"),
                ("PATCH_HOOK(0x10, word(1), guest(computed)) { return 0; }", "names the guest functions"),
                ("PATCH_HOOK(0x10, word(1), guest(1), guest(2)) { return 0; }", "guest(...) twice"),
                ("PATCH_HOOK(0x10, word(1)) { return 0; }\nPATCH_HOOK_SYM(f) { return 0; }\n"
                 "PATCH_HOOK(0x10, word(1)) { return 0; }", "t.c:3: a second hook of entry_0x10 (the first at t.c:1)"),
                ("HLE(f, guest(1)) {}", "an effect: reads(...) or clobbers(...)"),
                ("HLE(f, clobbers(why)) {}", 'clobbers("why")'),
                ("HLE(f) {}\nHLE(f) {}", "t.c:2: f declared again (the first at t.c:1)"),
                ("HLE(f) {}\nHLE_EFFECTS(f, clobbers(\"w\"));", "t.c:2: HLE_EFFECTS of f, which HLE declares at t.c:1"),
                ("HLE_EFFECTS(f);", "HLE_EFFECTS without an effect"),
                ("void g(void) { HLE(f); }", "t.c:1: HLE inside a function"),
                ("#if X\nNV(f) {}\n#endif\n", "t.c:2: NV inside conditional"),
                ("#define M(n) HLE(n) {}\n", "another macro")):
            with self.assertRaises(SystemExit) as e:
                patch_scan.scan_text(text)
            self.assertIn(what, str(e.exception), text)

    def test_hooks_in_a_plan(self):
        import json
        from array import array
        ps = patch_scan.scan_text(HOOKS)
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "image"))
            with open(os.path.join(d, "image", "image.json"), "w") as f:
                json.dump({"exports": {"memcpy": 0x80005000}}, f)
            gen = os.path.join(d, "gen")
            plan = {"bodies": {a: (array("Q", [a, a + 4]), array("I", [0xd503237f, 0])) for a in
                               (0x80001000, 0x80002000, 0x80005000)}}
            self.assertEqual(patch_scan.check_hooks(plan, gen, ps), {0x80001000: "entry_0x80001000",
                                                                     0x80002000: "entry_0x80002000",
                                                                     0x80005000: "entry_sym_memcpy"})
            plan["bodies"][0x80002000][1][0] = 0xd503201f
            with self.assertRaises(SystemExit) as e:
                patch_scan.check_hooks(plan, gen, ps)
            self.assertIn("t.c:4: PATCH_HOOK at 0x80002000: the function's first word is 0xd503201f, not 0xd503237f",
                          str(e.exception))
            del plan["bodies"][0x80002000]
            with self.assertRaises(SystemExit) as e:
                patch_scan.check_hooks(plan, gen, ps)
            self.assertIn("t.c:4: PATCH_HOOK at 0x80002000: no lifted function there", str(e.exception))
            with open(os.path.join(d, "image", "image.json"), "w") as f:
                json.dump({"exports": {}}, f)
            with self.assertRaises(SystemExit) as e:
                patch_scan.check_hooks(plan, gen, ps)
            self.assertIn("t.c:5: PATCH_HOOK_SYM: the image exports no memcpy", str(e.exception))

    def test_containing(self):
        from array import array
        plan = {"bodies": {0x100: (array("Q", [0x100, 0x104, 0x108]), None), 0x200: (array("Q", [0x200, 0x104]), None),
                           0x300: (array("Q", [0x300, 0x304]), None)}}
        plan["bodies"][0x200] = (array("Q", [0x104, 0x200]), None)
        self.assertEqual(patch_scan.containing(plan, [0x108, 0x104, 0x400]), {0x108: [0x100], 0x104: [0x100, 0x200],
                                                                             0x400: []})


# (host/hos.h's hook form, as the macros of host/patch.h use it)
HOOK_FORM = r'''typedef struct { uint64_t x[32]; } Ctx;
typedef struct { uint64_t x[8]; uint64_t lr; } HookArgs;
typedef struct HookCall HookCall;
int hook_return(HookCall *h, uint64_t x0);
#define ENTRY_HOOK_COUNTED(name, body) \
    static int body(const HookArgs *a, HookCall *h); \
    int name(Ctx *c) { const HookArgs a_ = {{c->x[0]}, c->x[30]}; return body(&a_, (HookCall *)c); } \
    static int body(const HookArgs *a, HookCall *h)
'''


@unittest.skipUnless(shutil.which("clang"), "clang")
class Macros(unittest.TestCase):
    def compile(self, d, text, name="t.c"):
        src = os.path.join(d, name)
        with open(src, "w") as f:
            f.write('#include <stdint.h>\ntypedef struct GuestCall GuestCall;\n'
                    'typedef uint64_t hos_q __attribute__((vector_size(16)));\n' + HOOK_FORM +
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

    def test_hooks_and_replaced(self):
        with tempfile.TemporaryDirectory() as d:
            r, obj, src = self.compile(d, HOOKS + "void use(Ctx *c) { n_nvnFoo(c, 0); }\n")
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("declarations: 7,", patch_scan.check_records([obj], [src]))
            nm = subprocess.run(["nm", obj], capture_output=True, text=True).stdout.split()
            for name in ("entry_0x80001000", "entry_0x80002000", "entry_sym_memcpy", "imp_nn_a", "imp_nn_b"):
                self.assertIn(name, nm)


class Host(unittest.TestCase):
    def test_hooks_and_replaced(self):
        ps = patch_scan.patches()
        self.assertGreater(len(ps.hooks), 30)
        self.assertGreater(len(ps.hle), 400)
        self.assertGreater(len(ps.nv), 400)
        for h in ps.hooks:
            self.assertEqual(h.addr is None, h.word is None, h.where)

    def test_declarations(self):
        values, ats = probes.tables()
        self.assertGreater(len(ats), 100)
        self.assertTrue(values)
        for p in values + ats:
            self.assertTrue(p.where[0].startswith("host/"), p.where)


if __name__ == "__main__":
    unittest.main()
