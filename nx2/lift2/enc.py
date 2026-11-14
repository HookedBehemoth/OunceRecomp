"""The AArch64 encodings lifter 2 decodes, as data, and the decoder built from them.

Each entry of ENCODINGS is (name, pattern, template, params, valid):
- pattern: the 32 bits from bit 31 down, as space-separated tokens: a run of literal bits ("100010") or a field
  ("Rn:5", "sf" for one bit). The literal bits make the entry's mask and value; the fields are extracted for it.
- template: the semantic template in sem_int.py that lifts it, with params (a dict) fixed for this entry; the fields
  give the rest. One template covers a family of encodings (all ADD/SUB immediate forms, all loads with an unsigned
  offset, ...).
- valid: a Python expression over the fields, false for the encodings the Arm ARM leaves unallocated inside the
  pattern (decoded as None, as the hardware would raise an undefined instruction).

Entries must not overlap: a word matches at most one valid entry (checked by tests/test_lift2.py). The decoder
dispatches on bits 28:25 (the Arm ARM's top-level split) and caches by word: the game has 2.0 M distinct words in 18 M.

Coverage is the integer subset of milestone 1: integer data processing, integer loads and stores (no atomics, no
acquire/release, no exclusives), branches, hints and PAC hints.
"""
import os
import re

# (name, pattern, template, params, valid)
ENCODINGS = [
    # ---- data processing, immediate
    ("ADR", "op immlo:2 10000 immhi:19 Rd:5", "adr", {}, "True"),
    ("ADDSUB_IMM", "sf op S 100010 sh imm12:12 Rn:5 Rd:5", "addsub_imm", {}, "True"),
    ("LOGIC_IMM", "sf opc:2 100100 N immr:6 imms:6 Rn:5 Rd:5", "logic_imm", {},
     "bitmask(N, imms, immr, 64 if sf else 32) is not None and (sf or not N)"),
    ("MOVEWIDE", "sf opc:2 100101 hw:2 imm16:16 Rd:5", "movewide", {}, "opc != 1 and (sf or hw < 2)"),
    ("BITFIELD", "sf opc:2 100110 N immr:6 imms:6 Rn:5 Rd:5", "bitfield", {},
     "opc != 3 and N == sf and (sf or (immr < 32 and imms < 32))"),
    ("EXTR", "sf 00 100111 N 0 Rm:5 imms:6 Rn:5 Rd:5", "extr", {}, "N == sf and (sf or imms < 32)"),
    # ---- data processing, register
    ("LOGIC_SHIFT", "sf opc:2 01010 shift:2 N Rm:5 imm6:6 Rn:5 Rd:5", "logic_shift", {}, "sf or imm6 < 32"),
    ("ADDSUB_SHIFT", "sf op S 01011 shift:2 0 Rm:5 imm6:6 Rn:5 Rd:5", "addsub_shift", {},
     "shift != 3 and (sf or imm6 < 32)"),
    ("ADDSUB_EXT", "sf op S 01011 00 1 Rm:5 option:3 imm3:3 Rn:5 Rd:5", "addsub_ext", {}, "imm3 <= 4"),
    ("ADDSUB_CARRY", "sf op S 11010000 Rm:5 000000 Rn:5 Rd:5", "addsub_carry", {}, "True"),
    ("CCMP_REG", "sf op 1 11010010 Rm:5 cond:4 0 0 Rn:5 0 nzcv:4", "ccmp", {"imm": False}, "True"),
    ("CCMP_IMM", "sf op 1 11010010 imm5:5 cond:4 1 0 Rn:5 0 nzcv:4", "ccmp", {"imm": True}, "True"),
    ("CSEL", "sf op 0 11010100 Rm:5 cond:4 0 o2 Rn:5 Rd:5", "csel", {}, "True"),
    ("DP2", "sf 0 0 11010110 Rm:5 opcode:6 Rn:5 Rd:5", "dp2", {}, "opcode in (2, 3, 8, 9, 10, 11)"),
    ("DP1", "sf 1 0 11010110 00000 opcode:6 Rn:5 Rd:5", "dp1", {},
     "opcode in (0, 1, 2, 4, 5) or (opcode == 3 and sf)"),
    ("DP3", "sf 00 11011 op31:3 Rm:5 o0 Ra:5 Rn:5 Rd:5", "dp3", {},
     "(op31 == 0) or (sf and op31 in (1, 5)) or (sf and op31 in (2, 6) and o0 == 0)"),
    # ---- loads and stores, integer registers (no atomics, acquire/release or exclusives yet)
    ("LDST_UIMM", "size:2 111 0 01 opc:2 imm12:12 Rn:5 Rt:5", "ldst", {"mode": "uimm"}, "ldst_ok(size, opc)"),
    ("LDST_UNSCALED", "size:2 111 0 00 opc:2 0 imm9:9 00 Rn:5 Rt:5", "ldst", {"mode": "unscaled"}, "ldst_ok(size, opc)"),
    ("LDST_POST", "size:2 111 0 00 opc:2 0 imm9:9 01 Rn:5 Rt:5", "ldst", {"mode": "post"},
     "ldst_ok(size, opc) and not (size == 3 and opc == 2)"),
    ("LDST_PRE", "size:2 111 0 00 opc:2 0 imm9:9 11 Rn:5 Rt:5", "ldst", {"mode": "pre"},
     "ldst_ok(size, opc) and not (size == 3 and opc == 2)"),
    ("LDST_UNPRIV", "size:2 111 0 00 opc:2 0 imm9:9 10 Rn:5 Rt:5", "ldst", {"mode": "unscaled"},
     "ldst_ok(size, opc) and not (size == 3 and opc == 2)"),
    ("LDST_REG", "size:2 111 0 00 opc:2 1 Rm:5 option:3 S 10 Rn:5 Rt:5", "ldst", {"mode": "reg"},
     "ldst_ok(size, opc) and option in (2, 3, 6, 7) and not (size == 3 and opc == 2 and Rt >= 24)"),
    ("LDST_PAIR", "opc:2 101 0 0 mode:2 L imm7:7 Rt2:5 Rn:5 Rt:5", "ldst_pair", {},
     "(opc == 0 or opc == 2 or (opc == 1 and L == 1 and mode != 0))"),
    ("LDR_LIT", "opc:2 011 0 00 imm19:19 Rt:5", "ldr_lit", {}, "True"),
    # ---- branches
    ("B", "op 00101 imm26:26", "b", {}, "True"),
    ("BCOND", "0101010 0 imm19:19 0 cond:4", "bcond", {}, "True"),
    ("CBZ", "sf 011010 op imm19:19 Rt:5", "cbz", {}, "True"),
    ("TBZ", "b5 011011 op b40:5 imm14:14 Rt:5", "tbz", {}, "True"),
    ("BR", "1101011 0 0 00 11111 000000 Rn:5 00000", "br", {}, "True"),
    ("BLR", "1101011 0 0 01 11111 000000 Rn:5 00000", "blr", {}, "True"),
    ("RET", "1101011 0 0 10 11111 000000 Rn:5 00000", "ret", {}, "True"),
    ("RETA", "1101011 0 0 10 11111 00001 M 11111 11111", "ret", {"pac": True}, "True"),
    # ---- hints (NOP, YIELD, WFE, the PAC hints PACIBSP/AUTIBSP and the others: no-ops here), UDF
    ("HINT", "11010101000000110010 CRm:4 op2:3 11111", "hint", {}, "True"),
    ("UDF", "0000000000000000 imm16:16", "udf", {}, "True"),
]


def ldst_ok(size, opc):
    """allocated integer load/store: opc 2 (sign-extend to 64, or PRFM at size 3) and opc 3 (sign-extend to 32)"""
    if opc == 3:
        return size < 2
    return True


def bitmask(n, imms, immr, width):
    """DecodeBitMasks (Arm ARM): the logical immediate, or None for a reserved encoding"""
    combined = (n << 6) | (~imms & 0x3F)
    if combined == 0:
        return None
    length = combined.bit_length() - 1
    if length < 1:
        return None
    esize = 1 << length
    if esize > width:
        return None
    levels = esize - 1
    s, r = imms & levels, immr & levels
    if s == levels:
        return None
    welem = (1 << (s + 1)) - 1
    elem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1)
    v = 0
    for i in range(width // esize):
        v |= elem << (i * esize)
    return v


class Entry:
    __slots__ = ("name", "mask", "value", "fields", "template", "params", "valid", "pattern")

    def __init__(self, name, pattern, template, params, valid, env=None):
        mask = value = 0
        fields = []
        bit = 32
        for tok in pattern.split():
            if re.fullmatch(r"[01]+", tok):
                for ch in tok:
                    bit -= 1
                    mask |= 1 << bit
                    value |= int(ch) << bit
            else:
                fname, _, w = tok.partition(":")
                w = int(w or 1)
                bit -= w
                fields.append((fname, bit, (1 << w) - 1))
        if bit != 0:
            raise ValueError("%s: pattern is %d bits" % (name, 32 - bit))
        self.name, self.mask, self.value, self.fields = name, mask, value, tuple(fields)
        self.template, self.params, self.pattern = template, params, pattern
        args = ", ".join(f for f, _, _ in fields)
        self.valid = eval("lambda %s: %s" % (args, valid), env or {"bitmask": bitmask, "ldst_ok": ldst_ok})

    def extract(self, w):
        return {f: (w >> s) & m for f, s, m in self.fields}


ENTRIES = []
_cache = {}
RENDER = {}   # entry name -> printer (asm.py's own printers are found by name; other tables bring theirs)
_BY_OP0 = [[] for _ in range(16)]
_ENV = {"bitmask": bitmask, "ldst_ok": ldst_ok}


def register(encodings, env=None, render=None):
    """Add a table of encodings (the format of ENCODINGS) to the decoder: this file's integer table, and every
    module enc_*.py of this package (ENCODINGS, and optionally ENV: names its validity expressions use, RENDER:
    printers by entry name). Entries of all tables must not overlap (tests/test_lift2.py)."""
    full_env = dict(_ENV, **(env or {}))
    for e in encodings:
        entry = Entry(*e, env=full_env)
        ENTRIES.append(entry)
        for op0 in range(16):   # (bits 28:25: an entry goes in every bucket its literal bits allow)
            if (op0 << 25) & entry.mask == entry.value & (0xF << 25) & entry.mask:
                _BY_OP0[op0].append(entry)
    RENDER.update(render or {})
    _cache.clear()


class Insn:
    """A decoded instruction: the entry and its fields (a dict)."""
    __slots__ = ("entry", "f", "word")

    def __init__(self, entry, f, word):
        self.entry, self.f, self.word = entry, f, word

    @property
    def name(self):
        return self.entry.name

    def __repr__(self):
        return "Insn(%s, %s)" % (self.entry.name, self.f)


def decode(w):
    """The Insn for a word, or None (not in the decoded subset, or unallocated)."""
    got = _cache.get(w, _cache)
    if got is not _cache:
        return got
    got = None
    for e in _BY_OP0[(w >> 25) & 0xF]:
        if w & e.mask == e.value:
            f = e.extract(w)
            if e.valid(**f):
                got = Insn(e, f, w)
                break
    _cache[w] = got
    return got


def matches(w):
    """Every valid entry a word matches (the overlap check)."""
    out = []
    for e in ENTRIES:
        if w & e.mask == e.value:
            f = e.extract(w)
            if e.valid(**f):
                out.append(e.name)
    return out


def _load_tables():
    import importlib
    import pkgutil
    register(ENCODINGS)
    for m in sorted(pkgutil.iter_modules([os.path.dirname(__file__)])):
        if m.name.startswith("enc_"):
            mod = importlib.import_module("." + m.name, __package__)
            register(mod.ENCODINGS, getattr(mod, "ENV", None), getattr(mod, "RENDER", None))


_load_tables()
