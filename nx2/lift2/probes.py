"""Probes: host code at guest addresses, where the game's code needs the host (docs/lifter.md section 10).

A probe names a function and one of its instructions, found by llvm-mc's text of the instruction (a pattern the
function has exactly once, as the text patches' anchors were), and says what the host does there:
- Value(func, match, fn, args): the instruction's result register gets fn(args) instead (a host function of the
  named guest values: "xN", "xN+K" for an address, "sN" for Sn as a float);
- the other kinds of section 9 (Before, After, CondOr, CallSite) are the same lookup with the call placed before or
  after the instruction's own semantics, or as the condition / the call target; they come with the patches that need
  them.
The lifter must find each probe's instruction: a missing one stops the build (another game version's code).
"""
import os
import re

from . import enc  # (first: its tables import asm)
from . import asm
from . import sem
from .sem_int import getx, setx
from .sem_fp import vread, vwrite


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
            m = re.fullmatch(r"s(\d+)", a)
            vals.append(vread(b, int(m.group(1)), "f32"))
        r = b.call(self.fn, "i32", *vals)
        setx(b, insn.f["Rd"], r)


# nx2/oot_patches.py WHOLE_SITES: the counters of these two functions advance by the host's whole frames (the frame
# step's FCVTZS of the frame count replaced by hos_frame_whole of the counter's address and the count, a float)
PROBES = [
    Value(0x80a87e50, r"fcvtzs w\d+, s0", "hos_frame_whole", ("x19+864", "s0")),
    Value(0x800672e0, r"fcvtzs w\d+, s0", "hos_frame_whole", ("x21", "s0")),
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


# ---- At: a host function of a few registers after an instruction (after a BL / BLR when it returns), its result in a
# register: "at pc: reg := fn(regs...)". The arguments: "xN" (i64), "wN" (i32, the low half), "sN" (Sn, a float),
# "#K" (an i32 constant); the result: "xN", "wN" (zero-extended, as every W write) or "sN" (a float, the rest of Vn
# zero, as every scalar FP write), or None. The host function (host/oot_probes.c) takes plain values and guest
# addresses, never the context; it may read and write guest memory (the call is never moved or removed). Each row names
# the instruction's word, checked at lift time: a word that differs stops the lift (another game version). The function
# must be lift 2's (mixed.prepare stops otherwise): the present lifter does not make these.
class At:
    def __init__(self, func, pc, word, fn, args=(), result=None, before=False):
        self.func, self.pc, self.word, self.fn, self.args, self.result = func, pc, word, fn, tuple(args), result
        self.before = before   # (before the instruction instead: pc must not be a branch target, so it runs on one edge)

    def post(self, b):
        vals = []
        for a in self.args:
            kind, n = a[0], a[1:]
            if kind == "#":
                vals.append(b.const(int(n, 0), "i32"))
            elif kind in "xw":
                vals.append(getx(b, int(n), kind == "x"))
            else:
                vals.append(vread(b, int(n), "f32"))
        ty = {None: "void", "x": "i64", "w": "i32", "s": "f32"}[self.result and self.result[0]]
        r = b.call(self.fn, ty, *vals, effects=True)
        if self.result is None:
            return
        n = int(self.result[1:])
        if self.result[0] == "s":
            vwrite(b, n, r)
        else:
            setx(b, n, r)


# (host/oot_probes.c says what each does and why; hos_mouse_camera: host/mouse_native.c)
AT = [
    # dynamic resolution and the fixed scale
    At(0x80825aa0, 0x80825bcc, 0xb9409508, "hos_oot_dynres_flags", ("w8",), "w8"),   # ldr w8, [x8, #148]
    At(0x80825aa0, 0x80826420, 0x52a7f009, "hos_oot_fixed_scale", (), "w9"),        # mov w9, #1.0
    At(0x831f3200, 0x831f3efc, 0xb94e2e89, "hos_oot_scale_recompute", ("x20", "w8"), "w8"),   # ldr w9, [x20, #3628]
    At(0x831f3200, 0x831f3f08, 0xf9471288, "hos_oot_scale_flags", ("x20", "x8"), "x8"),       # ldr x8, [x20, #3616]
    # native scene (HOS_SCENE_NATIVE) and the cached scale's refresh (HOS_FIX_RES)
    At(0x806702e0, 0x80670308, 0x3955280e, "hos_oot_scene_mode", ("x0", "w14"), "w14"),     # ldrb w14, [x0, #1354]
    At(0x831f3200, 0x831f3200, 0x39788008, "hos_oot_scene_check", ("x0",)),                # ldrb w8, [x0, #3616]
    # frame seconds: both step divides
    At(0x80706be0, 0x80706cc4, 0x1e201828, "hos_oot_frame_step_probe", ("s1", "s8", "x19"), "s8"),   # fdiv s8, s1, s0
    At(0x83b6ea90, 0x83b6eaf8, 0x1e201820, "hos_oot_frame_step", ("s1", "s0"), "s0"),                 # fdiv s0, s1, s0
    # movie frames
    At(0x83b7c710, 0x83b7c7c8, 0x11000508, "hos_oot_movie_count", ("w8", "x20"), "w8"),     # add w8, w8, #1
    # output size
    At(0x81222ce0, 0x81222ee4, 0xf2c0b408, "hos_oot_output_size", (), "x8"),               # movk x8, #1440, lsl #32
    At(0x810a7b70, 0x810a7b8c, 0xaa0103f4, "hos_oot_output_config", ("x20",)),             # mov x20, x1
    At(0x80ea2f30, 0x80ea2ffc, 0x29406418, "hos_oot_base_size", ("w24", "#0"), "w24"),     # ldp w24, w25, [x0]
    At(0x80ea2f30, 0x80ea2ffc, 0x29406418, "hos_oot_base_size", ("w25", "#1"), "w25"),
    At(0x80ea2f30, 0x80ea303c, 0x52a89e08, "hos_oot_base_size_bits", ("#0",), "w8"),       # mov w8, #1920.0
    At(0x80ea2f30, 0x80ea3044, 0x52a890e8, "hos_oot_base_size_bits", ("#1",), "w8"),       # mov w8, #1080.0
    # the render targets' pool
    At(0x836ce9d0, 0x836cea54, 0x39587668, "hos_oot_rt_pool_config", ("x19",)),            # ldrb w8, [x19, #1565]
    At(0x83b7b340, 0x83b7b438, 0xaa1703e2, "hos_oot_rt_heap_size", ("x0",), "x0"),         # mov x2, x23 (x0: the heap's size)
    At(0x83b7b340, 0x83b7b4dc, 0xd63f0100, "hos_oot_rt_heap_use", ("x0", "x21", "x22"), "x0"),   # blr x8 (returns the use)
    At(0x80b72340, 0x80b723a4, 0x52800024, "hos_oot_rt_rest", ("x0",), "x0"),              # mov w4, #1 (x0: the size)
    # text speed: the letters' time (s0) on the block's fall-through edge, past the hidden / no-text checks
    At(0x80b1e140, 0x80b1e1bc, 0x37201528, "hos_oot_text_time", ("s0",), "s0", before=True),   # tbnz w8, #4
    # native mouse camera: the player camera's update (x0: the camera), first
    At(0x80644c80, 0x80644c80, 0xd503237f, "hos_mouse_camera", ("x0",)),                   # hint #27 (PACIBSP)
    # the player's update, for HOS_BENCH's "in play" (x19 the player actor)
    At(0x82c415e0, 0x82c41728, 0xf9000bea, "hos_oot_player_update", ("x19",)),            # str x10, [sp, #16]
]
AT_FUNCS = {p.func for p in AT}


def at(func, pcs, word, before=False):
    """{pc: [fn(builder)]} of a function's At probes, for Program.after (before: the ones for Program.before);
    SystemExit when a word differs, or when a before probe's instruction is a branch target"""
    out = {}
    have = set(pcs)
    for p in AT:
        if p.func != func or p.before != before:
            continue
        if before:
            from .build_ir import target
            for pc in pcs:
                i = enc.decode(word(pc))
                if i is not None and target(i, pc) == p.pc:
                    raise SystemExit("lift2 probes: At f_%08x before %#x: a branch target (from %#x)" % (func, p.pc, pc))
        if p.pc not in have or word(p.pc) != p.word:
            raise SystemExit("lift2 probes: At f_%08x at %#x holds %s, not %08x (another game version?)"
                             % (func, p.pc, "%08x" % word(p.pc) if p.pc in have else "nothing", p.word))
        out.setdefault(p.pc, []).append(p.post)
    return out


# ---- C after an instruction (a64_lift.py Lifter.after: oot_wide, oot_jitter, oot_occlusion; SPLICE_AT's splices). The
# C works on the whole guest context; lift 2's generic entry keeps every written register stored there (write-through),
# so after the instruction lift2_after_<pc>(ctx) is called (that C, unchanged, as a host function: write_after_c), and
# the registers are the context's again after it. NZCV is not stored: none of the C reads it. A patch that needs only a
# few registers is an At probe instead (below): its values stay SSA values.
AFTER_HEAD = """/* Generated by nx2/lift2/probes.py from the C the present lifter puts after these instructions
   (nx2/oot_wide.py, oot_jitter.py, oot_occlusion.py): called by lift 2's code after each. Do not edit. */
#include "a64.h"
#include <stdint.h>
#include <string.h>
"""


def is_call(w):
    """a BL or BLR: what comes after it runs when the call returns"""
    i = enc.decode(w)
    return i is not None and (i.entry.template == "blr" or i.entry.template == "b" and i.f["op"] == 1)


def after_c(texts, pcs, word):
    """{pc: fn(builder)} for Program.after: the instructions of one function that have C after them"""
    out = {}
    for pc in pcs:
        if pc in texts:
            def post(b, pc=pc):
                b.call("lift2_after_%08x" % pc, "void", ctx=True, effects=True)
                b.after_call()
            out[pc] = post
    return out


def merge(*posts):
    """Program.after from several {pc: fn(builder)} (or {pc: [fn]}), in the order given"""
    out = {}
    for d in posts:
        for pc, fn in d.items():
            out.setdefault(pc, []).extend(fn if isinstance(fn, list) else [fn])
    return out


def write_after_c(texts, folder):
    """the host C file of every lift2_after_<pc>"""
    path = os.path.join(folder, "lift2_after.c")
    body = [AFTER_HEAD] + ["void lift2_after_%08x(Ctx *c) {\n%s\n}\n" % (pc, texts[pc]) for pc in sorted(texts)]
    text = "\n".join(body)
    if not os.path.exists(path) or open(path).read() != text:
        with open(path, "w") as f:
            f.write(text)
    return path


# ---- SPLICE_AT: host/*.c's SPLICE_AFTER(0xFUNC, "anchor") bodies, called after these guest instructions. The anchors
# (text of the present lifter's C) were turned into instruction addresses once (the present lifter's C with pc comments:
# the instruction whose C holds the anchor); each is checked against its word at lift time. f_8007fe60
# (anchored after a TBZ) and f_80084de0 (after a BLR) run on the fall-through: SPLICE_BEFORE calls them before the next
# instruction, which must not be a branch target (checked at lift time), so the call is on that edge alone.
SPLICE_AT = {
    0x8005ed00: ((0x8005fb74, 0xf9001eea), (0x80061814, 0xf9001eea)),
    0x80289590: ((0x80289cbc, 0xf9400108), (0x80289fe0, 0xf9400108)),
    0x80361a00: ((0x80361b3c, 0xf8286adf),),
    # (f_80361c80's SPLICE_AFTER_EACH: every str xzr, [x19, x8] of the function, 12)
    0x80361c80: ((0x80361d5c, 0xf8286a7f), (0x80362048, 0xf8286a7f), (0x803620b0, 0xf8286a7f), (0x80362118, 0xf8286a7f),
                 (0x80362180, 0xf8286a7f), (0x803621e8, 0xf8286a7f), (0x80362848, 0xf8286a7f), (0x803628b0, 0xf8286a7f),
                 (0x80362918, 0xf8286a7f), (0x80362b80, 0xf8286a7f), (0x80362be8, 0xf8286a7f), (0x80362c50, 0xf8286a7f)),
    0x80362ff0: ((0x80363294, 0xa9027d1f),),
    0x80666d20: ((0x80667108, 0xf9400108), (0x806674a8, 0xf9400108), (0x806675d4, 0xf9400108),
                 (0x80667a14, 0xf9400108), (0x80667a8c, 0xf9400108)),
    0x806688c0: ((0x80668c7c, 0xf9400108), (0x80668dd8, 0xf9400108), (0x80668fb0, 0xf9400108)),
    0x80846070: ((0x808460e4, 0x9a8803e0), (0x80846114, 0x9a8803e0), (0x80846144, 0x9a8803e0)),
    0x80c17040: ((0x80c17580, 0x9a8803e1),),
    0x82c02f20: ((0x82c02f94, 0xf9400108), (0x82c03014, 0xf9400108), (0x82c03024, 0xf9400108),
                 (0x82c030bc, 0xf9400108)),
    0x82c03170: ((0x82c031e4, 0xf9400108), (0x82c03264, 0xf9400108), (0x82c03274, 0xf9400108),
                 (0x82c0330c, 0xf9400108)),
    0x831fa4a0: ((0x831fa57c, 0xf9400104),),
}


# (func: (the instruction the anchor follows, its word), called before the next instruction)
SPLICE_BEFORE = {
    0x8007fe60: (0x800809cc, 0x36500057),   # tbz w23, #10: its fall-through
    0x80084de0: (0x800855a8, 0xd63f0100),   # blr x8: its return
}


def splice_before(func, pcs, word):
    """{pc: fn(builder)} of a function's SPLICE_BEFORE site, for Program.before; SystemExit when its word differs or the next instruction is
    a branch target (the call would run on another path too)"""
    if func not in SPLICE_BEFORE:
        return {}
    at, w = SPLICE_BEFORE[func]
    if word(at) != w:
        raise SystemExit("lift2 probes: SPLICE_BEFORE f_%08x at %#x holds %08x, not %08x" % (func, at, word(at), w))
    from .build_ir import target
    if enc.decode(word(at + 4)) is None:
        raise SystemExit("lift2 probes: SPLICE_BEFORE f_%08x: %#x is not an instruction" % (func, at + 4))
    for pc in pcs:
        i = enc.decode(word(pc))
        if i is not None and target(i, pc) == at + 4:
            raise SystemExit("lift2 probes: SPLICE_BEFORE f_%08x: %#x is a branch target (from %#x)" % (func, at + 4, pc))
    name = "splice_0x%08x" % func

    def lower(b):   # (Program.before: the next instruction may be a branch, as f_8007fe60's CBNZ)
        b.call(name, "void", ctx=True, effects=True)
        b.after_call()
    return {at + 4: lower}


def splice_texts(func, word):
    """{pc: C} of a function's SPLICE_AT sites; SystemExit when a site's word differs (another game version)"""
    out = {}
    for pc, w in SPLICE_AT.get(func, ()):
        if word(pc) != w:
            raise SystemExit("lift2 probes: SPLICE_AT f_%08x at %#x holds %08x, not %08x" % (func, pc, word(pc), w))
        out[pc] = "    void splice_0x%08x(Ctx *c); splice_0x%08x(c);" % (func, func)
    return out


def all_splice_texts():
    return {pc: "    void splice_0x%08x(Ctx *c); splice_0x%08x(c);" % (f, f)
            for f, sites in SPLICE_AT.items() for pc, _ in sites}
