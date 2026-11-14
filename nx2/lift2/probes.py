"""Probes: host code at guest addresses, the lift 2 form of the present lifter's text patches (docs/lifter2-design.md
section 9).

A probe names a function and one of its instructions, found by llvm-mc's text of the instruction (a pattern the
function has exactly once, as the text patches' anchors were), and says what the host does there:
- Value(func, match, fn, args): the instruction's result register gets fn(args) instead (a host function of the
  named guest values: "xN", "xN+K" for an address, "sNbits" for the bits of Sn);
- the other kinds of section 9 (Before, After, CondOr, CallSite) are the same lookup with the call placed before or
  after the instruction's own semantics, or as the condition / the call target; they come with the patches that need
  them.
The present lifter's patch of each probed function leaves a marker in its lifted C (MARKS): mixed.py takes a function
whose only host edit is that marker, and lift 2 must then find the probe's instruction (a missing one stops the build:
another game version's code).
"""
import re

from . import enc  # (first: its tables import asm)
from . import asm
from .sem_int import getx, setx
from .sem_fp import vread


class Value:
    def __init__(self, func, match, fn, args):
        self.func, self.match, self.fn, self.args = func, re.compile(match), fn, args

    def lower(self, b, insn, pc):
        vals = []
        for a in self.args:
            m = re.fullmatch(r"x(\d+)(?:\+(\d+))?", a)
            if m:
                v = getx(b, int(m.group(1)), 1)
                if m.group(2):
                    v = b.bin("add", v, b.const(int(m.group(2))))
                vals.append(v)
                continue
            m = re.fullmatch(r"s(\d+)bits", a)
            vals.append(vread(b, int(m.group(1)), "i32"))
        r = b.call(self.fn, "i32", *vals)
        setx(b, insn.f["Rd"], r)


# nx2/oot_patches.py WHOLE_SITES: the counters of these two functions advance by the host's whole frames (the frame
# step's FCVTZS of the frame count replaced by hos_frame_whole of the counter's address and the count's bits)
PROBES = [
    Value(0x80a87e50, r"fcvtzs w\d+, s0", "hos_frame_whole", ("x19+864", "s0bits")),
    Value(0x800672e0, r"fcvtzs w\d+, s0", "hos_frame_whole", ("x21", "s0bits")),
]
MARKS = {"/* (the step's whole frames: nx2/oot_patches.py) */"}
BY_FUNC = {}
for _p in PROBES:
    BY_FUNC.setdefault(_p.func, []).append(_p)


def find(func, pcs, word):
    """{pc: lowering} of the probes of one function; SystemExit when a probe's instruction is not there once"""
    out = {}
    for p in BY_FUNC.get(func, ()):
        hits = []
        for pc in pcs:
            i = enc.decode(word(pc))
            if i is not None and p.match.fullmatch(asm.render(i, pc)):
                hits.append((pc, i))
        if len(hits) != 1:
            raise SystemExit("lift2 probes: f_%08x: %d instructions match %r (expected one: another game version?)"
                             % (func, len(hits), p.match.pattern))
        out[hits[0][0]] = p.lower
    return out
