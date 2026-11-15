"""The lift: image (we are rtld) -> function discovery -> the plan the build lifts (nx2/lift2/game.py).

Usage:  python -I nx2/lift.py [--exefs <dir>] [--work <dir>] [--write] [--max-func N] [--layout eden|a64] [--aslr 0]
  --layout eden (default): Eglantine's addresses, rtld included, from 0x80000000 + --aslr (image.py);
  --layout a64: a64-kit's own layout from 0x08000000 (its native runtime, below 4 GB)
  Always: builds <work>/image (image.bin, image.json), discovers every function of rtld-less main + subsdk* + sdk
          (nx2/lift2/discover.py) and writes the plan (<work>/plan.pickle: every function's instructions and the
          program's facts).
  --write: also writes <work>/gen: the runtime's tables (a64_table.c, a64_hooks.c) and functions.txt.
  --standalone: the standalone build's lift (<work>/sa): the SDK's OS-facing functions hooked to the native runtime
           in host/ or to generated stubs, plus hos_image.c, a64_stubs.c and nvn2_exports.c (nx2/standalone.py).
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import kit
import nso2  # noqa: F401  (DT_RELR)
import standalone
import adr_entries  # noqa: E402
import nso
import nx_image
import image as eden_image


def module_of(img, pc):
    for lo, hi, name in img.code:
        if lo <= pc < hi:
            return name
    return "?"


def main():
    kit.wait_for_quiet('the lift')
    p = argparse.ArgumentParser()
    p.add_argument("--exefs", default=kit.EXEFS)
    p.add_argument("--work", default=kit.WORK)
    p.add_argument("--write", action="store_true")
    p.add_argument("--max-func", type=int, default=0)
    p.add_argument("--layout", choices=("eden", "a64"), default="eden")
    p.add_argument("--aslr", type=lambda v: int(v, 0), default=0)
    p.add_argument("--standalone", action="store_true", help="the standalone build's lift (nx2/standalone.py)")
    p.add_argument("--entries", help="extra entry addresses, one hex address a line (e.g. the misses ArmRecomp logged)")
    p.add_argument("--no-adr-entries", action="store_true",
                   help="no extra entries at the code addresses ADR / ADRP+ADD take (nx2/adr_entries.py)")
    a = p.parse_args()
    a.exefs = a.exefs or kit.need_exefs()
    t0 = time.time()
    hle = ()
    if a.standalone:
        hle, stubbed = standalone.hle_set(nso.load_exefs(a.exefs))
        print("standalone: %d HLE functions in host/, %d OS-facing imports stubbed" % (len(hle), len(stubbed)))
        hle = hle | stubbed
    img = eden_image.EdenImage(a.exefs, a.aslr, hle=hle) if a.layout == "eden" else nx_image.NxImage(a.exefs)
    out_dir = os.path.join(a.work, "sa") if a.standalone else a.work
    if a.write:   # (incomplete until the last step: a killed lift is never built from, nx2/lift_mark.py)
        import lift_mark
        lift_mark.begin(out_dir)
    img_dir = os.path.join(out_dir, "image")
    meta = img.write(img_dir)
    print("image: %s; %d entries, %d trampolines (%d unresolved non-weak imports) (%.0fs)" % (
        ", ".join("%s@%#x+%#x" % (m["name"], m["base"], m["size"]) for m in meta["modules"]),
        len(img.entries), len(img.tramp), len(img.unresolved), time.time() - t0))
    if img.unresolved:
        print("  unresolved: " + ", ".join(sorted(img.unresolved)[:16]))
    if a.entries:
        extra = {int(l.split()[0], 16) for l in open(a.entries) if l.strip() and not l.startswith("#")}
        new = {e for e in extra if img.in_code(e)} - img.entries
        img.entries |= new
        print("extra entries: %d of %d listed are new code addresses" % (len(new), len(extra)))
    if not a.no_adr_entries:   # (hookshot) callbacks reached only through a pointer the code builds
        spans = adr_entries.fde_spans(img.modules)
        refs = adr_entries.code_address_refs(open(os.path.join(img_dir, "image.bin"), "rb").read(), meta, spans=spans)
        new = {e for e in refs if img.in_code(e)} - img.entries
        img.entries |= new
        print("ADR / ADRP+ADD code addresses outside the %d unwind-covered functions: %d, %d of them new entries" % (
            len(spans), len(refs), len(new)))
    from lift2 import discover, game, tables
    L = discover.Discovery(img)
    if a.standalone:   # (hookshot) widescreen and ultrawide: C after a few of the game's instructions
        import oot_wide
        oot_wide.install(L, img)
        import oot_jitter   # (the temporal upscalers' camera jitter, DLSS's and FSR 3.1's: none unless one runs, host/rx_backend.c)
        oot_jitter.install(L, img)
        import oot_occlusion   # (no occlusion queries when HOOKSHOT_OCCLUSION_OFF=1: Fritchii's Occlusion Off)
        oot_occlusion.install(L, img)
    L.discover(max_insns=a.max_func)
    print("discovered %d functions (%.0fs)" % (len(L.funcs), time.time() - t0))
    jt = L.stats
    print("jump tables %d (bounded %d, bounded by function %d, ADR table %d), BR without a table %d, PLT stubs %d" % (
        len(L.jt), jt["jt_bounded"], jt["jt_unbounded"], jt["jt_adr_table"], len(L.jt_missing), len(L.plt)))
    if jt["jt_table_reg_reused"]:
        print("jump tables whose table register is reused before the br: %d" % jt["jt_table_reg_reused"])
    for pc, reg in sorted(L.jt_missing.items()):
        print("  BR without a table: %#x (x%d)" % (pc, reg))
    with open(os.path.join(img_dir, "functions.json"), "w") as f:
        json.dump({"%x" % e: [len(b), module_of(img, e)] for e, (b, _) in L.funcs.items()}, f)
    game.write_plan(L, img, os.path.join(out_dir, "plan.pickle"), game.hot_list(a.work))
    if a.write:
        gen = os.path.join(out_dir, "gen")
        os.makedirs(gen, exist_ok=True)
        for f in os.listdir(gen):   # (C an earlier lift wrote that this one does not)
            if f not in ("a64_table.c", "a64_hooks.c", "a64_stubs.c", "hos_image.c", "nvn2_exports.c", "functions.txt"):
                p = os.path.join(gen, f)
                if os.path.isfile(p):
                    os.remove(p)
        tables.write(gen, L.funcs, L.hooks)
        with open(os.path.join(gen, "functions.txt"), "w") as f:
            f.write("".join("%08x %d\n" % (e, len(L.funcs[e][0])) for e in sorted(L.funcs)))
        if a.standalone:
            start, exit_ = standalone.write_image_c(gen, img, meta, a.exefs)
            print("standalone: hos_image.c (thread hooks %#x, %#x), %d import stubs" % (
                start, exit_, standalone.gen_stubs(gen, meta)))
            nvn = standalone.nvn_exports(nso.load_exefs(a.exefs))
            standalone.write_nvn_exports(gen, nvn)
            print("standalone: nvn2_exports.c (%d sdk NVN functions routed to host/nvn2.c)" % len(nvn))
        print("wrote the tables and the image's C to %s (%.0fs)" % (gen, time.time() - t0))
        lift_mark.done(out_dir)
    print("done (%.0fs)" % (time.time() - t0))


if __name__ == "__main__":
    main()
