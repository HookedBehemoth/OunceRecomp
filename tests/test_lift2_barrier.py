"""The lifter's compiler barriers on the edges that close a cycle (nx2/lift2/build_ir.py cycle_edges), without the dump.

Small functions lifted from their words: a plain loop, nested loops, a backward branch onto no cycle, an irreducible
loop entered at two blocks, a self-loop, a loop closed through a jump table's switch and a branch back to the function's
own entry. Each check: the CFG without the barrier blocks has no cycle (so every cycle has a barrier), and the function
with no cycle has none. Then a spin loop on a plain load (ldr w0, [x1]; cbz w0, back) compiled at -O1 and -O2 still
loads in its loop, the barrier adds no instruction bytes, and YIELD is a pause.

python -I -B tests/test_lift2_barrier.py
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "nx2"))
from lift2 import build_ir, emit_llvm, llvm  # noqa: E402

BASE = 0x1000
RET = 0xd65f03c0
CASES = {
    # ldr w0, [x1]; cbz w0, back; ret
    "loop": ([0xb9400020, 0x34ffffe0, RET], {}),
    # ldr w0, [x1]; (inner) ldr w2, [x3]; cbz w2, inner; cbz w0, outer; ret
    "nested": ([0xb9400020, 0xb9400062, 0x34ffffe2, 0x34ffffa0, RET], {}),
    # b 0x100c; (cold) mov x0, #1; ret; cbz w0, cold; ret: a branch back to an earlier address on no cycle
    "acyclic": ([0x14000003, 0xd2800020, RET, 0x34ffffc0, RET], {}),
    # cbz w0, L2; L1: add x1, x1, #1; cbz w1, out; L2: add x2, x2, #1; cbnz w2, L1; ret; out: ret
    "irreducible": ([0x34000060, 0x91000421, 0x34000081, 0x91000442, 0x35ffffa2, RET, RET], {}),
    # b .
    "self": ([0x14000000], {}),
    # ldr w0, [x1]; br x2 (a jump table: back to the entry, or on); ret
    "switch": ([0xb9400020, 0xd61f0040, RET], {BASE + 4: [BASE, BASE + 8]}),
    # cbz w0, out; ldr w0, [x1]; b <the entry>; out: ret
    "own_entry": ([0x34000060, 0xb9400020, 0x17fffffe, RET], {}),
}


def lift(words, jt):
    prog = build_ir.Program(entries={BASE}, jt=dict(jt))
    return build_ir.build(BASE, [BASE + 4 * k for k in range(len(words))], lambda pc: words[(pc - BASE) // 4], prog)


def barrier_blocks(fn):
    return [b for b in fn.blocks.values() if any(v.op == "barrier" for v in b.ops)]


def has_cycle(blocks, removed):
    """a cycle among blocks, the removed ones left out"""
    keep = [b for b in blocks if id(b) not in removed]
    ids = {id(b) for b in keep}
    state = {}

    def visit(b):
        state[id(b)] = 1
        for s in b.succs():
            if id(s) not in ids:
                continue
            if state.get(id(s)) == 1 or (id(s) not in state and visit(s)):
                return True
        state[id(b)] = 2
        return False
    return any(id(b) not in state and visit(b) for b in keep)


class Barriers(unittest.TestCase):
    def test_every_cycle_has_one(self):
        for name, (words, jt) in CASES.items():
            with self.subTest(name):
                fn = lift(words, jt)
                blocks = list(fn.blocks.values())
                bar = barrier_blocks(fn)
                if name == "acyclic":
                    self.assertFalse(has_cycle(blocks, set()))
                    self.assertEqual(bar, [])
                else:
                    self.assertTrue(has_cycle(blocks, set()), "the case has a cycle")
                    self.assertTrue(bar)
                    self.assertFalse(has_cycle(blocks, {id(b) for b in bar}), "a cycle without a barrier")


def asm_of(words, opt):
    fn = lift(words, {})
    fn.name = "f_%08x" % BASE
    m = emit_llvm.Module()
    m.add(fn)
    r = subprocess.run([llvm.check("clang"), "-" + opt, "-w", "-x", "ir", "-", "-S", "-o", "-"], input=m.text(),
                       capture_output=True, text=True)
    if r.returncode:
        raise AssertionError(r.stderr[-2000:])
    body = r.stdout.split("f_%08x:" % BASE, 1)[1].split(".Lfunc_end", 1)[0]
    return [l.strip() for l in body.splitlines() if l.strip() and not l.strip().startswith((".", "#"))
            and not re.match(r"\.?L\w+:", l.strip())]


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux and clang of the pinned LLVM major")
class Code(unittest.TestCase):
    def test_spin_loop_reloads(self):
        for opt in ("O1", "O2"):
            with self.subTest(opt):
                lines = asm_of(CASES["loop"][0], opt)
                loads = [l for l in lines if re.match(r"movl\s+\(%\w+(,%\w+)?\),\s*%\w+", l)]
                jumps = [l for l in lines if re.match(r"j\w+\s", l)]
                self.assertTrue(loads and jumps, "\n".join(lines))
                self.assertFalse(any("call" in l for l in lines), "\n".join(lines))   # (no poll: nothing called)

    def test_barrier_emits_nothing(self):
        # the same loop without its barrier op: the same instructions but for the load the barrier keeps in the loop
        fn = lift(CASES["self"][0], {})
        fn.name = "f_%08x" % BASE
        m = emit_llvm.Module()
        m.add(fn)
        text = m.text()
        self.assertIn('call void asm sideeffect "", "~{memory}"()', text)
        r = subprocess.run([llvm.check("clang"), "-O2", "-w", "-x", "ir", "-", "-S", "-o", "-"], input=text,
                           capture_output=True, text=True, check=True)
        body = r.stdout.split("f_%08x:" % BASE, 1)[1].split(".Lfunc_end", 1)[0]
        code = [l.strip() for l in body.splitlines() if l.strip() and not l.strip().startswith((".", "#"))
                and not re.match(r"\.?L\w+:", l.strip())]
        self.assertEqual([l.split()[0] for l in code if not l.startswith("#")], ["jmp"], body)   # (b .: one jmp)

    def test_yield_is_pause(self):
        # yield; ret
        self.assertIn("pause", asm_of([0xd503203f, RET], "O1"))


if __name__ == "__main__":
    unittest.main()
