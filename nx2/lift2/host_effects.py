"""What a direct call of a host function keeps: the declared effects of host callees (nx2/lift2/bounds.py call_bounds).

A BL to an HLE'd function (a guest function replaced by `imp_<symbol>` in host/, gen/a64_hooks.c) or to a function with
an ENTRY_HOOK keeps x19-x29 and SP in SSA across the call, as a BL to a lifted callee with a full summary does:

- an HLE'd function is AAPCS-clean unless CLOBBERS names it: it reads its arguments (x0-x7, guest memory, the stack
  through SP) and writes its results (x0, v0) and guest memory; it leaves x19-x29 and SP as it got them. The
  whole-program summaries give every HLE'd callee these facts with its declared reads (summary.host_summary).
- an entry-hooked function keeps what its lifted body's summary proves preserved (the hook declines: the body runs; or
  the hook handled the call, which writes no callee-saved register, or ran the body itself through a64_lookup), and,
  for a hook that calls other guest code, what every callee CALLS lists keeps too. A body without a full summary keeps
  nothing.

The audit behind the default (2026-11-15): the host's C, compiled to LLVM IR, has stores to x19-x31 or v8-v15 of a
context only in thread and fiber setup (host/hos_thread.c thread_main, fiber_main; hos_main.c), in hos_call_guest and
the splices (x30), in splice 0x80084de0 (SP, restored) and in restores of saved values (direct_mesh_binds.c,
rt_slots.c, direct_depth_copy.c, direct_layer_ahead.c); no HLE'd function reaches one, a call of guest code, or a
context switch, except those CLOBBERS names. The verification build (HOOKSHOT_LIFT_VERIFY=1) checks every BL to a clean
HLE'd function: x19-x29 and SP on its return equal their values at the call (host/lift2_verify.c lift2_host_exit).

What the host reads (for the context stores a call must keep): an HLE'd function reads its AAPCS arguments (HLE_ARGS:
x0-x8, v0-v7, SP for stack arguments) and its return address (x30, a label for logs only: no host function changes its
behaviour by its caller; the caller's patch is an At row at the call site, nx2/lift2/probes.py), plus what HLE_READS
declares by name, with the reason. An entry hook reads x0-x7 and x30 (host/hos.h HookArgs: the hook sees nothing else of
the call), plus HOOK_READS by name (SP for a hook that calls guest code, hook_call_guest); a hook that runs the body
(hook_run) reads what the body reads, which its summary gives. tests/test_host_context_reads.py compiles host/*.c and
fails on a read of a context register outside these declarations, and on guest code a hook or a row's host function
enters (on any path through the host's calls) that CALLS or the row's guest=... does not declare.

A name in CLOBBERS or HLE_READS that is no HLE'd function, or a key of CALLS or HOOK_READS that is no entry hook, stops
the build.
"""
import os
import re

# HLE'd functions (their symbol, as in `imp_<symbol>`) a direct call reloads every register after, and why
CLOBBERS = {
    "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE":
        "context switch: the call returns when another context switches back (host/hos_thread.c fiber_switch)",
    "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi":
        "deletes a host fiber (DeleteFiber), which may be the running one",
    "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE":
        "deletes a host fiber (DeleteFiber), which may be the running one",
    # (sdk NVN exports with no host proc: nvn2_dispatch calls what the host call table gives for the name; not audited)
    "nvnInstallGlobalDebugCallback": "no host NVN proc of this name: not audited",
    "nvnBootstrapLoaderInternal": "no host NVN proc of this name: not audited",
    "nvnDevtoolsBootstrap": "no host NVN proc of this name: not audited",
    "_Z22nvndetailUseMiddleWarev": "no host NVN proc of this name: not audited",
    "nvnExternalMemoryPoolBuilderSetDefaults": "no host NVN proc of this name: not audited",
    "nvnExternalMemoryPoolBuilderSetDevice": "no host NVN proc of this name: not audited",
    "nvnExternalMemoryPoolBuilderSetNativeMemoryHandle": "no host NVN proc of this name: not audited",
    "nvnExternalMemoryPoolBuilderGetDevice": "no host NVN proc of this name: not audited",
    "nvnExternalMemoryPoolBuilderGetNativeMemoryHandle": "no host NVN proc of this name: not audited",
}

# entry hooks (their C name) that call guest code other than their own body: the guest functions they call
CALLS = {
    "entry_0x801a43e0": (0x801a2d70, 0x801a6a40),   # (host/direct_mesh_binds.c drawer: hos_call_guest of both)
    "entry_0x801a46a0": (0x801a2d70, 0x801a6a40),
    "entry_0x801a6720": (0x801a2d70, 0x801a6a40),
    "entry_0x8005ed00": (0x802737a0,),   # (host/rt_slots.c release_now: hook_call_guest of the free, from the alloc's sweep)
    "entry_0x802737a0": (0x802737a0,),   # (the same, from the free's own sweep: the function itself)
}

# the registers every HLE'd function may read (AAPCS64: arguments x0-x7, the indirect result x8, v0-v7, the stack; x30)
HLE_ARGS = tuple("x%d" % i for i in range(9)) + tuple("v%d" % i for i in range(8)) + ("sp", "x30")
# HLE'd functions (their symbol, as in imp_<symbol>, or the NVN proc's name) that read more of their caller, and why
_X29_LOG = "x29: the caller's frame-pointer chain, walked for a log line (host/nvn_native.c guest_chain, watch_*)"
HLE_READS = {
    "nvnCommandBufferBeginRecording": (("x29",), "x29: the frame pointer kept with the list's info as a log label "
                                       "(host/direct.c hsd_classify)"),
    "nvnMemoryPoolInitialize": (("x29",), "x29: the frame-pointer chain's return addresses hashed into the pool's creation "
                                "key (host/nvn_native.c pool_creation_key), which places the pool"),
    "nvnWindowSetPresentInterval": (("x19",), "x19: MKW's frame pacer object (hos_game_is_mkw only)"),
    **{n: (("x29",), _X29_LOG) for n in (
        "nvnCommandBufferBindTexture", "nvnCommandBufferBindTextures", "nvnCommandBufferBindTexturesDeferred",
        "nvnCommandBufferCallCommands", "nvnCommandBufferCopyBufferToTexture", "nvnCommandBufferCopyCommands",
        "nvnCommandBufferDrawElements", "nvnCommandBufferDrawElementsBaseVertex", "nvnCommandBufferDrawElementsInstanced",
        "nvnCommandBufferSetRenderTargets", "nvnTextureReadTexels", "nvnTextureReadTexelsStrided",
        "nvnTextureWriteTexels", "nvnTextureWriteTexelsStrided")},
}

# what every entry hook reads (host/hos.h ENTRY_HOOK: HookArgs), and the hooks that read more, by their C name
HOOK_ARGS = tuple("x%d" % i for i in range(8)) + ("x30",)
HOOK_READS = {
    "entry_0x801a43e0": ("sp",),   # (host/direct_mesh_binds.c drawer: hook_call_guest on the call's stack)
    "entry_0x801a46a0": ("sp",),
    "entry_0x801a6720": ("sp",),
    "entry_0x8005ed00": ("sp",),   # (host/rt_slots.c release_now: hook_call_guest of the free)
    "entry_0x802737a0": ("sp",),
}


def hle_reads(symbol):
    """the registers a call of the HLE'd function symbol reads"""
    return HLE_ARGS + HLE_READS.get(symbol, ((), ""))[0]


def hook_reads(name):
    """the registers the entry hook name reads (its body's inputs aside, for a hook that runs it)"""
    return HOOK_ARGS + HOOK_READS.get(name, ())


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


def check(names, hooks):
    """stops on a declared name that does not exist (names: hle_names; hooks: {address: entry hook C name})"""
    bad = sorted(set(CLOBBERS) - set(names.values()))
    bad += sorted(set(CALLS) - set(hooks.values()))
    bad += sorted(set(HOOK_READS) - set(hooks.values()))
    if bad:
        raise SystemExit("build: nx2/lift2/host_effects.py declares host callees that do not exist: %s"
                         % ", ".join(bad))
