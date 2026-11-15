"""IR -> textual LLVM IR (one module of functions).

Milestone 1 emits the generic entry only: `void @g_<addr>(ptr %ctx)` loads the entry block's parameters from the
guest context, and every register write is also stored to it where it happens (the IR's writeback operations). Context layout (rt/a64.h
GuestContext): x0-x30 and SP (x[31]) at 8 * n, N, Z, C, V as bytes at 0x108-0x10B, V0-V31 at 0x110 + 16 * n.
Vector and FP values are LLVM vectors and FP scalars (lltype); V registers are <2 x i64> in the context.

Guest memory: `getelementptr i8, ptr inttoptr (i64 A64_HOST_BASE to ptr), i64 <guest address>`.
"""
import re

from . import fp_estimate
from .ir import VARS, WIDTH, lanes

VARS_BY_BIT = {v: k for k, v in enumerate(VARS) if v not in ("n", "z", "c", "v")}

HOST_BASE = 0x100000000000
CTX_OFF = {**{"x%d" % i: 8 * i for i in range(31)}, "sp": 248, "n": 0x108, "z": 0x109, "c": 0x10A, "v": 0x10B,
           **{"v%d" % i: 0x110 + 16 * i for i in range(32)}}
FP_TYPES = {"f16": "half", "f32": "float", "f64": "double"}


def lltype(t):
    """the LLVM spelling of an IR type: v128 is <2 x i64>, "v4f32" <4 x float>, "f64" double"""
    if t[0] == "v":
        n, lane = lanes(t)
        return "<%d x %s>" % (n, FP_TYPES.get(lane, lane))
    return FP_TYPES.get(t, t)


def const_text(ty, x):
    """a constant of any IR type; FP and FP vector constants as the bit pattern, bitcast (exact, NaNs included)"""
    if ty[0] == "v":
        n, lane = lanes(ty)
        it = "i%d" % WIDTH[lane]
        if not any(x):
            return "zeroinitializer"
        iv = "<%d x %s> <%s>" % (n, it, ", ".join("%s %s" % (it, int_text(v, WIDTH[lane])) for v in x))
        return "bitcast (%s to %s)" % (iv, lltype(ty)) if lane[0] == "f" else iv[len("<%d x %s> " % (n, it)):]
    if ty in FP_TYPES:
        return "bitcast (i%d %s to %s)" % (WIDTH[ty], int_text(x[0], WIDTH[ty]), FP_TYPES[ty])
    if ty == "i1":
        return "true" if x[0] else "false"
    return int_text(x[0], WIDTH[ty])


def int_text(x, w):
    return str(x - (1 << w) if x >> (w - 1) & 1 else x)
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
        head = ['target triple = "%s"' % TRIPLE]
        defined = getattr(self, "defined", set())
        decls = [d for d in sorted(self.decls)
                 if not (d.startswith("declare void @") and d.split("@")[1].split("(")[0] in defined)]
        helpers = [no_capture(HELPERS[d[7:]]) for d in decls if d.startswith("helper:")]
        decls = [no_capture(d) for d in decls if not d.startswith("helper:")]
        return "\n".join(head + decls + helpers + self.funcs) + "\n"


# Every function the lifted code calls with the context (the lifted functions, the runtime's, the host's: A64_NOESC in
# rt/a64.h) keeps no copy of the pointer: `captures(none)` on that parameter. Without it the context escapes at the
# first call, and LLVM must assume any guest load or store (an address made from an integer) may read it, so it keeps
# context stores that a later store overwrites before anything reads them.
CTX_PARAM = re.compile(r"^declare (\S+) @(?!llvm\.)([\w.$]+)\(ptr(?=[,)])", re.M)


def no_capture(decl):
    """a declaration whose first parameter is the context, with captures(none) on it"""
    return CTX_PARAM.sub(r"declare \1 @\2(ptr captures(none)", decl)


def emit_function(fn, decls, flags_out=False):
    out = []
    names = {}

    def ref(v):
        if v.op in ("const", "vconst"):
            return const_text(v.ty, v.args)
        if v.op == "codeaddr":   # (a function's host pointer as the game's code pointer: lift2/code_ptrs.py)
            decls.add("declare void @f_%08x(ptr)" % v.args[0])
            return "ptrtoint (ptr @f_%08x to i64)" % v.args[0]
        return names.setdefault(v.id, "%%v%d" % v.id)

    def typed(v):
        return "%s %s" % (lltype(v.ty), ref(v))

    strict = getattr(fn, "strict_fp", False)
    tf = getattr(fn, "target_features", None)   # (AES-NI, PCLMUL: sem_crypto, sem_neon's PMULL 1Q)
    out.append("define void @%s(ptr noalias nocapture %%ctx)%s%s {" % (fn.name, " strictfp" if strict else "",
               ' "target-features"="%s"' % ",".join(sorted(tf)) if tf else ""))
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
            elif v.op == "sitepoison":
                used.update(var for var in VARS_BY_BIT if v.attrs["mask"] >> VARS_BY_BIT[var] & 1)
    for var in sorted(used, key=lambda x: CTX_OFF[x]):
        out.append("  %%cx.%s = getelementptr i8, ptr %%ctx, i64 %d" % (var, CTX_OFF[var]))
    for var, p in eb.params.items():
        if p.ty == "i1":
            out.append("  %%in.%s.b = load i8, ptr %%cx.%s, align 1" % (var, var))
            out.append("  %%in.%s = trunc i8 %%in.%s.b to i1" % (var, var))
        else:
            out.append("  %%in.%s = load %s, ptr %%cx.%s, align 8" % (var, lltype(p.ty), var))
        ins[var] = "%%in.%s" % var
    for blk in fn.blocks.values():   # (an At row's io struct: one slot block each, in the entry, nx2/lift2/probes.py)
        for v in blk.ops:
            if v.op == "rowio":
                out.append("  %%io%d = alloca [%d x i8], align 16" % (v.id, io_size(v)))
    out.append("  br label %%%s" % eb.name)
    for blk in fn.blocks.values():
        out.append("%s:" % blk.name)
        for var, p in blk.params.items():
            inc = []
            if blk is eb:
                inc.append("[ %s, %%entry ]" % ins[var])
            for pr in blk.preds:
                # (one entry per edge: a switch's cases may share a block)
                t = pr.term
                n = sum(b is blk for _, b in t[2]) + (t[3] is blk) if t[0] == "switch" else 1
                inc += ["[ %s, %%%s ]" % (ref(pr.defs[var]), pr.name)] * n
            out.append("  %s = phi %s %s" % (ref(p), lltype(p.ty), ", ".join(inc)))
        for v in blk.ops:
            if v.op == "writeback":
                var = v.attrs["var"]
                if var in ("n", "z", "c", "v") and not flags_out:
                    continue
                if v.args[0].ty == "i1":
                    out.append("  %%wb%d = zext i1 %s to i8" % (v.id, ref(v.args[0])))
                    out.append("  store i8 %%wb%d, ptr %%cx.%s, align 1" % (v.id, var))
                else:
                    out.append("  store %s, ptr %%cx.%s, align 8" % (typed(v.args[0]), var))
                continue
            if strict and emit_strict(v, ref, typed, out, decls):
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
            # (a host function pointer, as code pointers are: a tail call of it; a guest address: the runtime's path,
            # which counts it, lift2/code_ptrs.py)
            n = blk.name
            decls.add("declare void @a64_ijump_guest(ptr, i64, i64)")
            decls.add("declare i1 @llvm.expect.i1(i1, i1)")
            out.append("  %%%s.hi = lshr i64 %s, 32" % (n, ref(t[1])))
            out.append("  %%%s.h = icmp ne i64 %%%s.hi, 0" % (n, n))
            out.append("  %%%s.e = call i1 @llvm.expect.i1(i1 %%%s.h, i1 true)" % (n, n))
            out.append("  br i1 %%%s.e, label %%%s.host, label %%%s.guest" % (n, n, n))
            out.append("%s.host:" % n)
            out.append("  %%%s.f = inttoptr i64 %s to ptr" % (n, ref(t[1])))
            out.append("  musttail call void %%%s.f(ptr %%ctx)" % n)
            out.append("  ret void")
            out.append("%s.guest:" % n)
            out.append("  call void @a64_ijump_guest(ptr %%ctx, %s, i64 %d)" % (typed(t[1]), t[2]))
            out.append("  ret void")
        elif t[0] == "noreturn":   # (a call of a callee the summaries prove never returns came back)
            decls.add("declare void @a64_trap(ptr, i64, i32)")
            out.append("  call void @a64_trap(ptr %%ctx, i64 %d, i32 %d)" % (t[1], t[2]))
            out.append("  unreachable")
        elif t[0] == "trap":
            decls.add("declare void @a64_trap(ptr, i64, i32)")
            out.append("  call void @a64_trap(ptr %%ctx, i64 %d, i32 %d)" % (t[1], t[2]))
            out.append("  ret void")
    out.append("}")
    if strict:   # (every call of a strictfp function is strictfp: it may change the FP environment)
        out = [l + " strictfp" if re.match(r"\s+(%\S+ = )?call ", l) and not l.endswith("strictfp") else l
               for l in out]
    return "\n".join(out)


# internal functions the IR's call operations use, defined in each module that needs them
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
    # LDXP's low register through the runtime's pair monitor (its high one is the context's excl_hi afterwards)
    "lift2_ldxp_lo": """declare void @recomp_ldxp(ptr, i64, i32, ptr, ptr)
define internal i64 @lift2_ldxp_lo(ptr %ctx, i64 %a, i32 %n) alwaysinline {
  %lo = alloca i64
  %hi = alloca i64
  call void @recomp_ldxp(ptr %ctx, i64 %a, i32 %n, ptr %lo, ptr %hi)
  %v = load i64, ptr %lo
  ret i64 %v
}""",
}

# BLR: code pointers are host function pointers (lift2/code_ptrs.py; guest code addresses are below 4 GB, host code
# above), called directly; a guest address goes to the runtime, which counts it per site (rt/a64_rt.c)
HELPERS["lift2_icall"] = """define internal void @lift2_icall(ptr %ctx, i64 %t, i64 %site) alwaysinline {
  %hi = lshr i64 %t, 32
  %h = icmp ne i64 %hi, 0
  %e = call i1 @llvm.expect.i1(i1 %h, i1 true)
  br i1 %e, label %host, label %guest
host:
  %f = inttoptr i64 %t to ptr
  call void %f(ptr %ctx)
  ret void
guest:
  call void @a64_icall_guest(ptr %ctx, i64 %t, i64 %site)
  ret void
}"""

# FRECPE and FRSQRTE, inline and bit-exact (nx2/lift2/fp_estimate.py), and the declarations they need
HELPER_DECLS = {"lift2_icall": ("declare void @a64_icall_guest(ptr, i64, i64)", "declare i1 @llvm.expect.i1(i1, i1)")}
for _name, (_text, _decls) in fp_estimate.helpers().items():
    HELPERS[_name] = _text
    HELPER_DECLS[_name] = _decls


# A function that writes FPCR (MSR FPCR: Function.strict_fp, sem_fp.fp_sysreg_write) is strictfp: its FP operations
# are LLVM's constrained intrinsics with a dynamic rounding mode, which LLVM does not move across the FPCR write (a
# call) or evaluate in another FP environment. FP operations that do not depend on the mode (fneg, fabs, bit moves)
# stay plain.
STRICT_BIN = {"fadd", "fsub", "fmul", "fdiv"}
STRICT_ROUNDED = {"fma", "sqrt", "rint", "nearbyint"}                 # (take the rounding mode)
STRICT_EXACT = {"floor", "ceil", "trunc", "round", "roundeven", "maximum", "minimum"}
EXC = 'metadata !"fpexcept.ignore"'
RND = 'metadata !"round.dynamic"'


def emit_strict(v, ref, typed, out, decls):
    """v as a constrained FP intrinsic, if it is one of the operations that depend on the FP environment"""
    op, a, r = v.op, v.args, ref(v)
    if op in STRICT_BIN:
        name = "llvm.experimental.constrained.%s.%s" % (op, v.ty)
        args = [typed(x) for x in a] + [RND, EXC]
        tys = [lltype(x.ty) for x in a] + ["metadata", "metadata"]
    elif op == "fcmp":
        name = "llvm.experimental.constrained.fcmp.%s" % a[0].ty
        args = [typed(x) for x in a] + ['metadata !"%s"' % v.attrs["pred"], EXC]
        tys = [lltype(x.ty) for x in a] + ["metadata", "metadata"]
    elif op in ("sitofp", "uitofp", "fptrunc", "fpext"):
        name = "llvm.experimental.constrained.%s.%s.%s" % (op, v.ty, a[0].ty)
        args = [typed(a[0])] + ([RND] if op != "fpext" else []) + [EXC]
        tys = [lltype(a[0].ty)] + ["metadata"] * (len(args) - 1)
    elif op == "call" and v.attrs["fn"].startswith("llvm.") and v.attrs["fn"].split(".")[1] in \
            STRICT_ROUNDED | STRICT_EXACT:
        base, _, sfx = v.attrs["fn"][5:].partition(".")
        name = "llvm.experimental.constrained.%s.%s" % (base, sfx)
        args = [typed(x) for x in a] + ([RND] if base in STRICT_ROUNDED else []) + [EXC]
        tys = [lltype(x.ty) for x in a] + ["metadata"] * (len(args) - len(a))
    else:
        return False
    decls.add("declare %s @%s(%s)" % (lltype(v.ty), name, ", ".join(tys)))
    out.append("  %s = call %s @%s(%s) strictfp" % (r, lltype(v.ty), name, ", ".join(args)))
    return True


CASTS = ("zext", "sext", "trunc", "bitcast", "sitofp", "uitofp", "fptosi", "fptoui", "fpext", "fptrunc")


def io_slots(v):
    """[(offset, size)] of a rowio's slots: 8 bytes for an X register, 16 for a vector, at their alignment"""
    out, off = [], 0
    for x in v.args[v.attrs["nargs"]:]:
        size = 16 if x.ty[0] == "v" else 8
        off = (off + size - 1) // size * size
        out.append((off, size))
        off += size
    return out


def io_size(v):
    off, size = io_slots(v)[-1]
    return off + size


def emit_op(v, ref, typed, out, decls, flags_out=False):
    op, a, r = v.op, v.args, ref(v)
    if op in ("add", "sub", "mul", "and", "or", "xor", "shl", "lshr", "ashr", "udiv", "sdiv", "fadd", "fsub", "fmul",
              "fdiv"):
        out.append("  %s = %s %s, %s" % (r, op, typed(a[0]), ref(a[1])))
    elif op == "icmp" or op == "fcmp":
        out.append("  %s = %s %s %s, %s" % (r, op, v.attrs["pred"], typed(a[0]), ref(a[1])))
    elif op == "select":
        out.append("  %s = select %s, %s, %s" % (r, typed(a[0]), typed(a[1]), typed(a[2])))
    elif op in CASTS:
        out.append("  %s = %s %s to %s" % (r, op, typed(a[0]), lltype(v.ty)))
    elif op == "fneg":
        out.append("  %s = fneg %s" % (r, typed(a[0])))
    elif op == "extract":
        out.append("  %s = extractelement %s, i32 %d" % (r, typed(a[0]), v.attrs["i"]))
    elif op == "insert":
        out.append("  %s = insertelement %s, %s, i32 %d" % (r, typed(a[0]), typed(a[1]), v.attrs["i"]))
    elif op == "shuffle":
        mask = ", ".join("i32 %d" % k if k >= 0 else "i32 poison" for k in v.attrs["mask"])
        out.append("  %s = shufflevector %s, %s, <%d x i32> <%s>" % (r, typed(a[0]), typed(a[1]), len(v.attrs["mask"]),
                                                                    mask))
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
        out.append("  %s = load %s, ptr %s.p, align 1" % (r, lltype(v.ty), r))
    elif op == "store":
        p = "%%st%d" % v.id
        out.append("  %s.p = getelementptr i8, ptr inttoptr (i64 %d to ptr), %s" % (p, HOST_BASE, typed(a[0])))
        out.append("  store %s, ptr %s.p, align 1" % (typed(a[1]), p))
    elif op == "aload":   # LDAR/LDAPR
        al = WIDTH[v.ty] // 8
        out.append("  %s.p = getelementptr i8, ptr inttoptr (i64 %d to ptr), %s" % (r, HOST_BASE, typed(a[0])))
        out.append("  %s = load atomic %s, ptr %s.p %s, align %d" % (r, v.ty, r, v.attrs["order"], al))
    elif op == "astore":   # STLR
        p = "%%as%d" % v.id
        al = WIDTH[a[1].ty] // 8
        out.append("  %s.p = getelementptr i8, ptr inttoptr (i64 %d to ptr), %s" % (p, HOST_BASE, typed(a[0])))
        out.append("  store atomic %s, ptr %s.p %s, align %d" % (typed(a[1]), p, v.attrs["order"], al))
    elif op == "rmw":   # LD<op>, SWP
        al = WIDTH[v.ty] // 8
        out.append("  %s.p = getelementptr i8, ptr inttoptr (i64 %d to ptr), %s" % (r, HOST_BASE, typed(a[0])))
        out.append("  %s = atomicrmw %s ptr %s.p, %s seq_cst, align %d" % (r, v.attrs["kind"], r, typed(a[1]), al))
    elif op == "cas":   # CAS, CASP: the old value
        al = WIDTH[v.ty] // 8
        out.append("  %s.p = getelementptr i8, ptr inttoptr (i64 %d to ptr), %s" % (r, HOST_BASE, typed(a[0])))
        out.append("  %s.s = cmpxchg ptr %s.p, %s, %s seq_cst seq_cst, align %d" % (r, r, typed(a[1]), typed(a[2]), al))
        out.append("  %s = extractvalue {%s, i1} %s.s, 0" % (r, v.ty, r))
    elif op == "fence":
        out.append("  fence %s" % v.attrs["kind"])
    elif op == "ctxstore32":
        p = "%%cs%d" % v.id
        out.append("  %s = getelementptr i8, ptr %%ctx, i64 %d" % (p, v.attrs["off"]))
        out.append("  store %s, ptr %s, align 4" % (typed(a[0]), p))
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
            out.append("  %s = load %s, ptr %%cx.%s, align 8" % (r, lltype(v.ty), var))
    elif op == "call":
        # an LLVM intrinsic, a runtime function or an internal helper (HELPERS); a64_* runtime routines without
        # effects (host/hos_fp.c's estimates) are declared memory(none)
        fn, at = v.attrs["fn"], v.attrs
        args = (["ptr %ctx"] if at.get("ctx") else []) + [typed(x) for x in a]
        rt = lltype(v.ty) if v.ty != "void" else "void"
        tys = (["ptr"] if at.get("ctx") else []) + [lltype(x.ty) for x in a]
        pure = fn.startswith("a64_") and not at.get("effects") and not at.get("ctx")
        decls.add(("helper:%s" % fn) if at.get("helper") else "declare %s @%s(%s)%s" % (
            rt, fn, ", ".join(tys), " memory(none) nounwind willreturn" if pure else ""))
        decls.update(HELPER_DECLS.get(fn, ()) if at.get("helper") else ())
        lhs = "" if v.ty == "void" else "%s = " % r
        out.append("  %scall %s @%s(%s)" % (lhs, rt, fn, ", ".join(args)))
    elif op == "rowio":
        # an At row with io registers (probes.At): their values into the slots, the host function with a pointer to
        # them last, their new values read back by the rowout operations that follow
        fn, at = v.attrs["fn"], v.attrs
        n = at["nargs"]
        for k, (off, _) in enumerate(io_slots(v)):
            out.append("  %%io%d.%d = getelementptr i8, ptr %%io%d, i64 %d" % (v.id, k, v.id, off))
            out.append("  store %s, ptr %%io%d.%d, align %d" % (typed(a[n + k]), v.id, k, 16 if a[n + k].ty[0] == "v" else 8))
        args = (["ptr %ctx"] if at.get("ctx") else []) + [typed(x) for x in a[:n]] + ["ptr %%io%d" % v.id]
        rt = lltype(v.ty) if v.ty != "void" else "void"
        tys = (["ptr"] if at.get("ctx") else []) + [lltype(x.ty) for x in a[:n]] + ["ptr"]
        decls.add("declare %s @%s(%s)" % (rt, fn, ", ".join(tys)))
        out.append("  %scall %s @%s(%s)" % ("" if v.ty == "void" else "%s = " % r, rt, fn, ", ".join(args)))
    elif op == "rowout":
        k, src = v.attrs["k"], a[0]
        out.append("  %s = load %s, ptr %%io%d.%d, align %d" % (r, lltype(v.ty), src.id, k, 16 if v.ty[0] == "v" else 8))
    elif op == "gcall":
        decls.add("declare void @%s(ptr)" % v.attrs["sym"])
        out.append("  call void @%s(ptr %%ctx)" % v.attrs["sym"])
    elif op == "icall":   # (lift2_icall: a host function pointer called directly, a guest address counted)
        decls.add("helper:lift2_icall")
        decls.update(HELPER_DECLS["lift2_icall"])
        out.append("  call void @lift2_icall(ptr %%ctx, %s, i64 %d)" % (typed(a[0]), v.attrs["pc"]))
    elif op == "svc":
        decls.add("declare void @a64_svc(ptr, i32)")
        out.append("  call void @a64_svc(ptr %%ctx, i32 %d)" % v.attrs["imm"])
    elif op == "sitepoison":   # (verification: the fields no reader reads at a call, build_ir.omit_stores)
        from .build_ir import SITE_POISON
        for var, k in VARS_BY_BIT.items():
            if v.attrs["mask"] >> k & 1:
                pv = SITE_POISON << 48 | k << 40 | v.attrs["pc"] & 0xFFFFFFFFFF
                pv -= 1 << 64   # (as LLVM writes it: signed)
                val = "<2 x i64> <i64 %d, i64 %d>" % (pv, pv) if var[0] == "v" else "i64 %d" % pv
                out.append("  store %s, ptr %%cx.%s, align 8" % (val, var))
    elif op == "barrier":   # (no instruction; guest loads are not moved across it or merged over it)
        out.append('  call void asm sideeffect "", "~{memory}"()')
    elif op == "flagsout":
        if not flags_out or v.attrs and v.attrs.get("always"):   # (with flags_out every flag write is stored already)
            for k, var in enumerate("nzcv"):
                out.append("  %%fo%d.%s = zext i1 %s to i8" % (v.id, var, ref(a[k])))
                out.append("  store i8 %%fo%d.%s, ptr %%cx.%s, align 1" % (v.id, var, var))
    else:
        raise ValueError("emit: op %s" % op)
