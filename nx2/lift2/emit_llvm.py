"""IR -> textual LLVM IR (one module of functions).

Milestone 1 emits the generic entry only: `void @g_<addr>(ptr %ctx)` loads the entry block's parameters from the
guest context, and every return stores the registers the function writes back to it. Context layout (rt/a64.h
GuestContext): x0-x30 and SP (x[31]) at 8 * n, N, Z, C, V as bytes at 0x108-0x10B.

Guest memory: `getelementptr i8, ptr inttoptr (i64 A64_HOST_BASE to ptr), i64 <guest address>`. A store that marks
its pages writes 1 (a release store, as A64_MARK) into a64_dirty at the 4 KB pages of its first and last byte.
"""
from .ir import WIDTH

HOST_BASE = 0x100000000000
DIRTY_PAGES = 1 << 27
CTX_OFF = {**{"x%d" % i: 8 * i for i in range(31)}, "sp": 248, "n": 0x108, "z": 0x109, "c": 0x10A, "v": 0x10B}
TRIPLE = "x86_64-pc-linux-gnu"
INTRINSIC = {"ctlz": "llvm.ctlz.%s(%s, i1 false)", "bitreverse": "llvm.bitreverse.%s(%s)", "bswap": "llvm.bswap.%s(%s)",
             "fshr": "llvm.fshr.%s(%s)"}
OVF = {"uadd": "uadd", "sadd": "sadd", "usub": "usub", "ssub": "ssub"}


class Module:
    def __init__(self):
        self.funcs = []
        self.decls = set()

    def add(self, fn):
        self.funcs.append(emit_function(fn, self.decls))

    def text(self):
        head = ['target triple = "%s"' % TRIPLE,
                "@a64_dirty = external global [%d x i8]" % DIRTY_PAGES]
        return "\n".join(head + sorted(self.decls) + self.funcs) + "\n"


def emit_function(fn, decls):
    out = []
    names = {}

    def ref(v):
        if v.op == "const":
            x = v.args[0]
            if v.ty == "i1":
                return "true" if x else "false"
            w = WIDTH[v.ty]
            return str(x - (1 << w) if x >> (w - 1) & 1 else x)
        return names.setdefault(v.id, "%%v%d" % v.id)

    def typed(v):
        return "%s %s" % (v.ty, ref(v))

    out.append("define void @%s(ptr noalias nocapture %%ctx) {" % fn.name)
    out.append("entry:")
    eb = fn.entry_block()
    ins = {}
    for var, p in eb.params.items():
        out.append("  %%in.%s.p = getelementptr i8, ptr %%ctx, i64 %d" % (var, CTX_OFF[var]))
        if p.ty == "i1":
            out.append("  %%in.%s.b = load i8, ptr %%in.%s.p, align 1" % (var, var))
            out.append("  %%in.%s = trunc i8 %%in.%s.b to i1" % (var, var))
        else:
            out.append("  %%in.%s = load i64, ptr %%in.%s.p, align 8" % (var, var))
        ins[var] = "%%in.%s" % var
    out.append("  br label %%%s" % eb.name)
    for blk in fn.blocks.values():
        out.append("%s:" % blk.name)
        for var, p in blk.params.items():
            inc = []
            if blk is eb:
                inc.append("[ %s, %%entry ]" % ins[var])
            for pr in blk.preds:
                inc.append("[ %s, %%%s ]" % (ref(pr.defs[var]), pr.name))
            out.append("  %s = phi %s %s" % (ref(p), p.ty, ", ".join(inc)))
        for v in blk.ops:
            emit_op(v, ref, typed, out, decls)
        t = blk.term
        if t[0] == "br":
            out.append("  br label %%%s" % t[1].name)
        elif t[0] == "condbr":
            if t[2] is t[3]:
                out.append("  br label %%%s" % t[2].name)
            else:
                out.append("  br i1 %s, label %%%s, label %%%s" % (ref(t[1]), t[2].name, t[3].name))
        elif t[0] == "ret":
            for k, (var, v) in enumerate(t[1].items()):
                p = "%%out.%s.%s" % (blk.name, var)
                out.append("  %s = getelementptr i8, ptr %%ctx, i64 %d" % (p, CTX_OFF[var]))
                if v.ty == "i1":
                    out.append("  %s.b = zext i1 %s to i8" % (p, ref(v)))
                    out.append("  store i8 %s.b, ptr %s, align 1" % (p, p))
                else:
                    out.append("  store i64 %s, ptr %s, align 8" % (ref(v), p))
            out.append("  ret void")
        elif t[0] == "trap":
            decls.add("declare void @a64_trap(ptr, i64, i32)")
            out.append("  call void @a64_trap(ptr %%ctx, i64 %d, i32 %d)" % (t[1], t[2]))
            out.append("  ret void")
    out.append("}")
    return "\n".join(out)


def emit_op(v, ref, typed, out, decls):
    op, a, r = v.op, v.args, ref(v)
    if op in ("add", "sub", "mul", "and", "or", "xor", "shl", "lshr", "ashr", "udiv", "sdiv"):
        out.append("  %s = %s %s, %s" % (r, op, typed(a[0]), ref(a[1])))
    elif op == "icmp":
        out.append("  %s = icmp %s %s, %s" % (r, v.attrs["pred"], typed(a[0]), ref(a[1])))
    elif op == "select":
        out.append("  %s = select i1 %s, %s, %s" % (r, ref(a[0]), typed(a[1]), typed(a[2])))
    elif op in ("zext", "sext", "trunc"):
        out.append("  %s = %s %s to %s" % (r, op, typed(a[0]), v.ty))
    elif op == "ovf":
        k = v.attrs["kind"]
        t = a[0].ty
        name = "llvm.%s.with.overflow.%s" % (OVF[k], t)
        decls.add("declare {%s, i1} @%s(%s, %s)" % (t, name, t, t))
        out.append("  %s.s = call {%s, i1} @%s(%s, %s)" % (r, t, name, typed(a[0]), typed(a[1])))
        out.append("  %s = extractvalue {%s, i1} %s.s, 1" % (r, t, r))
    elif op == "intr":
        name = v.attrs["name"]
        t = v.ty
        if name == "ctlz":
            decls.add("declare %s @llvm.ctlz.%s(%s, i1)" % (t, t, t))
            out.append("  %s = call %s @llvm.ctlz.%s(%s, i1 false)" % (r, t, t, typed(a[0])))
        else:
            decls.add("declare %s @llvm.%s.%s(%s)" % (t, name, t, ", ".join([t] * len(a))))
            out.append("  %s = call %s @llvm.%s.%s(%s)" % (r, t, name, t, ", ".join(typed(x) for x in a)))
    elif op == "load":
        out.append("  %s.p = getelementptr i8, ptr inttoptr (i64 %d to ptr), %s" % (r, HOST_BASE, typed(a[0])))
        out.append("  %s = load %s, ptr %s.p, align 1" % (r, v.ty, r))
    elif op == "store":
        p = "%%st%d" % v.id
        out.append("  %s.p = getelementptr i8, ptr inttoptr (i64 %d to ptr), %s" % (p, HOST_BASE, typed(a[0])))
        out.append("  store %s, ptr %s.p, align 1" % (typed(a[1]), p))
        if v.attrs["mark"]:
            last = WIDTH[a[1].ty] // 8 - 1
            for k, addr in enumerate((ref(a[0]), None)):
                if addr is None:
                    out.append("  %s.e = add i64 %s, %d" % (p, ref(a[0]), last))
                    addr = "%s.e" % p
                out.append("  %s.m%d = lshr i64 %s, 12" % (p, k, addr))
                out.append("  %s.i%d = and i64 %s.m%d, %d" % (p, k, p, k, DIRTY_PAGES - 1))
                out.append("  %s.d%d = getelementptr i8, ptr @a64_dirty, i64 %s.i%d" % (p, k, p, k))
                out.append("  store atomic i8 1, ptr %s.d%d release, align 1" % (p, k))
    else:
        raise ValueError("emit: op %s" % op)
