"""The IR: typed SSA values in blocks, and the builder that keeps guest registers as SSA values.

Values are integers of a fixed width ("i1" ... "i128"), FP scalars ("f16" "f32" "f64") and vectors: "v128" (a guest V
register, LLVM <2 x i64>) and lane-typed views of it ("v4f32", "v16i8", "v2f32" for a 64-bit arrangement ...) reached
by bitcast. A block reads a guest register with get(); the first read of a
register the block has not written becomes a block parameter, and finish() gives every predecessor the value it must
pass (its own last write, or a parameter of its own, recursively): Braun et al.'s construction without the trivial-phi
removal, which LLVM does anyway. Guest registers are the variables "x0".."x30", "sp", the flags "n", "z", "c", "v"
(i1) and "v0".."v31" (v128). The IR has no notion of the guest context: the emitter loads the entry block's parameters and stores the written
registers at a return (the generic entry of milestone 1).

Operations (Value.op):
  const                  args (int,)
  param                  args (var,)  a block parameter
  add sub mul and or xor shl lshr ashr udiv sdiv     two operands of the value's type
  icmp                   attrs pred (eq ne ult ule ugt uge slt sle sgt sge), result i1
  select                 (cond, a, b)
  zext sext trunc        (a,), to the value's type
  ovf                    attrs kind (uadd sadd usub ssub): the overflow bit of a two-operand op, i1
  intr                   attrs name (ctlz bitreverse bswap fshr): an LLVM intrinsic on the value's type
  fadd fsub fmul fdiv     two operands of an FP or FP vector type; fneg (a,)
  fcmp                   attrs pred (LLVM's: oeq olt uno ...), result i1 or a vector of i1
  bitcast sitofp uitofp fptosi fptoui fpext fptrunc   (a,), to the value's type
  vconst                 args: the lane bits
  extract insert         (vec,) / (vec, value), attrs i: one lane
  shuffle                (a, b), attrs mask: shufflevector
  call                   args, attrs fn (an LLVM intrinsic or a runtime function), ctx (the context pointer first),
                         effects (not removable: FPCR/FPSR helpers)
  load                   (addr i64,), the value's type is the access width; attrs order (plain)
  store                  (addr i64, value), attrs mark (bool); a value of type "void"
  mark                   (base,), attrs lo, hi: the page marks of [base + lo, base + hi) (build_ir.group_marks)
  ctxload                attrs off: the context's 64-bit field at off (TPIDR_EL0, FPCR, ...)
  ctxstore               (value i64,), attrs off
  call                   (args...), attrs fn, ctx (bool: the context pointer first): a runtime function returning
                         the value's type ("void" for none); helper (an internal function emit_llvm.HELPERS defines)
  gcall                  attrs sym: a guest function through its generic entry `void sym(ptr ctx)` (BL); the
                         builder's after_call() follows
  icall                  (target i64,): BLR through the runtime's a64_icall(ctx, target); after_call() follows
  regload                attrs var: a guest register read from the context (after a call)
  poll                   the interrupt poll of a loop's back edge (emit_llvm's lift2_poll)
  flagsout               the flags written back to the context before control leaves for another function (a tail
                         call or indirect jump may enter code that reads them: a split function, a shared tail)
Terminators also: ("switch", index i64, [(case, block)], default block) for jump tables,
  ("tail", sym) a tail call of another function's generic entry, ("ijump", target) a BR the lift could not resolve.
  writeback              (value,), attrs var: the guest register var now holds value. The generic entry stores it
                         to the context there (write-through, so a return stores nothing and the entry loads only
                         the registers read before written; LLVM drops the stores a later one overwrites)
Terminators (Block.term): ("br", block), ("condbr", i1, block, block), ("ret",), ("trap", pc, word).
"""
import re


class _Width(dict):
    """bit width of a type name: integers ("i33" included: carry computations widen by one bit), FP scalars
    ("f16" "f32" "f64") and vectors ("v128", lane-typed "v4f32" "v8i16" "v2f32" ...)"""
    def __missing__(self, t):
        if t == "v128":
            w = 128
        elif t[0] == "v":
            n, _, e = re.fullmatch(r"v(\d+)(([if])\d+)", t).groups()
            w = int(n) * int(_[1:])
        else:
            w = int(t[1:])
        self[t] = w
        return w


WIDTH = _Width()
VARS = ["x%d" % i for i in range(31)] + ["sp", "n", "z", "c", "v"] + ["v%d" % i for i in range(32)]
VAR_TYPE = {v: ("i1" if v in ("n", "z", "c", "v") else "v128" if v[0] == "v" else "i64") for v in VARS}


def lanes(t):
    """(lane count, lane type) of a vector type: "v4f32" -> (4, "f32"); "v128" is (2, "i64")"""
    if t == "v128":
        return 2, "i64"
    m = re.fullmatch(r"v(\d+)([if]\d+)", t)
    return int(m.group(1)), m.group(2)


def vtype(n, lane):
    return "v%d%s" % (n, lane)


class Value:
    __slots__ = ("id", "ty", "op", "args", "attrs")

    def __init__(self, id, ty, op, args, attrs=None):
        self.id, self.ty, self.op, self.args, self.attrs = id, ty, op, args, attrs

    def __repr__(self):
        if self.op == "const":
            return "%s %d" % (self.ty, self.args[0])
        return "%%%d" % self.id


class Block:
    """A block at a guest address, or a block the lifter adds (key (kind, pc): a tail call's or an indirect jump's
    exit). called: a call in the block made every guest register the context's again (reads after it load)."""
    __slots__ = ("pc", "ops", "params", "defs", "term", "preds", "name", "called")

    def __init__(self, key):
        if isinstance(key, tuple):
            kind, pc = key
            self.name = "%s%08x" % (kind, pc)
        else:
            pc = key
            self.name = "b%08x" % pc
        self.pc, self.ops, self.params, self.defs, self.term, self.preds = pc, [], {}, {}, None, []
        self.called = False

    def succs(self):
        t = self.term
        if t[0] == "br":
            return [t[1]]
        if t[0] == "condbr":
            return [t[2], t[3]] if t[2] is not t[3] else [t[2]]
        if t[0] == "switch":
            out = []
            for blk in [t[3]] + [b for _, b in t[2]]:
                if blk not in out:
                    out.append(blk)
            return out
        return []


class Function:
    def __init__(self, entry):
        self.entry = entry
        self.name = "g_%08x" % entry   # (the LLVM symbol)
        self.blocks = {}       # pc -> Block, in creation order
        self.written = set()   # guest variables some block writes
        self._n = 0
        self.consts = {}

    def new_value(self, ty, op, args, attrs=None):
        self._n += 1
        return Value(self._n, ty, op, args, attrs)

    def block(self, pc):
        b = self.blocks.get(pc)
        if b is None:
            b = self.blocks[pc] = Block(pc)
        return b

    def entry_block(self):
        return self.blocks[self.entry]


class Builder:
    """Appends operations to one block at a time; guest registers through get()/set()."""

    def __init__(self, fn):
        self.fn = fn
        self.blk = None
        self.cmp = None   # (n, z, c, v, a, b, kind) of the last compare: cond() folds a condition on it into one icmp

    def at(self, block):
        self.blk = block
        self.cmp = None

    # ---- values
    def const(self, v, ty="i64"):
        if ty[0] == "v":   # (a vector type: v in every lane)
            return self.vconst(v, ty)
        v &= (1 << WIDTH[ty]) - 1
        key = (v, ty)
        c = self.fn.consts.get(key)
        if c is None:
            c = self.fn.consts[key] = self.fn.new_value(ty, "const", (v,))
        return c

    def op(self, op, ty, *args, **attrs):
        v = self.fn.new_value(ty, op, args, attrs or None)
        self.blk.ops.append(v)
        return v

    def get(self, var):
        b = self.blk
        v = b.defs.get(var)
        if v is None:
            if b.called:   # (after a call the context holds every register)
                v = self.op("regload", VAR_TYPE[var], var=var)
            else:
                v = b.params.get(var)
                if v is None:
                    v = b.params[var] = self.fn.new_value(VAR_TYPE[var], "param", (var,))
            b.defs[var] = v
        return v

    def after_call(self):
        """the callee may have changed any guest register: the block's later reads load them from the context"""
        self.blk.defs.clear()
        self.blk.called = True
        self.cmp = None

    def set(self, var, value):
        assert value.ty == VAR_TYPE[var], (var, value.ty)
        self.blk.defs[var] = value
        self.fn.written.add(var)
        self.op("writeback", "void", value, var=var)

    # ---- integer helpers (ty is the operation width: "i32" or "i64")
    def bin(self, op, a, b):
        assert a.ty == b.ty, (op, a.ty, b.ty)
        if a.op == "const" and b.op == "const":
            r = fold(op, a.args[0], b.args[0], WIDTH[a.ty])
            if r is not None:
                return self.const(r, a.ty)
        if op == "add" and b.op == "const" and a.op == "add" and a.args[1].op == "const":
            # (x + c1) + c2 -> x + (c1 + c2): an address chain (STP's second address, pre-index then offset)
            return self.bin("add", a.args[0], self.const(a.args[1].args[0] + b.args[0], a.ty))
        if op == "add" and b.op == "const" and b.args[0] == 0:
            return a
        return self.op(op, a.ty, a, b)

    def icmp(self, pred, a, b):
        return self.op("icmp", "i1", a, b, pred=pred)

    def select(self, c, a, b):
        if c.op == "const":
            return a if c.args[0] else b
        return self.op("select", a.ty, c, a, b)

    def zext(self, a, ty):
        if a.ty == ty:
            return a
        if a.op == "const":
            return self.const(a.args[0], ty)
        return self.op("zext", ty, a)

    def sext(self, a, ty):
        if a.ty == ty:
            return a
        if a.op == "const":
            w = WIDTH[a.ty]
            v = a.args[0]
            return self.const(v - (1 << w) if v >> (w - 1) & 1 else v, ty)
        return self.op("sext", ty, a)

    def trunc(self, a, ty):
        if a.ty == ty:
            return a
        if a.op == "const":
            return self.const(a.args[0], ty)
        if a.op == "zext" and a.args[0].ty == ty:
            return a.args[0]
        return self.op("trunc", ty, a)

    def intr(self, name, ty, *args):
        return self.op("intr", ty, *args, name=name)

    def not_(self, a):
        return self.bin("xor", a, self.const(-1, a.ty))

    # ---- vectors and FP. Constants of FP types are kept as their bits; a vector constant is a tuple of lane bits.
    def vconst(self, bits, ty):
        """a vector constant: bits is one value per lane (or one int, splat to every lane)"""
        n, lane = lanes(ty)
        if isinstance(bits, int):
            bits = (bits,) * n
        m = (1 << WIDTH[lane]) - 1
        key = (tuple(x & m for x in bits), ty)
        c = self.fn.consts.get(key)
        if c is None:
            c = self.fn.consts[key] = self.fn.new_value(ty, "vconst", key[0])
        return c

    def zero(self, ty):
        return self.vconst(0, ty) if ty[0] == "v" else self.const(0, ty)

    def bitcast(self, a, ty):
        if a.ty == ty:
            return a
        if a.op == "bitcast":
            return self.bitcast(a.args[0], ty)
        if a.op == "const" and ty[0] != "v":
            return self.const(a.args[0], ty)
        return self.op("bitcast", ty, a)

    def cast(self, op, a, ty):
        """sitofp uitofp fptosi fptoui fpext fptrunc (and the integer casts on vectors)"""
        return self.op(op, ty, a)

    def call(self, fn, ty, *args, **attrs):
        """a call of an LLVM intrinsic or a runtime function returning ty ("void" for none); attrs: ctx (the context
        pointer passed first), effects (the call reads or writes state: never removed or moved)"""
        return self.op("call", ty, *args, fn=fn, **attrs)

    def extract(self, vec, i):
        return self.op("extract", lanes(vec.ty)[1], vec, i=i)

    def insert(self, vec, val, i):
        assert lanes(vec.ty)[1] == val.ty, (vec.ty, val.ty)
        return self.op("insert", vec.ty, vec, val, i=i)

    def shuffle(self, a, b, mask):
        """shufflevector: lanes of a then b by index (-1: poison), the result has len(mask) lanes"""
        return self.op("shuffle", vtype(len(mask), lanes(a.ty)[1]), a, b, mask=tuple(mask))


def fold(op, a, b, w):
    m = (1 << w) - 1
    if op == "add":
        return (a + b) & m
    if op == "sub":
        return (a - b) & m
    if op == "and":
        return a & b
    if op == "or":
        return a | b
    if op == "xor":
        return a ^ b
    if op == "mul":
        return (a * b) & m
    if op == "shl":
        return (a << b) & m if b < w else None
    if op == "lshr":
        return a >> b if b < w else None
    return None


def finish(fn):
    """predecessor lists, and the values each predecessor passes for its successors' parameters"""
    for b in fn.blocks.values():
        b.preds = []
    for b in fn.blocks.values():
        for s in b.succs():
            s.preds.append(b)
    work = [(b, var) for b in fn.blocks.values() for var in b.params]
    while work:
        b, var = work.pop()
        for p in b.preds:
            if var not in p.defs:
                if p.called:
                    v = p.defs[var] = fn.new_value(VAR_TYPE[var], "regload", (), {"var": var})
                    p.ops.append(v)
                else:
                    p.params[var] = p.defs[var] = fn.new_value(VAR_TYPE[var], "param", (var,))
                    work.append((p, var))
