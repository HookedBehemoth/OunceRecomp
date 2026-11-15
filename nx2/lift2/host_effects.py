"""What a direct call of a host function keeps: the rules over the declared effects of host callees (nx2/lift2/bounds.py
call_bounds). The effects themselves are declared in the host's C (host/patch.h: reads(...), guest(...), clobbers(...)
in PATCH_HOOK, HLE, NV and HLE_EFFECTS), read by nx2/lift2/patch_scan.py.

A BL to an HLE'd function (a guest function replaced by `imp_<symbol>` in host/, gen/a64_hooks.c) or to a function with
an entry hook (PATCH_HOOK) keeps x19-x29 and SP in SSA across the call, as a BL to a lifted callee with a full summary
does:

- an HLE'd function is AAPCS-clean unless its declaration says clobbers("..."): it reads its arguments (x0-x7, guest
  memory, the stack through SP) and writes its results (x0, v0) and guest memory; it leaves x19-x29 and SP as it got
  them. The whole-program summaries give every HLE'd callee these facts with its declared reads (summary.host_summary).
- an entry-hooked function keeps what its lifted body's summary proves preserved (the hook declines: the body runs; or
  the hook handled the call, which writes no callee-saved register, or ran the body itself through a64_lookup), and,
  for a hook that calls other guest code, what every callee its guest(...) names keeps too. A body without a full
  summary keeps nothing.

The audit behind the default (2026-11-15): the host's C, compiled to LLVM IR, has stores to x19-x31 or v8-v15 of a
context only in thread and fiber setup (host/hos_thread.c thread_main, fiber_main; hos_main.c), in hos_call_guest
(x30), in the guest calls of hooks and rows (host/hos_hostcall.c call_restoring: every register put back) and in
restores of saved values; no HLE'd function reaches one, a call of guest code, or a context switch, except those
declared clobbers("..."). The verification build (HOOKSHOT_LIFT_VERIFY=1) checks every BL to a clean HLE'd function:
x19-x29 and SP on its return equal their values at the call (host/lift2_verify.c lift2_host_exit).

What the host reads (for the context stores a call must keep): an HLE'd function reads its AAPCS arguments (HLE_ARGS:
x0-x8, v0-v7, SP for stack arguments) and its return address (x30, a label for logs only: no host function changes its
behaviour by its caller; the caller's patch is a PATCH_AT at the call site), plus its declaration's reads(...). An entry
hook reads x0-x7 and x30 (host/hos.h HookArgs: the hook sees nothing else of the call), plus its reads(...) (SP for a
hook that calls guest code, hook_call_guest); a hook that runs the body (hook_run) reads what the body reads, which its
summary gives. tests/test_host_context_reads.py compiles the host and fails on a read of a context register outside
these declarations, and on guest code a hook or a row's host function enters that its guest(...) does not declare.

An HLE_EFFECTS of a symbol the image does not replace stops the build (check); a hook of no lifted function too
(patch_scan.check_hooks).
"""
import os
import re

# the registers every HLE'd function may read (AAPCS64: arguments x0-x7, the indirect result x8, v0-v7, the stack; x30)
HLE_ARGS = tuple("x%d" % i for i in range(9)) + tuple("v%d" % i for i in range(8)) + ("sp", "x30")
# what every entry hook reads (host/hos.h ENTRY_HOOK_COUNTED: HookArgs)
HOOK_ARGS = tuple("x%d" % i for i in range(8)) + ("x30",)


def _patches():
    from . import patch_scan
    return patch_scan.patches()


def clobbers(symbol):
    """the reason a call of the HLE'd function symbol keeps no register (its clobbers("...")), or None"""
    d = _patches().effects(symbol)
    return d.clobbers if d is not None else None


def hle_reads(symbol):
    """the registers a call of the HLE'd function symbol reads"""
    d = _patches().effects(symbol)
    return HLE_ARGS + (d.reads if d is not None else ())


def hook_reads(name):
    """the registers the entry hook name (its C name) reads (its body's inputs aside, for a hook that runs it)"""
    h = _patches().hook_named.get(name)
    return HOOK_ARGS + (h.reads if h is not None else ())


def hook_calls(name):
    """the guest functions the entry hook name calls (hook_call_guest), its own body aside"""
    h = _patches().hook_named.get(name)
    return tuple(h.guest) if h is not None else ()


HOOK_LINE = re.compile(r"^void f_([0-9a-f]{8})\(Ctx \*A64_NOESC c\) \{ TAILCALL\(imp_(\w+)\); \}", re.M)


def hle_symbols(hooks):
    """{guest address: symbol} of the HLE'd functions from the image's hooks ({address: host C name, imp_<symbol>})"""
    return {a: n[4:] for a, n in hooks.items() if n.startswith("imp_")}


def hle_names(gen):
    """{guest address: symbol} of the HLE'd functions, from gen/a64_hooks.c (the lift writes it)"""
    path = os.path.join(gen, "a64_hooks.c")
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return {int(a, 16): s for a, s in HOOK_LINE.findall(f.read())}


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
