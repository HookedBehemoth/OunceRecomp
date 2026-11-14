"""Lifter 2 on the game's own functions (milestone 1): random integer leaves lifted by nx2/lift2 and by the present
lifter, run natively from the same random states, and compared with Unicorn and with each other.

    python -I nx2/test_lift2_leaf.py [--count 500] [--calls] [--states 4] [--seed 1] [--list] [--keep]

Without --calls a function qualifies when lift2 can take it as a leaf (nx2/lift2/build_ir.scope: no call, no branch
out of its body) and the present lifter emits it without calls, 3-600 instructions. With --calls the root may call:
its call tree (BL, tail branches, PLT stubs; no BLR, SVC or unresolved BR, at most --max-tree instructions) must be
liftable by the present lifter, the root by lift2; every function of the tree lift2 can take is lifted by it, the rest
come from the present lifter, and the two meet at the generic entries (`void f(Ctx *)`). The draw follows
test_leaf.py's module shares. Per case:
- lift2 against Unicorn: X0-X30, SP, NZCV (leaves only: a callee from the present lifter drops dead flags) and the
  changed 4 KB pages (contents); X16/X17 not with calls (test_leaf.py's PLT rule);
- lift2 against the present lifter: X0-X30, SP and the changed pages (NZCV is not compared: the present lifter drops
  flag stores no later instruction of the function reads).
Cases where Unicorn faults or does not return are skipped and counted, as in test_leaf.py; a function none of whose
random states runs gets up to 16 more with X0-X7 all pointing into the scratch area.

It also reports the x86 instruction counts of both lifts of the same functions (each compiled alone with the game
build's flags for lifted code at -O1) and lift2's speed (decode, IR and LLVM IR per guest instruction).
Needs the dump, Unicorn and Linux; a local tool, like test_leaf.py.
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
import test_leaf as TL   # noqa: E402  (the present lifter's picker, Unicorn harness, states and scratch area)
from lift2 import build_ir, emit_llvm, llvm   # noqa: E402
import kit   # noqa: E402
import image as eden_image   # noqa: E402
import a64_lift   # noqa: E402
import a64_rtgen   # noqa: E402
import rtgen   # noqa: E402
from unicorn import arm64_const as A   # noqa: E402

FLAGS = ["-march=x86-64-v3", "-ffp-contract=off", "-fno-strict-aliasing", "-mcx16", "-g0", "-Wno-everything",
         "-mllvm", "-rotation-max-header-size=0", "-ffunction-sections", "-mllvm",
         "-capture-tracking-max-uses-to-explore=5000", "-DA64_POLLING", "-DA64_WRITE_TRACK"]

IR_FLAGS = ["-march=x86-64-v3", "-g0", "-Wno-everything", "-mllvm", "-rotation-max-header-size=0", "-ffunction-sections"]

HARNESS = r"""
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
static const A64Fn T_OLD[] = {%(old)s};
static const A64Fn T_NEW[] = {%(new)s};
uint8_t a64_dirty[A64_DIRTY_PAGES];
void a64_trap(Ctx *c, uint64_t pc, uint32_t insn) { (void)pc; (void)insn; c->halted = 9; }
void a64_ret_mismatch(Ctx *c, uint64_t pc) { (void)pc; c->halted = 10; }
void recomp_unhandled(GuestContext *c, uint32_t i, uint64_t pc) { (void)i; (void)pc; c->halted = 7; }
void recomp_barrier(void) {}
uint64_t recomp_cntpct(GuestContext *c) { (void)c; return 0; }
A64Fn a64_lookup(uint64_t a) { (void)a; return 0; }
void a64_icall(Ctx *c, uint64_t t) { (void)t; c->halted = 11; }
void a64_ijump(Ctx *c, uint64_t t) { (void)t; c->halted = 12; }
void a64_icall_site(Ctx *c, uint64_t t, uint64_t *s) { (void)t; (void)s; c->halted = 11; }
void a64_ijump_site(Ctx *c, uint64_t t, uint64_t *s) { (void)t; (void)s; c->halted = 12; }
static uint64_t polls;
static Ctx *poll_ctx;
void a64_poll_cur(void) { polls++; poll_ctx->poll = (polls %% 3) != 0; }
void a64_poll(Ctx *c) { c->poll = 0; a64_poll_cur(); }

#define IMG_BASE %(img_base)dULL
#define IMG_SIZE %(img_size)dULL
#define SCR_BASE %(scratch_base)dULL
#define SCR_SIZE %(scratch)dULL
static const uint64_t regions[][2] = {%(regions)s};
#define NREG (sizeof regions / sizeof regions[0])
static uint8_t *p_img, *p_scr;
static uint8_t *pristine(uint64_t a) { return a >= SCR_BASE ? p_scr + (a - SCR_BASE) : p_img + (a - IMG_BASE); }
static uint64_t page_hash(const uint8_t *p) {
    const uint64_t *w = (const uint64_t *)p; uint64_t a = 0, b = 0;
    for (uint64_t i = 0; i < 512; i++) { a += w[i] * (2 * i + 1); b += w[i] ^ (i * 0x9E3779B97F4A7C15ULL); }
    return a ^ b;
}
static void make_scratch(uint64_t seed, uint64_t *out) {
    for (uint64_t i = 0; i < SCR_SIZE / 8; i++) {
        uint64_t z = seed + (i + 1) * 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; z ^= z >> 31;
        out[i] = (z & 3) == 0 ? ((SCR_BASE + (z >> 12) %% (SCR_SIZE - 256)) & ~7ULL) : z;
    }
}
static sigjmp_buf jb;
static void on_fault(int sig) { siglongjmp(jb, sig); }
static int run(A64Fn f, Ctx *c) {
    poll_ctx = c;
    int sig = sigsetjmp(jb, 1);
    if (sig) return sig;
    f(c);
    return 0;
}
static void *map_at(uint64_t guest, uint64_t size) {
    void *p = mmap(A64_PTR(guest), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p == A64_PTR(guest) ? p : NULL;
}
int main(int argc, char **argv) {
    FILE *img = fopen(argv[1], "rb"), *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
    if (!map_at(IMG_BASE, IMG_SIZE) || !map_at(SCR_BASE, SCR_SIZE)) return 3;
    if (fread(A64_PTR(IMG_BASE), 1, IMG_SIZE, img) != IMG_SIZE) return 5;
    struct sigaction sa = {0}; sa.sa_handler = on_fault; sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGFPE, &sa, 0); sigaction(SIGILL, &sa, 0);
    p_img = malloc(IMG_SIZE); p_scr = malloc(SCR_SIZE);
    memcpy(p_img, A64_PTR(IMG_BASE), IMG_SIZE);
    static uint8_t state[%(state)d], state0[%(state)d];
    unsigned idx; uint64_t seed;
    while (fread(&idx, 4, 1, in) == 1 && fread(&seed, 8, 1, in) == 1 && fread(state0, sizeof state0, 1, in) == 1) {
        make_scratch(seed, (uint64_t *)p_scr);
        for (int which = 0; which < 2; which++) {
            memcpy(A64_PTR(SCR_BASE), p_scr, SCR_SIZE);
            memcpy(state, state0, sizeof state);
            Ctx c; memset(&c, 0, sizeof c);
            memcpy(c.x, state, 256); memcpy(&c.n, state + 256, 4);
            memcpy(&c.fpcr, state + 264, 8); memcpy(&c.fpsr, state + 272, 8); memcpy(c.vreg, state + 280, 512);
            memcpy(&c.tpidr_el0, state + 792, 8); memcpy(&c.tpidrro_el0, state + 800, 8);
            c.poll = 1;
            int fault = run(which ? T_NEW[idx] : T_OLD[idx], &c);
            memcpy(state, c.x, 256); memcpy(state + 256, &c.n, 4);
            memcpy(state + 264, &c.fpcr, 8); memcpy(state + 272, &c.fpsr, 8); memcpy(state + 280, c.vreg, 512);
            fwrite(&c.halted, 4, 1, out); fwrite(&fault, 4, 1, out); fwrite(state, sizeof state, 1, out);
            uint32_t ndiff = 0; static uint64_t diff[2 * 8192];
            for (size_t r = 0; r < NREG; r++)
                for (uint64_t a = regions[r][0]; a < regions[r][1]; a += 4096)
                    if (memcmp(A64_PTR(a), pristine(a), 4096)) {
                        if (ndiff < 8192) { diff[2 * ndiff] = a; diff[2 * ndiff + 1] = page_hash(A64_PTR(a)); }
                        ndiff++;
                        memcpy(A64_PTR(a), pristine(a), 4096);
                    }
            fwrite(&ndiff, 4, 1, out); fwrite(diff, 16, ndiff < 8192 ? ndiff : 8192, out);
        }
    }
    fclose(out);
    return 0;
}
"""


def program(P, lifted):
    """what lift2 knows about the other functions: the discovery's entries and tables; a call reaches lift2's generic
    entry when lift2 lifts the callee, else the present lifter's function"""
    L = P.L
    return build_ir.Program(entries=L.entries, jt=L.jt, callee=L.callee, plt=L.plt,
                            symbol=lambda e: ("g_%08x" if e in lifted else "f_%08x") % e)


def reads_clock(img, body):
    """MRS CNTPCT/CNTVCT: the counters differ between Unicorn and the harness"""
    return any((img.word(pc) & 0xFFFFFFC0) == 0xD53BE000 for pc in body)


def select(P, img, counts, rng, calls, max_tree, only=None):
    """roots (module, entry, instructions of the tree), every function of their trees, and the ones lift2 lifts;
    only: these functions (a bisection's list), all of them that qualify"""
    by_mod = collections.defaultdict(list)
    for e in sorted(only if only is not None else img.entries):
        by_mod[next(n for lo, hi, n in img.code if lo <= e < hi)].append(e)
    if only is not None:
        counts = {m: len(v) for m, v in by_mod.items()}
    roots, funcs, lifted, why = [], set(), set(), collections.Counter()
    prog = program(P, lifted) if calls else build_ir.LEAF
    for mod, want in counts.items():
        cands = by_mod[mod]
        rng.shuffle(cands)
        got = 0
        for e in cands:
            if got >= want:
                break
            if e in img.hooks:
                why["hooked"] += 1
                continue
            if not calls:
                if not P.liftable_body(e, False):
                    why["old lifter: calls, svc or traps"] += 1
                    continue
                if not 3 <= len(P.L.funcs[e][0]) <= 600:
                    why["size"] += 1
                    continue
            tree = P.tree(e, calls, max_tree if calls else 600)
            if tree is None:
                why["old lifter: not liftable here (BLR, SVC, traps, size)"] += 1
                continue
            body = P.L.funcs[e][0]
            r = build_ir.scope(e, sorted(body), img.word, prog)
            if r is not None:
                why[re.sub(r" at .*| [0-9a-f]{8}.*", "", r)] += 1
                continue
            if any(reads_clock(img, P.L.funcs[f][0]) for f in tree):
                why["reads the counter"] += 1
                continue
            for f in tree:
                if f not in lifted and build_ir.scope(f, sorted(P.L.funcs[f][0]), img.word, prog) is None:
                    lifted.add(f)
            roots.append((mod, e, sum(len(P.L.funcs[f][0]) for f in tree)))
            funcs |= tree
            got += 1
    return roots, funcs, lifted, why, prog


def lift_new(P, lifted, img, flags_out, prog):
    """lift2's module text and its time (decode, IR, emission)"""
    t0 = time.perf_counter()
    m = emit_llvm.Module(flags_out)
    for e in sorted(lifted):
        m.add(build_ir.build(e, sorted(P.L.funcs[e][0]), img.word, prog))
    text = m.text()
    return text, time.perf_counter() - t0


def old_source(P, funcs):
    names = sorted(funcs)
    body = "\n".join(P.emitted[e][0] for e in names)
    return ['#include "a64.h"'] + ["void f_%08x(Ctx *A64_NOESC);" % e for e in names] + [a64_lift.mark_groups(body)]


def count_insns(obj):
    """x86 instructions per function symbol of an object"""
    out = subprocess.run(["objdump", "-d", "--no-show-raw-insn", obj], capture_output=True, text=True).stdout
    n, cur = collections.Counter(), None
    for line in out.split("\n"):
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:$", line)
        if m:
            cur = m.group(1)
        elif cur and re.match(r"^\s+[0-9a-f]+:\t", line):
            n[cur] += 1
    return n


def main():
    kit.wait_for_quiet("the lift2 function test")
    p = argparse.ArgumentParser()
    p.add_argument("--count", type=int, default=500)
    p.add_argument("--calls", action="store_true")
    p.add_argument("--funcs", help="a file of guest addresses (hex, one per line): test these functions, not a draw")
    p.add_argument("--max-tree", type=int, default=4000)
    p.add_argument("--states", type=int, default=4)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--list", action="store_true")
    p.add_argument("--keep", action="store_true", help="keep the temporary folder (sources, objects, harness)")
    a = p.parse_args()
    clang = llvm.check("clang")
    rng = random.Random(a.seed)
    t0 = time.time()
    img = eden_image.EdenImage(kit.need_exefs())
    meta = img.write(os.path.join(kit.WORK, "image_test"))
    P = TL.Picker(img)
    share = TL.module_shares(img)
    only = None
    if a.funcs:
        only = {int(l.split()[0], 16) for l in open(a.funcs) if l.strip() and not l.startswith("#")}
    roots, funcs, lifted, why, prog = select(P, img, {m: max(1, int(a.count * s)) for m, s in share.items()}, rng,
                                             a.calls, a.max_tree, only)
    ninsn = sum(len(P.L.funcs[e][0]) for e in lifted)
    callers = sum(1 for _, e, _ in roots if any(k[0] == "bl" or (k[0] == "b" and k[1] not in P.L.funcs[e][0])
                                                for k, _ in P.L.funcs[e][0].values()))
    print("%d roots (%d with calls), %d functions in their trees, %d lifted by lift2 (%d instructions) (%.0fs); "
          "passed over: %s" % (len(roots), callers, len(funcs), len(lifted), ninsn, time.time() - t0,
                               ", ".join("%s %d" % kv for kv in why.most_common(10))), flush=True)
    new_text, t_new = lift_new(P, lifted, img, True, prog)   # (NZCV stored: compared with Unicorn)
    count_text = lift_new(P, lifted, img, False, prog)[0]      # (as the build will lift: flags dead at a return)
    print("lift2: %.3f s for %d instructions: %.0f instructions/s (decode, IR, LLVM IR)" % (t_new, ninsn, ninsn / t_new))
    tmp = tempfile.mkdtemp(prefix="oot_lift2_")
    a64_rtgen.write(tmp)
    with open(os.path.join(tmp, "a64_fpx.h"), "w", newline="\n") as f:
        f.write(a64_lift.fpx_header(P.fpx))
    rt = rtgen.write(os.path.join(tmp, "rt64"))
    old_c, new_ll = os.path.join(tmp, "old.c"), os.path.join(tmp, "new.ll")
    with open(old_c, "w") as f:
        f.write("\n".join(old_source(P, funcs)) + "\n")
    with open(new_ll, "w") as f:
        f.write(new_text)
    with open(new_ll + ".count.ll", "w") as f:
        f.write(count_text)
    inc = ["-I" + rt, "-I" + tmp]
    # x86 instruction counts: each lift alone, the lifted code's flags
    subprocess.run([clang, "-c", "-O1"] + FLAGS + inc + [old_c, "-o", old_c + ".o"], check=True)
    subprocess.run([clang, "-c", "-O1"] + IR_FLAGS + [new_ll + ".count.ll", "-o", new_ll + ".o"], check=True)
    n_old, n_new = count_insns(old_c + ".o"), count_insns(new_ll + ".o")
    # the harness: both lifts, one table each, same order
    order = [e for _, e, _ in roots]
    roots_tree = [P.tree(e, a.calls, a.max_tree if a.calls else 600) for e in order]
    src = old_source(P, funcs) + ["void g_%08x(Ctx *A64_NOESC);" % e for e in order]
    src.append(HARNESS % {
        "old": ",".join("f_%08x" % e for e in order), "new": ",".join("g_%08x" % e for e in order),
        "regions": ",".join("{%#xULL,%#xULL}" % r for r in TL.writable_regions(meta)),
        "scratch": TL.SCRATCH_SIZE, "scratch_base": TL.SCRATCH, "img_base": meta["base"],
        "img_size": len(img.mem), "state": TL.STATE.size})
    hc = os.path.join(tmp, "harness.c")
    with open(hc, "w") as f:
        f.write("\n".join(src))
    exe = os.path.join(tmp, "harness")
    subprocess.run([clang, "-O1"] + FLAGS + inc + [hc, new_ll, os.path.join(tmp, "a64_suyu.c"), "-o", exe], check=True)
    print("harness built (%.0fs)" % (time.time() - t0), flush=True)
    emu = TL.Emu(img.mem, meta, TL.writable_regions(meta))
    cases, expect, skipped = [], [], 0
    retried = uncompared = 0
    for i, (mod, e, _) in enumerate(roots):
        ran = 0
        for k in range(a.states + 16):
            if k >= a.states and ran:
                break
            st = TL.rand_state(rng)
            if k >= a.states:   # (no state ran: X0-X7 all into the scratch area, whose qwords are a quarter pointers)
                st[0][:8] = [TL.SCRATCH + 0x1000 + 8 * rng.randrange((TL.SCRATCH_SIZE - 0x22000) // 8) for _ in range(8)]
                retried += k == a.states
            seed = rng.getrandbits(64)
            res = emu.run(e, st, TL.scratch_words(seed).tobytes())
            if res is None:
                skipped += 1
                continue
            ran += 1
            nz = emu.uc.reg_read(A.UC_ARM64_REG_NZCV) >> 28
            cases.append((i, seed, st))
            expect.append(res + ([nz >> 3 & 1, nz >> 2 & 1, nz >> 1 & 1, nz & 1],))
        uncompared += not ran
    print("%d comparable cases, %d skipped (Unicorn fault or no return); %d roots retried with pointer arguments, "
          "%d never ran in Unicorn (%.0fs)" % (len(cases), skipped, retried, uncompared, time.time() - t0), flush=True)
    inp, outp = os.path.join(tmp, "in.bin"), os.path.join(tmp, "out.bin")
    with open(inp, "wb") as f:
        for i, seed, st in cases:
            x, nzcv, fpcr, fpsr, v, tpidr, tpidrro = st
            f.write(struct.pack("<IQ", i, seed) + TL.STATE.pack(*(x + nzcv + [fpcr, fpsr] + v + [tpidr, tpidrro])))
    r = subprocess.run([exe, os.path.join(kit.WORK, "image_test", "image.bin"), inp, outp], timeout=900)
    if r.returncode:
        raise SystemExit("harness exited with %d" % r.returncode)

    def read_one(f):
        halted, fault = struct.unpack("<ii", f.read(8))
        vals = TL.STATE.unpack(f.read(TL.STATE.size))
        ndiff = struct.unpack("<I", f.read(4))[0]
        raw = f.read(16 * min(ndiff, 8192))
        pages = {struct.unpack_from("<Q", raw, 16 * k)[0]: struct.unpack_from("<Q", raw, 16 * k + 8)[0]
                 for k in range(min(ndiff, 8192))}
        return halted, fault, vals, pages

    fail_uc, fail_old = collections.defaultdict(list), collections.defaultdict(list)
    with open(outp, "rb") as f:
        for (i, seed, st), (ex_x, ex_v, ex_fpsr, ex_pages, ex_nz) in zip(cases, expect):
            old, new = read_one(f), read_one(f)
            d = []
            if new[0] or new[1]:
                d.append("new halted %d fault %d" % (new[0], new[1]))
            else:
                vals = new[2]
                d += ["%s %#x != %#x" % ("sp" if r == 31 else "x%d" % r, vals[r], ex_x[r]) for r in range(32)
                      if vals[r] != ex_x[r] and not (a.calls and r in (16, 17))]
                if len(roots_tree[i]) == 1:   # (a callee of the present lifter's drops dead flag stores)
                    d += ["%s %d != %d" % (n, vals[32 + k], ex_nz[k]) for k, n in enumerate("nzcv")
                          if vals[32 + k] != ex_nz[k]]
                if new[3] != ex_pages:
                    d.append("memory: pages %s" % sorted("%x" % pg for pg in set(new[3]) ^ set(ex_pages)
                                                         or [pg for pg in new[3] if new[3][pg] != ex_pages.get(pg)])[:4])
            if d:
                fail_uc[i].append(d[:4])
            d = []
            if old[0] or old[1] or new[0] or new[1]:
                if (old[0], old[1]) != (new[0], new[1]):
                    d.append("halted/fault old %s new %s" % (old[:2], new[:2]))
            else:
                d += ["%s old %#x new %#x" % ("sp" if r == 31 else "x%d" % r, old[2][r], new[2][r]) for r in range(32)
                      if old[2][r] != new[2][r] and not (a.calls and r in (16, 17))]
                if old[3] != new[3]:
                    d.append("memory differs")
            if d:
                fail_old[i].append(d[:4])
    both = sorted(lifted)   # (every function lift2 lifts; the present lifter lifts them too)
    tot_old = sum(n_old["f_%08x" % e] for e in both)
    tot_new = sum(n_new["g_%08x" % e] for e in both)
    nonleaf = [e for e in both if any(k[0] in ("bl", "blr") for k, _ in P.L.funcs[e][0].values())]
    if nonleaf:
        print("functions with calls (%d): present lifter %d, lift2 %d x86 instructions (%.3f)" % (
            len(nonleaf), sum(n_old["f_%08x" % e] for e in nonleaf), sum(n_new["g_%08x" % e] for e in nonleaf),
            sum(n_new["g_%08x" % e] for e in nonleaf) / max(1, sum(n_old["f_%08x" % e] for e in nonleaf))))
    print("x86 instructions at -O1, the %d functions lift2 lifts: present lifter %d, lift2 %d (%.3f); per guest "
          "instruction %.2f / %.2f" % (len(both), tot_old, tot_new, tot_new / max(tot_old, 1), tot_old / ninsn,
                                       tot_new / ninsn))
    worse = sorted(((n_new["g_%08x" % e] / max(n_old["f_%08x" % e], 1), e) for e in both), reverse=True)[:5]
    print("largest new/old ratios: %s" % ", ".join("f_%08x %.2f" % (e, q) for q, e in worse))
    tested = len({i for i, _, _ in cases})
    print("%d functions, %d run in at least one comparable case, %d cases: against Unicorn %d functions differ; "
          "against the present lifter %d differ" % (len(roots), tested, len(cases), len(fail_uc), len(fail_old)))
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
