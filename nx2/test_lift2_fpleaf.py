"""Lifter 2 on the game's FP and NEON leaf functions: lifted by nx2/lift2 (host FP mode) and by the present lifter (exact
FP), run natively from the same random and edge-value states, and compared with Unicorn and with each other.

    python -I nx2/test_lift2_fpleaf.py [--count 600] [--states 6] [--seed 1] [--list] [--keep]

As nx2/test_lift2_leaf.py, with:
- the draw: leaves lift2 takes that contain at least one FP, NEON or vector load/store instruction;
- the states: V0-V31 lanes from an edge-value pool (signed zeros, infinities, quiet and signalling NaNs, subnormals,
  the smallest normal, values at rounding ties, integer saturation edges) and random values; FPCR 0, FZ, or FZ with
  a rounding mode;
- lift2's code runs with MXCSR set from the case's FPCR (FZ -> DAZ|FTZ, RMode -> RC), as the runtime will set it, and
  with a64_fpcr_read/write and a64_fpsr_read/write as the runtime milestone must provide them;
- compared: X0-X30, SP, NZCV, V0-V31 and the changed pages' contents. A difference is put in an allowed-deviation
  class (docs/lifter2-design.md section 7.1) when every differing lane is explained by it, and the classes are
  counted; a difference no class explains is a failure.
"""
import argparse
import collections
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_leaf as TL   # noqa: E402
import test_lift2_leaf as L2   # noqa: E402
from lift2 import build_ir, enc, emit_llvm, llvm   # noqa: E402
import kit   # noqa: E402
import image as eden_image   # noqa: E402
import a64_lift   # noqa: E402
import a64_rtgen   # noqa: E402
import rtgen   # noqa: E402
from unicorn import arm64_const as A   # noqa: E402

FP_TABLES = ("enc_fp", "enc_neon")
PAGE_HASH = TL.page_hash   # (Unicorn's pages are kept whole below; this hashes them where the harness kept a hash)


def fp_entries():
    names = set()
    for m in FP_TABLES:
        try:
            names |= {e[0] for e in __import__("lift2." + m, fromlist=["x"]).ENCODINGS}
        except ImportError:
            pass
    return names


HELPERS = r"""
#include <immintrin.h>
static unsigned mxcsr_of(uint64_t fpcr) {
    unsigned m = 0x1F80, rm = fpcr >> 22 & 3;
    if (fpcr >> 24 & 1) m |= 0x8040;
    return m | (rm == 1 ? 2u : rm == 2 ? 1u : rm) << 13;
}
uint64_t a64_fpcr_read(Ctx *c) { return c->fpcr; }
void a64_fpcr_write(Ctx *c, uint64_t v) { c->fpcr = v; _mm_setcsr(mxcsr_of(v)); }
uint64_t a64_fpsr_read(Ctx *c) {
    unsigned m = _mm_getcsr(); uint64_t f = c->fpsr;
    f |= (m & 1 ? 1 : 0) | (m & 4 ? 2 : 0) | (m & 8 ? 4 : 0) | (m & 16 ? 8 : 0) | (m & 32 ? 16 : 0);
    if ((m & 2) && (c->fpcr >> 24 & 1)) f |= 128;
    return f;
}
void a64_fpsr_write(Ctx *c, uint64_t v) { c->fpsr = v; _mm_setcsr(_mm_getcsr() & ~0x3Fu); }
"""


def harness():
    h = L2.HARNESS
    h = h.replace("static const A64Fn T_OLD[]", HELPERS + "static const A64Fn T_OLD[]")
    h = h.replace("int fault = run(which ? T_NEW[idx] : T_OLD[idx], &c);",
                  "if (which) _mm_setcsr(mxcsr_of(c.fpcr));\n"
                  "            int fault = run(which ? T_NEW[idx] : T_OLD[idx], &c);\n"
                  "            _mm_setcsr(0x1F80);")
    # the changed pages' contents (up to 64), for lane-wise classification
    h = h.replace("uint32_t ndiff = 0; static uint64_t diff[2 * 8192];",
                  "uint32_t ndiff = 0; static uint64_t diff[2 * 8192]; static uint8_t pg[64][4096];")
    h = h.replace("if (ndiff < 8192) { diff[2 * ndiff] = a; diff[2 * ndiff + 1] = page_hash(A64_PTR(a)); }",
                  "if (ndiff < 8192) { diff[2 * ndiff] = a; diff[2 * ndiff + 1] = page_hash(A64_PTR(a)); }\n"
                  "                        if (ndiff < 64) memcpy(pg[ndiff], A64_PTR(a), 4096);")
    h = h.replace("fwrite(&ndiff, 4, 1, out); fwrite(diff, 16, ndiff < 8192 ? ndiff : 8192, out);",
                  "fwrite(&ndiff, 4, 1, out); fwrite(diff, 16, ndiff < 8192 ? ndiff : 8192, out);\n"
                  "            fwrite(pg, 4096, ndiff < 64 ? ndiff : 64, out);")
    assert h.count("mxcsr_of(c.fpcr)") == 1 and "pg[ndiff]" in h and "fwrite(pg" in h
    return h


# ---- states
F32_EDGE = [0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FC12345, 0x7FA00000,
            0x00000001, 0x80000001, 0x007FFFFF, 0x00800000, 0x80800000, 0x00400000, 0x3F800000, 0xBF800000,
            0x3F000000, 0x4B000000, 0x4AFFFFFF, 0x3FC00000, 0x40200000, 0x4F000000, 0xCF000000, 0x5F000000,
            0xDF000000, 0x7F7FFFFF, 0xFF7FFFFF, 0x4EFFFFFF, 0x4F800000, 0x3EAAAAAB, 0x0C000000, 0x1F800000]
F64_EDGE = [0, 1 << 63, 0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000,
            0x7FF4000000000000, 1, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x3FF0000000000000, 0xBFF0000000000000,
            0x3FE0000000000000, 0x4330000000000000, 0x41E0000000000000, 0xC1E0000000000000, 0x43E0000000000000,
            0xC3E0000000000000, 0x7FEFFFFFFFFFFFFF, 0x3FF8000000000000, 0x4004000000000000, 0x41DFFFFFFFC00000]


def rand_lane32(rng):
    r = rng.random()
    if r < 0.3:
        return rng.choice(F32_EDGE)
    if r < 0.75:
        return struct.unpack("<I", struct.pack("<f", rng.choice([rng.uniform(-100, 100), rng.uniform(-2, 2),
                                                                rng.uniform(-1e6, 1e6)])))[0]
    if r < 0.85:
        return rng.choice([0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7FFF, 0x8000, 0xFF, 0x80, rng.randrange(256)])
    return rng.getrandbits(32)


def rand_v(rng):
    v = []
    for _ in range(32):
        r = rng.random()
        if r < 0.6:
            lanes = [rand_lane32(rng) for _ in range(4)]
            v += [lanes[0] | lanes[1] << 32, lanes[2] | lanes[3] << 32]
        elif r < 0.8:
            v += [rng.choice(F64_EDGE) if rng.random() < 0.4 else
                  struct.unpack("<Q", struct.pack("<d", rng.uniform(-1e3, 1e3)))[0] for _ in range(2)]
        else:
            v += [rng.getrandbits(64), rng.getrandbits(64)]
    return v


def rand_state(rng):
    x, nzcv, fpcr, fpsr, v, tpidr, tpidrro = TL.rand_state(rng)
    r = rng.random()
    fpcr = 0 if r < 0.6 else (1 << 24) if r < 0.85 else (rng.randrange(4) << 22) | (rng.randrange(2) << 24)
    return x, nzcv, fpcr, fpsr, rand_v(rng), tpidr, tpidrro


# ---- deviation classes
def nan32(x):
    return x & 0x7F800000 == 0x7F800000 and x & 0x7FFFFF


def nan64(x):
    return x & 0x7FF0000000000000 == 0x7FF0000000000000 and x & 0xFFFFFFFFFFFFF


def nan16(x):
    return x & 0x7C00 == 0x7C00 and x & 0x3FF


def classify64(a, b, fz):
    """the allowed-deviation classes that explain a != b (two 64-bit chunks of a register or memory), or None"""
    if a == b:
        return set()
    if nan64(a) and nan64(b):
        return {"nan"}
    out = set()
    for sh in (0, 32):
        x, y = a >> sh & 0xFFFFFFFF, b >> sh & 0xFFFFFFFF
        if x == y:
            continue
        if nan32(x) and nan32(y):
            out.add("nan")
            continue
        if fz and {x & 0x7FFFFFFF, y & 0x7FFFFFFF} == {0, 0x00800000} and x >> 31 == y >> 31:
            out.add("fz-boundary")
            continue
        h = [(x >> s & 0xFFFF, y >> s & 0xFFFF) for s in (0, 16)]
        if all(p == q or (nan16(p) and nan16(q)) for p, q in h):
            out.add("nan")
            continue
        if fz and a & (1 << 63) == b & (1 << 63) and {a & ~(1 << 63), b & ~(1 << 63)} == {0, 0x0010000000000000}:
            return {"fz-boundary"}
        if fz and x >> 31 == y >> 31 and {x & 0x7F800000, y & 0x7F800000} == {0} and 0 in (x & 0x7FFFFFFF,
                                                                                      y & 0x7FFFFFFF):
            out.add("fz-minmax")   # (a subnormal passed through unflushed where ARM gives the signed zero)
            continue
        return None
    return out


def classify_bytes(a, b, fz):
    out = set()
    for k in range(0, len(a), 8):
        x, y = struct.unpack_from("<Q", a, k)[0], struct.unpack_from("<Q", b, k)[0]
        c = classify64(x, y, fz)
        if c is None:
            return None
        out |= c
    return out


def compare(got, ref, fz, nzcv_ref=None, old=False):
    """(classes, failures) of one case: got and ref are (x[32], nzcv or None, v[64], pages {addr: bytes})"""
    gx, gnz, gv, gp = got
    rx, rnz, rv, rp = ref
    classes, fails = collections.Counter(), []
    for r in range(32):
        if gx[r] != rx[r]:
            c = classify64(gx[r], rx[r], fz)
            if c is None:
                fails.append("%s %#x != %#x" % ("sp" if r == 31 else "x%d" % r, gx[r], rx[r]))
            else:
                classes.update(c)
    for r in range(32):
        for h in range(2):
            a, b = gv[2 * r + h], rv[2 * r + h]
            if a != b:
                c = classify64(a, b, fz)
                if c is None:
                    fails.append("v%d %032x != %032x" % (r, gv[2 * r + 1] << 64 | gv[2 * r], rv[2 * r + 1] << 64 | rv[2 * r]))
                    break
                classes.update(c)
    if gnz is not None and rnz is not None and list(gnz) != list(rnz):
        fails.append("nzcv %s != %s" % (gnz, rnz))
    if set(gp) != set(rp):
        fails.append("pages %s" % sorted("%x" % p for p in set(gp) ^ set(rp))[:4])
    else:
        for p in gp:
            if gp[p] != rp[p]:
                if isinstance(gp[p], int) or isinstance(rp[p], int):   # (past the 64 pages kept: hashes)
                    h = [x if isinstance(x, int) else PAGE_HASH(x) for x in (gp[p], rp[p])]
                    if h[0] != h[1]:
                        fails.append("page %x differs (hash; contents not kept)" % p)
                    continue
                c = classify_bytes(gp[p], rp[p], fz)
                if c is None:
                    k = next(i for i in range(0, 4096, 8) if gp[p][i:i + 8] != rp[p][i:i + 8])
                    fails.append("memory %x: %s != %s" % (p + k, gp[p][k:k + 8].hex(), rp[p][k:k + 8].hex()))
                else:
                    classes.update(c)
    return classes, fails


def has_fp(img, body, fp_names):
    for pc in body:
        i = enc.decode(img.word(pc))
        if i is not None and i.entry.name in fp_names:
            return True
    return False


def main():
    kit.wait_for_quiet("the lift2 FP function test")
    p = argparse.ArgumentParser()
    p.add_argument("--count", type=int, default=600)
    p.add_argument("--states", type=int, default=6)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--list", action="store_true")
    p.add_argument("--keep", action="store_true")
    p.add_argument("--only", default=None, help="comma-separated roots (hex) instead of a draw")
    p.add_argument("--calls", action="store_true", help="roots with call trees (test_lift2_leaf.py --calls)")
    p.add_argument("--max-tree", type=int, default=4000)
    a = p.parse_args()
    clang = llvm.check("clang")
    rng = random.Random(a.seed)
    t0 = time.time()
    img = eden_image.EdenImage(kit.need_exefs())
    meta = img.write(os.path.join(kit.WORK, "image_test"))
    P = TL.Picker(img)
    fp_names = fp_entries()
    if a.only:
        lifted = set()
        prog = L2.program(P, lifted) if a.calls else build_ir.LEAF
        roots, funcs, why = [], set(), collections.Counter()
        for s_ in a.only.split(","):
            e = int(s_, 16)
            P.liftable_body(e, a.calls)
            tree = P.tree(e, a.calls, a.max_tree)
            if tree is None:
                raise SystemExit("f_%08x: the present lifter cannot lift its tree here" % e)
            for f_ in tree:
                r = build_ir.scope(f_, sorted(P.L.funcs[f_][0]), img.word, prog)
                if r is None:
                    lifted.add(f_)
                elif f_ == e:
                    raise SystemExit("f_%08x: lift2 passes it over: %s" % (e, r))
            roots.append(("?", e, sum(len(P.L.funcs[f_][0]) for f_ in tree)))
            funcs |= tree
    else:
        # (the integer side's draw, oversampled, then the roots whose tree has FP/NEON instructions kept)
        share = TL.module_shares(img)
        roots, funcs, lifted, why, prog = L2.select(P, img, {m: max(1, int(3 * a.count * s)) for m, s in share.items()},
                                                    rng, a.calls, a.max_tree)
        keep = [r for r in roots if has_fp(img, P.L.funcs[r[1]][0], fp_names)][:a.count]
        why["no FP/NEON in the root"] += len(roots) - len(keep)
        roots = keep
        funcs = set().union(*(P.tree(e, a.calls, a.max_tree if a.calls else 600) for _, e, _ in roots))
        lifted = {f_ for f_ in lifted if f_ in funcs}
    ninsn = sum(len(P.L.funcs[e][0]) for e in lifted)
    nfp = sum(1 for e in lifted for pc in P.L.funcs[e][0]
              if enc.decode(img.word(pc)) is not None and enc.decode(img.word(pc)).entry.name in fp_names)
    print("%d roots, %d functions lifted by lift2, %d instructions (%d FP/NEON) (%.0fs); passed over: %s" % (
        len(roots), len(lifted), ninsn, nfp, time.time() - t0, ", ".join("%s %d" % kv for kv in why.most_common(10))),
        flush=True)
    new_text, t_new = L2.lift_new(P, lifted, img, True, prog)
    count_text = L2.lift_new(P, lifted, img, False, prog)[0]
    print("lift2: %.3f s for %d instructions: %.0f instructions/s (decode, IR, LLVM IR)" % (t_new, ninsn, ninsn / t_new))
    tmp = tempfile.mkdtemp(prefix="oot_lift2fp_")
    a64_rtgen.write(tmp)
    with open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n") as f:
        f.write(a64_lift.fpx_header(P.fpx))
    rt = rtgen.write(os.path.join(tmp, "rt64"))
    old_c, new_ll = os.path.join(tmp, "old.c"), os.path.join(tmp, "new.ll")
    with open(old_c, "w") as f:
        f.write("\n".join(L2.old_source(P, funcs)) + "\n")
    with open(new_ll, "w") as f:
        f.write(new_text)
    with open(new_ll + ".count.ll", "w") as f:
        f.write(count_text)
    inc = ["-I" + rt, "-I" + tmp]
    subprocess.run([clang, "-c", "-O1"] + L2.FLAGS + inc + [old_c, "-o", old_c + ".o"], check=True)
    subprocess.run([clang, "-c", "-O1"] + L2.IR_FLAGS + [new_ll + ".count.ll", "-o", new_ll + ".o"], check=True)
    n_old, n_new = L2.count_insns(old_c + ".o"), L2.count_insns(new_ll + ".o")
    order = [e for _, e, _ in roots]
    src = L2.old_source(P, funcs) + ["void g_%08x(Ctx *A64_NOESC);" % e for e in order]
    src.append(harness() % {
        "old": ",".join("f_%08x" % e for e in order), "new": ",".join("g_%08x" % e for e in order),
        "regions": ",".join("{%#xULL,%#xULL}" % r for r in TL.writable_regions(meta)),
        "scratch": TL.SCRATCH_SIZE, "scratch_base": TL.SCRATCH, "img_base": meta["base"],
        "img_size": len(img.mem), "state": TL.STATE.size})
    hc = os.path.join(tmp, "harness.c")
    with open(hc, "w") as f:
        f.write("\n".join(src))
    exe = os.path.join(tmp, "harness")
    rt_fp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lift2", "rt_fp.c")
    subprocess.run([clang, "-O1"] + L2.FLAGS + inc + [hc, new_ll, rt_fp, os.path.join(tmp, "a64_suyu.c"), "-o", exe],
                   check=True)
    print("harness built (%.0fs)" % (time.time() - t0), flush=True)
    TL.page_hash = bytes   # (Unicorn's changed pages kept whole, for lane-wise classification)
    emu = TL.Emu(img.mem, meta, TL.writable_regions(meta))
    cases, expect, skipped = [], [], 0
    for i, (mod, e, _) in enumerate(roots):
        ran = 0
        for k in range(a.states + 16):
            if k >= a.states and ran:
                break
            st = rand_state(rng)
            if k >= a.states:   # (no state ran: X0-X7 all into the scratch area, as test_lift2_leaf.py retries)
                st[0][:8] = [TL.SCRATCH + 0x1000 + 8 * rng.randrange((TL.SCRATCH_SIZE - 0x22000) // 8) for _ in range(8)]
            seed = rng.getrandbits(64)
            res = emu.run(e, st, TL.scratch_words(seed).tobytes())
            if res is None:
                skipped += 1
                continue
            ran += 1
            nz = emu.uc.reg_read(A.UC_ARM64_REG_NZCV) >> 28
            cases.append((i, seed, st))
            expect.append(res + ([nz >> 3 & 1, nz >> 2 & 1, nz >> 1 & 1, nz & 1],))
    print("%d comparable cases, %d skipped (Unicorn fault or no return) (%.0fs)" % (len(cases), skipped,
                                                                                   time.time() - t0), flush=True)
    inp, outp = os.path.join(tmp, "in.bin"), os.path.join(tmp, "out.bin")
    with open(inp, "wb") as f:
        for i, seed, st in cases:
            x, nzcv, fpcr, fpsr, v, tpidr, tpidrro = st
            f.write(struct.pack("<IQ", i, seed) + TL.STATE.pack(*(x + nzcv + [fpcr, fpsr] + v + [tpidr, tpidrro])))
    r = subprocess.run([exe, os.path.join(kit.WORK, "image_test", "image.bin"), inp, outp], timeout=1800)
    if r.returncode:
        raise SystemExit("harness exited with %d" % r.returncode)

    def read_one(f):
        halted, fault = struct.unpack("<ii", f.read(8))
        vals = TL.STATE.unpack(f.read(TL.STATE.size))
        ndiff = struct.unpack("<I", f.read(4))[0]
        raw = f.read(16 * min(ndiff, 8192))
        addrs = [struct.unpack_from("<QQ", raw, 16 * k) for k in range(min(ndiff, 8192))]
        pages = {}
        for k, (pg, h) in enumerate(addrs):
            pages[pg] = f.read(4096) if k < 64 else h
        return halted, fault, vals, pages

    fail_uc, fail_old = collections.defaultdict(list), collections.defaultdict(list)
    cls_uc, cls_old = collections.Counter(), collections.Counter()
    fn_cls = collections.defaultdict(set)
    with open(outp, "rb") as f:
        for (i, seed, st), (ex_x, ex_v, ex_fpsr, ex_pages, ex_nz) in zip(cases, expect):
            old, new = read_one(f), read_one(f)
            fz = st[2] >> 24 & 1
            if new[0] or new[1]:
                fail_uc[i].append(["new halted %d fault %d" % (new[0], new[1])])
            else:
                vals = new[2]
                gx, rx = list(vals[:32]), list(ex_x)
                if a.calls:   # (x16/x17: PLT veneers' scratch, as test_lift2_leaf.py)
                    gx[16:18] = rx[16:18]
                single = len(P.tree(roots[i][1], a.calls, a.max_tree if a.calls else 600)) == 1
                got = (gx, list(vals[32:36]) if single else None, list(vals[38:102]), new[3])
                c, d = compare(got, (rx, ex_nz, ex_v, ex_pages), fz)
                cls_uc.update(c)
                for k in c:
                    fn_cls[k].add(i)
                if d:
                    fail_uc[i].append(d[:4])
            if old[0] or old[1] or new[0] or new[1]:
                if (old[0], old[1]) != (new[0], new[1]):
                    fail_old[i].append(["halted/fault old %s new %s" % (old[:2], new[:2])])
            else:
                o, n = old[2], new[2]
                nx_, ox_ = list(n[:32]), list(o[:32])
                if a.calls:
                    nx_[16:18] = ox_[16:18]
                c, d = compare((nx_, None, list(n[38:102]), new[3]), (ox_, None, list(o[38:102]), old[3]), fz)
                cls_old.update(c)
                if d:
                    fail_old[i].append(d[:4])
    order = sorted(lifted)   # (the counts: every function lift2 lifts; the present lifter lifts them too)
    tot_old = sum(n_old["f_%08x" % e] for e in order)
    tot_new = sum(n_new["g_%08x" % e] for e in order)
    print("x86 instructions at -O1: present lifter %d, lift2 %d (%.3f); per guest instruction %.2f / %.2f" % (
        tot_old, tot_new, tot_new / max(tot_old, 1), tot_old / ninsn, tot_new / ninsn))
    worse = sorted(((n_new["g_%08x" % e] / max(n_old["f_%08x" % e], 1), e) for e in order), reverse=True)[:5]
    print("largest new/old ratios: %s" % ", ".join("f_%08x %.2f" % (e, q) for q, e in worse))
    tested = len({i for i, _, _ in cases})
    print("%d functions, %d run in at least one comparable case, %d cases: against Unicorn %d functions differ; "
          "against the present lifter %d differ" % (len(roots), tested, len(cases), len(fail_uc), len(fail_old)))
    print("allowed deviations (lanes or chunks) against Unicorn: %s; functions: %s" % (
        dict(cls_uc), {k: len(v) for k, v in fn_cls.items()}))
    print("allowed deviations against the present lifter: %s" % dict(cls_old))
    for title, fails in (("Unicorn", fail_uc), ("present lifter", fail_old)):
        for i in sorted(fails)[:None if a.list else 20]:
            mod, e, n = roots[i]
            print("  %s FAIL f_%08x (%s, %d insns) %d cases: %s" % (title, e, mod, n, len(fails[i]), "; ".join(fails[i][0])))
    if a.keep:
        print("kept: %s" % tmp)
    else:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    ok = not fail_uc and not fail_old
    print("PASS" if ok else "FAIL", "(%.0fs)" % (time.time() - t0))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
