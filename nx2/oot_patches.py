"""Changes to the lifted C for Ocarina of Time, applied by lift.py after the present lifter writes its code (and by hand:
python -I nx2/oot_patches.py).

What is left here is for the present lifter's C alone: the counters cut to whole frames (WHOLE_SITES), which lift 2
makes as probes (nx2/lift2/probes.py). The frame-rate and resolution patches are lift 2's At probes (nx2/lift2/probes.py
AT, host/oot_probes.c); the patches parked while the present lifter is retired (heart protection, flight, the free and
mouse cameras, text speed, the menus' mouse) are in nx2/oot_parked.py, which nothing applies.
"""
import glob
import os
import sys
import whole_write   # (rewrites written beside and moved over: issue 1197)

LIFE_BUILD_ID = "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348"   # (the supported build's main: OoT 1.1.1)


# Counters that add the frame step, or the step times a whole number, cut to whole frames (host/frame_whole.h). At 60 fps the step
# is 1; under a Frame rate cap above 60 it is the frame's own time, under 1 (0.42 at 144 fps), and the cut is 0 every frame:
# - f_80a87e50, the pick-up indicator above Link's head (a rupee, a Deku Stick): the count it shows (+864) moves toward the
#   wallet's (+860) by (int)(step * max(distance / 50, 1)) and the indicator fades when they meet. Cut to 0, it stayed at the old
#   count until a door or a talk replaced it (issue 1832).
# - f_800672e0, the water renderer: its ripple simulation runs (int)step times the water's substeps (+76 of the pass) each frame.
#   Cut to 0, the rings Link and the actors make in water stood still above 60 fps. Found by atk3001 for Lichii (zortos293/
#   Lichii#1152), as ported to Fritchii (Frizti/Fritchii d389685a4, 063dbb741): its remainder is kept per water object (x21).
# hos_frame_whole carries the fractions per counter while the frames' own time is the step, so each moves at the game's rate;
# otherwise it cuts as the game does. A site is added here once it is seen to misbehave in game. (A site's function, its code to
# replace, and the counter's address for its remainder.)
WHOLE_SITES = (
    # (the cut alone, the function's one: the multiply before it is a64_fpx_hot_mul32_fma32 in a lift where the function is
    # among A64_HOT_FUNCS)
    ("void f_80a87e50(Ctx *__restrict A64_NOESC c) {@",
     "    c->x[12]=recomp_fp_to_int(c->vreg[0][0]&UINT64_C(0xffffffff),32,32,1,3,c->fpcr,&c->fpsr);@",
     "c->x[19] + 864"),
    # (the cut alone, the function's one)
    ("void f_800672e0(Ctx *__restrict A64_NOESC c) {@",
     "    c->x[9]=recomp_fp_to_int(c->vreg[0][0]&UINT64_C(0xffffffff),32,32,1,3,c->fpcr,&c->fpsr);@",
     "c->x[21]"),
)
WHOLE_SITES = tuple((h.replace("@", "\n"), o.replace("@", "\n"), k) for h, o, k in WHOLE_SITES)
WHOLE_MARK = "/* (the step's whole frames: nx2/oot_patches.py) */"
WHOLE_DECL = "extern uint32_t hos_frame_whole(uint64_t at, float amount);\n"
# (the present lifter keeps S0 as bits in the context: the float made of them)
WHOLE_AMOUNT = "({ float _f; const uint32_t _u = (uint32_t)c->vreg[0][0]; memcpy(&_f, &_u, 4); _f; })"


def whole_new(old, key):
    """old with its cut to whole frames (the one recomp_fp_to_int) made hos_frame_whole's for the counter at key"""
    lines = old.split("\n")
    for i, line in enumerate(lines):
        if "recomp_fp_to_int(c->vreg[0][0]" in line:
            reg = line.split("=", 1)[0]
            lines[i] = "%s=hos_frame_whole(%s, %s);   %s" % (reg, key, WHOLE_AMOUNT, WHOLE_MARK)
    return "\n".join(lines)


def patch_whole_frames(s):
    """The counters of WHOLE_SITES take hos_frame_whole's whole frames: (s, the sites patched here). A file without the function,
    one patched before, or one whose code differs (another game version) is left as it is."""
    done = 0
    for head_text, old, key in WHOLE_SITES:
        head = s.find(head_text)
        if head < 0:
            continue
        end = s.find("\n}\n", head)
        body = s[head:end if end >= 0 else len(s)]
        new = whole_new(old, key)
        if new in body or body.count(old) != 1:
            continue
        at = head + body.index(old)
        s = s[:at] + new + s[at + len(old):]
        done += 1
    if done and WHOLE_DECL not in s:
        s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + WHOLE_DECL, 1)
    return s, done


def whole_probed(gen):
    """the WHOLE_SITES lift 2 makes as probes (nx2/lift2/probes.py): their functions are not the present lifter's
    (lift2_owned.txt beside gen), so their C is not here; lift 2 stops when a probe's instruction is missing"""
    from lift2 import owned, probes
    got = owned.load(os.path.dirname(os.path.abspath(gen)))
    if got is None:
        return 0
    return sum(1 for p in probes.PROBES if p.fn == "hos_frame_whole" and p.func not in got[0])


def apply(gen):
    """the whole-frame counters in the present lifter's C; a warning when a site is neither there nor lift 2's probe"""
    whole_sites = 0
    for path in sorted(glob.glob(os.path.join(glob.escape(gen), "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        out, whole = patch_whole_frames(s)
        whole_sites += out.count(WHOLE_MARK)   # (made now, or by an earlier run over this file)
        if whole:
            print("oot_patches: %d frame counter(s) with the step's whole frames in %s" % (whole, os.path.basename(path)))
        if out != s:
            whole_write.write_whole(path, out, newline="")
    if whole_sites + whole_probed(gen) != len(WHOLE_SITES):   # (the frames' own time without them: the pick-up indicator stays above 60 fps)
        print("oot_patches: WARNING: %d of %d frame counters found for the step's whole frames (above 60 fps a counter can stop: "
              "the pick-up indicator)" % (whole_sites, len(WHOLE_SITES)))


if __name__ == "__main__":
    apply(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "sa", "gen"))
