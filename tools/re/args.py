"""Call arguments for re fn: the callee's parameters (the NVN headers' proc typedefs, the SDK symbols' demangled
signatures) and the values x0-x7 and s/d0-7 hold at a call, as far as the listing tracks them in address order."""
import os
import re as regex
import shutil
import subprocess

from disasm import A, regkey

NVN_HEADER = os.path.join("renderer", "api", "nvn_api.h")
CLOBBER = set(range(19)) | {30} | set(range(32, 40)) | set(range(48, 64))
FLOAT_T = regex.compile(r"^(const\s+)?(float|double|f32|f64)$")
SHORT = {"commandBuffer": "cmd"}
TYPE_WORDS = {"const", "volatile", "unsigned", "signed", "struct", "enum", "int", "long", "short", "char", "bool",
              "float", "double", "void", "size_t"}


class Param:
    """one parameter: its name (None: the register's), float (s/d) or not, and width ('x'/'w'/'s'/'d')"""

    def __init__(self, name, width):
        self.name, self.width = name, width


def split_params(s):
    out, depth, cur = [], 0, ""
    for c in s:
        if c in "<([":
            depth += 1
        elif c in ">)]":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    if cur.strip():
        out.append(cur.strip())
    return [] if out in ([], ["void"]) else out


def classify_param(p, wide64):
    """Param of one C/C++ parameter text"""
    p = regex.sub(r"\s+", " ", p.strip())
    while "<" in p and regex.search(r"<[^<>]*>", p):
        p = regex.sub(r"<[^<>]*>", "", p)          # (template arguments)
    ptr = "*" in p or "&" in p
    words = regex.findall(r"[A-Za-z_][\w:]*", p)
    name = None
    if len(words) >= 2 and "::" not in words[-1] and words[-1] not in TYPE_WORDS and not p.endswith(("*", "&")):
        name = words[-1]
        words = words[:-1]
    ty = next((w for w in reversed(words) if w not in ("const", "volatile", "struct", "enum")), "")
    if not ptr and regex.search(r"\b(float)\b", p):
        return Param(name, "s")
    if not ptr and regex.search(r"\b(double)\b", p):
        return Param(name, "d")
    if ptr or ty in wide64 or regex.search(r"\b(long|size_t|u?int64_t|uintptr_t|intptr_t|ptrdiff_t)\b", p):
        width = "x"
    else:
        width = "w"
    if name is None:
        base = ty.split("::")[-1].split("<")[0]
        if base.startswith("NVN") and len(base) > 3:
            base = base[3:]
        nvn = ty.startswith("NVN") and ty != "NVNboolean"
        if base and (nvn or ptr and base not in TYPE_WORDS and not base.endswith("_t")
                     or base[:1].isupper() and len(base) > 2):
            name = SHORT.get(base[0].lower() + base[1:], base[0].lower() + base[1:])
    return Param(name, width)


def nvn_sigs(root):
    """proc name in upper case + PROC -> [Param], from the NVN header's PFN typedefs"""
    p = os.path.join(root, NVN_HEADER)
    if not os.path.exists(p):
        return {}
    text = open(p, encoding="utf-8").read()
    wide = set(regex.findall(r"typedef u?int64_t (NVN\w+);", text))
    out = {}
    for m in regex.finditer(r"typedef[^;()]*\(\*PFN(NVN\w+)PROC\)\s*\(([^;]*?)\);", text, regex.S):
        out[m.group(1)] = [classify_param(q, wide) for q in split_params(m.group(2))]
    return out


def demangle(names):
    if not names:
        return {}
    # (binutils' c++filt, else LLVM's: Windows has only the latter)
    tool = shutil.which("c++filt") or shutil.which("llvm-cxxfilt") or "c++filt"
    r = subprocess.run([tool], input="\n".join(names), capture_output=True, text=True)
    return dict(zip(names, r.stdout.split("\n")))


def cxx_sig(mangled, full):
    """[Param] from a demangled C++ signature (the implicit this when the scope looks like a class), or None"""
    if not mangled.startswith("_Z") or not full or full == mangled:
        return None
    t = regex.sub(r"\s*(const|volatile|&|&&|noexcept)?\s*$", "", full).rstrip()
    while t.endswith((" const", " volatile")):
        t = t.rsplit(" ", 1)[0]
    if not t.endswith(")"):
        return None
    depth, k = 0, len(t) - 1
    while k >= 0:
        depth += t[k] == ")"
        depth -= t[k] == "("
        if depth == 0:
            break
        k -= 1
    head, params = t[:k], split_params(t[k + 1:-1])
    if "..." in params:
        return None
    ps = [classify_param(q, set()) for q in params]
    scope = head
    depth, cut = 0, -1
    for j in range(len(head) - 1, 0, -1):
        c = head[j]
        depth += c == ">"
        depth -= c == "<"
        if depth == 0 and head[j - 1:j + 1] == "::":
            cut = j - 1
            break
    if cut > 0:
        scope = head[:cut].split("::")[-1].split("<")[0].strip()
        if scope[:1].isupper() or mangled.startswith("_ZNK"):
            ps.insert(0, Param("this", "x"))
    return ps


class Sigs:
    def __init__(self, g, root):
        self.g, self.root = g, root
        self._nvn = None
        self._cxx = {}

    def nvn(self, proc):
        if self._nvn is None:
            self._nvn = nvn_sigs(self.root)
        return self._nvn.get(proc.upper()) if proc.startswith("nvn") else None

    def prefetch(self, targets):
        need = [self.g.mangled[t] for t in targets if t in self.g.mangled and self.g.mangled[t] not in self._cxx]
        for m, full in demangle(sorted(set(need))).items():
            self._cxx[m] = cxx_sig(m, full)

    def of(self, t):
        m = self.g.mangled.get(t)
        if m is None:
            return None
        if m not in self._cxx:
            self.prefetch([t])
        return self._cxx.get(m)


class Regs:
    """register values in one path through the listing: key -> (text, deps, const); missing = unknown"""

    def __init__(self, entry=False, fixed=None):
        self.v, self.w, self.set = dict(fixed or {}), {}, set()
        if entry:
            for k in range(8):
                self.v[k] = ("arg%d" % k, frozenset(), None)

    def copy(self):
        r = Regs()
        r.v, r.w, r.set = dict(self.v), dict(self.w), set(self.set)
        return r

    @staticmethod
    def merge(states):
        r = states[0].copy()
        for s in states[1:]:
            for k in list(r.v):
                if s.v.get(k) != r.v[k]:
                    del r.v[k]
            r.set |= s.set
        return r

    def name(self, k, width=None):
        c = width or self.w.get(k, "x" if k < 32 else "d")
        return "%s%d" % (c, k if k < 32 else k - 32) if k != 31 else "sp"

    def text(self, g, k, width=None):
        x = self.v.get(k)
        if x is None:
            return self.name(k, width)
        t, _, c = x
        if c is not None:
            bits = 32 if width == "w" else 64
            c &= (1 << bits) - 1
            if g.in_image(c):
                return g.addr_text(c)
            if c >= (1 << bits) - 0x10000:
                return "-%#x" % ((1 << bits) - c)
            return str(c) if c < 10 else "%#x" % c
        return t

    def operand(self, g, k):
        """(text, deps) of a register read as a base or source"""
        if k is None:
            return "0", frozenset()
        if k == 31:
            return "sp", frozenset()
        x = self.v.get(k)
        if x is None:
            return self.name(k), frozenset([k])
        if x[2] is not None:
            return self.text(g, k), frozenset()
        if len(x[0]) > 40:
            return self.name(k), frozenset([k])
        return x[0], x[1]

    def put(self, k, val, width):
        self.kill(k)
        if val is not None:
            self.v[k] = val
        self.w[k] = width
        if k < 8 or 32 <= k < 40:
            self.set.add(k)

    def kill(self, k):
        self.v.pop(k, None)
        for q in [q for q, x in self.v.items() if k in x[1]]:
            del self.v[q]

    def call(self, ret):
        for k in CLOBBER:
            self.kill(k)
            self.w.pop(k, None)
        self.set = set()
        if ret:
            self.v[0] = (ret, frozenset(), None)

    def step(self, g, x):
        """one instruction's effect"""
        i = x.i
        if i is None or x.kind in ("bl", "blr", "b", "cond", "br", "ret"):
            return
        mn, ops = i.mnemonic, i.operands
        defs = sorted(k for k in x.defs if k != 64)
        if not defs:
            return
        d0 = ops[0] if ops and ops[0].type == A.ARM64_OP_REG else None
        dname = i.reg_name(d0.reg) if d0 is not None else ""
        dk = regkey(dname) if d0 is not None else None
        width = dname[:1] if dname else "x"
        val = None
        try:
            val = self.value(g, i, mn, ops, dk, width)
        except (IndexError, AttributeError):
            val = None
        if mn in ("ldp", "ldnp", "ldpsw") and len(ops) == 3 and ops[2].type == A.ARM64_OP_MEM and not i.writeback:
            size = {"x": 8, "d": 8, "w": 4, "s": 4, "q": 16}.get(width, 8)
            m = ops[2].mem
            b, deps = self.operand(g, regkey(i.reg_name(m.base)))
            k2 = regkey(i.reg_name(ops[1].reg))
            self.put(dk, (mem_text(b, m.disp), deps, None), width)
            self.put(k2, (mem_text(b, m.disp + size), deps, None), width)
            for k in defs:
                if k not in (dk, k2):
                    self.put(k, None, "x")
            return
        for k in defs:
            if k != dk:
                self.put(k, None, "x")
        if dk is not None and dk in defs:
            self.put(dk, val, width)

    def value(self, g, i, mn, ops, dk, width):
        if dk is None or dk == 31 or i.writeback:
            return None
        mask = 0xFFFFFFFF if width == "w" else 0xFFFFFFFFFFFFFFFF
        if mn in ("mov", "movz") and len(ops) == 2:
            if ops[1].type == A.ARM64_OP_IMM:
                return (None, frozenset(), (ops[1].imm << (ops[1].shift.value if ops[1].shift.type else 0)) & mask)
            if ops[1].type == A.ARM64_OP_REG and "." not in i.op_str:
                sk = regkey(i.reg_name(ops[1].reg))
                if sk is None:
                    return (None, frozenset(), 0)
                if sk >= 32 and dk < 32 or sk < 32 and dk >= 32:
                    return None
                x = self.v.get(sk)
                if x is not None:
                    return x
                return (i.reg_name(ops[1].reg), frozenset([sk]), None)
        if mn == "movn" and ops[1].type == A.ARM64_OP_IMM:
            return (None, frozenset(), ~(ops[1].imm << ops[1].shift.value) & mask)
        if mn == "movk" and ops[1].type == A.ARM64_OP_IMM:
            x = self.v.get(dk)
            if x is None or x[2] is None:
                return None
            sh = ops[1].shift.value if ops[1].shift.type else 0
            return (None, frozenset(), (x[2] & ~(0xFFFF << sh) | (ops[1].imm << sh)) & mask)
        if mn in ("adrp", "adr"):
            return (None, frozenset(), ops[1].imm)
        if mn in ("add", "sub") and len(ops) == 3 and ops[1].type == A.ARM64_OP_REG and ops[2].type == A.ARM64_OP_IMM:
            sk = regkey(i.reg_name(ops[1].reg))
            imm = ops[2].imm << (ops[2].shift.value if ops[2].shift.type else 0)
            imm = imm if mn == "add" else -imm
            x = self.v.get(sk)
            if x is not None and x[2] is not None:
                return (None, frozenset(), (x[2] + imm) & mask)
            b, deps = self.operand(g, sk)
            return (offset_text(b, imm), deps, None)
        if mn == "fmov" and len(ops) == 2 and ops[1].type == A.ARM64_OP_FP:
            return ("%g" % ops[1].fp, frozenset(), None)
        if mn.startswith(("ldr", "ldur", "ldapr", "ldar")) and len(ops) == 2:
            if ops[1].type == A.ARM64_OP_IMM:          # (a literal)
                size = 8 if width in "xd" else 4
                return (None, frozenset(), int.from_bytes(g.read(ops[1].imm, size), "little"))
            if ops[1].type != A.ARM64_OP_MEM:
                return None
            m = ops[1].mem
            bk = regkey(i.reg_name(m.base))
            x = self.v.get(bk)
            if x is not None and x[2] is not None and not m.index:
                return (g.data_text(x[2] + m.disp), frozenset(), None)
            b, deps = self.operand(g, bk)
            if m.index:
                ik = regkey(i.reg_name(m.index))
                t, d2 = self.operand(g, ik)
                sh = ops[1].shift.value if ops[1].shift.type else 0
                return ("[%s+%s%s]" % (b, t, "<<%d" % sh if sh else ""), deps | d2, None)
            return (mem_text(b, m.disp), deps, None)
        return None


SP_OFF = regex.compile(r"^sp(?:([+-])(0x[0-9a-f]+))?$")


def offset_text(b, imm):
    m = SP_OFF.match(b)
    if m and m.group(1):
        imm += int(m.group(2), 16) * (1 if m.group(1) == "+" else -1)
        b = "sp"
    if imm == 0:
        return b
    return "%s%s%#x" % (b, "+" if imm > 0 else "-", abs(imm))


def mem_text(b, disp):
    return "[%s]" % offset_text(b, disp)


def call_text(g, regs, params, target=None):
    """the argument list at a call: the callee's parameters, else the registers set since the previous call (at least
    as many as the callee reads before it writes them)"""
    parts = []
    if params is not None:
        ni, nf, seen = 0, 0, {}
        for p in params:
            if p.width in "sd":
                if nf >= 8:
                    continue
                k, nf = 32 + nf, nf + 1
            else:
                if ni >= 8:
                    continue
                k, ni = ni, ni + 1
            label = p.name or regs.name(k, p.width)
            seen[label] = seen.get(label, 0) + 1
            if seen[label] > 1:
                label += str(seen[label])
            parts.append("%s=%s" % (label, regs.text(g, k, p.width)))
        return "(%s)" % ", ".join(parts)
    ints = [k for k in regs.set if k < 8]
    n = max(ints) + 1 if ints else 0
    if target is not None:
        n = max(n, g.callee_inputs(target))
    fl = [k for k in regs.set if 32 <= k < 40]
    nf = max(fl) - 31 if fl else 0
    for k in range(n):
        parts.append("%s=%s" % (regs.name(k, "x"), regs.text(g, k, regs.w.get(k))))
    for k in range(32, 32 + nf):
        parts.append("%s=%s" % (regs.name(k), regs.text(g, k, regs.w.get(k))))
    return "(%s)" % ", ".join(parts)
