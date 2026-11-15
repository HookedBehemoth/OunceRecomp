"""Lift 2's function discovery (milestone 19): function bodies from the entries, jump tables, PLT stubs, setjmp
owners and the split of very large functions, without the present lifter.

Ported from the present lifter's discovery (third_party/a64-kit/lift/a64_lift.py, Lifter.discover and what it calls,
with nx2/insn.py's pointer-authenticated branches and nx2/lift_fixes.py's jump-table fixes built in),
the same rules on the same raw words, so that it can replace it: nx2/lift2/discover_check.py compares the two over the
whole game (entries, bodies, jump tables, PLT stubs, setjmp owners). The model: a function is an entry plus everything
reachable from it without crossing another entry; branches into another entry are tail calls.

Input (an image): .entries, .hooks {addr: name}, .word(addr), .read(addr, n), .in_code(addr), optional .names and
.eh_starts (function starts from the unwind info: the extent a jump table's targets must lie in).
Output (Discovery): funcs {entry: (body {pc: (kind, word)}, labels)}, jt {br pc: [targets]}, jt_missing, plt {stub:
(final target, GOT slot)}, setjmp_owner {owner: [return addresses]}, entries, stats.
"""
import bisect
import struct
from collections import Counter


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def classify(w, pc):
    """(kind, ...) for control flow and specials; ("op",) for everything the emitter translates. The pointer-
    authenticated forms (nx2/insn.py's classify_ext): RETAA/RETAB are returns; BRAA*/BRAB* and BLRAA*/BLRAB* are BR and
    BLR through Xn."""
    if w in (0xD65F0BFF, 0xD65F0FFF):                 # RETAA / RETAB
        return ("ret", 30)
    if (w & 0xFEFFF800) == 0xD61F0800:                # BRAA, BRAB, BRAAZ, BRABZ
        return ("br", (w >> 5) & 31)
    if (w & 0xFEFFF800) == 0xD63F0800:                # BLRAA, BLRAB, BLRAAZ, BLRABZ
        return ("blr", (w >> 5) & 31)
    if (w & 0x7C000000) == 0x14000000:
        return ("bl" if w & 0x80000000 else "b", pc + 4 * sx(w & 0x3FFFFFF, 26))
    if (w & 0xFF000010) == 0x54000000:
        return ("bcond", pc + 4 * sx((w >> 5) & 0x7FFFF, 19), w & 15)
    if (w & 0x7E000000) == 0x34000000:
        return ("cbz", pc + 4 * sx((w >> 5) & 0x7FFFF, 19), w & 31, w >> 31, (w >> 24) & 1)
    if (w & 0x7E000000) == 0x36000000:
        return ("tbz", pc + 4 * sx((w >> 5) & 0x3FFF, 14), w & 31, ((w >> 31) << 5) | ((w >> 19) & 31), (w >> 24) & 1)
    if (w & 0xFFFFFC1F) == 0xD61F0000:
        return ("br", (w >> 5) & 31)
    if (w & 0xFFFFFC1F) == 0xD63F0000:
        return ("blr", (w >> 5) & 31)
    if (w & 0xFFFFFC1F) == 0xD65F0000:
        return ("ret", (w >> 5) & 31)
    if (w & 0xFFE0001F) == 0xD4000001:
        return ("svc", (w >> 5) & 0xFFFF)
    if (w & 0xFFE0001F) in (0xD4200000, 0xD4400000):
        return ("trap",)
    if (w & 0xFFFFF0FF) == 0xD50330DF:
        return ("isb",)
    if (w & 0xFFFFFFE0) == 0xD50B7520:
        return ("ic",)
    return ("op",)


def _auth_end(w):
    """RETAA, RETAB, BRAAZ/BRABZ, BRAA/BRAB: the end of a block, as ret and br are"""
    return w in (0xD65F0BFF, 0xD65F0FFF) or (w & 0xFFFFF81F) == 0xD61F081F or (w & 0xFFFFF800) == 0xD71F0800


class Discovery:
    def __init__(self, img):
        self.img = img
        self.entries = set(img.entries) | set(img.hooks)
        self.hooks = dict(img.hooks)
        self.names = getattr(img, "names", {}) or {}
        self.funcs = {}       # entry -> (body {pc: (kind, word)}, labels set)
        self.jt = {}          # br pc -> [targets]
        self.jt_missing = {}  # br pc -> its register, for BRs whose table address was not found
        self.plt = {}         # stub entry -> (final target, GOT slot)
        self.stats = Counter()
        # function starts from unwind info bound jump-table targets; without them, every entry does
        self.eh_starts = sorted(getattr(img, "eh_starts", None) or self.entries)

    # ---- discovery ----------------------------------------------------------------------------------
    def discover(self, max_insns=0):
        for e in sorted(self.entries):
            if e in self.hooks:
                continue
            self.funcs[e] = self.walk(e)
        # A table found through another switch's edges needs those resolved first: retry the misses once with
        # every function's tables known, and re-walk the functions that contain a newly resolved one.
        missing, self.jt_missing = dict(self.jt_missing), {}
        self.__dict__.pop("_preds", None)
        redo = set()
        for pc, reg in missing.items():
            targets = self.jump_table(pc, reg)
            if targets:
                self.jt[pc] = targets
                redo |= {e for e, (body, _) in self.funcs.items() if pc in body}
        for e in redo:
            self.funcs[e] = self.walk(e)
        self.stats["jt_second_pass"] = len(missing) - len(self.jt_missing)
        self.stats["jt_no_table"] = len(self.jt_missing)
        if max_insns:
            self.split_large(max_insns)
        for e in self.funcs:
            t = self.plt_target(e)
            if t is not None:
                self.plt[e] = t
        self.find_setjmp_owners()   # (after the PLT stubs: their callees are resolved through them)

    SETJMP_NAMES = ("__setjmp", "setjmp", "_setjmp")

    def find_setjmp_owners(self):
        """(hookshot) the functions that call __setjmp, with the call's return addresses: entries (walked here), so
        a longjmp can run the owner's code after its setjmp (emit_function wraps the owner's callers)"""
        self.setjmp_owner = {}
        for e, (body, _) in list(self.funcs.items()):
            for pc, (k, w) in body.items():
                if k[0] == "bl" and self.names.get(self.callee(k[1])) in self.SETJMP_NAMES:
                    self.setjmp_owner.setdefault(e, []).append(pc + 4)
        for rets in self.setjmp_owner.values():
            for r in rets:
                if r not in self.funcs:
                    self.entries.add(r)
                    self.funcs[r] = self.walk(r)
        self.stats["setjmp_owners"] = len(self.setjmp_owner)

    def split_large(self, max_insns):
        """Give functions longer than max_insns extra entries, about every max_insns / 2 instructions, only where
        no branch of the function crosses the cut (so neither half re-walks code of the other). Extra entries are
        always correct in this model: control reaching one tail-calls it. A cut trades one direct tail call for
        compile time, which grows faster than linearly with function size at -O1 (TOTK's largest function is
        18,156 instructions). Functions that are one big loop or switch cannot be cut and stay whole."""
        new = set()
        for e, (body, _) in self.funcs.items():
            if len(body) <= max_insns:
                continue
            pcs = sorted(body)
            diff = [0] * (len(pcs) + 1)
            for pc, (k, _w) in body.items():
                targets = [k[1]] if k[0] in ("b", "bcond", "cbz", "tbz") else self.jt.get(pc, []) if k[0] == "br" else []
                for t in targets:
                    if t not in body or t == pc:
                        continue
                    if t > pc:      # forward: a cut strictly between pc and t splits the edge
                        lo, hi = bisect.bisect_right(pcs, pc), bisect.bisect_left(pcs, t)
                    else:           # backward: a cut in (t, pc] puts the branch after the cut and its target before
                        lo, hi = bisect.bisect_right(pcs, t), bisect.bisect_right(pcs, pc)
                    if lo < hi:
                        diff[lo] += 1
                        diff[hi] -= 1
            step = max_insns // 2
            last, cover = 0, 0
            for i, pc in enumerate(pcs):
                cover += diff[i]
                if i - last >= step and cover == 0 and i and pcs[i - 1] == pc - 4:
                    new.add(pc)
                    last = i
        new -= self.entries
        if not new:
            return
        self.entries |= new
        self.stats["split_entries"] += len(new)
        affected = {e for e, (body, _) in self.funcs.items() if not new.isdisjoint(body.keys())}
        for a in sorted(new | affected):
            self.funcs[a] = self.walk(a)

    def walk(self, entry):
        img, entries = self.img, self.entries
        body, labels, todo = {}, set(), [entry]

        def visit(t):
            if t != entry and t in entries:
                return
            if img.in_code(t):
                labels.add(t)
                todo.append(t)

        while todo:
            pc = todo.pop()
            while pc not in body:
                if (pc != entry and pc in entries) or not img.in_code(pc):
                    break
                w = img.word(pc)
                k = classify(w, pc)
                body[pc] = (k, w)
                kind = k[0]
                if kind == "b":
                    visit(k[1])
                    break
                if kind in ("bcond", "cbz", "tbz"):
                    visit(k[1])
                elif kind == "br":
                    targets = self.jump_table(pc, k[1])
                    if targets:
                        self.jt[pc] = targets
                        self.add_jt_edges(pc, targets)
                        for t in targets:
                            visit(t)
                    break
                elif kind in ("ret", "trap"):
                    break
                pc += 4
        return body, labels

    # ---- jump tables ---------------------------------------------------------------------------------
    def jump_table(self, br_pc, reg, window=32):
        """the jump table of a BR (nx2/lift_fixes.py's fixes 2 and 3 around the scan): the window starts after the
        last pointer-authenticated return or branch; when the window writes the table load's base register again
        after the load, that register is resolved at the load"""
        img = self.img
        for k in range(1, window + 1):
            pc = br_pc - 4 * k
            if img.in_code(pc) and _auth_end(img.word(pc)):
                window = k - 1
                self.stats["jt_window_cut_at_auth_ret"] += 1
                break
        hit = self._table_load(br_pc, window)
        if not hit:
            return self._jump_table(br_pc, reg, window)
        load_pc, treg = hit
        redirected = {}
        for name in ("reaching_constants", "hoisted_constant", "function_constant"):
            bound = getattr(self, name)

            def at_load(at, r, *rest, _f=bound):
                return _f(load_pc if (at, r) == (br_pc, treg) else at, r, *rest)
            redirected[name] = at_load
        self.__dict__.update(redirected)
        try:
            out = self._jump_table(br_pc, reg, window)
        finally:
            for name in redirected:
                del self.__dict__[name]
        if out:
            self.stats["jt_table_reg_reused"] += 1
        return out

    def _table_load(self, br_pc, window):
        """(pc, base register) of the last register-offset integer load in the BR's window, if the window writes
        its base register again after it"""
        img = self.img
        for pc in range(br_pc - 4, br_pc - 4 * window, -4):
            if not img.in_code(pc):
                return None
            w = img.word(pc)
            if (w & 0xFC000000) == 0x14000000 or (w & 0xFFFFFC1F) in (0xD65F0000, 0xD61F0000):
                return None
            if (w & 0x3B200C00) == 0x38200800 and not (w >> 26) & 1 and (w >> 22) & 3:   # LDR* (register offset)
                n = (w >> 5) & 31
                if any(self.writes(img.word(q), n) for q in range(pc + 4, br_pc, 4)):
                    return pc, n
                return None
        return None

    def _jump_table(self, br_pc, reg, window=32):
        """Symbolically run the straight-line code before a BR. Recognises LLVM's AArch64 tables: the target is
        base + (entry << k), entry loaded from a constant table. Seen in TOTK:
          - the table address hoisted out of the window (loop-invariant): found by scanning further back;
          - the bound on a copy of the index (mov w8, w26 / and x12, x1, #0xff) or set by CCMP;
          - no bound at all (index from cset/bfi): entries are read until one leaves the function.
          - one register holding a different table on each path into a shared dispatch: every constant reaching
            the br through the CFG is a candidate (reaching_constants).
        Targets must lie in the containing function ([start, next .eh_frame function start))."""
        img = self.img
        R = {}
        bound = None
        bounds = {}   # register -> number of table entries its value can index (cmp + b.hi/b.hs, and-mask, copies)
        for pc in range(br_pc - 4 * window, br_pc, 4):
            if not img.in_code(pc):
                R.clear()
                bounds.clear()
                continue
            w = img.word(pc)
            if (w & 0xFC000000) == 0x14000000 or (w & 0xFFFFFC1F) in (0xD65F0000, 0xD61F0000):
                # ret / b / br: what follows is reached from elsewhere, so nothing above is known there
                R.clear()
                bounds.clear()
                continue
            d = w & 31
            n = (w >> 5) & 31
            m = (w >> 16) & 31
            # index bounds, tracked beside the values
            if (w & 0x7F80001F) == 0x7100001F or (w & 0x7FE00C10) == 0x7A400800:      # CMP / CCMP #imm
                last_cmp = (n, (w >> 10) & 0xFFF if (w & 0x7F80001F) == 0x7100001F else (w >> 16) & 31)
                bounds["cmp"] = last_cmp
            elif (w & 0xFF00001F) in (0x54000008, 0x54000002) and "cmp" in bounds:       # B.HI / B.HS
                r, imm = bounds["cmp"]
                bounds[r] = imm + 1 if (w & 15) == 8 else imm
            elif (w & 0x7F800000) == 0x12000000 and not (w >> 16) & 63:                    # AND #(2^k - 1)
                imms, nbit = (w >> 10) & 63, (w >> 22) & 1
                if (nbit or not w >> 31) and imms < 31:
                    bounds[d] = 1 << (imms + 1)
                else:
                    bounds.pop(d, None)
            elif (w & 0x7FE0FFE0) == 0x2A0003E0:                                           # MOV Wd/Xd, Wm/Xm
                if m in bounds:
                    bounds[d] = bounds[m]
                else:
                    bounds.pop(d, None)
            elif d != 31 and self.writes(w, d):
                bounds.pop(d, None)
            if (w & 0x9F000000) == 0x90000000:                                   # ADRP
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                R[d] = ("c", (pc & ~0xFFF) + (sx(imm, 21) << 12))
            elif (w & 0x9F000000) == 0x10000000:                                 # ADR
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                R[d] = ("c", pc + sx(imm, 21))
            elif (w & 0xFF800000) == 0x91000000:                                 # ADD Xd, Xn, #imm{, lsl 12}
                v = R.get(n, ("reg", n))
                imm = ((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0)
                if v and v[0] == "c":
                    R[d] = ("c", v[1] + imm)
                elif v and v[0] == "reg" and d == n:
                    R.pop(d, None)   # adrp before the window, add inside it: resolved at the br (reaching_constants)
                elif v and v[0] == "reg" and len(v) == 2:
                    R[d] = ("reg", n, imm)
                else:
                    R[d] = None
            elif (w & 0x3B200C00) == 0x38200800 and not (w >> 26) & 1:           # LDR* (register offset)
                size, opc, S = w >> 30, (w >> 22) & 3, (w >> 12) & 1
                v = R.get(n, ("reg", n))                                          # an undefined base: hoisted
                R[d] = ("ld", v, m, 1 << size, opc >= 2, size if S else 0) if opc != 0 and v else None
            elif (w & 0xFF200000) == 0x8B000000 and (w >> 22) & 3 == 0:          # ADD Xd, Xn, Xm, LSL #k
                R[d] = ("add", R.get(n), R.get(m), (w >> 10) & 63)
            elif (w & 0xFFE00000) == 0x8B200000:                                 # ADD Xd, Xn, Wm, <ext> #k
                R[d] = ("add", R.get(n), R.get(m), (w >> 10) & 7)
            elif (w & 0x7F80001F) == 0x7100001F:                                 # CMP Wn/Xn, #imm
                bound = {"imm": (w >> 10) & 0xFFF}
            elif (w & 0x7FE00C10) == 0x7A400800:                                 # CCMP Wn/Xn, #imm5, #nzcv, cond
                bound = {"imm": (w >> 16) & 31}
            elif (w & 0xFF00001F) in (0x54000008, 0x54000002) and bound and "count" not in bound:   # B.HI / B.HS
                bound["count"] = bound["imm"] + 1 if (w & 15) == 8 else bound["imm"]
            elif self.writes(w, d):
                # anything else that writes Xd forgets it (stores, compares, branches and SIMD ops do not)
                R[d] = None
            rt2 = (w >> 10) & 31
            if ((w >> 27) & 7) == 5 and (w >> 22) & 1 and not (w >> 26) & 1:   # LDP also writes Rt2
                R[rt2] = None
                bounds.pop(rt2, None)
        v = R.get(reg)
        if v and v[0] == "c":   # br to a code constant: `adr x19, label ... br x19` (a continuation in a register)
            lo, hi = self.function_extent(br_pc)
            if lo <= v[1] < hi and img.in_code(v[1]):
                self.stats["br_constant"] += 1
                return [v[1]]
            return None
        if not v or v[0] != "add":
            return self.adr_targets(br_pc, reg)
        a, b, k = v[1], v[2], v[3]
        if a and a[0] == "ld":
            a, b = b, a
        if not (a and a[0] == "c" and b and b[0] == "ld"):
            self.stats["jt_unrecognized"] += 1
            self.jt_missing[br_pc] = reg
            return None
        base, (_, table, idx, size, signed, _shift) = a[1], b
        if isinstance(bounds.get(idx), int):        # a bound on the very register that indexes the table wins
            bound = {"count": bounds[idx]}
        if table[0] == "reg":
            # The table address was set outside the window. Every ADRP(+ADD) constant reaching the br through the
            # function's CFG is a candidate (one register can hold a different table on each path into a shared
            # dispatch), plus the address-order guesses; the switch takes the union of their targets.
            reg_t = table[1]
            off = table[2] if len(table) > 2 else 0   # add xT, xN, #off in the window, xN set before it
            tables = set(self.reaching_constants(br_pc, reg_t))
            guess = self.hoisted_constant(br_pc, reg_t)   # from the br itself: the load does not write reg_t
            if guess is None:
                guess = self.function_constant(br_pc, reg_t)
            if guess is not None:
                tables.add(guess)
            tables = {t + off for t in tables}
            if not tables:
                self.stats["jt_no_table"] += 1
                self.jt_missing[br_pc] = reg
                return None
            if len(tables) > 1:
                self.stats["jt_multi_table"] += 1
        else:
            tables = {table[1]}
        lo, hi = self.function_extent(br_pc)
        count = bound.get("count") if bound else None
        if not (count and 0 < count <= 4096):
            count = None
        fmt = {1: "b", 2: "h", 4: "i", 8: "q"}[size]
        fmt = fmt if signed else fmt.upper()

        def read(table, limit, strict):
            out = []
            for i in range(limit):
                try:
                    e = struct.unpack("<" + fmt, img.read(table + i * size, size))[0]
                except ValueError:
                    break
                t = (base + (e << k)) & 0xFFFFFFFFFFFFFFFF
                if not (lo <= t < hi and img.in_code(t)):
                    if strict:
                        return None
                    break
                out.append(t)
            return out

        targets, bounded = [], True
        for table in sorted(tables):
            got = read(table, count, True) if count else None
            if got is None:
                # no bound, or one that over-reads into the next table: read until an entry leaves the function
                if count:
                    self.stats["jt_out_of_function"] += 1
                got = read(table, 256 if size == 1 else 1024, False)
                bounded = False
            targets += [t for t in got if t not in targets]
        if not targets:
            return None
        self.stats["jt_bounded" if bounded and count else "jt_unbounded"] += 1
        return targets

    def preds(self, lo, hi):
        """Predecessor map of the code in [lo, hi): fall-through, direct branches, and jump tables resolved so far.
        Cached per function extent."""
        cache = self.__dict__.setdefault("_preds", {})
        if lo in cache:
            return cache[lo]
        img = self.img
        p = {}
        for pc in range(lo, hi, 4):
            if not img.in_code(pc):
                continue
            k = classify(img.word(pc), pc)
            kind = k[0]
            if kind == "b":
                succ = (k[1],)
            elif kind in ("bcond", "cbz", "tbz"):
                succ = (k[1], pc + 4)
            elif kind == "br":
                succ = tuple(self.jt.get(pc, ()))
            elif kind in ("ret", "trap"):
                succ = ()
            else:
                succ = (pc + 4,)
            for s in succ:
                p.setdefault(s, []).append(pc)
        cache[lo] = p
        return p

    def add_jt_edges(self, br_pc, targets):
        """Keep a cached predecessor map current when a jump table of its function is resolved."""
        cache = self.__dict__.get("_preds")
        if cache:
            lo, _ = self.function_extent(br_pc)
            if lo in cache:
                for t in targets:
                    cache[lo].setdefault(t, []).append(br_pc)

    def reaching_constants(self, at, reg, limit=50000, depth=0):
        """Every ADRP (+ ADD #imm) constant that reaches `at` in `reg` along the function's CFG, walking back from
        `at` until each path writes `reg`. A value copied (mov) or spilled to a stack slot and reloaded (ldr/ldp
        from [sp, #off], LLVM's spill of a hoisted table address under register pressure) is followed to where it
        was set. Paths that write something else, or a call that clobbers a caller-saved `reg`, contribute
        nothing (the switch's default covers them)."""
        return sorted(self._reaching(at, ("reg", reg), limit, depth))

    def _reaching(self, at, loc, limit, depth):
        img = self.img
        lo, hi = self.function_extent(at)
        if hi - lo > 0x40000 or depth > 4:
            return set()
        preds = self.preds(lo, hi)
        found, seen, todo = set(), set(), [at]
        kind_loc, x = loc
        while todo and len(seen) < limit:
            for p in preds.get(todo.pop(), ()):
                if p in seen:
                    continue
                seen.add(p)
                w = img.word(p)
                if kind_loc == "reg":
                    if classify(w, p)[0] in ("bl", "blr") and (x <= 18 or x == 30):
                        continue
                    if not self.writes(w, x):
                        todo.append(p)
                        continue
                    if (w & 0x9F000000) == 0x90000000:                                   # ADRP
                        imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                        found.add((p & ~0xFFF) + (sx(imm, 21) << 12))
                    elif (w & 0xFF800000) == 0x91000000:                                 # ADD Xreg, Xn, #imm
                        v = self.hoisted_constant(p, (w >> 5) & 31, 64)
                        if v is not None:
                            found.add(v + (((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0)))
                    elif (w & 0xFFE0FFE0) == 0xAA0003E0:                                 # MOV Xreg, Xm
                        found |= self._reaching(p, ("reg", (w >> 16) & 31), limit, depth + 1)
                    elif (w & 0xFFC003E0) == 0xF94003E0:                                 # LDR Xreg, [sp, #off]
                        found |= self._reaching(p, ("slot", ((w >> 10) & 0xFFF) * 8), limit, depth + 1)
                    elif (w & 0xFFC003E0) == 0xA94003E0:                                 # LDP X, X, [sp, #off]
                        off = sx((w >> 15) & 0x7F, 7) * 8 + (8 if (w & 31) != x else 0)
                        found |= self._reaching(p, ("slot", off), limit, depth + 1)
                    continue
                # a stack slot: the store that last wrote it
                if (w & 0xFFC003E0) == 0xF90003E0 and ((w >> 10) & 0xFFF) * 8 == x:    # STR Xt, [sp, #off]
                    found |= self._reaching(p, ("reg", w & 31), limit, depth + 1)
                    continue
                if (w & 0xFFC003E0) == 0xA90003E0:                                       # STP Xt1, Xt2, [sp, #off]
                    off = sx((w >> 15) & 0x7F, 7) * 8
                    if x in (off, off + 8):
                        found |= self._reaching(p, ("reg", w & 31 if x == off else (w >> 10) & 31), limit, depth + 1)
                        continue
                if ((w & 31) == 31 and (w & 0x7F800000) in (0x51000000, 0x11000000)) or (    # ADD/SUB sp, #imm
                        (w >> 5) & 31 == 31 and ((w & 0x3B200C00) in (0x38000C00, 0x38000400)
                                                 or (w & 0x3A800000) == 0x28800000)):    # [sp] pre/post-index
                    continue                                                             # sp moved: slots differ
                todo.append(p)
        return found

    @classmethod
    def writes(cls, w, reg):
        """True if the instruction w may write X`reg`: Rd/Rt (bits 0-4) of anything but a store, compare or
        branch, and Rt2 of a load pair."""
        if ((w >> 25) & 7) == 7:
            # SIMD & FP data processing: Rd is a V register, except FP<->integer conversions/moves and UMOV/SMOV
            return (w & 31) == reg and ((w & 0x5F20FC00) == 0x1E200000 or (w & 0xBFE0DC00) == 0x0E002C00)
        if (w & 0x0A000000) == 0x08000000 and (w >> 26) & 1:
            return False   # SIMD & FP load/store: Rt (and Rt2) are V registers
        if (w & 31) == reg and not cls.reads_rt_only(w):
            return True
        return ((w >> 27) & 7) == 5 and (w >> 22) & 1 and not (w >> 26) & 1 and ((w >> 10) & 31) == reg

    def function_constant(self, br_pc, reg):
        """the one constant the function assigns to reg (ADRP+ADD; else, nx2/lift_fixes.py's fix 1, also ADR)"""
        v = self._function_constant(br_pc, reg)
        if v is not None:
            return v
        img = self.img
        lo, hi = self.function_extent(br_pc)
        if hi - lo > 0x40000:
            return None
        page, found = {}, set()
        for pc in range(lo, hi, 4):
            if not img.in_code(pc):
                continue
            w = img.word(pc)
            d = w & 31
            if (w & 0x9F000000) == 0x90000000:                                   # ADRP
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                page[d] = (pc & ~0xFFF) + (sx(imm, 21) << 12)
            elif (w & 0xFF800000) == 0x91000000 and ((w >> 5) & 31) in page:    # ADD Xd, Xn, #imm
                if d == reg:
                    found.add(page[(w >> 5) & 31] + (((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0)))
            elif (w & 0x9F000000) == 0x10000000 and d == reg:                    # ADR
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                found.add(pc + sx(imm, 21))
        if len(found) == 1:
            self.stats["jt_adr_table"] += 1
            return found.pop()
        return None

    def _function_constant(self, br_pc, reg):
        """The one ADRP(+ADD) constant the function ever assigns to `reg`, or None if it assigns none or several.
        Catches table addresses hoisted far from their use, where a backward scan in address order runs into an
        unrelated path's write first (an epilogue's `ldp x26, ...` after a `ret`)."""
        img = self.img
        lo, hi = self.function_extent(br_pc)
        if hi - lo > 0x40000:
            return None
        page = {}
        found = set()
        for pc in range(lo, hi, 4):
            w = img.word(pc)
            d = w & 31
            if (w & 0x9F000000) == 0x90000000:                                   # ADRP
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                page[d] = (pc & ~0xFFF) + (sx(imm, 21) << 12)
            elif (w & 0xFF800000) == 0x91000000 and ((w >> 5) & 31) in page:    # ADD Xd, Xn, #imm
                v = page[(w >> 5) & 31] + (((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0))
                if d == reg:
                    found.add(v)
        return found.pop() if len(found) == 1 else None

    def adr_targets(self, br_pc, reg):
        """Every `adr xreg, label` into the same function: the candidate targets of a `br xreg` whose value the
        straight-line window could not pin down (a continuation set on several paths). The switch keeps a
        default case, so a candidate list that misses one still falls back to an indirect jump."""
        img = self.img
        lo, hi = self.function_extent(br_pc)
        if hi - lo > 0x40000:
            return None
        targets = []
        for pc in range(lo, hi, 4):
            w = img.word(pc)
            if (w & 0x9F00001F) == (0x10000000 | reg):
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                t = pc + sx(imm, 21)
                if lo <= t < hi and img.in_code(t):
                    targets.append(t)
        if targets:
            self.stats["br_adr_candidates"] += 1
        return targets or None

    def hoisted_constant(self, before, reg, scan=1024):
        """The constant ADRP (+ ADD) last wrote to `reg` before address `before`, scanning back in address order.
        Any other write to `reg` on the way makes it unknown. Address order is a heuristic for 'earlier': it is
        how LLVM lays out the hoisted setup of a loop-invariant table address."""
        img = self.img
        pc = before - 4
        for _ in range(scan):
            if not img.in_code(pc):
                return None
            w = img.word(pc)
            if (w & 31) == reg and not self.reads_rt_only(w):
                if (w & 0x9F000000) == 0x90000000:                               # ADRP Xreg, page
                    imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                    return (pc & ~0xFFF) + (sx(imm, 21) << 12)
                if (w & 0xFF800000) == 0x91000000:                               # ADD Xreg, Xn, #imm
                    imm = ((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0)
                    v = self.hoisted_constant(pc, (w >> 5) & 31, 64)
                    return None if v is None else v + imm
                return None
            pc -= 4
        return None

    @staticmethod
    def reads_rt_only(w):
        """True for encodings whose bits 0-4 are not a destination: stores, compares, branches."""
        return ((w & 0x3A000000) == 0x38000000 and not (w >> 22) & 1) or (w & 0x1C000000) == 0x14000000 \
            or (w & 0x7E000000) in (0x34000000, 0x36000000) \
            or (((w >> 27) & 7) == 5 and not (w >> 22) & 1)                     # STP / STNP

    def function_extent(self, pc):
        starts = self.eh_starts
        i = bisect.bisect_right(starts, pc)
        lo = starts[i - 1] if i else 0
        hi = starts[i] if i < len(starts) else 1 << 64
        return lo, hi

    # ---- PLT stubs -----------------------------------------------------------------------------------
    def plt_target(self, e):
        img = self.img
        try:
            w0, w1, w2, w3 = (img.word(e + 4 * i) for i in range(4))
        except Exception:
            return None
        if (w0 & 0x9F00001F) != 0x90000010 or (w1 & 0xFFC003FF) != 0xF9400211 or (w2 & 0xFFC003FF) != 0x91000210 \
                or w3 != 0xD61F0220:
            return None
        imm = ((w0 >> 3) & 0x1FFFFC) | ((w0 >> 29) & 3)
        slot = (e & ~0xFFF) + (sx(imm, 21) << 12) + ((w1 >> 10) & 0xFFF) * 8
        t = struct.unpack("<Q", img.read(slot, 8))[0]
        return (t, slot) if t in self.entries else None

    def callee(self, t):
        seen = 0
        while t in self.plt and seen < 4:
            t = self.plt[t][0]
            seen += 1
        return t


def adopt(L, D):
    """the present lifter's emitter takes lift 2's discovery: its functions, tables, stubs and setjmp owners"""
    L.entries, L.funcs, L.jt, L.jt_missing, L.plt = D.entries, D.funcs, D.jt, D.jt_missing, D.plt
    L.setjmp_owner = D.setjmp_owner
    L.stats.update(D.stats)


def compare(L, D, show=12):
    """the present lifter's discovery (L) against lift 2's (D): entries, bodies (addresses and words), labels, jump
    tables, PLT stubs, setjmp owners; prints each kind of difference with examples; True when there is none"""
    same = True

    def report(what, a, b):
        nonlocal same
        only_a, only_b = sorted(set(a) - set(b)), sorted(set(b) - set(a))
        if only_a or only_b:
            same = False
            print("discovery check: %s: %d only in the present lifter's (%s), %d only in lift 2's (%s)" % (
                what, len(only_a), " ".join("%x" % x for x in only_a[:show]), len(only_b),
                " ".join("%x" % x for x in only_b[:show])))
    report("entries", L.entries, D.entries)
    report("functions", L.funcs, D.funcs)
    diff_body = [e for e in L.funcs if e in D.funcs and L.funcs[e][0] != D.funcs[e][0]]
    diff_labels = [e for e in L.funcs if e in D.funcs and L.funcs[e][1] != D.funcs[e][1]]
    for e in diff_body[:3]:   # (the first differences of a few, to classify them)
        a, b = L.funcs[e][0], D.funcs[e][0]
        pa, pb = sorted(set(a) - set(b)), sorted(set(b) - set(a))
        dv = [pc for pc in a if pc in b and a[pc] != b[pc]][:2]
        print("discovery check: f_%08x: %d pcs only the present lifter's (%s), %d only lift 2's (%s), values %s" % (
            e, len(pa), " ".join("%x" % x for x in pa[:4]), len(pb), " ".join("%x" % x for x in pb[:4]),
            "; ".join("%x %r / %r" % (pc, a[pc], b[pc]) for pc in dv)))
    for what, d in (("bodies", diff_body), ("labels", diff_labels)):
        if d:
            same = False
            print("discovery check: %d functions with other %s (%s)" % (len(d), what,
                                                                        " ".join("%x" % e for e in d[:show])))
    report("jump tables", L.jt, D.jt)
    dt = [pc for pc in L.jt if pc in D.jt and L.jt[pc] != D.jt[pc]]
    if dt:
        same = False
        print("discovery check: %d jump tables with other targets (%s)" % (len(dt), " ".join("%x" % x for x in dt[:show])))
    report("BR without a table", L.jt_missing, D.jt_missing)
    report("PLT stubs", L.plt, D.plt)
    if any(L.plt[e] != D.plt[e] for e in L.plt if e in D.plt):
        same = False
        print("discovery check: PLT stubs with other targets")
    report("setjmp owners", getattr(L, "setjmp_owner", {}), D.setjmp_owner)
    return same
