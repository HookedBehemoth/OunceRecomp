"""Probes: host code at guest addresses, where the game's code needs the host (docs/lifter.md, "Hooks and probes").

Every probe names a function, one of its instructions and that instruction's word; a word that differs, or an
instruction missing, stops the build (another game version's code). Every probe declares the guest registers it reads
and writes: the host function takes them as plain values (or a typed struct) and returns what it changes, never the
guest context, so the compiler holds the C to its declaration and the lifter knows what each site needs (the values
stay SSA values; the summaries count a row's reads and writes as the instruction's: ROW_EFFECTS).
- Value(func, pc, word, fn, args): the instruction's result register gets fn(args) instead ("xN", "xN+K" for an
  address, "sN" for Sn as a float);
- At(func, pc, word, fn, args, result, before=False, io=(), guest=None, replace=False): fn(args) after the instruction
  (after a BL / BLR when it returns), or before it, or in place of a BL / BLR (replace: the call is not made; x30 is
  set as the call would), its result in a register; io: registers the host function reads and rewrites through a
  struct; guest: the host function calls guest code (declared callees) with every register as it was after.
"""
import re

from . import enc  # (first: its tables import asm)
from . import ir, sem
from .sem_int import getx, setx
from .sem_fp import vread, vwrite


class Value:
    def __init__(self, func, pc, word, fn, args):
        self.func, self.pc, self.word, self.fn, self.args = func, pc, word, fn, args

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

    def effects(self):
        return {_var(a.split("+")[0]) for a in self.args}, set()


# The step's whole frames (host/frame_whole.h): the counters of these two functions advance by the host's whole frames
# (the frame step's FCVTZS of the frame count replaced by hos_frame_whole of the counter's address and the count, a float):
# f_80a87e50, the pick-up indicator above Link's head (issue 1832); f_800672e0, the water renderer's ripples (atk3001's
# fix for Lichii, zortos293/Lichii#1152, as ported to Fritchii)
PROBES = [
    Value(0x80a87e50, 0x80a88300, 0x1e38000c, "hos_frame_whole", ("x19+864", "s0")),   # fcvtzs w12, s0
    Value(0x800672e0, 0x800675f8, 0x1e380009, "hos_frame_whole", ("x21", "s0")),       # fcvtzs w9, s0
]
BY_FUNC = {}
for _p in PROBES:
    BY_FUNC.setdefault(_p.func, []).append(_p)


def find(func, pcs, word):
    """{pc: lowering} of the Value probes of one function; SystemExit when a probe's instruction is not the word it
    names (another game version)"""
    out = {}
    have = set(pcs)
    for p in BY_FUNC.get(func, ()):
        if p.pc not in have or word(p.pc) != p.word:
            raise SystemExit("lift2 probes: Value f_%08x at %#x holds %s, not %08x (another game version?)"
                             % (func, p.pc, "%08x" % word(p.pc) if p.pc in have else "nothing", p.word))
        out[p.pc] = p.lower
    return out


def _var(a):
    """the guest variable (ir.VARS) a register operand names: xN, wN -> "xN"; sN, qN -> "vN"; sp"""
    if a == "sp":
        return "sp"
    return ("x%s" if a[0] in "xw" else "v%s") % a[1:]


# ---- At: a host function of a few registers after an instruction (after a BL / BLR when it returns), its result in a
# register: "at pc: reg := fn(regs...)". The arguments:
#   "xN" (i64), "wN" (i32, the low half), "sN" (Sn, a float), "qN" (Vn whole, a 16-byte vector: hos_q in C), "sp" (i64),
#   "#K" (an i32 constant), "[xN+K]" / "[sp+K]" (the 64-bit guest word there, loaded by the lifted code: a frame slot
#   the host needs is handed over as a value, never read through SP by the host);
# the result: "xN", "wN" (zero-extended, as every W write), "sN" (a float, the rest of Vn zero, as every scalar FP
# write), "qN" (Vn), or None. io: registers ("xN", "wN" as 64-bit slots, "qN" as 16-byte slots, in that order, each
# at its natural alignment) the host function gets as a pointer to a struct (its last argument) holding their values
# and leaves their new values in. guest: the host function calls guest code: its first argument is a GuestCall
# handle (host/hos.h guest_call), and guest is the tuple of the guest functions it calls, or "computed" (a target
# read from guest memory). guest_call restores every register, so the row writes none but its result and io.
# The host function takes plain values and guest addresses, never the context; it may read and write guest memory
# (the call is never moved or removed). Each row names the instruction's word, checked at lift time: a word that
# differs stops the lift (another game version). A result or io register must be caller-saved (x0-x18, v0-v7,
# v16-v31) or written by the instruction itself: a row never changes what the function preserves.
ARG = re.compile(r"[xw]([0-9]|[12][0-9]|30)|[sq]([0-9]|[12][0-9]|3[01])|sp|#(0x[0-9a-f]+|\d+)|"
                 r"\[(x([0-9]|[12][0-9]|30)|sp)\+(0x[0-9a-f]+|\d+)\]")
RESULT = re.compile(r"[xw]([0-9]|[12][0-9]|30)|[sq]([0-9]|[12][0-9]|3[01])")
IO = re.compile(r"[xwq]([0-9]|[12][0-9]|30)|q31")


class At:
    def __init__(self, func, pc, word, fn, args=(), result=None, before=False, io=(), guest=None, replace=False):
        self.func, self.pc, self.word, self.fn, self.args, self.result = func, pc, word, fn, tuple(args), result
        self.before = before   # (before the instruction instead: pc must not be a branch target, so it runs on one edge)
        self.replace = replace   # (in place of the BL / BLR at pc: the caller's patch of a call it makes)
        assert not (before and replace), hex(pc)
        self.io, self.guest = tuple(io), guest
        for a in self.args:
            assert ARG.fullmatch(a), (hex(pc), a)
        assert result is None or RESULT.fullmatch(result), (hex(pc), result)
        for r in self.io:
            assert IO.fullmatch(r), (hex(pc), r)
        assert guest is None or guest == "computed" or (isinstance(guest, tuple) and guest), (hex(pc), guest)

    def effects(self):
        """(the guest variables the row reads, those it writes)"""
        reads = set()
        for a in self.args:
            if a[0] == "#":
                continue
            reads.add(_var(a[1:].split("+")[0] if a[0] == "[" else a))
        reads |= {_var(r) for r in self.io}
        writes = {_var(r) for r in self.io} | ({_var(self.result)} if self.result else set())
        return reads, writes

    def post(self, b):
        vals = []
        for a in self.args:
            kind, n = a[0], a[1:]
            if kind == "#":
                vals.append(b.const(int(n, 0), "i32"))
            elif a == "sp":
                vals.append(b.get("sp"))
            elif kind == "[":
                base, off = n.rstrip("]").split("+")
                v = b.get("sp") if base == "sp" else getx(b, int(base[1:]), 1)
                vals.append(b.op("load", "i64", b.bin("add", v, b.const(int(off, 0))), order="plain"))
            elif kind in "xw":
                vals.append(getx(b, int(n), kind == "x"))
            elif kind == "q":
                vals.append(vread(b, int(n), "v2i64"))
            else:
                vals.append(vread(b, int(n), "f32"))
        ty = {None: "void", "x": "i64", "w": "i32", "s": "f32", "q": "v2i64"}[self.result and self.result[0]]
        guest = self.guest is not None
        if self.io:
            iov = [vread(b, int(r[1:]), "v2i64") if r[0] == "q" else getx(b, int(r[1:]), 1) for r in self.io]
            call = b.op("rowio", ty, *(vals + iov), fn=self.fn, nargs=len(vals), ctx=guest)
            for k, r in enumerate(self.io):
                v = b.op("rowout", "v2i64" if r[0] == "q" else "i64", call, k=k)
                if r[0] == "q":
                    vwrite(b, int(r[1:]), v)
                else:
                    setx(b, int(r[1:]), b.trunc(v, "i32") if r[0] == "w" else v)
            r = call
        else:
            r = b.call(self.fn, ty, *vals, effects=True, ctx=guest)
        if self.result is None:
            return
        n = int(self.result[1:])
        if self.result[0] in "sq":
            vwrite(b, n, r)
        else:
            setx(b, n, r)


def io_layout(io):
    """[(offset, size)] of an io struct's slots: xN / wN 8 bytes, qN 16 bytes, each at its natural alignment"""
    out, off = [], 0
    for r in io:
        size = 16 if r[0] == "q" else 8
        off = (off + size - 1) // size * size
        out.append((off, size))
        off += size
    return out


# the renderer's patches (host/direct_*.c, host/rt_slots.c, host/eng_jobs.c: each says what it does and why)
DIRECT = [
    # depth copies: the copy proc just loaded into x8 pointed at the host's (host/direct_depth_copy.c); #0 and #1 name
    # the two copies that get barriers (the call at 0x80289cc0 and at 0x806675d8), #2 the other calls there
    *[At(0x80289590, pc, 0xf9400108, "direct_depth_copy_retarget", ("x8", "#%d" % site), "x8")
      for pc, site in ((0x80289cbc, 0), (0x80289fe0, 2))],
    *[At(0x80666d20, pc, 0xf9400108, "direct_depth_copy_retarget", ("x8", "#%d" % site), "x8")
      for pc, site in ((0x80667108, 2), (0x806674a8, 2), (0x806675d4, 1), (0x80667a14, 2), (0x80667a8c, 2))],
    # job sync (host/direct_job_sync.c): the wait / barrier procs pointed at the host's
    *[At(f, pc, 0xf9400108, "direct_job_sync_retarget", ("x8",), "x8")
      for f, pcs in ((0x82c02f20, (0x82c02f94, 0x82c03014, 0x82c03024, 0x82c030bc)),
                     (0x82c03170, (0x82c031e4, 0x82c03264, 0x82c03274, 0x82c0330c))) for pc in pcs],
    At(0x831fa4a0, 0x831fa57c, 0xf9400104, "direct_job_sync_retarget", ("x4",), "x4"),   # ldr x4, [x8]
    # f_806688c0's CallCommands, with the job's view record (x21) and draw context ([sp + 0x10]) handed over
    *[At(0x806688c0, pc, 0xf9400108, "direct_job_sync_call", ("x8", "x21", "[sp+0x10]"), "x8")
      for pc in (0x80668c7c, 0x80668dd8, 0x80668fb0)],
    # mesh lists (host/direct_mesh_lists.c): after each `str xzr, [x19, x8]` / `str xzr, [x22, x8]`
    *[At(0x80361c80, pc, 0xf8286a7f, "direct_mesh_lists_reset", ("x19", "x8"))
      for pc in (0x80361d5c, 0x80362048, 0x803620b0, 0x80362118, 0x80362180, 0x803621e8, 0x80362848, 0x803628b0,
                 0x80362918, 0x80362b80, 0x80362be8, 0x80362c50)],
    At(0x80361a00, 0x80361b3c, 0xf8286adf, "direct_mesh_lists_reset", ("x22", "x8")),
    # mesh args (host/direct_mesh_args.c): after `stp xzr, xzr, [x8, #32]`
    At(0x80362ff0, 0x80363294, 0xa9027d1f, "direct_mesh_args_header", ("x19", "x8")),
    # sky LUT (host/direct_sky_lut.c): x0 the storage, x1 the part, x2 its size; x2 0 when uploaded
    *[At(0x80846070, pc, 0x9a8803e0, "direct_sky_lut_part", ("x0", "x1", "x2"), "x2")
      for pc in (0x808460e4, 0x80846114, 0x80846144)],
    # terrain tiles (host/direct_assets.c): x0 the storage, x1 the blob, x2 its size, x11 the record; x2 0 when uploaded
    At(0x80c17040, 0x80c17580, 0x9a8803e1, "direct_assets_tile", ("x0", "x1", "x2", "x11"), "x2"),
    # render-target slots (host/rt_slots.c): x23 block, x10 offset, x8 pool, the alignment at [sp + 192]; x10 the slot
    *[At(0x8005ed00, pc, 0xf9001eea, "rt_slots_offset", ("x8", "x10", "x23", "sp", "[sp+192]"), "x10")
      for pc in (0x8005fb74, 0x80061814)],
    # system texture ids (host/direct_rt_ids.c): f_80667d20's bl f_800634b0 (entry, view, 0) replaced: the call is skipped
    # while the entry, its view and texture are unchanged, else made through guest_call
    At(0x80667d20, 0x806680d8, 0x97e7ecf6, "direct_rt_ids_reregister", ("sp", "x0", "x1", "x2"), replace=True,
       guest=(0x800634b0,)),
    # the texture data pool (host/direct_assets.c): agl's pool wrapper f_80f6c9d0 names the pool its blr makes
    At(0x80f6c9d0, 0x80f6cb18, 0xf9400108, "direct_assets_agl_pool"),                  # ldr x8, [x8]
    # the local shadow map's alloc (host/rt_slots.c): f_802640a0 names it just before its bl f_8005ed00
    At(0x802640a0, 0x80264128, 0xb9003be8, "rt_slots_local_shadow"),                   # str w8, [sp, #0x38]
    # mesh system (host/direct_mesh_system.c): after the job prologue's SetDepthRange (blr x8), the system binds
    At(0x80084de0, 0x800855a8, 0xd63f0100, "direct_mesh_system_bind", ("sp",), guest=(0x801060f0, 0x80100210)),
    # layer ahead (host/direct_layer_ahead.c): the job loop, on the TBZ's fall-through (before the CBNZ there); the
    # scheduler record at [sp + 0x48]; runs a layer job (its vtable's +0x10) ahead
    At(0x8007fe60, 0x800809d0, 0xb5004ee8, "direct_layer_ahead_run", ("sp", "[sp+0x48]"), before=True,
       guest="computed"),
    # inline passes (host/direct_passes.c): before the two calls of f_80681320 that draw a recorded pass inline: f_806688c0
    # (x6 the key + 0xc0, x1 the list's draw context), f_8028a2a0 (x2 a copy of the key, x1 the draw context)
    At(0x806688c0, 0x80668d7c, 0xf90483e8, "direct_passes_inline_job", ("x6", "x1")),    # str x8, [sp, #0x900]
    At(0x8028a2a0, 0x8028a584, 0xaa1303e1, "direct_passes_inline_key", ("x2", "x1")),    # mov x1, x19
    # the job's view record (x25) for BeginRecording's list (host/eng_jobs.c), before its blr x8
    At(0x80084de0, 0x80085334, 0xf9400108, "eng_jobs_view", ("x25",)),                   # ldr x8, [x8]
    # the kind of list each recording begins (host/direct.c hsd_classify): after the ldr of nvnCommandBufferBeginRecording
    # just before its blr, the function that records names its list
    *[At(f, pc, w, "direct_list_kind", ("#%d" % kind,)) for f, pc, w, kind in (
        (0x804581b0, 0x804582a4, 0xf9400108, 1), (0x80293890, 0x80293b0c, 0xf9400108, 2),
        (0x806002e0, 0x806006d0, 0xf9400108, 3), (0x80445020, 0x8044530c, 0xf9400108, 4),
        (0x8044e8e0, 0x8044ea18, 0xf9400108, 5), (0x80293190, 0x80293458, 0xf9400108, 6),
        (0x80084de0, 0x80085334, 0xf9400108, 16), (0x8006cd60, 0x8006d250, 0xf9400108, 17),
        (0x8007ad40, 0x8007b0c0, 0xf9400108, 17), (0x8007b520, 0x8007b9f8, 0xf9400108, 17),
        (0x802eb390, 0x802eb3f4, 0xf94002e8, 18), (0x806650d0, 0x80665578, 0xf9400108, 19),
        (0x80665980, 0x80665ff4, 0xf9400108, 20))],
]

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
    # widescreen (nx2/oot_wide.py: where they come from; after the output size at 0x81222ee4)
    At(0x81222ce0, 0x81222ee4, 0xf2c0b408, "hos_oot_wide_out", ("x8",), "x8"),     # movk x8, #1440, lsl #32: {2560, 1440}
    At(0x83213890, 0x832139f8, 0xf2c0b408, "hos_oot_wide_out", ("x8",), "x8"),     # likewise, the display settings
    At(0x83213890, 0x83213a04, 0xf2c08708, "hos_oot_wide_scene", ("x8",), "x8"),   # movk x8, #1080, lsl #32: {1920, 1080}
    At(0x804e48c0, 0x804e5334, 0xbd414260, "hos_oot_wide_ratio", ("s0",), "s0"),   # ldr s0, [x19, #0x140]: the camera's ratio
    At(0x834a5cc0, 0x834a5cc0, 0x2d558400, "hos_oot_wide_projection_ratio", ("s1", "x0"), "s1"),   # ldp s0, s1, [x0, #0xac]
    At(0x8006ec60, 0x8006f1e8, 0x2d558ea0, "hos_oot_wide_projection_ratio", ("s3", "x21"), "s3"),  # (its inlined copies)
    At(0x8025b910, 0x8025b9a8, 0x2d558c00, "hos_oot_wide_projection_ratio", ("s3", "x0"), "s3"),
    At(0x8035a8c0, 0x8035b82c, 0x2d558440, "hos_oot_wide_projection_ratio", ("s1", "x2"), "s1"),
    At(0x83b7c710, 0x83b7c758, 0xa941a269, "hos_oot_wide_movie", ("x29",)),         # ldp x9, x8, [x19, #0x18]
    # the HUD's projection rows (ldp q1, q4, [x2]; x0 the canvas) and the save picture's viewport (ldr x8, [x8] before
    # blr x8: SetViewport(w1, w2, w3, w4)), nx2/oot_wide.py
    At(0x80794330, 0x80794368, 0xad401041, "hos_oot_wide_hud", ("x0", "q1", "q4"), "q1"),
    At(0x802a46e0, 0x802a4824, 0xf9400108, "hos_oot_wide_save_picture", (), io=("w1", "w2", "w3", "w4")),
    # the frame body's frame-end clock read: GetSystemTick replaced by one that also notes x19, the graphics module, for
    # nn_vi.c's real-time step (host/hos_thread.c)
    At(0x8348c680, 0x8348c76c, 0x941f5619, "hos_oot_frame_end_tick", ("x19",), "x0", replace=True),
    # the calc jobs runner's three SleepThread(20 us) polls of its sub-stages: a timed sleep, not a spin (host/hos_thread.c
    # calc_poll_nap); and the present's event poll (HOS_PRESENT_POLL, off by default: hos_oot_present_sleep)
    *[At(0x800998a0, pc, w, "hos_oot_calc_sleep", ("x0", "#%#x" % (pc + 4)), replace=True)
      for pc, w in ((0x8009a474, 0x94ef225b), (0x8009afec, 0x94ef1f7d), (0x8009b230, 0x94ef1eec))],
    At(0x805902c0, 0x8059051c, 0x94db4a31, "hos_oot_present_sleep", ("x0", "x22", "x23", "x24"), replace=True),
    # occlusion off (cLohan's community patch, as Fritchii ships it): the byte that decides the queries (ldrb w8, [x0])
] + [At(f, pc, 0x39400008, "hos_oot_occlusion_flag", ("w8",), "w8")
     for f, pc in ((0x811cc790, 0x811cce68), (0x811cc790, 0x811d828c), (0x811cc790, 0x811d82a4),
                   (0x811cc790, 0x811ccec8), (0x811cc790, 0x811d82dc), (0x811cc790, 0x811d82f4))] + [
    # per-core rings of the actor singleton (host/oot_probes.c hos_oot_ring_write): after each store of a ring's count,
    # the ring's address in the register the store used; #0 a push (count + 1), #1 a pop (count - 1)
    At(f, pc, w, "hos_oot_ring_write", (reg, "#%d" % pop, "#%#x" % pc)) for f, pc, w, reg, pop in (
        (0x8004ab40, 0x8004c820, 0xb9002128, "x9", 0), (0x800aff80, 0x800b0b1c, 0xb9002128, "x9", 0),
        (0x800b3ce0, 0x800b4228, 0xb900212b, "x9", 0), (0x800b73d0, 0x800b7630, 0xb9002128, "x9", 0),
        (0x8073a090, 0x8073a17c, 0xb900212b, "x9", 0), (0x83af6150, 0x83af6328, 0xb9002128, "x9", 0),
        (0x83af59c0, 0x83af5bb4, 0xb9002128, "x9", 0),
        (0x8004ab40, 0x8004c988, 0xb9002109, "x8", 1), (0x800aff80, 0x800b0ca0, 0xb9002109, "x8", 1),
        (0x800b3ce0, 0x800b4468, 0xb900210a, "x8", 1), (0x800b73d0, 0x800b787c, 0xb900210a, "x8", 1),
        (0x8073c900, 0x8073c9d0, 0xb900210a, "x8", 1), (0x83af6150, 0x83af6724, 0xb900210a, "x8", 1),
        (0x83af59c0, 0x83af5d9c, 0xb900210a, "x8", 1))] + DIRECT
AT_FUNCS = {p.func for p in AT}

# the registers a row may write without changing what its function preserves (and those the instruction writes)
CALLER_SAVED = {"x%d" % i for i in range(19)} | {"v%d" % i for i in list(range(8)) + list(range(16, 32))}


def _written(w):
    """the guest variables the instruction word writes (its template's writes)"""
    i = enc.decode(w)
    t = i.entry.template
    if t == "b" and i.f["op"] or t == "blr":
        return {"x30"} | CALLER_SAVED
    if t in ("b", "bcond", "cbz", "tbz", "ret", "br", "udf", "exc"):
        return set()
    fn = ir.Function(0)
    b = ir.Builder(fn)
    b.at(fn.block(0))
    sem.TEMPLATES[t](b, i.f, 0x1000, i.entry.params)
    return set(fn.written)


def row_effects():
    """{pc: (reads, writes, before)} of every Value probe and At row: the summaries count them as the instruction's
    (nx2/lift2/summary.py Func)"""
    out = {}
    for p in PROBES:
        r, w = p.effects()
        out.setdefault(p.pc, []).append((r, w, False))
    for p in AT:
        r, w = p.effects()
        out.setdefault(p.pc, []).append((r, w, p.before))
    return out


def at(func, pcs, word, before=False, replace=False):
    """{pc: [fn(builder)]} of a function's At probes, for Program.after (before: the ones for Program.before; replace:
    those for Program.replace); SystemExit when a word differs, when a before probe's instruction is a branch target,
    when a replacing row's instruction is not a BL or BLR, or when a row writes a callee-saved register its instruction
    does not"""
    out = {}
    have = set(pcs)
    for p in AT:
        if p.func != func or p.before != before or p.replace != replace:
            continue
        if replace and p.pc in have and not is_call(word(p.pc)):
            raise SystemExit("lift2 probes: At f_%08x replacing %#x: not a BL or BLR" % (func, p.pc))
        if before:
            from .build_ir import target
            for pc in pcs:
                i = enc.decode(word(pc))
                if i is not None and target(i, pc) == p.pc:
                    raise SystemExit("lift2 probes: At f_%08x before %#x: a branch target (from %#x)" % (func, p.pc, pc))
        if p.pc not in have or word(p.pc) != p.word:
            raise SystemExit("lift2 probes: At f_%08x at %#x holds %s, not %08x (another game version?)"
                             % (func, p.pc, "%08x" % word(p.pc) if p.pc in have else "nothing", p.word))
        bad = p.effects()[1] - CALLER_SAVED - (set() if before else _written(p.word))
        if bad:
            raise SystemExit("lift2 probes: At f_%08x at %#x writes %s: callee-saved, not written by the instruction"
                             % (func, p.pc, ", ".join(sorted(bad))))
        out.setdefault(p.pc, []).append(p.post)
    return out


def is_call(w):
    """a BL or BLR: what comes after it runs when the call returns"""
    i = enc.decode(w)
    return i is not None and (i.entry.template == "blr" or i.entry.template == "b" and i.f["op"] == 1)


def merge(*posts):
    """Program.after from several {pc: fn(builder)} (or {pc: [fn]}), in the order given"""
    out = {}
    for d in posts:
        for pc, fn in d.items():
            out.setdefault(pc, []).extend(fn if isinstance(fn, list) else [fn])
    return out
