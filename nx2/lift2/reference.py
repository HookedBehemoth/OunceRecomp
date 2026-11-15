"""Unicorn as the reference for the lifter's tools on the game's own functions (nx2/test_lift2_leaf.py,
nx2/lift2/dump.py): random states and a scratch area, the image's writable regions, a function's call tree from the
discovery, and the run in Unicorn of a function from a state (registers, FPSR, and the 4 KB pages it changed).
Needs the dump and Unicorn: local tools.
"""
import struct

import numpy as np
import unicorn
from unicorn import arm64_const as A

from . import discover

SCRATCH_SIZE = 0x100000
SENTINEL = 0x7F000000      # LR of the outermost call: a mapped page with no code, below the image
PAGE = 0x1000
STATE = struct.Struct("<32Q4B4xQQ64QQQ")   # x0-x30, sp | nzcv | fpcr fpsr | v0-v31 (lo, hi) | tpidr tpidrro
XREG = [getattr(A, "UC_ARM64_REG_X%d" % i) for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
QREG = [getattr(A, "UC_ARM64_REG_Q%d" % i) for i in range(32)]
M64 = (1 << 64) - 1
SCRATCH = 0x1000000000


def scratch_words(seed):
    """the scratch area of one case (splitmix64; harness.c makes the same)"""
    i = np.arange(SCRATCH_SIZE // 8, dtype=np.uint64)
    with np.errstate(over="ignore"):
        z = np.uint64(seed) + (i + np.uint64(1)) * np.uint64(0x9E3779B97F4A7C15)
        z = (z ^ (z >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
        z = (z ^ (z >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
        z = z ^ (z >> np.uint64(31))
    ptr = np.uint64(SCRATCH) + ((z >> np.uint64(12)) % np.uint64(SCRATCH_SIZE - 256)) & ~np.uint64(7)
    return np.where((z & np.uint64(3)) == 0, ptr, z)


def page_hash(b):
    """hash of one 4 KB page (harness.c: the same)"""
    w = np.frombuffer(b, dtype=np.uint64)
    i = np.arange(len(w), dtype=np.uint64)
    with np.errstate(over="ignore"):
        return int((w * (i * np.uint64(2) + np.uint64(1))).sum() ^ (w ^ (i * np.uint64(0x9E3779B97F4A7C15))).sum())


class Picker:
    """The functions of the game as the lift discovers them (nx2/lift2/discover.py), walked on demand, and their call
    trees: P.L is the discovery (funcs, entries, jt, plt, callee)."""

    def __init__(self, img):
        self.img = img
        self.L = discover.Discovery(img)

    def body(self, e):
        L = self.L
        if e not in L.funcs:
            L.funcs[e] = L.walk(e)
            t = L.plt_target(e)
            if t is not None:
                L.plt[e] = t
        return L.funcs[e][0]

    def liftable_body(self, e, calls):
        """a body with no BLR, SVC, trap or unresolved BR (and no BL without calls): what runs alone in the harness"""
        body = self.body(e)
        kinds = {k[0] for k, _ in body.values()}
        bad = {"blr", "svc", "trap"} | (set() if calls else {"bl"})
        if not body or kinds & bad:
            return False
        if e not in self.L.plt and any(k[0] == "br" and pc not in self.L.jt for pc, (k, _) in body.items()):
            return False
        return True

    def callees(self, e):
        """the functions e calls (BL) or branches into (a tail call), PLT stubs resolved"""
        L, body = self.L, self.body(e)
        out = set()
        for pc, (k, _) in body.items():
            if k[0] == "bl" or (k[0] in ("b", "bcond", "cbz", "tbz") and k[1] not in body):
                out.add(L.callee(k[1]))
            if k[0] == "br" and pc in L.jt:
                out.update(L.callee(t) for t in L.jt[pc] if t not in body)
            if pc + 4 not in body and k[0] not in ("b", "br", "ret", "trap") and (pc + 4) in L.entries:
                out.add(pc + 4)   # (falls into the next function)
        if e in L.plt:
            out.add(L.callee(e))
        return out - {e}

    def tree(self, root, calls, max_insns, max_funcs=64):
        """the root's call tree (entries), or None if a function in it does not run alone in the harness"""
        seen, todo = set(), [root]
        while todo:
            e = todo.pop()
            if e in seen:
                continue
            seen.add(e)
            if len(seen) > max_funcs or e in self.img.hooks or not self.liftable_body(e, calls):
                return None
            if sum(len(self.L.funcs[x][0]) for x in seen) > max_insns:
                return None
            todo += [c for c in self.callees(e) if c not in seen]
        if not calls and len(seen) > 1:
            return None
        return seen


def module_shares(img):
    """how many of the drawn functions come from each module: main 55 %, sdk 30 %, the subsdk* modules the rest by
    code size, rtld 2 % (mkw-recomp's split, for whatever modules this exefs has)"""
    size = {n: hi - lo for lo, hi, n in img.code}
    share = {"main": 0.55, "sdk": 0.30, "rtld": 0.02}
    subs = [n for n in size if n.startswith("subsdk")]
    rest = 1.0 - sum(v for k, v in share.items() if k in size)
    for n in subs:
        share[n] = rest * size[n] / max(1, sum(size[s] for s in subs))
    return {k: v for k, v in share.items() if k in size}


def writable_regions(meta):
    regs = []
    for m in meta["modules"]:
        data = next(s for s in m["segments"] if s["name"] == ".data")
        lo = data["addr"] & ~(PAGE - 1)
        hi = (max(m["bss"][1], data["addr"] + data["size"]) + PAGE - 1) & ~(PAGE - 1)
        regs.append((lo, hi))
    regs.append((SCRATCH, SCRATCH + SCRATCH_SIZE))
    return regs


def rand_state(rng):
    x = []
    for i in range(31):
        r = rng.random()
        if i < 8 and r < 0.45:
            x.append(SCRATCH + 0x1000 + 8 * rng.randrange((SCRATCH_SIZE - 0x22000) // 8))
        elif r < 0.7:
            x.append(rng.choice([0, 1, 2, 3, 4, 7, 8, 16, 31, 32, 64, 100, 255, 0xFFFFFFFF, M64, rng.randrange(1024)]))
        else:
            x.append(rng.getrandbits(64))
    x[30] = SENTINEL
    x.append(SCRATCH + SCRATCH_SIZE - 0x10000 - 16 * rng.randrange(256))   # sp
    v = []
    for _ in range(32):
        if rng.random() < 0.5:
            v += [rng.getrandbits(64), rng.getrandbits(64)]
        else:
            lanes = [struct.unpack("<I", struct.pack("<f", rng.uniform(-100, 100)))[0] for _ in range(2)]
            v += [lanes[0] | lanes[1] << 32, struct.unpack("<Q", struct.pack("<d", rng.uniform(-1e3, 1e3)))[0]]
    fpcr = 0 if rng.random() < 0.75 else 1 << 24
    return x, [0, 0, 0, 0], fpcr, 0, v, SCRATCH + 0x800, SCRATCH + 0x400


class Emu:
    def __init__(self, image, meta, regions):
        uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        uc.ctl_set_cpu_model(A.UC_CPU_ARM64_MAX)
        base, size = meta["base"], (len(image) + PAGE - 1) & ~(PAGE - 1)
        uc.mem_map(base, size)
        uc.mem_write(base, bytes(image))
        uc.mem_map(SCRATCH, SCRATCH_SIZE)
        uc.mem_map(SENTINEL, PAGE)
        self.uc, self.dirty, self.regions = uc, set(), regions
        self.pristine = {}
        for lo, hi in regions[:-1]:
            for pg in range(lo, hi, PAGE):
                self.pristine[pg] = bytes(uc.mem_read(pg, PAGE))
        uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, self._write)

    def _write(self, uc, access, addr, size, value, data):
        self.dirty.add(addr & ~(PAGE - 1))
        self.dirty.add((addr + size - 1) & ~(PAGE - 1))

    def run(self, entry, st, scratch):
        uc = self.uc
        x, nzcv, fpcr, fpsr, v, tpidr, tpidrro = st
        uc.mem_write(SCRATCH, scratch)
        self.dirty = set()
        for i in range(31):
            uc.reg_write(XREG[i], x[i])
        uc.reg_write(A.UC_ARM64_REG_SP, x[31])
        uc.reg_write(A.UC_ARM64_REG_NZCV, 0)
        uc.reg_write(A.UC_ARM64_REG_FPCR, fpcr)
        uc.reg_write(A.UC_ARM64_REG_FPSR, fpsr)
        uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, tpidr)
        uc.reg_write(A.UC_ARM64_REG_TPIDRRO_EL0, tpidrro)
        for i in range(32):
            uc.reg_write(QREG[i], v[2 * i] | v[2 * i + 1] << 64)
        try:
            uc.emu_start(entry, SENTINEL, count=400000)
            ok = uc.reg_read(A.UC_ARM64_REG_PC) == SENTINEL
        except unicorn.UcError:
            ok = False
        res = None
        if ok:
            ox = [uc.reg_read(XREG[i]) for i in range(31)] + [uc.reg_read(A.UC_ARM64_REG_SP)]
            ov = []
            for i in range(32):
                q = uc.reg_read(QREG[i])
                ov += [q & M64, q >> 64]
            pages = {}
            for a in self.dirty:
                before = scratch[a - SCRATCH:a - SCRATCH + PAGE] if SCRATCH <= a < SCRATCH + SCRATCH_SIZE \
                    else self.pristine.get(a)
                now = bytes(uc.mem_read(a, PAGE))
                if before is not None and now != before:
                    pages[a] = page_hash(now)
            res = (ox, ov, uc.reg_read(A.UC_ARM64_REG_FPSR), pages)
        for a in self.dirty:          # the image pages put back for the next case
            if a in self.pristine:
                uc.mem_write(a, self.pristine[a])
        return res
