"""re: the reverse engineering command. One game's executable as compact text for agents, each output within a fixed
budget (cut with a note). Run through tools/re/re (work/venv's python -I).

  re [--game games/oot|games/dkb] <command> ...
    fn <addr|label> [--lines N]       the annotated listing: blocks, callees (with the name of a stored reading),
                                      strings, globals, NVN procs called through their slots
    tree <addr|label> [--depth N]     the callee tree: label, sdk, read: <name> (stored pseudo-C) or UNREAD
    pseudo <addr|label> [--set <file|->]  the stored pseudo-C (the re agent writes it)
    pc <addr|label>                   experimental pseudo-C without a model
    callers <addr|label>              who calls it (BL, tail B, 8-byte pointers), one line each
    callees <addr|label>              what it calls, one line each
    xref <string|addr>                code that takes the address of a string or a global (ADRP+ADD/LDR), pointers
    field <offset> [--all]            functions that load, store or add that offset (not sp/fp based), one line each
    fieldname <offset> <name>         a name fn shows at that offset
    asm <addr|label> [n]              raw instructions with their words: for patch sites only
    export                            refresh the lifter's view (work/re/<game>/export, ounce-lift re-export)

The default game is games/oot (OUNCE_RE_GAME sets another). The lifter's view (function bodies, jump tables, PLT,
symbols) is made once by ounce-lift re-export from the game's exefs: --exefs <dir>, else OUNCE_RE_EXEFS, else the
first line of work/<game>/dump.txt, else (OoT) work/dump.txt. Addresses are guest addresses (Eden layout), labels
those of games/<game>/labels.txt.
"""
import bisect
import hashlib
import os
import re as regex
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import pseudo  # noqa: E402

LIMIT_CHARS = 9000


def main_root():
    """the main checkout (an agent worktree's work/re would be its own: the cache is shared)"""
    g = os.path.join(ROOT, ".git")
    if os.path.isfile(g):
        line = open(g).read().strip()
        if line.startswith("gitdir:"):
            d = line.split(":", 1)[1].strip()
            return os.path.dirname(os.path.dirname(os.path.dirname(d)))
    return ROOT


WORK = os.path.join(main_root(), "work")


class Out:
    """the command's text within a budget of lines and characters"""

    def __init__(self, lines=80, chars=LIMIT_CHARS):
        self.lines, self.chars, self.buf, self.n, self.size = lines, chars, [], 0, 0

    def __call__(self, s=""):
        for line in str(s).split("\n"):
            self.n += 1
            if len(self.buf) < self.lines and self.size + len(line) < self.chars:
                if len(line) > 200:
                    line = line[:197] + "..."
                self.buf.append(line)
                self.size += len(line) + 1

    def done(self, hint="--lines N for more"):
        print("\n".join(self.buf))
        if self.n > len(self.buf):
            print("... (%d more lines cut; %s)" % (self.n - len(self.buf), hint))


# ---- the game


def read_dump_txt(p):
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith("#"):
                return line
    return None


def find_exefs(game, override=None):
    cands = [override, os.environ.get("OUNCE_RE_EXEFS"), read_dump_txt(os.path.join(WORK, game, "dump.txt"))]
    if game == "oot":
        cands.append(read_dump_txt(os.path.join(WORK, "dump.txt")))
    for c in cands:
        if not c:
            continue
        if os.path.exists(os.path.join(c, "main")):
            return c
        if os.path.exists(os.path.join(c, "exefs", "main")):
            return os.path.join(c, "exefs")
    sys.exit("re: no exefs for %s: pass --exefs <dir>, set OUNCE_RE_EXEFS, or write the folder into %s"
             % (game, os.path.join(WORK, game, "dump.txt")))


def lifter():
    for r in (ROOT, main_root()):
        p = os.path.join(r, "ounce", "target", "release", "ounce-lift")
        if os.path.exists(p):
            return p
    sys.exit("re: no ounce-lift (cargo build --release --manifest-path ounce/Cargo.toml -p ounce-lift)")


def export(gamedir, game, exefs=None):
    out = os.path.join(WORK, "re", game, "export")
    cmd = [lifter(), "re-export", out, "--game", gamedir, "--root", ROOT, "--work", WORK,
           "--exefs", find_exefs(game, exefs)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("re: %s failed:\n%s" % (" ".join(cmd), r.stderr[-2000:]))
    return out


LABEL = regex.compile(r"^([0-9a-f]{8})\s+([a-z][a-z0-9_]*)\s*(?:#\s?(.*))?$")


def read_labels(path):
    labels, notes = {}, {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            m = LABEL.match(line.rstrip("\n"))
            if m:
                a = int(m.group(1), 16)
                labels[a], notes[a] = m.group(2), (m.group(3) or "").strip()
    return labels, notes


class Game:
    def __init__(self, gamedir, exefs=None, hide=()):
        self.dir = gamedir
        self.game = os.path.basename(os.path.normpath(gamedir))
        self.cache = os.path.join(WORK, "re", self.game)
        ex = os.path.join(self.cache, "export")
        if not os.path.exists(os.path.join(ex, "names.tsv")):
            export(gamedir, self.game, exefs)
        meta = [line.rstrip("\n").split("\t") for line in open(os.path.join(ex, "meta.tsv"))]
        self.base = int(meta[0][1], 16)
        self.end = int(meta[1][1], 16)
        self.modules = [(m[1], int(m[2], 16), int(m[3], 16), int(m[4], 16), int(m[5], 16)) for m in meta[2:]]
        self.mem = np.memmap(os.path.join(ex, "image.bin"), dtype=np.uint8, mode="r")
        f = np.fromfile(os.path.join(ex, "funcs.bin"), dtype="<u8")
        n = int(f[0])
        self.entries = f[1:1 + n]
        self.offs = f[1 + n:2 + 2 * n]
        runs = f[2 + 2 * n:].reshape(-1, 2)
        self.runs = runs
        fn = np.repeat(np.arange(n), np.diff(self.offs.astype(np.int64)))
        order = np.argsort(runs[:, 0], kind="stable")
        self.rs, self.rl, self.rf = runs[order, 0], runs[order, 1], fn[order]
        self.entry_list = self.entries.tolist()
        self.entry_set = set(self.entry_list)
        self.jt = {}
        for line in open(os.path.join(ex, "jt.tsv")):
            br, load, idx, ts = line.rstrip("\n").split("\t")
            self.jt[int(br, 16)] = (int(load, 16), int(idx), [int(t, 16) for t in ts.split(",") if t])
        self.plt = {}
        for line in open(os.path.join(ex, "plt.tsv")):
            s, t, _ = line.split("\t")
            self.plt[int(s, 16)] = int(t, 16)
        self.sym = {}
        for line in open(os.path.join(ex, "names.tsv"), encoding="utf-8", errors="replace"):
            p = line.rstrip("\n").split("\t")
            if len(p) == 3:
                a = int(p[0], 16)
                if a not in self.sym:
                    self.sym[a] = short(p[2])
        self.labels, self.notes = read_labels(os.path.join(gamedir, "labels.txt"))
        for a in hide:
            self.labels.pop(a, None)
        self.by_label = {v: k for k, v in self.labels.items()}
        self.by_sym = {}
        for a, s in self.sym.items():
            self.by_sym.setdefault(s, a)
        self._inputs = {}
        self._calls = None
        self._fields = None
        self._procs = None
        self.hide_name = None

    # -- memory

    def read(self, a, n):
        o = a - self.base
        if o < 0 or o + n > len(self.mem):
            return b""
        return bytes(self.mem[o:o + n])

    def u32(self, a):
        return int.from_bytes(self.read(a, 4), "little")

    def u64(self, a):
        return int.from_bytes(self.read(a, 8), "little")

    def in_image(self, a):
        return self.base <= a < self.end

    def module_of(self, a):
        for m in self.modules:
            if m[1] <= a < m[2]:
                return m
        return None

    def in_code(self, a):
        return any(m[3] <= a < m[4] for m in self.modules)

    # -- functions

    def func_of(self, pc):
        i = int(np.searchsorted(self.rs, pc, side="right")) - 1
        for j in range(i, max(i - 8, -1), -1):
            if self.rs[j] <= pc < self.rs[j] + 4 * self.rl[j]:
                return int(self.entries[self.rf[j]])
        return None

    def body(self, entry):
        i = bisect.bisect_left(self.entry_list, entry)
        if i >= len(self.entry_list) or self.entry_list[i] != entry:
            return []
        pcs = []
        for s, n in self.runs[int(self.offs[i]):int(self.offs[i + 1])].tolist():
            pcs.extend(range(s, s + 4 * n, 4))
        return pcs

    def body_hash(self, entry):
        h = hashlib.sha1()
        for pc in self.body(entry):
            h.update(self.read(pc, 4))
        return h.hexdigest()[:16]

    def resolve(self, t):
        return self.plt.get(t, t)

    def name(self, a):
        a = self.resolve(a)
        if a in self.labels:
            return self.labels[a]
        if a in self.sym:
            return self.sym[a]
        return "f_%08x" % a

    def known(self, a):
        """a name that says what the function is (a label or a symbol)"""
        a = self.resolve(a)
        return a in self.labels or a in self.sym

    def parse_addr(self, s):
        if s in self.by_label:
            return self.by_label[s]
        if s in self.by_sym:
            return self.by_sym[s]
        t = s[2:] if s.startswith(("0x", "f_")) else s
        try:
            return int(t, 16)
        except ValueError:
            sys.exit("re: %s is neither an address nor a label" % s)

    def string_at(self, a):
        if not self.in_image(a) or self.in_code(a):
            return None
        data = self.read(a, 120)
        e = data.find(b"\0")
        if e < 1:
            return None
        s = data[:e]
        try:
            t = s.decode("utf-8")
        except UnicodeDecodeError:
            return None
        if any(ord(c) < 32 and c not in "\n\t\r" for c in t):
            return None
        if e < 2 and not t.isalnum():
            return None
        t = t.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
        if len(t) > 60:
            t = t[:57] + "..."
        return '"%s"' % t

    def addr_text(self, v):
        if v in self.labels or v in self.sym or v in self.entry_set:
            return "&" + self.name(v)
        if v in self.procs():
            return "&pfn_" + self.procs()[v]
        s = self.string_at(v)
        if s:
            return s
        return "&g_%x" % v

    def data_text(self, v):
        """the name of the data at v: a label, a symbol, a proc slot (pfn_<proc>), a GOT entry of one (&pfn_<proc>)"""
        if v in self.labels or v in self.sym:
            return self.name(v)
        procs = self.procs()
        if v in procs:
            return "pfn_" + procs[v]
        if self.u64(v) in procs:
            return "&pfn_" + procs[self.u64(v)]
        return "g_%x" % v

    # -- proc tables: pointer slots the game fills by name (nvnDeviceGetProcAddress and alike), from its loaders

    def procs(self):
        """slot address -> proc name, derived from the game's loaders: a call with a proc name string in x0 or x1
        whose result x0 is stored to a fixed address. Cached in work/re/<game>/procs.tsv."""
        if self._procs is not None:
            return self._procs
        p = os.path.join(self.cache, "procs.tsv")
        src = os.path.join(self.cache, "export", "funcs.bin")
        if os.path.exists(p) and os.path.getmtime(p) >= os.path.getmtime(src):
            self._procs = {}
            for line in open(p, encoding="utf-8"):
                a, n = line.rstrip("\n").split("\t")
                self._procs[int(a, 16)] = n
            return self._procs
        self._procs = find_procs(self)
        os.makedirs(self.cache, exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            for a in sorted(self._procs):
                f.write("%x\t%s\n" % (a, self._procs[a]))
        return self._procs

    def callee_inputs(self, t):
        """how many of x0..x7 the callee reads before it writes them, in its straight-line start"""
        t = self.resolve(t)
        if t in self._inputs:
            return self._inputs[t]
        data = self.read(t, 4 * 48)
        written, n = set(), 0
        if data:
            for i in pseudo.MD.disasm(data, t):
                x = pseudo.classify(i.address, 0, i)
                if x.kind in ("bl", "blr", "b", "br", "ret"):
                    if x.kind in ("b", "br"):
                        n = max([n] + [k + 1 for k in x.uses if k < 8 and k not in written])
                    break
                for k in x.uses:
                    if k < 8 and k not in written:
                        n = max(n, k + 1)
                written |= x.defs
        self._inputs[t] = n
        return n

    # -- the call index (every module's BL and tail B), cached

    def calls(self):
        if self._calls is not None:
            return self._calls
        p = os.path.join(self.cache, "calls.npz")
        src = os.path.join(self.cache, "export", "funcs.bin")
        if os.path.exists(p) and os.path.getmtime(p) >= os.path.getmtime(src):
            z = np.load(p)
            self._calls = (z["bl_s"], z["bl_t"], z["b_s"], z["b_t"])
            return self._calls
        bls, blt, bs, bt = [], [], [], []
        ent = self.entries.astype(np.int64)
        for _, _, _, lo, hi in self.modules:
            w = np.frombuffer(self.mem[lo - self.base:hi - self.base].tobytes(), dtype="<u4").astype(np.int64)
            pc = lo + np.arange(len(w), dtype=np.int64) * 4
            imm = w & 0x3FFFFFF
            imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
            tgt = pc + imm * 4
            isbl = (w & 0xFC000000) == 0x94000000
            isb = (w & 0xFC000000) == 0x14000000
            bls.append(pc[isbl])
            blt.append(tgt[isbl])
            cand_s, cand_t = pc[isb], tgt[isb]
            ok = np.isin(cand_t, ent)
            cand_s, cand_t = cand_s[ok], cand_t[ok]
            i = np.searchsorted(self.rs.astype(np.int64), cand_s, side="right") - 1
            owner = ent[self.rf[np.maximum(i, 0)]]
            keep = owner != cand_t
            bs.append(cand_s[keep])
            bt.append(cand_t[keep])
        c = tuple(np.concatenate(x) for x in (bls, blt, bs, bt))
        os.makedirs(self.cache, exist_ok=True)
        np.savez(p, bl_s=c[0], bl_t=c[1], b_s=c[2], b_t=c[3])
        self._calls = c
        return c

    def targets_of(self, f):
        """f and the PLT stubs that lead to it"""
        return [f] + [s for s, t in self.plt.items() if t == f]

    def callers(self, f):
        bl_s, bl_t, b_s, b_t = self.calls()
        ts = np.array(self.targets_of(f), dtype=np.int64)
        return bl_s[np.isin(bl_t, ts)].tolist(), b_s[np.isin(b_t, ts)].tolist()

    def pointers(self, v):
        """8-byte aligned words equal to v outside the code"""
        m = self.mem[:len(self.mem) // 8 * 8].view("<u8")
        hits = (np.nonzero(m == v)[0] * 8 + self.base).tolist()
        return [h for h in hits if not self.in_code(h)]

    def adrp_index(self):
        p = os.path.join(self.cache, "adrp.npz")
        src = os.path.join(self.cache, "export", "funcs.bin")
        if os.path.exists(p) and os.path.getmtime(p) >= os.path.getmtime(src):
            z = np.load(p)
            return z["pc"], z["page"], z["w"]
        pcs, pages, ws = [], [], []
        for _, _, _, lo, hi in self.modules:
            w = np.frombuffer(self.mem[lo - self.base:hi - self.base].tobytes(), dtype="<u4").astype(np.int64)
            pc = lo + np.arange(len(w), dtype=np.int64) * 4
            isadrp = (w & 0x9F000000) == 0x90000000
            imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
            imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
            page = (pc & ~0xFFF) + (imm << 12)
            pcs.append(pc[isadrp])
            pages.append(page[isadrp])
            ws.append(w[isadrp])
        z = (np.concatenate(pcs), np.concatenate(pages), np.concatenate(ws))
        np.savez(p, pc=z[0], page=z[1], w=z[2])
        return z

    def adrp_refs(self, target, span=0):
        """pcs of ADRP+ADD/LDR/STR pairs that reach target (or [target, target + span))"""
        pc, page, w = self.adrp_index()
        hits = []
        lo_page, hi_page = target & ~0xFFF, (target + max(span, 1) - 1) & ~0xFFF
        for k in np.nonzero((page >= lo_page) & (page <= hi_page))[0].tolist():
            a, pg, rd = int(pc[k]), int(page[k]), int(w[k]) & 31
            for j in range(1, 10):
                x = self.u32(a + 4 * j)
                rn = (x >> 5) & 31
                if (x & 0x7F800000) == 0x11000000 and rn == rd:       # ADD imm
                    v = pg + (((x >> 10) & 0xFFF) << (12 * ((x >> 22) & 1)))
                elif (x & 0x3B000000) == 0x39000000 and rn == rd:     # LDR/STR unsigned imm
                    size = x >> 30
                    if (x >> 26) & 1 and (x >> 23) & 1:
                        size = 4
                    v = pg + (((x >> 10) & 0xFFF) << size)
                else:
                    if (x & 31) == rd and (x & 0x9F000000) != 0x90000000 and not (x & 0x3B000000) == 0x39000000:
                        break
                    continue
                if target <= v < target + max(span, 1):
                    hits.append((a + 4 * j, v))
                if (x & 31) == rd:
                    break
        return hits

    def where(self, pc):
        f = self.func_of(pc)
        if f is None:
            return "%#x" % pc
        return "%s+%#x" % (self.name(f), pc - f)


PROC_NAME = regex.compile(rb"(?<=\0)((?:nvn|vk|gl|egl)[A-Z][A-Za-z0-9_]{2,80})(?=\0)")


def find_procs(g):
    """the proc slots of the loaders (see Game.procs): the functions that take the address of a proc name string,
    walked in order with the registers' known values (ADRP, ADD, 64-bit LDR, MOV)"""
    data = g.mem.tobytes()
    names = {g.base + m.start(1): m.group(1).decode() for m in PROC_NAME.finditer(data)}
    if not names:
        return {}
    pc, page, w = g.adrp_index()
    pages = np.unique(np.array([a & ~0xFFF for a in names], dtype=np.int64))
    funcs = set()
    for k in np.nonzero(np.isin(page, pages))[0].tolist():
        a, pg, rd = int(pc[k]), int(page[k]), int(w[k]) & 31
        for j in range(1, 8):
            x = g.u32(a + 4 * j)
            if (x & 0xFF800000) == 0x91000000 and (x >> 5) & 31 == rd:      # ADD x, imm
                if pg + ((x >> 10) & 0xFFF) in names:
                    f = g.func_of(a)
                    if f is not None:
                        funcs.add(f)
                break
    slots = {}
    for f in funcs:
        val = {}        # reg -> ("a", address) or ("p", proc name)
        for q in g.body(f):
            x = g.u32(q)
            rd, rn = x & 31, (x >> 5) & 31
            if (x & 0x9F000000) == 0x90000000:                               # ADRP
                imm = (((x >> 5) & 0x7FFFF) << 2) | ((x >> 29) & 3)
                imm = imm - (1 << 21) if imm & (1 << 20) else imm
                val[rd] = ("a", (q & ~0xFFF) + (imm << 12))
            elif (x & 0xFF800000) == 0x91000000:                             # ADD x, imm
                v = val.get(rn)
                val.pop(rd, None)
                if v and v[0] == "a":
                    val[rd] = ("a", v[1] + (((x >> 10) & 0xFFF) << (12 * ((x >> 22) & 1))))
            elif (x & 0xFFE0FFE0) == 0xAA0003E0:                             # MOV x, x
                v = val.get((x >> 16) & 31)
                val.pop(rd, None)
                if v:
                    val[rd] = v
            elif (x & 0xFFC00000) == 0xF9400000:                             # LDR x, [xn, imm]
                v = val.get(rn)
                val.pop(rd, None)
                if v and v[0] == "a":
                    t = g.u64(v[1] + ((x >> 10) & 0xFFF) * 8)
                    if g.in_image(t):
                        val[rd] = ("a", t)
            elif (x & 0xFFC00000) == 0xF9000000:                             # STR x, [xn, imm]
                v, b = val.get(rd), val.get(rn)
                if v and v[0] == "p" and b and b[0] == "a":
                    slots[b[1] + ((x >> 10) & 0xFFF) * 8] = v[1]
            elif (x & 0xFC000000) == 0x94000000 or (x & 0xFFFFFC1F) == 0xD63F0000:   # BL, BLR
                arg = next((names[v[1]] for v in (val.get(1), val.get(0)) if v and v[0] == "a" and v[1] in names),
                           None)
                for k in range(19):
                    val.pop(k, None)
                if arg:
                    val[0] = ("p", arg)
            elif (x >> 25) & 5 == 4 and (x >> 22) & 1 == 0:                  # other stores
                pass
            elif (x >> 26) & 7 == 5:                                          # branches
                pass
            else:
                val.pop(rd, None)
    return slots


def short(name):
    """a demangled name made short: no return type, parameters or (anonymous namespace)"""
    name = name.replace("(anonymous namespace)::", "")
    return name


# ---- the stored readings: pseudo-C per function (the re agent's), keyed by its body hash


def store_path(g, kind, a):
    return os.path.join(g.cache, kind, "%08x.txt" % a)


def stored(g, kind, a):
    """(text, fresh) of the stored pseudo-C, or (None, False)"""
    p = store_path(g, kind, a)
    if not os.path.exists(p):
        return None, False
    head, _, text = open(p, encoding="utf-8").read().partition("\n")
    return text.rstrip("\n"), head.strip() == "body " + g.body_hash(a)


def store(g, kind, a, text):
    p = store_path(g, kind, a)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write("body %s\n%s\n" % (g.body_hash(a), text.strip("\n")))
    return p


SIGNATURE = regex.compile(r"^[A-Za-z_][\w \*]*?\b([a-z_][a-z0-9_]*)\s*\(")


def read_name(g, a):
    """the suggested name on the stored pseudo-C's signature line: (name, fresh), or (None, False) when unread"""
    t, fresh = stored(g, "pseudo", a)
    if not t:
        return None, False
    for line in t.split("\n"):
        m = SIGNATURE.match(line.strip())
        if m and not line.startswith((" ", "\t")):
            return m.group(1), fresh
    return "?", fresh


def read_tag(g, a):
    """'read: <name>' (stale: the body changed since) or None"""
    n, fresh = read_name(g, a)
    if n is None:
        return None
    return "read: %s%s" % (n, "" if fresh else ", stale")


def cmd_pseudo(g, args):
    a = fn_entry(g, args)
    if "--set" in args:
        k = args.index("--set")
        src = args[k + 1] if k + 1 < len(args) else "-"
        text = sys.stdin.read() if src == "-" else open(src, encoding="utf-8").read()
        print("re: pseudo-C stored: %s" % store(g, "pseudo", a, text))
        return
    t, fresh = stored(g, "pseudo", a)
    if not t:
        print("// no stored pseudo-C for %s (re fn for the listing; the re agent writes it)" % g.name(a))
        return
    o = Out(int(opt(args, "--lines", 200)))
    if not fresh:
        o("// (stale: the body changed since this was written)")
    o("// pseudo-C of %s @ %#x, written by the re agent from re fn (the cheap model's reading)" % (g.name(a), a))
    o(t)
    o.done()


# ---- field names (offset -> name, any struct: only for distinctive offsets)


def field_names(g):
    if g._fields is None:
        g._fields = {}
        p = os.path.join(g.cache, "fields.tsv")
        if os.path.exists(p):
            for line in open(p, encoding="utf-8"):
                q = line.rstrip("\n").split("\t")
                if len(q) >= 2:
                    g._fields[int(q[0], 16)] = q[1]
    return g._fields


def cmd_fieldname(g, args):
    if len(args) < 2:
        sys.exit("usage: re fieldname <offset> <name>")
    off = int(args[0], 0)
    p = os.path.join(g.cache, "fields.tsv")
    names = field_names(g)
    names[off] = args[1]
    with open(p, "w", encoding="utf-8") as f:
        for k in sorted(names):
            f.write("%x\t%s\n" % (k, names[k]))
    print("re: field %#x = %s" % (off, args[1]))


# ---- the listing


def listing(g, a, hide_name=False):
    pcs = g.body(a)
    ins = pseudo.decode(g, pcs)
    blocks, bmap = pseudo.build_blocks(g, a, pcs, ins)
    bname = {b.start: "B%d" % n for n, b in enumerate(blocks)}
    bl, b_ = g.callers(a)
    ptr = g.pointers(a)
    nm = "f_%08x" % a if hide_name else g.name(a)
    fields = field_names(g)
    out = ["// %s @ %#x: %d insns, %d blocks; %s; callers: %d bl, %d tail b, %d pointers; frame saves not shown" %
           (nm, a, len(pcs), len(blocks), ("args x0..x%d read first" % (g.callee_inputs(a) - 1)) if g.callee_inputs(a) else "no args read first", len(bl), len(b_), len(ptr))]
    if not hide_name and a in g.notes and g.notes[a]:
        out.append("// label note: " + g.notes[a][:300])
    own = None if hide_name else read_tag(g, a)
    if own:
        out.append("// %s (re pseudo)" % own)
    pages = {}
    procs = g.procs()
    held = {}       # reg -> ("slot", address) or ("proc", name): calls through a proc slot get its name
    for blk in sorted(blocks, key=lambda b: b.start):
        preds = ",".join(bname.get(p, "?") for p in sorted(blk.preds))
        succ = ",".join(bname.get(t, "?") for t in blk.succs)
        loop = " loop" if any(p >= blk.start for p in blk.preds) else ""
        out.append("%s %x:%s  <- %s  -> %s" % (bname[blk.start], blk.start, loop, preds or "entry", succ or "out"))
        skipped = 0
        for pc in blk.pcs:
            x = ins[pc]
            i = x.i
            if i is None:
                out.append("  %x  .word %#010x" % (pc, x.w))
                continue
            mn, ops = i.mnemonic, i.operands
            if frame_insn(i):
                skipped += 1
                continue
            if skipped and x.kind == "ret":
                out.append("  ..      (epilogue: %d restores)" % skipped)
                skipped = 0

            t = i.op_str
            note = ""
            got = None
            if x.kind in ("b", "cond", "bl") and x.target is not None:
                tg = x.target
                if tg in bname and x.kind != "bl" and (tg != a or x.kind == "cond"):
                    t = regex.sub(r"#0x[0-9a-f]+$", bname[tg], t)
                else:
                    r = g.resolve(tg)
                    t = regex.sub(r"#0x[0-9a-f]+$", g.name(r), t)
                    if x.kind != "bl":
                        note = "tail call"
                    n = read_tag(g, r) if r not in g.labels else None
                    if n:
                        note = (note + "; " if note else "") + n
            elif mn == "adrp":
                pages[ops[0].reg] = ops[1].imm
                t = "%s, %#x" % (i.reg_name(ops[0].reg), ops[1].imm)
            elif mn == "adr":
                note = g.addr_text(ops[1].imm)
            elif mn == "add" and len(ops) == 3 and ops[1].type == pseudo.A.ARM64_OP_REG and ops[2].type == pseudo.A.ARM64_OP_IMM:
                if ops[1].reg in pages:
                    note = g.addr_text(pages[ops[1].reg] + ops[2].imm)
                    if pages[ops[1].reg] + ops[2].imm in procs:
                        got = ("slot", pages[ops[1].reg] + ops[2].imm)
                elif ops[2].imm in fields and ops[2].imm >= 0x10:
                    note = "&." + fields[ops[2].imm]
            elif x.kind == "blr" and held.get(ops[0].reg, ("",))[0] == "proc":
                note = held[ops[0].reg][1]
            elif mn.startswith(("ld", "st")) and any(o.type == pseudo.A.ARM64_OP_MEM for o in ops):
                m = next(o for o in ops if o.type == pseudo.A.ARM64_OP_MEM)
                if m.mem.base in pages:
                    ea = pages[m.mem.base] + m.mem.disp
                    note = g.data_text(ea)
                    if mn == "ldr" and i.reg_name(ops[0].reg)[0] == "x":
                        got = ("proc", procs[ea]) if ea in procs else ("slot", g.u64(ea)) if g.u64(ea) in procs else None
                elif held.get(m.mem.base, ("",))[0] == "slot" and held[m.mem.base][1] + m.mem.disp in procs:
                    note = "pfn_" + procs[held[m.mem.base][1] + m.mem.disp]
                    if mn == "ldr":
                        got = ("proc", procs[held[m.mem.base][1] + m.mem.disp])
                elif m.mem.disp in fields and m.mem.disp >= 0x10:
                    note = "." + fields[m.mem.disp]
            elif mn.startswith("ldr") and len(ops) == 2 and ops[1].type == pseudo.A.ARM64_OP_IMM:
                size = 8 if i.reg_name(ops[0].reg)[0] in "xd" else 4
                v = int.from_bytes(g.read(ops[1].imm, size), "little")
                note = "= %#x" % v
                if size == 8 and g.in_image(v):
                    note += " " + g.addr_text(v)
            elif x.kind == "br" and pc in g.jt:
                cases = {}
                for n_, tg in enumerate(g.jt[pc][2]):
                    cases.setdefault(bname.get(tg, "%x" % tg), []).append(n_)
                note = "switch x%d: " % g.jt[pc][1] + " ".join("%s=%s" % (pseudo.ranges(v), k) for k, v in cases.items())
            if mn != "adrp":
                for o in ops:
                    if o.type == pseudo.A.ARM64_OP_REG and o.access & capstone_write():
                        pages.pop(o.reg, None)
                        held.pop(o.reg, None)
            if x.kind in ("bl", "blr"):
                for r in [r for r in held if not callee_saved(i.reg_name(r))]:
                    del held[r]
            elif got:
                held[ops[0].reg] = got
            out.append("  %x  %-7s %s%s" % (pc, mn, t, ("   ; " + note) if note else ""))
    return out


def callee_saved(r):
    return r in ("sp", "fp", "x29", "x30", "lr") or (r[0] == "x" and r[1:].isdigit() and 19 <= int(r[1:]) <= 28)


SAVED = {"x%d" % k for k in range(19, 29)} | {"fp", "lr", "x29", "x30"} | {"d%d" % k for k in range(8, 16)}


def frame_insn(i):
    """the prologue's and epilogue's own instructions: pointer authentication, the sp adjustment, saves of x19-x30/d8-d15
    (the frame pointer's set-up stays: locals are addressed through it)"""
    mn = i.mnemonic
    if mn in ("pacibsp", "paciasp", "autibsp", "autiasp", "bti"):
        return True
    ops = i.operands
    if mn in ("sub", "add") and len(ops) == 3 and ops[0].type == pseudo.A.ARM64_OP_REG and \
            i.reg_name(ops[0].reg) == "sp" and i.reg_name(ops[1].reg) == "sp" and ops[2].type == pseudo.A.ARM64_OP_IMM:
        return True
    if mn in ("stp", "ldp", "str", "ldr"):
        regs = [i.reg_name(o.reg) for o in ops if o.type == pseudo.A.ARM64_OP_REG]
        mem = [o for o in ops if o.type == pseudo.A.ARM64_OP_MEM]
        if regs and mem and i.reg_name(mem[0].mem.base) == "sp" and all(r in SAVED for r in regs):
            return True
    return False


def capstone_write():
    return 2      # (CS_AC_WRITE)


def cmd_fn(g, args):
    lines = int(opt(args, "--lines", 220))
    a = fn_entry(g, args)
    o = Out(lines, 16000)
    for line in listing(g, a, g.hide_name == a):
        o(line)
    o.done()


def cmd_tree(g, args):
    """the callee tree to a depth, each with its label, symbol or stored pseudo-C's name: what is still unread"""
    depth = int(opt(args, "--depth", 3))
    a = fn_entry(g, args)
    o = Out(int(opt(args, "--lines", 80)))
    seen = set()

    def go(f, d):
        _, calls = callees(g, f)
        for t in calls:
            if t is None:
                o("  " * d + "(indirect call)")
                continue
            t = g.resolve(t)
            known = t in g.labels or t in g.sym
            tag = "label" if t in g.labels else "sdk" if t in g.sym else read_tag(g, t) or "UNREAD"
            o("  " * d + "%x %s  [%s]%s" % (t, g.name(t), tag, " (above)" if t in seen else ""))
            if t in seen or known or d >= depth or not g.body(t):
                continue
            seen.add(t)
            go(t, d + 1)
    o("%x %s" % (a, g.name(a)))
    seen.add(a)
    go(a, 1)
    o.done()


# ---- commands


def fn_entry(g, args):
    a = g.parse_addr(args[0])
    if not g.body(a):
        f = g.func_of(a)
        if f is None:
            sys.exit("re: no function at %#x" % a)
        print("// %#x is in %s (+%#x)" % (a, g.name(f), a - f))
        a = f
    return a


def cmd_pc(g, args):
    lines = int(opt(args, "--lines", 150))
    a = g.parse_addr(args[0])
    if not g.body(a):
        f = g.func_of(a)
        if f is None:
            sys.exit("re: no function at %#x" % a)
        print("// %#x is in %s (+%#x)" % (a, g.name(f), a - f))
        a = f
    o = Out(lines)
    for line in fn_text(g, a):
        o(line)
    o.done()


def fn_text(g, a, hide_name=False):
    fn, body = pseudo.pseudo(g, a)
    bl, b = g.callers(a)
    ptr = g.pointers(a)
    nm = "f_%08x" % a if hide_name else g.name(a)
    out = ["// %s @ %#x: %d insns, %d blocks; callers: %d bl, %d tail b, %d pointers" %
           (nm, a, len(fn.pcs), len(fn.blocks), len(bl), len(b), len(ptr))]
    if not hide_name and a in g.notes and g.notes[a]:
        out.append("// label note: " + g.notes[a][:300])
    out.append("%s %s(%s) {" % ("u64" if fn.retval else "void", nm, ", ".join("x%d" % k for k in fn.args)))
    out.extend(body)
    out.append("}")
    return out


def opt(args, name, default):
    if name in args:
        k = args.index(name)
        v = args[k + 1]
        del args[k:k + 2]
        return v
    return default


def cmd_callers(g, args):
    a = g.parse_addr(args[0])
    bl, b = g.callers(a)
    ptr = g.pointers(a)
    o = Out(int(opt(args, "--lines", 60)))
    o("// %s @ %#x: %d bl, %d tail b, %d pointers" % (g.name(a), a, len(bl), len(b), len(ptr)))
    by = {}
    for kind, sites in (("bl", bl), ("b", b)):
        for s in sites:
            f = g.func_of(s)
            by.setdefault(f, []).append((kind, s))
    for f, sites in sorted(by.items(), key=lambda x: (-(len(x[1])), x[0] or 0)):
        nm = g.name(f) if f is not None else "?"
        o("%-10s %-40s %s" % ("%#x" % f if f else "?", nm, " ".join("%s@+%#x" % (k, s - (f or 0)) for k, s in sites[:6])
                             + (" ..." if len(sites) > 6 else "")))
    for p in ptr[:20]:
        o("ptr at %#x%s" % (p, "  (%s)" % g.where(p) if g.func_of(p) else ""))
    o.done("--lines N for more")


def callees(g, a):
    fn = pseudo.Fn(g, a)
    seen = {}
    for pc, t in fn.calls:
        seen.setdefault(t, []).append(pc)
    return fn, seen


def cmd_callees(g, args):
    a = g.parse_addr(args[0])
    fn, seen = callees(g, a)
    o = Out(int(opt(args, "--lines", 60)))
    o("// %s @ %#x calls %d functions (%d sites)" % (g.name(a), a, len(seen), len(fn.calls)))
    for t, pcs in seen.items():
        if t is None:
            o("indirect   (blr)                                    x%d at %s" % (len(pcs), ",".join("+%#x" % (p - a) for p in pcs[:6])))
            continue
        tag = "label" if t in g.labels else "sym" if t in g.sym else ""
        o("%-10s %-40s x%d %s" % ("%#x" % t, g.name(t), len(pcs), tag))
    o.done()


def cmd_xref(g, args):
    q = args[0]
    o = Out(int(opt(args, "--lines", 60)))
    targets = []
    try:
        if q in g.by_label or q in g.by_sym or regex.fullmatch(r"(0x|f_)?[0-9a-fA-F]{8,}", q):
            targets = [("addr", g.parse_addr(q))]
    except SystemExit:
        targets = []
    if not targets:
        needle = q.encode()
        mem = g.mem
        exact = b"\0" + needle + b"\0"
        data = mem.tobytes() if not hasattr(g, "_bytes") else g._bytes
        g._bytes = data
        i = data.find(exact)
        while i >= 0 and len(targets) < 8:
            targets.append(("string", g.base + i + 1))
            i = data.find(exact, i + 1)
        if not targets:
            i = data.find(needle)
            while i >= 0 and len(targets) < 8:
                s = data.rfind(b"\0", max(0, i - 200), i) + 1
                targets.append(("in string", g.base + s))
                i = data.find(needle, i + 1)
        if not targets:
            o("// %r: no such bytes in the image" % q)
    for kind, t in targets:
        refs = g.adrp_refs(t)
        ptr = g.pointers(t)
        o("// %s %#x %s: %d code refs, %d pointers" % (kind, t, g.string_at(t) or g.name(t), len(refs), len(ptr)))
        for pc, _ in refs[:30]:
            o("  %-10s %s" % ("%#x" % pc, g.where(pc)))
        for p in ptr[:10]:
            o("  ptr at %#x" % p)
        if kind == "addr" and t in g.entry_set:
            bl, b = g.callers(t)
            o("  (a function: %d bl, %d tail b callers; re callers)" % (len(bl), len(b)))
    o.done()


def cmd_field(g, args):
    allmods = "--all" in args
    args = [x for x in args if x != "--all"]
    o = Out(int(opt(args, "--lines", 50)))
    off = int(args[0], 0)
    hits = {}
    for name, _, _, lo, hi in g.modules:
        if not allmods and name != "main":
            continue
        w = np.frombuffer(g.mem[lo - g.base:hi - g.base].tobytes(), dtype="<u4").astype(np.int64)
        pc = lo + np.arange(len(w), dtype=np.int64) * 4
        rn = (w >> 5) & 31
        frame = (rn == 31) | (rn == 29)
        size = (w >> 30) & 3
        vbit = (w >> 26) & 1
        opc = (w >> 22) & 3
        # LDR/STR unsigned immediate
        m = ((w & 0x3B000000) == 0x39000000) & ~frame
        sc = np.where((vbit == 1) & ((opc & 2) == 2), 4, size)
        o1 = ((w >> 10) & 0xFFF) << sc
        sel = m & (o1 == off)
        # LDUR/STUR
        m2 = ((w & 0x3B200C00) == 0x38000000) & ~frame
        imm9 = (w >> 12) & 0x1FF
        imm9 = np.where(imm9 & 0x100, imm9 - 0x200, imm9)
        sel2 = m2 & (imm9 == off)
        # LDP/STP (signed offset and indexed)
        m3 = ((w & 0x3A000000) == 0x28000000) & ~frame
        imm7 = (w >> 15) & 0x7F
        imm7 = np.where(imm7 & 0x40, imm7 - 0x80, imm7)
        psz = np.where(vbit == 1, 4 << ((w >> 30) & 3), 4 << ((w >> 31) & 1))
        po = imm7 * psz
        sel3 = m3 & ((po == off) | (po + psz == off))
        # ADD immediate (the field's address)
        m4 = ((w & 0x7F800000) == 0x11000000) & ~frame
        o4 = ((w >> 10) & 0xFFF) << (12 * ((w >> 22) & 1))
        sel4 = m4 & (o4 == off) & (off != 0)
        for sel_, kind in ((sel, "ls"), (sel2, "ls"), (sel3, "pair"), (sel4, "add")):
            for p, x in zip(pc[sel_].tolist(), w[sel_].tolist()):
                if kind == "add":
                    k = "add"
                elif kind == "pair":
                    k = ("ldp" if (x >> 22) & 1 else "stp")
                else:
                    ld = ((x >> 22) & 3) != 0
                    bytes_ = 1 << (((x >> 30) & 3) if not ((x >> 26) & 1 and (x >> 23) & 1) else 4)
                    k = ("ld" if ld else "st") + str(bytes_ * 8)
                    if (x >> 26) & 1:
                        k += "f"
                hits.setdefault(g.func_of(p), []).append(k)
    o("// offset %#x: %d functions%s (ld/st<bits>, f float, ldp/stp, add = the address taken)" %
      (off, len(hits), "" if allmods else " in main"))
    for f, ks in sorted(hits.items(), key=lambda x: (-len(x[1]), x[0] or 0)):
        cnt = {}
        for k in ks:
            cnt[k] = cnt.get(k, 0) + 1
        o("%-10s %-40s %s" % ("%#x" % f if f else "?", g.name(f) if f else "?",
                             " ".join("%s x%d" % (k, n) if n > 1 else k for k, n in sorted(cnt.items()))))
    o.done()


def cmd_asm(g, args):
    a = g.parse_addr(args[0])
    n = int(args[1]) if len(args) > 1 else 16
    n = min(n, 200)
    o = Out(n + 4)
    data = g.read(a, 4 * n)
    f = g.func_of(a)
    o("// %s" % (g.where(a) if f is not None else "%#x" % a))
    regs = {}
    for k in range(n):
        pc = a + 4 * k
        w = int.from_bytes(data[4 * k:4 * k + 4], "little")
        ins = next(pseudo.MD.disasm(data[4 * k:4 * k + 4], pc), None)
        if ins is None:
            o("%#x  %08x  .word" % (pc, w))
            continue
        t = "%s %s" % (ins.mnemonic, ins.op_str)
        note = ""
        if ins.mnemonic in ("bl", "b") and ins.operands and ins.operands[0].type == pseudo.A.ARM64_OP_IMM:
            note = g.name(ins.operands[0].imm)
        elif ins.mnemonic == "adrp":
            regs[ins.operands[0].reg] = ins.operands[1].imm
        elif ins.mnemonic in ("add", "ldr", "str") and len(ins.operands) >= 2:
            op1 = ins.operands[1]
            if ins.mnemonic == "add" and op1.type == pseudo.A.ARM64_OP_REG and op1.reg in regs and len(ins.operands) == 3:
                v = regs[op1.reg] + ins.operands[2].imm
                note = g.addr_text(v)
            elif op1.type == pseudo.A.ARM64_OP_MEM and op1.mem.base in regs:
                note = g.data_text(regs[op1.mem.base] + op1.mem.disp)
        if pc in g.entry_set:
            o("%s:" % g.name(pc))
        o("%#x  %08x  %-40s%s" % (pc, w, t, ("  ; " + note) if note else ""))
    o.done()


def cmd_export(g, args):
    print(export(g.dir, g.game, opt(args, "--exefs", None)))


def main(argv):
    gamedir = os.environ.get("OUNCE_RE_GAME", "games/oot")
    exefs = None
    while argv and argv[0].startswith("--"):
        if argv[0] == "--game":
            gamedir, argv = argv[1], argv[2:]
        elif argv[0] == "--exefs":
            exefs, argv = argv[1], argv[2:]
        else:
            break
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return
    if not os.path.isabs(gamedir) and not os.path.exists(gamedir):
        gamedir = os.path.join(ROOT, gamedir)
    cmd, args = argv[0], argv[1:]
    if cmd == "export":
        print(export(gamedir, os.path.basename(os.path.normpath(gamedir)), exefs))
        return
    hide = ()
    if "--hide-label" in args:
        args.remove("--hide-label")
        hide = None
    g = Game(gamedir, exefs)
    if hide is None and args:
        a = g.parse_addr(args[0])
        g.labels.pop(a, None)
        g.by_label = {v: k for k, v in g.labels.items()}
        g.hide_name = a
        args[0] = "%x" % a
    if os.environ.get("OUNCE_RE_HIDE"):       # (the accuracy test: these functions keep no label)
        for x in os.environ["OUNCE_RE_HIDE"].split(","):
            a = int(x, 16)
            g.labels.pop(a, None)
        g.by_label = {v: k for k, v in g.labels.items()}
    cmds = {"fn": cmd_fn, "pc": cmd_pc, "pseudo": cmd_pseudo, "tree": cmd_tree,
            "fieldname": cmd_fieldname, "callers": cmd_callers, "callees": cmd_callees, "xref": cmd_xref, "field": cmd_field,
            "asm": cmd_asm}
    if cmd not in cmds or not args:
        sys.exit(__doc__)
    cmds[cmd](g, args)


if __name__ == "__main__":
    main(sys.argv[1:])
