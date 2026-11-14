"""Changes to the lifted C for Ocarina of Time, applied by lift.py after it writes the code (and by hand: python -I nx2/oot_patches.py).

Each is a runtime switch where it can be, so the game's own path stays reachable.

- Dynamic resolution (Lichii's built-in "Max Resolution" patch, main+0x805BD0): the engine's config flag at +0x94 picks the path that
  follows the GPU-time controller; with hos_fixed_resolution set it takes the other path, the 3D at full scale. The controller made
  a new set of render targets at each size, ~11 images a frame at times, and held the GPU near its budget.
- The fixed path's scale (hos_fixed_scale_bits: the Insert menu's Resolution) reaching the 3D, in the caller f_831f3200
  (main+0x31d3200). It recomputes the 3D's scale (+0xe5c = scale * +0xe58 / +0xe50) only while +0xe2c bit 3 is set. The dynamic
  path sets it on a level change; the fixed path's flag 1 calls f_831f41d0, whose bit 3 the apply block clears in the same frame,
  before any recompute. So the fixed path kept the dynamic path's last scale (0.75 docked: 1536x864 at every setting). Flag 1 now
  also asks for the recompute, and on the fixed path the flags +0xe20 bits 3/4 (a hardcoded 0.75 / 0.5 in place of the scale)
  are not looked at.
The patches parked while the present lifter is retired (heart protection, flight, the free and mouse cameras, text speed,
the menus' mouse) are in nx2/oot_parked.py, which nothing applies.
"""
import glob
import os
import sys
import whole_write   # (rewrites written beside and moved over: issue 1197)

DECL = "extern volatile int hos_fixed_resolution;\nextern volatile int hos_dynres_runs;\nextern volatile uint32_t hos_fixed_scale_bits;\n"
OLD = "if (!((c->x[8] >> 0) & 1)) { goto L_8082640c; }"
NEW = "if (hos_fixed_resolution || !((c->x[8] >> 0) & 1)) { goto L_8082640c; }\n    hos_dynres_runs++;"
LABEL = "L_8082640c:;"
SCALE_OLD = "c->x[9] = 0x3f800000ULL;"
SCALE_NEW = "c->x[9] = hos_fixed_scale_bits;"   # (the fixed path's scale, 1.0 as shipped)
DECL2 = "extern volatile int hos_fixed_resolution;\nextern volatile uint64_t hos_res_module;\nextern volatile uint32_t hos_fixed_scale_bits;\n"
APPLY_OLD = "L_831f3efc:;\n    c->x[9]=recomp_load32(c,c->x[20] + 3628);"
APPLY_NEW = "L_831f3efc:;\n    if (hos_fixed_resolution) {   /* (only while +0xe5c is not the chosen scale: a recompute every frame remade the targets) */\n        union { uint32_t u; float f; } now = {(uint32_t)recomp_load32(c,c->x[20] + 3676)}, want = {hos_fixed_scale_bits},\n            e50 = {(uint32_t)recomp_load32(c,c->x[20] + 3664)}, e58 = {(uint32_t)recomp_load32(c,c->x[20] + 3672)};\n        const float d = now.f - (e50.f != 0 ? want.f * e58.f / e50.f : want.f);\n        if (d > 1e-4f || d < -1e-4f) c->x[8] |= 8;\n    }\n    c->x[9]=recomp_load32(c,c->x[20] + 3628);"
FLAGS_OLD = "L_831f3f08:;\n    c->x[8]=recomp_load64(c,c->x[20] + 3616);\n"
FLAGS_NEW = FLAGS_OLD + "    hos_res_module = c->x[20];\n    if (hos_fixed_resolution) c->x[8] &= ~0x18ULL;\n"


def fixed_scale(t):
    """The fixed path's scale reaching the 3D (the Resolution row). (t, 1) when applied here"""
    if APPLY_OLD not in t or FLAGS_OLD not in t:
        return t, 0
    t = t.replace(APPLY_OLD, APPLY_NEW, 1).replace(FLAGS_OLD, FLAGS_NEW, 1)
    return t.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL2, 1), 1

LIFE_BUILD_ID = "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348"
def patch_dynres(s):
    if OLD not in s:
        return None
    s = s.replace(OLD, NEW, 1)
    at = s.index(LABEL)
    scale = s.index(SCALE_OLD, at)
    s = s[:scale] + SCALE_NEW + s[scale + len(SCALE_OLD):]
    return s.replace('#include "a64.h"\n', '#include "a64.h"\n' + DECL, 1)


# The frame's real time in place of the nominal rate, for a Frame rate cap above 60 (host/nn_vi.c hos_frame_seconds_bits; 0, as
# at a cap of 60: the game's own divide, nothing changes). The engine has two places where its integer nominal rate (+0x2c8 of
# the graphics module) becomes a time step, both a float divide "base / nominal":
# - f_80706be0: the frame's step, base 60.0 (30.0 in the half-rate mode), added to the game clock (the double at
#   [x19+0xd58], in 1/60 s) and handed on as the frame's seconds;
# - f_83b6ea90: the global step (60.0 / nominal, a float in .data) the rest of the engine reads.
# The engine's own rate is 60 / <whole display intervals>, never above 60, and its controller puts 60 back over a higher value
# written from outside before either divide runs: above 60 fps every frame still stepped 1/60 s and the game ran fps / 60 times
# too fast. With the host's measured frame time the step is base * seconds, whatever the rate. hos_step_probe: f_80706be0's
# x19, which tells the host the hook is in this build and gives it the game clock for its log (HOS_FPS_LOG=1).
STEP_SITES = (("void f_80706be0(Ctx *__restrict A64_NOESC c) {\n", 8, "\n    hos_step_probe = c->x[19];"),
              ("void f_83b6ea90(Ctx *__restrict A64_NOESC c) {\n", 0, ""))
STEP_OLD = "    _v=a64_fpx_div32_div32(c,_a,_b);\n    a64_vset64(c->vreg[%d],_v);}"
STEP_HOOK = ("    _v=a64_fpx_div32_div32(c,_a,_b);\n"
             "    { const uint32_t _hs = hos_frame_seconds_bits; if (_hs) { float _base, _secs; uint32_t _w; memcpy(&_base,&_a,4); "
             "memcpy(&_secs,&_hs,4); _base *= _secs; memcpy(&_w,&_base,4); _v = _w; } }   /* (base * the frame's seconds: nx2/oot_patches.py) */\n"
             "    a64_vset64(c->vreg[%d],_v);}")
STEP_MARK = "hos_frame_seconds_bits; if (_hs)"   # (once per divide with the hook)
STEP_DECL = "extern volatile uint32_t hos_frame_seconds_bits;\nextern volatile uint64_t hos_step_probe;\n"


def patch_frame_seconds(s):
    """The host's frame time at the step divides of f_80706be0 and f_83b6ea90: (s, the sites patched here). A site whose
    function is not in the file, is patched already, or whose code differs (another game version) is left as it is."""
    done = 0
    for head_text, reg, extra in STEP_SITES:
        head = s.find(head_text)
        if head < 0:
            continue
        end = s.find("\nvoid f_", head + len(head_text))
        body = s[head:end if end >= 0 else len(s)]
        old, new = STEP_OLD % reg, STEP_HOOK % reg + extra
        if new in body or body.count(old) != 1 or body.count("a64_fpx_div32_div32") != 1:
            continue
        at = head + body.index(old)
        s = s[:at] + new + s[at + len(old):]
        done += 1
    if done and STEP_DECL not in s:
        s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + STEP_DECL, 1)
    return s, done


def unpatch_frame_seconds(s):
    """s without the hook, as the lift wrote it: for a lift where only one of the two divides was found (apply)"""
    for _, reg, extra in STEP_SITES:
        s = s.replace(STEP_HOOK % reg + extra, STEP_OLD % reg)
    return s if STEP_MARK in s else s.replace(STEP_DECL, "", 1)   # (a hook of another shape stays, with its declarations)


# A movie's frame count above a 60 cap. The game's movie player counts its updates while a movie plays (f_83b7c710: one more
# at +760 of the player each frame), and a cutscene that shows a movie takes that count as its timeline's frame (f_83b5ec10,
# f_83b5e9a0): the narration, the rumble and the cutscene's end are placed by it. The movie itself follows the clock, so above
# 60 fps those cues ran ahead of the picture, fps / 60 times too fast (issue 1742). The count now grows by the frames' time, as
# the game's clock does (hos_movie_frames, host/nn_vi.c: 1 as before while the host gives no frame time).
MOVIE_HEAD = "void f_83b7c710(Ctx *__restrict A64_NOESC c) {\n"
# (the load and the add alone: the store after them is rewritten by the page marks' pass, which runs after these edits)
MOVIE_OLD = ("    c->x[8]=recomp_load32(c,c->x[20] + 760);\n"
             "    { uint64_t _a=c->x[8], _b=1ULL; uint64_t _r=_a+_b; _r&=0xFFFFFFFFULL; c->x[8]=_r; }\n")
MOVIE_NEW = MOVIE_OLD.replace("_b=1ULL", "_b=hos_movie_frames(c->x[20])")
MOVIE_DECL = "uint32_t hos_movie_frames(uint64_t player);\n"


def patch_movie_frames(s):
    """The movie player's count by the frames' time; None when the file has no such function, s unchanged when the function is
    there but its code differs (another game version) or has the patch already."""
    head = s.find(MOVIE_HEAD)
    if head < 0:
        return None
    end = s.find("\nvoid f_", head + len(MOVIE_HEAD))
    body = s[head:end if end >= 0 else len(s)]
    if body.count(MOVIE_OLD) != 1:
        return s
    at = head + body.index(MOVIE_OLD)
    s = s[:at] + MOVIE_NEW + s[at + len(MOVIE_OLD):]
    return s if MOVIE_DECL in s else s.replace('#include "a64.h"\n', '#include "a64.h"\n' + MOVIE_DECL, 1)


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
WHOLE_DECL = "extern uint32_t hos_frame_whole(uint64_t at, uint32_t amount_bits);\n"


def whole_new(old, key):
    """old with its cut to whole frames (the one recomp_fp_to_int) made hos_frame_whole's for the counter at key"""
    lines = old.split("\n")
    for i, line in enumerate(lines):
        if "recomp_fp_to_int(c->vreg[0][0]" in line:
            reg = line.split("=", 1)[0]
            lines[i] = "%s=hos_frame_whole(%s, (uint32_t)c->vreg[0][0]);   %s" % (reg, key, WHOLE_MARK)
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


def apply(gen, *, required=False):
    """required: the lift of the supported build (LIFE_BUILD_ID), whole: a missing output-size edit stops it (apply_output_size).
    The other patches only warn when their place is not found once."""
    import oot_native_resolution   # (HOS_SCENE_NATIVE=1: the scene's base scale 1.0; off by default since #849's output resolution)
    oot_native_resolution.apply(gen)
    done = {"dynamic resolution branch": 0, "fixed path's scale": 0, "movie's frame count": 0}
    step_sites, step_files = 0, []   # (the divides with the hook, and the files that have it)
    whole_sites = 0   # (the counters of WHOLE_SITES with the whole frames: patch_whole_frames)
    for path in sorted(glob.glob(os.path.join(glob.escape(gen), "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        out = s
        d = patch_dynres(out)
        if d is not None:
            out = d
            done["dynamic resolution branch"] += 1
            print("oot_patches: dynamic resolution switch in", os.path.basename(path))
        movie = patch_movie_frames(out)
        if movie is not None:
            if MOVIE_NEW in movie:   # (made now, or by an earlier run over this file)
                done["movie's frame count"] += 1
            if movie != out:
                print("oot_patches: the movie's frame count by the frames' time in", os.path.basename(path))
            out = movie
        out, n = fixed_scale(out)
        if n:
            done["fixed path's scale"] += 1
            print("oot_patches: the fixed path's scale in", os.path.basename(path))
        out, sites = patch_frame_seconds(out)
        if STEP_MARK in out:   # (made now, or by an earlier run over this file)
            step_sites += out.count(STEP_MARK)
            step_files.append(path)
        if sites:
            print("oot_patches: the frame's time at %d step divide(s) in %s" % (sites, os.path.basename(path)))
        out, whole = patch_whole_frames(out)
        whole_sites += out.count(WHOLE_MARK)   # (made now, or by an earlier run over this file)
        if whole:
            print("oot_patches: %d frame counter(s) with the step's whole frames in %s" % (whole, os.path.basename(path)))
        if out != s:
            whole_write.write_whole(path, out, newline="")
    for what, n in done.items():
        if n != 1:
            print("oot_patches: WARNING: the %s was found %d times" % (what, n))
    if step_sites != len(STEP_SITES):
        # (both or neither: with one divide timed and the other stepping 1/60 s the two clocks of the game would run apart. Without
        # the hook the host keeps the rate up to the cap, as before: host/nn_vi.c, hos_step_probe)
        for path in step_files:
            with open(path, encoding="utf-8", newline="") as f:
                s = f.read()
            whole_write.write_whole(path, unpatch_frame_seconds(s), newline="")
        print("oot_patches: WARNING: the frame's time was found at %d of %d step divides, none patched (a Frame rate cap above 60 "
              "keeps the game's own step)" % (step_sites, len(STEP_SITES)))
    elif whole_sites + whole_probed(gen) != len(WHOLE_SITES):   # (the frames' own time without them: the pick-up indicator stays above 60 fps)
        print("oot_patches: WARNING: %d of %d frame counters found for the step's whole frames (above 60 fps a counter can stop: "
              "the pick-up indicator)" % (whole_sites, len(WHOLE_SITES)))
    apply_output_size(gen, required=required)


# Output size (host/nn_applet.c hos_output_size: the Insert menu's Output resolution, 2560 x 1440 as shipped):
# - nnMain (f_81222ce0) puts the display size in the framework's create arg (+20/+24, the framework's +156/+160); the window
#   textures are made at it.
# - f_810a7b70 sets up the display-size object (B = [[0x84ca29c0]+216]) from a config (x1): its docked size list (+8, first
#   2560 x 1440) is the frame's base (B+244; the frame is the base times the graphics scale's factor, f_806702e0, capped by the
#   scale's display class: docked 3840 x 2160 at scale 0). Its first entry asks the host when it differs.
OUT_DECL = "uint32_t hos_output_size(int height);\nuint64_t hos_rt_pool_size(uint64_t game);\n"
OUT_EDITS = [
    # (the picture's 2560 x 1440 at 0x81202ee4, not f_831f3200's copy at +0xb68; nx2/oot_wide.py's widescreen size, after it,
    # stays over it: host/hos_main.c makes that one at this height)
    ("    c->x[8] = 0xa00ULL;\n    c->x[8] = (c->x[8] & ~(0xFFFFULL<<32)) | (0x5a0ULL<<32);\n"
     "{ extern volatile uint64_t hos_wide_out; const uint64_t _s = hos_wide_out; if (_s) c->x[8] = _s; }   /* (nx2/oot_wide.py: hos_wide_out) */\n"
     "    { uint64_t _b=c->x[21]; int64_t _o=20; recomp_store64(c,(_b+_o),c->x[8]); }",
     "    c->x[8] = hos_output_size(0) | ((uint64_t)hos_output_size(1) << 32);\n"
     "{ extern volatile uint64_t hos_wide_out; const uint64_t _s = hos_wide_out; if (_s) c->x[8] = _s; }   /* (nx2/oot_wide.py: hos_wide_out) */\n"
     "    { uint64_t _b=c->x[21]; int64_t _o=20; recomp_store64(c,(_b+_o),c->x[8]); }"),
    ("    { uint64_t _a=c->x[0], _b=724ULL; uint64_t _r=_a+_b; c->x[21]=_r; }\n"
     "    { uint64_t _r = ((uint64_t)0 | ((c->x[1]) << 0)); c->x[20] = _r; }\n",
     "    { uint64_t _a=c->x[0], _b=724ULL; uint64_t _r=_a+_b; c->x[21]=_r; }\n"
     "    { uint64_t _r = ((uint64_t)0 | ((c->x[1]) << 0)); c->x[20] = _r; }\n"
     "    if (hos_output_size(0) != 2560 && recomp_load32(c, c->x[20] + 8) == 2560) { recomp_store32(c, c->x[20] + 8, hos_output_size(0)); recomp_store32(c, c->x[20] + 12, hos_output_size(1)); }\n"),
    # - f_80ea2f30: the 3D's largest size, ResolutionParam's BaseResolution (or 1920 x 1080 as constants), 3/4 of 2560 x 1440;
    #   above it the game takes a smaller upscale network (1/3: 1280 x 720 at 4K). Scaled with the output.
    ("c->x[30] = 0x80ea2ffcULL; f_831cca50(c);\n"
     "    { uint64_t _b=c->x[0]; int64_t _o=0; { uint64_t _p0,_p1; recomp_ldp32(c,(_b+_o),&_p0,&_p1); c->x[24]=_p0; c->x[25]=_p1; } }\n",
     "c->x[30] = 0x80ea2ffcULL; f_831cca50(c);\n"
     "    { uint64_t _b=c->x[0]; int64_t _o=0; { uint64_t _p0,_p1; recomp_ldp32(c,(_b+_o),&_p0,&_p1); c->x[24]=_p0; c->x[25]=_p1; } }\n"
     "    if (hos_output_size(0) != 2560) { c->x[24] = (uint32_t)c->x[24] * hos_output_size(0) / 2560; c->x[25] = (uint32_t)c->x[25] * hos_output_size(1) / 1440; }\n"),
    ("    c->x[8] = 0x44f00000ULL;\n    c->x[8] &= 0xFFFFFFFFULL;\n    recomp_store32(c,c->x[19] + 3668,c->x[8]);\n"
     "    c->x[8] = 0x44870000ULL;\n    c->x[8] &= 0xFFFFFFFFULL;\n",
     "    c->x[8] = hos_output_size(0) * 3 / 4;\n    { float _f = (float)c->x[8]; uint32_t _u; memcpy(&_u, &_f, 4); c->x[8] = _u; }\n"
     "    recomp_store32(c,c->x[19] + 3668,c->x[8]);\n"
     "    c->x[8] = hos_output_size(1) * 3 / 4;\n    { float _f = (float)c->x[8]; uint32_t _u; memcpy(&_u, &_f, 4); c->x[8] = _u; }\n"),
    # - The render targets' GPU pool: 0x13100000 bytes (the graphics config's +1464, read in f_836ce9d0, which takes its storage from
    #   the "Graphics" heap and makes a physical pool and a virtual one over it: f_80c4a1c0 -> f_80c49070 -> f_80c49380 -> f_811c9ba0).
    #   Sized for 2560 x 1440: a 3840x2160 3D at the 4K output ran out of it and the next target came back null (crash in f_8005ed00,
    #   the pool's allocator, its +0x28 0x13100000). Scaled with the output's pixels (host/nn_applet.c
    #   hos_rt_pool_size: 687 MB at 3840 x 2160), at the config, so the storage and the pool agree.
    ("L_836cea54:;\n",
     "L_836cea54:;\n"
     "    if (recomp_load64(c, c->x[19] + 1464) == 0x13100000ULL) recomp_store64(c, c->x[19] + 1464, hos_rt_pool_size(0x13100000ULL));\n"),
    # - The "Graphics" heap holds it, made by f_83b7b340 with the other module heaps (twice: 0x34900000 bytes in f_83b7a2c0's first
    #   pass, then the heap kept, 0x348ffe20; it had under 16 MB to spare): grown by as much as the pool.
    ("c->x[30] = 0x83b7b440ULL; f_809df710(c);\n",
     "if (c->x[0] == 0x34900000ULL || c->x[0] == 0x348ffe20ULL) c->x[0] += hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL;\n"
     "c->x[30] = 0x83b7b440ULL; f_809df710(c);\n"),
    # - f_83b7b340 then takes each module heap's use (its vtable +120) off the module's budget (the configured size, a table of slots)
    #   and aborts below zero (f_8351c280): the Graphics heap's use counts as the shipped one.
    ("{ uint64_t _t = c->x[8]; c->x[30] = 0x83b7b4e0ULL; A64_ICALL(c, _t); }\n",
     "{ uint64_t _t = c->x[8]; c->x[30] = 0x83b7b4e0ULL; A64_ICALL(c, _t); }\n"
     "{ const uint64_t _b = recomp_load64(c, c->x[21] + (c->x[22] << 3)), _g = hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL; "
     "if ((_b == 0x34900000ULL || _b == 0x348ffe20ULL) && c->x[0] > _g) c->x[0] -= _g; }\n"),
    # - f_80b72340 (from the graphics init f_836cf4b0) makes a heap of what the Graphics heap can still give, which took the growth as
    #   well (0x1b6cb48 bytes as shipped, 0x1996cb48 grown) and left too little for the pool: it keeps its shipped size.
    ("c->x[30] = 0x80b723acULL; f_809df700(c);\n",
     "if (c->x[0] > hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL) c->x[0] -= hos_rt_pool_size(0x13100000ULL) - 0x13100000ULL;\n"
     "c->x[30] = 0x80b723acULL; f_809df700(c);\n"),
]


def apply_output_size(gen, required=False):
    """required (the supported build's whole lift): each edit must be made, or found made, exactly once, else SystemExit: without
    one the Insert menu's Output resolution is ignored, or a 4K output runs out of the render targets' pool. Elsewhere (another
    build, part of a lift, by hand) a warning"""
    found = [0] * len(OUT_EDITS)
    for path in sorted(glob.glob(os.path.join(glob.escape(gen), "code_*.c"))):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        hit = False
        for i, (old, new) in enumerate(OUT_EDITS):
            # (every place: each made one, and each old text outside a made one; a new text can hold its old one. A file with
            # both is counted for both, and its old ones are left: a second made place or a stray raw one is not once)
            made = s.count(new)
            raw = (s.replace(new, "\0") if made else s).count(old)
            found[i] += made + raw
            if raw and not made:
                s = s.replace(old, new)
                hit = True
        if hit:
            if OUT_DECL not in s:
                s = s.replace('#include "a64.h"\n', '#include "a64.h"\n' + OUT_DECL, 1)
            whole_write.write_whole(path, s, newline="")
            print("oot_patches: output size in", os.path.basename(path))
    if found != [1] * len(OUT_EDITS):
        if required:
            raise SystemExit("oot_patches: the output size places were found %s times, not once each (OUT_EDITS %s): the "
                             "lifted C no longer has the text they match" % (found, [i for i, n in enumerate(found) if n != 1]))
        print("oot_patches: WARNING: the output size places were found %s times" % found)


if __name__ == "__main__":
    apply(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "sa", "gen"))
