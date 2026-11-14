"""The IR: typed SSA values in blocks, and the builder that keeps guest registers as SSA values.

Values are integers of a fixed width ("i1" ... "i128"). A block reads a guest register with get(); the first read of a
register the block has not written becomes a block parameter, and finish() gives every predecessor the value it must
pass (its own last write, or a parameter of its own, recursively): Braun et al.'s construction without the trivial-phi
removal, which LLVM does anyway. Guest registers are the variables "x0".."x30", "sp" and the flags "n", "z", "c", "v"
(i1). The IR has no notion of the guest context: the emitter loads the entry block's parameters and stores the written
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
  load                   (addr i64,), the value's type is the access width; attrs order (plain)
  store                  (addr i64, value), attrs mark (bool); a value of type "void"
  mark                   (base,), attrs lo, hi: the page marks of [base + lo, base + hi) (build_ir.group_marks)
  writeback              (value,), attrs var: the guest register var now holds value. The generic entry stores it
                         to the context there (write-through, so a return stores nothing and the entry loads only
                         the registers read before written; LLVM drops the stores a later one overwrites)
Terminators (Block.term): ("br", block), ("condbr", i1, block, block), ("ret",), ("trap", pc, word).
"""

class _Width(dict):
    """bit width of an integer type name ("i33" included: carry computations widen by one bit)"""
    def __missing__(self, t):
        return int(t[1:])


WIDTH = _Width()
VARS = ["x%d" % i for i in range(31)] + ["sp", "n", "z", "c", "v"]
VAR_TYPE = {v: ("i1" if v in ("n", "z", "c", "v") else "i64") for v in VARS}


class Value:
    __slots__ = ("id", "ty", "op", "args", "attrs")

    def __init__(self, id, ty, op, args, attrs=None):
        self.id, self.ty, self.op, self.args, self.attrs = id, ty, op, args, attrs

    def __repr__(self):
        if self.op == "const":
            return "%s %d" % (self.ty, self.args[0])
        return "%%%d" % self.id


class Block:
    __slots__ = ("pc", "ops", "params", "defs", "term", "preds", "name")

    def __init__(self, pc):
        self.pc, self.ops, self.params, self.defs, self.term, self.preds = pc, [], {}, {}, None, []
        self.name = "b%08x" % pc

    def succs(self):
        t = self.term
        if t[0] == "br":
            return [t[1]]
        if t[0] == "condbr":
            return [t[2], t[3]] if t[2] is not t[3] else [t[2]]
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
            v = b.params.get(var)
            if v is None:
                v = b.params[var] = self.fn.new_value(VAR_TYPE[var], "param", (var,))
            b.defs[var] = v
        return v

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
                p.params[var] = p.defs[var] = fn.new_value(VAR_TYPE[var], "param", (var,))
                work.append((p, var))
