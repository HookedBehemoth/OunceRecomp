"""What a direct call of an HLE'd function keeps: the rules over the declared effects of host callees
(nx2/lift2/bounds.py call_bounds). The effects themselves are declared in the host's C (host/patch.h: reads(...),
clobbers(...) in HLE, NV and HLE_EFFECTS), read by nx2/lift2/patch_scan.py. (A function replaced by a typed host
function, PATCH_FUNC, has its record from its declaration: bounds.func_bound, summary.func_summary.)

A BL to an HLE'd function (a guest function replaced by `imp_<symbol>` in host/, gen/a64_hooks.c) keeps x19-x29 and SP
in SSA across the call, as a BL to a lifted callee with a full summary does: an HLE'd function is AAPCS-clean unless its
declaration says clobbers("..."): it reads its arguments (x0-x7, guest memory, the stack through SP) and writes its
results (x0, v0) and guest memory; it leaves x19-x29 and SP as it got them. The whole-program summaries give every
HLE'd callee these facts with its declared reads (summary.host_summary).

The audit behind the default (2026-11-15): the host's C, compiled to LLVM IR, has stores to x19-x31 or v8-v15 of a
context only in thread and fiber setup (host/hos_thread.c thread_main, fiber_main; hos_main.c), in hos_call_guest
(x30), in the guest calls of rows and replaced functions (host/hos_hostcall.c call_restoring: every register put back) and in
restores of saved values; no HLE'd function reaches one, a call of guest code, or a context switch, except those
declared clobbers("..."). The verification build (HOOKSHOT_LIFT_VERIFY=1) checks every BL to a clean HLE'd function:
x19-x29 and SP on its return equal their values at the call (host/lift2_verify.c lift2_host_exit).

What the host reads (for the context stores a call must keep): an HLE'd function reads its AAPCS arguments (HLE_ARGS:
x0-x8, v0-v7, SP for stack arguments) and its return address (x30, a label for logs only: no host function changes its
behaviour by its caller; the caller's patch is a PATCH_AT at the call site), plus its declaration's reads(...).
tests/test_host_context_reads.py compiles the host and fails on a read of a context register outside these
declarations (a replaced function's adapter reads its operands only), and on guest code a row's or a replaced
function's host function enters that its guest(...) does not declare.

An HLE_EFFECTS of a symbol the image does not replace stops the build (check); a PATCH_FUNC of no lifted function too
(patch_scan.check_funcs).
"""
import os
import re

from horizon import nvn_proc_name

# the registers every HLE'd function may read (AAPCS64: arguments x0-x7, the indirect result x8, v0-v7, the stack; x30)
HLE_ARGS = tuple("x%d" % i for i in range(9)) + tuple("v%d" % i for i in range(8)) + ("sp", "x30")


def _patches():
    from . import patch_scan
    return patch_scan.patches()


def clobbers(symbol):
    """the reason a call of the HLE'd function symbol keeps no register (its clobbers("...")), or None"""
    d = _patches().effects(symbol)
    return d.clobbers if d is not None else None


def answer(symbol):
    """the value a PATCH_ANSWER of symbol answers (host/patch.h: the lift writes it to x0 at each direct call, and the
    function's generic entry is lifted IR doing the same, game.answers_ir), or None"""
    a = _patches().answers.get(symbol) if symbol is not None else None
    return a.value if a is not None else None


def stops(symbol):
    """a call of the hooked function symbol stops the game, never returning: a generated import stub (the host neither
    implements nor answers it: nx2/standalone.py gen_stubs, host/hos_hostcall.c hos_import_stop), or an NVN function the
    SDK exports that no NV(...) implements (host/nvn_native.c nvn2_dispatch: unimplemented). A BL to it is a noreturn
    call (bounds.call_bounds, summary.host_summary)"""
    if symbol is None:
        return False
    ps = _patches()
    if symbol in ps.hle or symbol in ps.answers or symbol in ps.declared:   # (HLE_EFFECTS: a body declared elsewhere)
        return False
    proc = nvn_proc_name(symbol)
    return proc not in ps.nv if proc else True


def hle_reads(symbol):
    """the registers a call of the HLE'd function symbol reads"""
    d = _patches().effects(symbol)
    return HLE_ARGS + (d.reads if d is not None else ())


HOOK_LINE = re.compile(r"^void f_([0-9a-f]{8})\(Ctx \*A64_NOESC c\) \{ TAILCALL\(imp_(\w+)\); \}", re.M)
# (an answered function: its f_<address> is lifted IR, nx2/lift2/tables.py)
ANSWER_LINE = re.compile(r"^/\* f_([0-9a-f]{8}) answers imp_(\w+): lifted IR \*/$", re.M)


def hle_symbols(hooks):
    """{guest address: symbol} of the HLE'd functions from the image's hooks ({address: host C name, imp_<symbol>})"""
    return {a: n[4:] for a, n in hooks.items() if n.startswith("imp_")}


def hle_names(gen):
    """{guest address: symbol} of the HLE'd functions, from gen/a64_hooks.c (the lift writes it)"""
    path = os.path.join(gen, "a64_hooks.c")
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        text = f.read()
    return {int(a, 16): s for a, s in HOOK_LINE.findall(text) + ANSWER_LINE.findall(text)}


def errors(names):
    """[(where, what is wrong)]: an HLE_EFFECTS declaration of a function the image does not replace (names:
    hle_names)"""
    have = set(names.values())
    return [(r.where, "HLE_EFFECTS of %s: no HLE'd function of that symbol" % r.symbol)
            for r in sorted(_patches().declared.values(), key=lambda r: r.where) if r.symbol not in have]


def check(names):
    """stops with file:line on an HLE_EFFECTS declaration of a function the image does not replace (errors)"""
    from . import patch_scan
    for where, msg in errors(names)[:1]:
        patch_scan.fail(where, msg)
