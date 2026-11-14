"""IR -> textual LLVM IR (one module of functions).

Milestone 1 emits the generic entry only: `void @g_<addr>(ptr %ctx)` loads the entry block's parameters from the
guest context, and every register write is also stored to it where it happens (the IR's writeback operations). Context layout (rt/a64.h
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
    """flags_out: the generic entry also stores N, Z, C, V to the context (for tests against Unicorn; the AAPCS64
    leaves the flags undefined at a return, so the build leaves them out)"""

    def __init__(self, flags_out=False):
        self.funcs = []
        self.decls = set()
        self.flags_out = flags_out

    def add(self, fn):
        self.funcs.append(emit_function(fn, self.decls, self.flags_out))
        self.defined = getattr(self, "defined", set()) | {fn.name}

    def text(self):
        head = ['target triple = "%s"' % TRIPLE,
                "@a64_dirty = external global [%d x i8]" % DIRTY_PAGES, MARK_FN]
        defined = getattr(self, "defined", set())
        decls = [d for d in sorted(self.decls)
                 if not (d.startswith("declare void @") and d.split("@")[1].split("(")[0] in defined)]
        helpers = [HELPERS[d[7:]] for d in decls if d.startswith("helper:")]
        decls = [d for d in decls if not d.startswith("helper:")]
        return "\n".join(head + decls + helpers + self.funcs) + "\n"


def emit_function(fn, decls, flags_out=False):
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
    # one address per context field the function touches (entry loads and write-through stores share it)
    used = set(eb.params)
    for blk in fn.blocks.values():
        for v in blk.ops:
            if v.op == "writeback" and (flags_out or v.attrs["var"] not in ("n", "z", "c", "v")):
                used.add(v.attrs["var"])
            elif v.op == "regload":
                used.add(v.attrs["var"])
            elif v.op == "flagsout":
                used.update("nzcv")
    for var in sorted(used, key=lambda x: CTX_OFF[x]):
        out.append("  %%cx.%s = getelementptr i8, ptr %%ctx, i64 %d" % (var, CTX_OFF[var]))
    for var, p in eb.params.items():
        if p.ty == "i1":
            out.append("  %%in.%s.b = load i8, ptr %%cx.%s, align 1" % (var, var))
            out.append("  %%in.%s = trunc i8 %%in.%s.b to i1" % (var, var))
        else:
            out.append("  %%in.%s = load i64, ptr %%cx.%s, align 8" % (var, var))
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
            if v.op == "writeback":
                var = v.attrs["var"]
                if var in ("n", "z", "c", "v") and not flags_out:
                    continue
                if v.args[0].ty == "i1":
                    out.append("  %%wb%d = zext i1 %s to i8" % (v.id, ref(v.args[0])))
                    out.append("  store i8 %%wb%d, ptr %%cx.%s, align 1" % (v.id, var))
                else:
                    out.append("  store i64 %s, ptr %%cx.%s, align 8" % (ref(v.args[0]), var))
                continue
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
            out.append("  ret void")
        elif t[0] == "switch":
            cases = " ".join("i64 %d, label %%%s" % (k, blk2.name) for k, blk2 in t[2])
            out.append("  switch i64 %s, label %%%s [ %s ]" % (ref(t[1]), t[3].name, cases))
        elif t[0] == "tail":
            decls.add("declare void @%s(ptr)" % t[1])
            out.append("  musttail call void @%s(ptr %%ctx)" % t[1])
            out.append("  ret void")
        elif t[0] == "ijump":
            decls.add("declare void @a64_ijump(ptr, i64)")
            out.append("  call void @a64_ijump(ptr %%ctx, %s)" % typed(t[1]))
            out.append("  ret void")
        elif t[0] == "trap":
            decls.add("declare void @a64_trap(ptr, i64, i32)")
            out.append("  call void @a64_trap(ptr %%ctx, i64 %d, i32 %d)" % (t[1], t[2]))
            out.append("  ret void")
    out.append("}")
    return "\n".join(out)


MARK_FN = """define internal void @mark(i64 %a, i64 %b) alwaysinline {
  %m0 = lshr i64 %a, 12
  %i0 = and i64 %m0, DIRTY_MASK
  %d0 = getelementptr i8, ptr @a64_dirty, i64 %i0
  store atomic i8 1, ptr %d0 release, align 1
  %m1 = lshr i64 %b, 12
  %i1 = and i64 %m1, DIRTY_MASK
  %d1 = getelementptr i8, ptr @a64_dirty, i64 %i1
  store atomic i8 1, ptr %d1 release, align 1
  ret void
}""".replace("DIRTY_MASK", str(DIRTY_PAGES - 1))


# internal functions the IR's call/poll operations use, defined in each module that needs them
HELPERS = {
    # CRC32 over the low nbits of data, bit-reflected (the Arm ARM's CRC32 with the polynomial already reflected)
    "lift2_crc32": """define internal i32 @lift2_crc32(i32 %crc, i64 %data, i32 %nbits, i32 %poly) alwaysinline {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %c = phi i32 [ %crc, %entry ], [ %c1, %loop ]
  %d = phi i64 [ %data, %entry ], [ %d1, %loop ]
  %dl = trunc i64 %d to i32
  %x = xor i32 %c, %dl
  %bit = and i32 %x, 1
  %m = sub i32 0, %bit
  %p = and i32 %m, %poly
  %cs = lshr i32 %c, 1
  %c1 = xor i32 %cs, %p
  %d1 = lshr i64 %d, 1
  %i1 = add i32 %i, 1
  %more = icmp ult i32 %i1, %nbits
  br i1 %more, label %loop, label %done
done:
  ret i32 %c1
}""",
    # the loop back edge's interrupt poll (nx2/poll.py): a64_poll_cur is called with nothing, so it cannot touch the
    # context (noalias) and the loop's registers stay in host registers; it may read and write guest memory
    "lift2_poll": """declare void @a64_poll_cur()
define internal void @lift2_poll(ptr %ctx) alwaysinline {
entry:
  %p = getelementptr i8, ptr %ctx, i64 856
  %v = load atomic i32, ptr %p monotonic, align 4
  %set = icmp ne i32 %v, 0
  br i1 %set, label %slow, label %done
slow:
  store atomic i32 0, ptr %p monotonic, align 4
  call void @a64_poll_cur()
  br label %done
done:
  ret void
}""",
}


def emit_mark(p, base, lo, hi, out):
    """A64_MARK: 1 into a64_dirty at the pages of base + lo and base + hi - 1, as release stores (@mark, inlined)"""
    out.append("  %s.a = add i64 %s, %d" % (p, base, lo))
    out.append("  %s.b = add i64 %s, %d" % (p, base, hi - 1))
    out.append("  call void @mark(i64 %s.a, i64 %s.b)" % (p, p))


def emit_op(v, ref, typed, out, decls, flags_out=False):
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
            emit_mark(p, ref(a[0]), 0, WIDTH[a[1].ty] // 8, out)
    elif op == "mark":
        emit_mark("%%mk%d" % v.id, ref(a[0]), v.attrs["lo"], v.attrs["hi"], out)
    elif op == "ctxload":
        out.append("  %s.p = getelementptr i8, ptr %%ctx, i64 %d" % (r, v.attrs["off"]))
        out.append("  %s = load i64, ptr %s.p, align 8" % (r, r))
    elif op == "ctxstore":
        p = "%%cs%d" % v.id
        out.append("  %s = getelementptr i8, ptr %%ctx, i64 %d" % (p, v.attrs["off"]))
        out.append("  store %s, ptr %s, align 8" % (typed(a[0]), p))
    elif op == "regload":
        var = v.attrs["var"]
        if v.ty == "i1":
            out.append("  %s.b = load i8, ptr %%cx.%s, align 1" % (r, var))
            out.append("  %s = trunc i8 %s.b to i1" % (r, r))
        else:
            out.append("  %s = load i64, ptr %%cx.%s, align 8" % (r, var))
    elif op == "call":
        fn, at = v.attrs["fn"], v.attrs
        args = (["ptr %ctx"] if at.get("ctx") else []) + [typed(x) for x in a]
        tys = (["ptr"] if at.get("ctx") else []) + [x.ty for x in a]
        decls.add(("helper:%s" % fn) if at.get("helper") else "declare %s @%s(%s)" % (v.ty, fn, ", ".join(tys)))
        lhs = "" if v.ty == "void" else "%s = " % r
        out.append("  %scall %s @%s(%s)" % (lhs, v.ty, fn, ", ".join(args)))
    elif op == "gcall":
        decls.add("declare void @%s(ptr)" % v.attrs["sym"])
        out.append("  call void @%s(ptr %%ctx)" % v.attrs["sym"])
    elif op == "icall":
        decls.add("declare void @a64_icall(ptr, i64)")
        out.append("  call void @a64_icall(ptr %%ctx, %s)" % typed(a[0]))
    elif op == "poll":
        decls.add("helper:lift2_poll")
        out.append("  call void @lift2_poll(ptr %ctx)")
    elif op == "flagsout":
        if not flags_out:   # (with flags_out every flag write is stored already)
            for k, var in enumerate("nzcv"):
                out.append("  %%fo%d.%s = zext i1 %s to i8" % (v.id, var, ref(a[k])))
                out.append("  store i8 %%fo%d.%s, ptr %%cx.%s, align 1" % (v.id, var, var))
    else:
        raise ValueError("emit: op %s" % op)
