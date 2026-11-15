"""Facts of the platform (Horizon, the Nintendo SDK, its C and C++ runtime and the libraries its titles link), not of
one game: the lifter and the standalone build read them for any title on this SDK. A game's facts are in its
description (games/<game>/game.py, nx2/gamedesc.py) and its patch declarations (host/patch.h).
"""
import re

# the setjmp functions: a function that calls one is a setjmp owner (nx2/lift2/discover.py)
SETJMP_NAMES = ("__setjmp", "setjmp", "_setjmp")

# functions that do not return whatever their code says (they unwind, exit or abort through the host): seeds of the
# noreturn fixed point, by symbol name (the image's dynamic symbols; nx2/lift2/summary.py): the C runtime, the C++
# runtime's throws, the SDK's aborts and libpng's error exits
NORETURN_NAMES = ("abort", "__assert_fail", "_Exit", "exit", "__cxa_throw", "__cxa_rethrow", "_Unwind_RaiseException",
                  "_Unwind_ForcedUnwind", "_Unwind_Resume", "png_error", "png_chunk_error", "png_fixed_error")
NORETURN_PARTS = ("AbortImpl", "UnexpectedDefaultImpl", "ExitProcess", "QuickExit", "terminateEv", "__throw_",
                  "CriticalSectionAbort")

# a64-kit's list (TOTK), plus the Switch 2 SDK's service namespaces MKW imports (imports.py)
OS_NAMESPACES = {"audio", "hid", "oe", "prepo", "nfp", "time", "account", "vi", "swkbd", "pl", "err", "codec", "diag",
                 "detail", "settings", "ns", "applet", "album", "friends", "nifm", "ssl", "socket", "ec", "bcat",
                 "web", "ldn", "irsensor", "aoc", "olsc", "pctl", "apm", "ro",
                 "chat", "camera", "gs", "movie", "npns", "news", "fatal", "lbl", "capture", "syncpt"}


def nvn_proc_name(sym):
    """The NVN proc a symbol is: a C name (nvnFoo), or a C++ one (_Z<len>nvnFoo<args>: the Switch 2 driver exports
    some, nvnNativeImageInitialize among them); None for anything else"""
    if sym.startswith("nvn"):
        return sym
    m = re.match(r"_Z(\d+)(nvn\w+)", sym)
    return m.group(2)[:int(m.group(1))] if m else None
