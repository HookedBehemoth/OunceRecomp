"""Compile the lifted game (work/gen, from `lift.py --write`) into work/build/hookshot_game.dll (or a static library).

The flags are a64-kit's for lifted code (its build.py, cmd_native): -O1, LoopRotate's header copies capped (most of
the compile time in goto-dense giant functions otherwise), capture tracking past LLVM's 100 uses (the context
parameter is A64_NOESC), one section per function. The guest model is the 64-bit window (rtgen.py), shadowing the
vendored rt/, with the loop-header interrupt polls on (A64_POLLING, nx2/poll.py). Incremental: an object is rebuilt
only when its source or the flags changed.

hookshot_game.dll = the lifted chunks + a64-kit's tables and runtime + nx2/rt/eden_shim.c (the emulator ABI, mkw_host.h)
+ mkw_info.c (the image base and main's build id, generated here from work/image/image.json). Linking it is also the
check that the whole lifted game closes over itself: any unresolved symbol fails the link.

Heavy: 44 chunks of ~25 MB, ~100-200 s and ~2-3.5 GB each at -O1. The PC is shared with games: run it when the
machine is idle, and keep --jobs low otherwise.

--standalone builds work/sa/build/hookshot.exe instead: the standalone lift (work/sa/gen, `lift.py --standalone --write`)
+ the native runtime in host/ (TOTKPort's hos/, adapted). Objects are cached by their key in work/objcache, shared by
both builds: the two lifts share their chunk boundaries, so every chunk without a hooked function is the same source
and is not compiled twice.

Usage:  python -I nx2/build.py [--jobs 1] [--only 0,7] [--opt O1] [--no-polls] [--dll] [--lib] [--standalone]
  --only: just these code_NNN.c chunks (to measure); --dll: link hookshot_game.dll; --lib: archive hookshot_game.lib
  --dry-run: the start-up with no lift, up to the compile; fails if a file the compile reads (the lift's aside) is missing

  HOOKSHOT_TIMES=<build_times.json>: a front end's record of the earlier builds (docs/builder-notes.md). With it, this run says which files it
  really rebuilds and how long each took before (@@plan, @@file, @@eta), which a front end shows as a countdown.
"""
import argparse
import concurrent.futures
import hashlib
import json
import re
import os
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import kit
import lifted_c
import rtgen
import toolchain
import version

NX2_RT = os.path.join(kit.HERE, "rt")
HOST = os.path.join(kit.HERE, "..", "host")
OPUS = os.path.join(kit.HERE, "..", "third_party", "opus")
OBJ_CACHE = os.path.join(kit.WORK, "objcache")
# (hookshot) work/hot_order.txt: the functions that run, hottest first (sampled), packed together by the linker; RX_NO_ORDER=1: off
PGO = os.environ.get("RX_PGO", "")


# (linux) Linux builds: an ELF executable and hookshot_direct.so, the Win32 calls of host/ on POSIX
# (host/posix: its windows.h is found first), the window and sound on SDL2. HOOKSHOT_SYSROOT=<dir>: compile and link
# against that root (the AppImage's: glibc 2.35, so the game runs on older systems than the one that built it).
if toolchain.windows():
    raise SystemExit("build.py: the host is built for Linux only (RX_TARGET=windows is not supported)")
POSIX = os.path.join(kit.HERE, "..", "host", "posix")
SYSROOT = os.environ.get("HOOKSHOT_SYSROOT", "")
EXE = "hookshot"
DIRECT_LIB = "hookshot_direct.so"


def check_backend_symbols(so):
    """The back end is dlopened with RTLD_NOW, so one unresolved symbol stops the whole thing
    loading -- and the only sign of that at run time is "the renderer did not start: the window
    stays black", with no symbol named. The link itself does not fail: a shared object is allowed to
    leave symbols for its loader to supply, which is exactly what the host executable does for the
    weak hos_* ones.

    So: anything still undefined here that is not weak and not the C or C++ runtime's is a mistake,
    and this says which. It has caught two, each of which had cost a run apiece to find by hand: a
    host function referenced without __attribute__((weak)), and an `extern` of one of our own
    globals written inside an anonymous namespace, which makes a different symbol that nothing
    defines.
    """
    nm = shutil.which("nm") or shutil.which("llvm-nm")
    if not nm:
        return True   # (not a reason to fail a build; the check is a guard, not a dependency)
    try:
        out = subprocess.run([nm, "-D", "--undefined-only", so], capture_output=True, text=True,
                             check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return True
    bad = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2 or parts[0] != "U":   # ("w" is weak: the host supplies those, or does not)
            continue
        name = parts[1]
        if "@GLIBC" in name or "@CXXABI" in name or "@GLIBCXX" in name or "@GCC" in name:
            continue
        bad.append(name)
    if not bad:
        return True
    print("%s has %d unresolved symbol(s); it would not load:" % (os.path.basename(so), len(bad)))
    for name in bad[:20]:
        print("    " + name)
    print("  a host function wants __attribute__((weak)); one of our own globals wants an extern at")
    print("  namespace scope, not inside an anonymous namespace")
    return False

def target():
    """toolchain.native_target(), with the sysroot on Linux"""
    return toolchain.native_target() + (["--sysroot=" + SYSROOT] if SYSROOT else [])


def HOT_ORDER():
    p = os.path.join(kit.WORK, "hot_order.txt")
    if not os.path.exists(p) or os.environ.get("RX_NO_ORDER"):
        return []
    return ["-Wl,--symbol-ordering-file=" + p, "-Wl,--no-warn-symbol-ordering"]


HOST_LIBS = ["-lpthread", "-ldl", "-lm"]
GPU = os.path.join(kit.HERE, "..", "gpu")
def tree_headers(*roots, skip=()):
    """Every header under these folders, as one dependency blob (suyu's headers sit in many folders); folders named
    in skip are left out"""
    h = hashlib.sha1(usedforsecurity=False)
    for root in roots:
        found = []
        # (the walk is pruned as it goes: sorting the whole walk first would make skip do nothing)
        for d, dirs, files in os.walk(root):
            dirs[:] = sorted(x for x in dirs if x not in skip)
            found += [os.path.join(d, f) for f in files if f.endswith((".h", ".hpp", ".inc"))]
        for path in sorted(found):
            h.update(os.path.basename(path).encode() + open(path, "rb").read())
    return h.hexdigest().encode()


def cpm_dirs():
    """CPM packages: RX_CPM, packaged tools/cpm, then legacy project caches.
    The packages provide Vulkan and SPIR-V headers, VMA and FFmpeg."""
    eden_ns2 = os.path.dirname(kit.ROOT)
    return [d for d in (os.environ.get("RX_CPM"), os.path.join(kit.ROOT, "cpm"),
                        os.path.join(kit.WORK, "eg", ".cache", "cpm"),
                        os.path.join(eden_ns2, "mkw-recomp", "work", "eg", ".cache", "cpm"),
                        os.path.join(eden_ns2, "Eglantine", ".cache", "cpm")) if d and os.path.isdir(d)]


def cpm_package(name):
    """The newest version of a package in the first CPM cache that has it (cpm_dirs), or None"""
    for root in cpm_dirs():
        d = os.path.join(root, name)
        if os.path.isdir(d) and os.listdir(d):
            return os.path.join(d, sorted(os.listdir(d))[-1])
    return None


def header_package(cpm_name, probe, env=None):
    """A package root with include/<probe>: CPM first, then env (package root or its include/ folder),
    then common system prefixes (/usr/local, /usr). Distro vulkan-headers / spirv-headers install
    under /usr/include; this is what makes a source build find them without a CPM tree."""
    parts = probe.split("/")

    def ok(root):
        return bool(root) and os.path.isfile(os.path.join(root, "include", *parts))

    pkg = cpm_package(cpm_name)
    if ok(pkg):
        return pkg
    if env:
        e = os.environ.get(env) or ""
        if e:
            e = os.path.abspath(e)
            if ok(e):
                return e
            if os.path.isfile(os.path.join(e, *parts)):
                parent = os.path.dirname(e)
                if os.path.normpath(os.path.join(parent, "include")) == os.path.normpath(e):
                    return parent
    for prefix in ("/usr/local", "/usr"):
        if ok(prefix):
            return prefix
    return None


def cache_put(obj, key):
    """Hard-link a built object into the cache under its key (a copy across volumes)"""
    os.makedirs(OBJ_CACHE, exist_ok=True)
    dst = os.path.join(OBJ_CACHE, key + ".o")
    if os.path.exists(dst):
        return
    try:
        os.link(obj, dst)
    except OSError:
        import shutil
        shutil.copyfile(obj, dst)


def drop_stale_objects(cc, obj_dirs):
    """Objects compiled against other system headers are not reused (a cached object made with an older libc's headers
    links to symbols the new one lacks). The stamp is the compiler's version line and stdio.h's bytes (HOOKSHOT_SYSROOT's
    when set), kept in the cache folder itself so every worktree sharing work/objcache compares the same note. When it
    changes, the cache and this build's objects go; a cache with no note yet is kept and the note written."""
    stdio = os.path.join(SYSROOT, "usr", "include", "stdio.h") if SYSROOT else "/usr/include/stdio.h"
    try:
        body = open(stdio, "rb").read()
        ver = subprocess.run([cc, "--version"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
    except (OSError, subprocess.SubprocessError):
        return
    first = ver.stdout.splitlines()[:1]
    if ver.returncode or not first:
        return
    stamp = hashlib.sha1(body + first[0].encode(), usedforsecurity=False).hexdigest()
    os.makedirs(OBJ_CACHE, exist_ok=True)
    note = os.path.join(OBJ_CACHE, "system_headers.txt")
    try:
        with open(note) as f:
            had = f.read().strip()
    except OSError:
        had = ""
    if had and had != stamp:
        print("the system headers or the compiler changed: the cached objects are not reused", flush=True)
        for f in os.listdir(OBJ_CACHE):
            if f.endswith(".o"):
                os.remove(os.path.join(OBJ_CACHE, f))
        for d in obj_dirs:
            for f in os.listdir(d) if os.path.isdir(d) else []:
                if f.endswith((".o", ".sha1")):
                    os.remove(os.path.join(d, f))
    if had != stamp:
        with open(note, "w") as f:
            f.write(stamp + chr(10))


OBJ_CACHE_KEEP_DAYS = float(os.environ.get("HOOKSHOT_OBJCACHE_DAYS", "3"))


def prune_object_cache(now=None):
    """After a build: cached objects that no build folder links any more (st_nlink 1: compile_one and cache_put hard-link
    each object between its build's obj folder and the cache, and work/objcache is shared by the main checkout and every
    agent worktree) and that were put there more than HOOKSHOT_OBJCACHE_DAYS (3) days ago go. Every live build keeps its
    objects whatever their age; a dropped worktree's or a previous lift's are kept long enough to switch back. Returns
    (removed, bytes removed, bytes kept)."""
    if not os.path.isdir(OBJ_CACHE):
        return 0, 0, 0
    limit = (now if now is not None else time.time()) - OBJ_CACHE_KEEP_DAYS * 86400
    removed = freed = kept = 0
    for f in os.listdir(OBJ_CACHE):
        if not f.endswith(".o"):
            continue
        p = os.path.join(OBJ_CACHE, f)
        try:
            st = os.stat(p)
        except OSError:
            continue
        if st.st_nlink <= 1 and st.st_mtime < limit:
            try:
                os.remove(p)
                removed, freed = removed + 1, freed + st.st_size
                continue
            except OSError:
                pass
        kept += st.st_size
    return removed, freed, kept


# (what one compiler is counted at where its own need is not worked out: a hot piece (0.8-1.7 GB measured, -O2 and the profile),
# and the compile countdown's guess of how many the free memory holds. About the median chunk's peak)
COMPILER_MB = 1500


def free_mb():
    """The memory free now (MemAvailable), in MB; None: not known"""
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) >> 10
    except (OSError, ValueError, AttributeError):
        return None
    return None


def tree_rss(pid):
    """(linux) The resident bytes of a process and the processes it started (a driver that runs its compiler as a child, or ccache)"""
    with open("/proc/%d/statm" % pid) as f:
        n = int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    for task in os.listdir("/proc/%d/task" % pid):
        try:
            with open("/proc/%d/task/%s/children" % (pid, task)) as f:
                for child in f.read().split():
                    try:
                        n += tree_rss(int(child))
                    except (OSError, ValueError, IndexError):
                        pass
        except OSError:
            pass
    return n


def rss_mb(proc):
    """A running compiler's resident memory in MB (0: exited, not known, or not started yet)"""
    if proc is None:
        return 0
    try:
        # A retained Popen can outlive its driver while PID counters still remain.
        if proc.poll() is not None:
            return 0
        return tree_rss(proc.pid) >> 20
    except (OSError, ValueError, AttributeError, IndexError):
        return 0


def mem_need(src):
    """What compiling a file takes at its peak, in MB, a little over. Measured on a cold build (clang 23, x86-64): a chunk of lifted
    code 0.4 GB + 50 MB per MB of C + 250 MB per MB of its largest function (1.5 GB for 24 MB of small functions, 2.2 GB for a 6 MB
    function alone); a hot piece 0.8-1.3 GB whatever its size (-O2, the profile, inlining); the runtime's C files ~0.2 GB, the C++
    files up to ~1 GB"""
    name = os.path.basename(src)
    if name.startswith("code_hot"):
        return COMPILER_MB
    if name.startswith("code_"):
        with open(src, "rb") as f:
            text = f.read()
        # (each function from the newline before its definition, as this was first measured)
        starts = [m.start() - 1 for m in lifted_c.HEAD_BYTES.finditer(text)] + [len(text)]
        largest = max([b - a for a, b in zip(starts, starts[1:])] or [0])
        return int(400 + 50 * len(text) / (1 << 20) + 250 * largest / (1 << 20))
    return 1000 if src.endswith(".cpp") else 400


class MemoryGate:
    """(issue 418) Compilers started by the memory there is, not only by --jobs: one per CPU thread ran PCs out of memory (the kernel
    stopped a compiler, or the whole build), and a count low enough for the biggest files and the PC's other programs left most of
    the threads idle. A compile starts when the memory free, less what the compilers running may still take on top of what they
    hold now (mem_need), keeps its own need and HOOKSHOT_MEM_RESERVE_GB (default 1.5) free. The floor is one
    (issue 531: Linux stops a compiler that does not fit, and it is compiled again alone). The free memory is read again as compiles start and end, so a game or browser opened during the build makes later
    compiles wait"""

    def __init__(self):
        self.cv = threading.Condition()
        self.running = {}   # (per-compile lease: [need, Popen or None]; one source can have several objects)
        self.reserve = float(os.environ.get("HOOKSHOT_MEM_RESERVE_GB") or 1.5) * 1024
        self.floor = 1

    def enter(self, src):
        need = mem_need(src)
        with self.cv:
            while len(self.running) >= self.floor:
                free = free_mb()
                if free is None:
                    break
                owed = sum(max(0, n - rss_mb(p)) for n, p in self.running.values())
                if free - owed - self.reserve >= need:
                    break
                self.cv.wait(1.0)
            lease = object()
            self.running[lease] = [need, None]
            return lease

    def started(self, lease, proc):
        with self.cv:
            self.running[lease][1] = proc

    def expected(self, jobs):
        """How many compilers the memory free now is likely to hold at once (COMPILER_MB each), between the floor and the jobs:
        the compile countdown's estimate"""
        free = free_mb()
        if free is None:
            return jobs
        return max(self.floor, min(jobs, int((free - self.reserve) // COMPILER_MB)))

    def leave(self, lease):
        with self.cv:
            self.running.pop(lease, None)
            self.cv.notify_all()


GATE = None   # (main's MemoryGate; None: compiles start as the jobs allow)


FFMPEG_LIBS = ("avcodec", "swscale", "swresample", "avutil")


def ffmpeg_linux():
    """(linux) FFmpeg's compile and link flags: HOOKSHOT_FFMPEG=<prefix> with static libraries (the AppImage's, with
    dav1d for OoT's AV1 movies), else the system's through pkg-config; None without either"""
    pre = os.environ.get("HOOKSHOT_FFMPEG")
    if pre and os.path.exists(os.path.join(pre, "lib", "libavcodec.a")):
        libs = [os.path.join(pre, "lib", "lib%s.a" % l) for l in FFMPEG_LIBS]
        libs += [os.path.join(pre, "lib", "libdav1d.a")] if os.path.exists(os.path.join(pre, "lib", "libdav1d.a")) else []
        return ["-I" + os.path.join(pre, "include")], libs + ["-lm", "-lpthread"]
    try:
        names = ["lib" + l for l in FFMPEG_LIBS]
        cflags = subprocess.run(["pkg-config", "--cflags"] + names, capture_output=True, text=True, check=True).stdout.split()
        libs = subprocess.run(["pkg-config", "--libs"] + names, capture_output=True, text=True, check=True).stdout.split()
        return cflags, libs
    except (OSError, subprocess.CalledProcessError):
        return None


FFMPEG_LX = ffmpeg_linux()


def ffmpeg_version_key():
    """The FFmpeg headers this build compiles against, so a host object from another major version is not reused.
    Codec numbers and struct layouts differ between them."""
    incs = []
    if FFMPEG_LX:
        incs += [f[2:] for f in FFMPEG_LX[0] if f.startswith("-I")]
    incs.append("/usr/include")
    for inc in incs:
        parts = []
        for name in ("version_major.h", "version.h"):
            path = os.path.join(inc, "libavcodec", name)
            if os.path.exists(path):
                parts.append(open(path, "rb").read())
        if parts:
            return b"".join(parts)
    return b""


# the soft write watch (rt/a64.h A64_MARK): lifted stores mark their 4 KB page, and the renderer reads the marks
# instead of Windows' write watch (GetWriteWatch over the 6 GB MKW's GPU sees took ~20 ms a scan, in one thread or
# four: the race's replay waited ~12 ms a frame for it). RX_SOFT_WRITE_WATCH=0: Windows' write watch (a full rebuild)
WRITE_TRACK = [] if os.environ.get("RX_SOFT_WRITE_WATCH") == "0" else ["-DA64_WRITE_TRACK"]

# (hookshot) the host CPU the lifted game and the runtime are built for: x86-64-v3 (AVX2, FMA, BMI1/2, LZCNT,
# MOVBE; Haswell and Zen 1 on). With FMA the FPX1 fast paths cover double-precision FMA (RECOMP_FPX_FMA64; it took
# the exact software body on baseline x86-64) and the bit counts are single instructions. No contraction, so a C
# expression is rounded as written (the exact bodies and FPX1's proofs assume it). RX_MARCH=x86-64: the old baseline
# (issue 363: a Xeon E5-2667 v2 (Ivy Bridge, no AVX2) built the game and then stopped at its first AVX2 instruction,
# 0xc000001d. The game is built on the PC it runs on: a CPU without x86-64-v3's features gets x86-64-v2, or x86-64)
from host_cpu import host_march

MARCH = os.environ.get("RX_MARCH") or host_march()
if MARCH != "x86-64-v3":
    print("build: this CPU has no AVX2: the game is built for %s (a little slower)" % MARCH, flush=True)
CPU = ["-march=" + MARCH, "-ffp-contract=off"]


def flags(gen, rt64, opt, polls):
    return target() + CPU + [
        "-I" + rt64, "-I" + gen, "-I" + os.path.join(kit.KIT, "rt"), "-I" + NX2_RT, "-Wno-everything",
        "-fno-strict-aliasing", "-mcx16", "-" + opt, "-g0", "-mllvm", "-rotation-max-header-size=0",
        "-ffunction-sections", "-mllvm", "-capture-tracking-max-uses-to-explore=5000"] + \
        (["-DA64_POLLING"] if polls else []) + WRITE_TRACK


def write_info(out):
    meta = json.load(open(os.path.join(kit.WORK, "image", "image.json")))
    mods = {m["name"]: m for m in meta["modules"]}
    path = os.path.join(out, "mkw_info.c")
    text = ('/* Generated by nx2/build.py from work/image/image.json. */\n#include <stdint.h>\n'
            'const uint64_t mkw_image_base = 0x%xULL;\nconst char mkw_main_build_id[] = "%s";\n'
            % (meta["base"], mods["main"]["build_id"]))
    if not os.path.exists(path) or open(path).read() != text:
        open(path, "w", newline="\n").write(text)
    return path


def flag_text(fl):
    """The flags an object key is named by. Include folders are left out (their headers are in deps). The builder's
    own folder is <root> (issue 565: versions\\<tag>\\... made every update look new). --sysroot= is <sysroot>: the
    AppImage mount is a new /tmp/.mount_... path every start, and that path is not a change in the headers."""
    kept = []
    for f in fl:
        if f.startswith("-I"):
            continue
        if f.startswith("--sysroot="):
            f = "--sysroot=<sysroot>"
        else:
            f = f.replace(kit.ROOT, "<root>")
        kept.append(f)
    return " ".join(kept)


def object_key(src, fl, deps):
    """What an object depends on: its source, the flags (flag_text), and the headers."""
    return hashlib.sha1(open(src, "rb").read() + flag_text(fl).encode() + deps, usedforsecurity=False).hexdigest()


def times_from(path):
    """The seconds each module took on this PC, from a front end's build_times.json (HOOKSHOT_TIMES), or {} when there is no record to
    use (a first build, one half written, or one another version of the tools wrote): the countdown then falls back to
    the rate this PC's other measurements ran at."""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        return {str(k): float(v) for k, v in data["seconds"].items()
                if isinstance(v, (int, float)) and not isinstance(v, bool)}
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return {}


def eta_left(pending, jobs):
    """How long the rest of the compile takes, in seconds, with this many compilers: the work spread over them, with
    the longest file still left as the floor (each file is still one compiler)."""
    if not pending:
        return 0.0
    return max(max(pending), sum(pending) / max(1, jobs))


def per_file_seconds(jobs, history):
    """One expected duration for each job, in order: the seconds this PC measured for that module before, or, for a
    module it never compiled (a new release adds or renames files), the rate those measurements ran at. On a PC with
    no measurements at all, the first build uses the rates a full build on the development PC showed."""
    sizes = [max(0.0, os.path.getsize(src) / (1 << 20)) for _, src, *_ in jobs]
    measured = [(history[os.path.basename(src)], mb) for (_, src, *_), mb in zip(jobs, sizes)
                if os.path.basename(src) in history and mb > 0]
    rate = sum(secs for secs, _ in measured) / sum(mb for _, mb in measured) if measured else None
    out = []
    for (_, src, *_), mb in zip(jobs, sizes):
        name = os.path.basename(src)
        if name in history:
            out.append(max(0.05, history[name]))
        elif rate is not None:
            out.append(max(0.5, rate * mb))
        else:   # (seconds per MB measured on a full build: the eight hot files, the game chunks, the rest)
            out.append(23.5 * mb if name.startswith("code_hot") else 3.4 * mb + 1 if name.startswith("code_") else 1.3)
    return out


def plan_jobs(jobs):
    """Which jobs this run really compiles, and every job's object key. A job whose object is stamped with its key, or
    whose key is in the object cache (the other target's build), is only linked in: not work, and not in the countdown.
    The keys go to compile_one, which then does not hash every source a second time."""
    work, keys = [], {}
    for cc, src, obj, fl, deps in jobs:
        key = object_key(src, fl, deps)
        keys[(src, obj)] = key
        stamp = obj + ".sha1"
        if os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key:
            continue
        if os.path.exists(os.path.join(OBJ_CACHE, key + ".o")):
            continue
        work.append((cc, src, obj, fl, deps))
    return work, keys


def migrate_stamps(gen, obj_dir, fl, deps):
    """Objects stamped with the old key (flags with include folders) are restamped with object_key and cached, so
    a build from before the cache does not have to be compiled again"""
    if not os.path.isdir(obj_dir):
        return
    if not os.path.isdir(gen) or not os.path.isdir(obj_dir):   # (a first build in a fresh folder)
        return
    for f in os.listdir(gen):
        if not (f.startswith("code_") and f.endswith(".c")):
            continue
        obj = os.path.join(obj_dir, f[:-2] + ".o")
        stamp = obj + ".sha1"
        if not (os.path.exists(obj) and os.path.exists(stamp)):
            continue
        src = os.path.join(gen, f)
        content = open(src, "rb").read()
        if open(stamp).read() == hashlib.sha1(content + " ".join(fl).encode() + deps, usedforsecurity=False).hexdigest():
            key = object_key(src, fl, deps)
            open(stamp, "w").write(key)
            cache_put(obj, key)


PLANNED = {}   # (src, obj) -> the object key plan_jobs worked out, so compile_one does not hash every source again


# (hookshot) the order of the compile: the longest first (LPT), so no long one starts late and runs alone at the end. The lift's
# estimate (gen/chunk_costs.txt, nx2/chunks.py fn_cost: functions cost with the square of their size) where it is there, else the size.
# HOT_COST: how much longer the hot flags (-O2, the profile, the hot callees inlined) take for the same estimate. Fitted to a cold build
# on a 24-thread desktop; by size alone the hot pieces (9 MB, up to 333 s) started after every 25 MB chunk (45 s)
HOT_COST = 4


def chunk_costs(gen):
    try:
        with open(os.path.join(gen, "chunk_costs.txt")) as f:
            return {name: int(n) for name, _, n in (line.partition(" ") for line in f) if n.strip().isdigit()}
    except OSError:
        return {}


def compile_cost(src, costs):
    """A file's compile time, roughly (see HOT_COST)"""
    name = os.path.basename(src)
    return costs.get(name, os.path.getsize(src)) * (HOT_COST if name.startswith("code_hot") else 1)


def compile_one(job):
    cc, src, obj, fl, deps = job
    key = PLANNED.get((src, obj)) or object_key(src, fl, deps)
    stamp = obj + ".sha1"
    if os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key:
        return src, 0.0, None
    cached = os.path.join(OBJ_CACHE, key + ".o")
    if os.path.exists(cached):   # (the same source and flags built for the other target)
        if os.path.exists(obj):
            os.remove(obj)
        try:
            os.link(cached, obj)
        except OSError:
            import shutil
            shutil.copyfile(cached, obj)
        open(stamp, "w").write(key)
        return src, 0.0, None
    lease = GATE.enter(src) if GATE else None
    t = time.time()
    try:
        with subprocess.Popen([cc] + fl + ["-c", src, "-o", obj], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as p:
            if GATE:
                GATE.started(lease, p)
            out, err = p.communicate()
    finally:
        if GATE:
            GATE.leave(lease)
    r = subprocess.CompletedProcess(p.args, p.returncode, out, err)
    if r.returncode:
        failure = r.stderr or r.stdout or "Compiler exited with status %d without diagnostics." % r.returncode
        return src, time.time() - t, failure[-4000:]
    open(stamp, "w").write(key)
    cache_put(obj, key)
    return src, time.time() - t, None


def main():
    kit.wait_for_quiet('the compile')
    p = argparse.ArgumentParser()
    p.add_argument("--jobs", type=int, default=1)
    p.add_argument("--only", default="")
    p.add_argument("--opt", default="O1")
    p.add_argument("--no-polls", action="store_true")
    p.add_argument("--dll", action="store_true")
    p.add_argument("--lib", action="store_true")
    p.add_argument("--standalone", action="store_true")
    # (a check with no game: everything up to the compile, without the lift's files; every other file the compile would
    # read must be there, and nothing is compiled)
    p.add_argument("--dry-run", action="store_true")
    a = p.parse_args()
    sa = os.path.join(kit.WORK, "sa")
    gen = os.path.join(sa if a.standalone else kit.WORK, "gen")
    out = os.path.join(sa if a.standalone else kit.WORK, "build")
    obj_dir = os.path.join(out, "obj")
    os.makedirs(obj_dir, exist_ok=True)
    if os.environ.get("RX_DIRECT_ONLY") or a.dry_run:   # (the direct back end needs no game: built so with no lift)
        os.makedirs(gen, exist_ok=True)
    else:
        import lift_mark   # (a lift that was stopped before its end is not built from)
        why = lift_mark.check(os.path.dirname(gen))
        if why:
            raise SystemExit("build: " + why)
    rt64 = rtgen.write(os.path.join(out, "rt64"))
    cc = toolchain.need(toolchain.find("clang"))
    drop_stale_objects(cc, [obj_dir])
    fl = flags(gen, rt64, a.opt, not a.no_polls) + (["-DA64_POLL_CTX"] if a.dll else [])   # (the shim's poll takes the context: rtgen.py)
    # (which Hookshot this is, for the log's first line and the Insert menu: given to those two files only, so a new
    # release name rebuilds them and nothing else)
    stamp_def = version.define()
    print("version: %s" % version.describe(), flush=True)
    # (the lifted chunks include only the guest model: the emulator ABI header is a dependency of the shim alone)
    deps = b"".join(open(os.path.join(d, f), "rb").read() for d in (gen, rt64) for f in sorted(os.listdir(d))
                    if f.endswith(".h"))
    shim_deps = deps + open(os.path.join(NX2_RT, "mkw_host.h"), "rb").read()
    eden_gen, eden_out = os.path.join(kit.WORK, "gen"), os.path.join(kit.WORK, "build")
    migrate_stamps(eden_gen, os.path.join(eden_out, "obj"),
                   flags(eden_gen, os.path.join(eden_out, "rt64"), a.opt, not a.no_polls) +
                   (["-DA64_POLL_CTX"] if a.dll else []), deps)   # (as fl below: the flags its objects were built with)
    migrate_stamps(gen, obj_dir, fl, deps)
    # (a lift with a hot set leaves code_hot.c whole; compiled as one file it takes ~79 minutes on one core. split_hot.py makes
    # the eight pieces the build compiles side by side, and renames the whole file code_hot.c.whole, so this runs once a lift)
    if os.path.exists(os.path.join(gen, "code_hot.c")) and not a.only:
        import split_hot
        split_hot.split(gen, 8)
    chunks = sorted(f for f in os.listdir(gen) if f.startswith("code_") and f.endswith(".c"))
    if a.only:
        want = {"code_%03d.c" % int(n) for n in a.only.split(",")}
        chunks = [c for c in chunks if c in want]
    srcs = [os.path.join(gen, c) for c in chunks]
    host_srcs = []
    if not a.only:
        # (a64_suyu.c: the present lifter's runtime, written by its lift; lift1_traps.c: HOOKSHOT_LIFT1=0's traps)
        srcs += [os.path.join(gen, f) for f in ("a64_table.c", "a64_hooks.c", "a64_suyu.c", "lift1_traps.c")
                 if f in ("a64_table.c", "a64_hooks.c") or os.path.exists(os.path.join(gen, f))] + \
                [os.path.join(rt64, "a64_rt.c")]
        if a.standalone:
            srcs += [os.path.join(gen, f) for f in ("a64_stubs.c", "hos_image.c", "nvn2_exports.c")]
            host_srcs = sorted(os.path.join(HOST, f) for f in os.listdir(HOST) if f.endswith(".c") and (f != "pgo_rt.c" or PGO == "gen")
                               )
            host_srcs += [  # (the window on SDL2, the Win32-named calls on POSIX)
                os.path.join(POSIX, f) for f in ("win32_posix.c", "display_sdl.c", "linux_stubs.c", "prof.c")]
        else:
            srcs += [os.path.join(NX2_RT, "eden_shim.c"), write_info(out)]
    if os.environ.get("RX_DIRECT_ONLY"):   # (no lift needed: the back end alone)
        srcs = [s for s in srcs if os.path.exists(s)]
    if a.dry_run:   # (the lift's files: there is no game here)
        srcs = [s for s in srcs if os.path.dirname(s) != gen]
    # (the mixed build, the default: the functions lift 2 can take come from it, nx2/lift2/mixed.py; HOOKSHOT_LIFT2=0:
    # the present lifter's alone)
    lift2_objs = []
    from lift2 import mixed
    if mixed.spec() and not a.only and not a.dry_run and not os.environ.get("RX_DIRECT_ONLY"):
        srcs, lift2_objs, line = mixed.prepare(mixed.spec(), gen, out, srcs, a.jobs, a.opt,
                                               os.environ.get("RX_HOT_OPT", "O2"), target() + CPU, OBJ_CACHE)
        print(line, flush=True)
    # the longest first, so the long ones do not end up alone at the end (compile_cost)
    costs = chunk_costs(gen)
    srcs.sort(key=lambda s: -compile_cost(s, costs))
    # (hookshot) code_hot.c, the lift's hot functions together (A64_HOT_FUNCS + A64_HOT_CHUNK=1): at RX_HOT_OPT
    # (default O2), so the hot callees inline into their hot callers
    hot_fl = [("-" + os.environ.get("RX_HOT_OPT", "O2")) if f == "-" + a.opt else f for f in fl]
    # (hookshot) RX_PGO=gen: code_hot.c counts its branches (host/pgo_rt.c writes them; -O1 builds much sooner, and the counters are
    # placed before the optimiser, so a profile from it fits the -O2 build). RX_PGO=<merged .prof>: built with that profile
    if PGO == "gen":
        hot_fl = [f if f != "-O2" else "-O1" for f in hot_fl] + ["-fprofile-instr-generate", "-mllvm", "-disable-vp=true"]
    elif PGO:
        # (the profile's contents in the flags the object key hashes: its path alone stays the same across releases, so an update with a
        # new profile kept the hot code built from the old one)
        with open(PGO, "rb") as f:
            hot_fl = hot_fl + ["-fprofile-instr-use=" + PGO, "-DHOOKSHOT_PROFILE_SHA1=" + hashlib.sha1(f.read(), usedforsecurity=False).hexdigest()[:16]]
    jobs = [(cc, s, os.path.join(obj_dir, os.path.basename(s)[:-2] + ".o"),
             hot_fl if os.path.basename(s).startswith("code_hot") else fl,
             shim_deps if os.path.dirname(s) == NX2_RT else deps) for s in srcs]
    # (issue 624) what the game code's object keys are made of, as short fingerprints: two builds' logs side by side say which part
    # changed when an update compiled everything again (the headers, the flags, the profile, or the lifted code itself)
    fp = lambda b: hashlib.sha1(b, usedforsecurity=False).hexdigest()[:10]
    norm = lambda f: flag_text(f).encode()
    code = hashlib.sha1(usedforsecurity=False)
    for code_src in sorted(x for x in srcs if os.path.basename(x).startswith("code_")):
        code.update(os.path.basename(code_src).encode() + open(code_src, "rb").read())
    print("object keys: game-code headers %s, flags %s, hot flags %s, lifted code %s (%d files)" % (
        fp(deps), fp(norm(fl)), fp(norm(hot_fl)), code.hexdigest()[:10],
        sum(1 for x in srcs if os.path.basename(x).startswith("code_"))), flush=True)
    # the runtime: ordinary C at -O2, with its own headers (any host/*.h change rebuilds all of it)
    host_fl = target() + CPU + ["-I" + POSIX, "-D_GNU_SOURCE"] + \
        ["-I" + HOST, "-I" + rt64, "-I" + gen, "-Wno-everything", "-O2", "-g0", "-mcx16", "-DA64_POLLING"] + WRITE_TRACK + \
        ["-Werror=implicit-function-declaration"]   # (a call without a declaration returns an int: a 64-bit value cut)
    host_deps = deps + b"".join(open(os.path.join(HOST, f), "rb").read() for f in sorted(os.listdir(HOST))
                                if f.endswith(".h"))
    host_deps += b"".join(open(os.path.join(POSIX, f), "rb").read() for f in sorted(os.listdir(POSIX)) if f.endswith(".h"))
    host_deps += b"".join(open(os.path.join(GPU, "api", f), "rb").read() for f in sorted(os.listdir(os.path.join(GPU, "api"))))
    host_deps += ffmpeg_version_key()
    jobs += [(cc, s, os.path.join(obj_dir, "host_" + os.path.basename(s)[:-2] + ".o"),
              host_fl + ["-I" + os.path.join(OPUS, "include")] +
              (FFMPEG_LX[0] if FFMPEG_LX else []) +
              ([stamp_def] if os.path.basename(s) == "hos_main.c" else []), host_deps) for s in host_srcs]
    # libopus 1.5.2 (third_party/opus, from TOTKPort): nn::codec's hardware Opus decoder (host/nn_codec.c)
    # (hookshot: built for the same CPU target as the rest (CPU), so its float code can be vectorized where the target has
    # AVX2: the vendored copy has no x86 intrinsics files of its own)
    if host_srcs:
        oflags = target() + CPU + ["-O2", "-g0", "-Wno-everything", "-DOPUS_BUILD", "-DUSE_ALLOCA",
                                              "-DHAVE_LRINTF", "-DHAVE_LRINT"] +             ["-I" + os.path.join(OPUS, d) for d in ("include", "celt", "silk", "silk/float", "src")]
        for rel in open(os.path.join(OPUS, "sources.txt")).read().split():
            jobs.append((cc, os.path.join(OPUS, rel), os.path.join(obj_dir, "opus_" + rel.replace("/", "_")[:-2] + ".o"),
                         oflags, b"opus-1.5.2"))
    # the direct back end, hookshot_direct.so beside the exe (gpu/direct/*.cpp and its translator fork gpu/direct/eden), Linux only:
    # CPM cache, HOOKSHOT_*_HEADERS, or system includes for the Vulkan and SPIR-V headers and VMA
    direct_jobs = []
    vk_headers = header_package("vulkanheaders", "vulkan/vulkan.h", "HOOKSHOT_VULKAN_HEADERS")
    spirv_headers = header_package("spirv-headers", "spirv/unified1/spirv.h", "HOOKSHOT_SPIRV_HEADERS")
    vma_headers = header_package("vulkanmemoryallocator", "vk_mem_alloc.h", "HOOKSHOT_VMA_HEADERS")
    build_direct = a.standalone and not a.only
    if build_direct and not (vk_headers and spirv_headers and vma_headers):
        missing = [n for n, p in (("Vulkan", vk_headers), ("SPIR-V", spirv_headers), ("VMA", vma_headers)) if not p]
        print("no headers for %s (missing %s). Set RX_CPM to a CPM cache, or HOOKSHOT_VULKAN_HEADERS / "
              "HOOKSHOT_SPIRV_HEADERS / HOOKSHOT_VMA_HEADERS (package root or its include/ folder), or install "
              "distro packages vulkan-headers + spirv-headers + vulkan-memory-allocator." % (DIRECT_LIB, ", ".join(missing)),
              flush=True)
        return 1
    if build_direct:
        print("the direct back end: Vulkan %s, SPIR-V %s, VMA %s" % (vk_headers, spirv_headers, vma_headers), flush=True)
        cxx = toolchain.need(toolchain.find("clang++"))
        direct = os.path.join(GPU, "direct")
        dx_eden = os.path.join(direct, "eden")   # (Eden's recompiler, sirit and their compat headers: the translator)
        zstd = os.path.normpath(os.path.join(kit.HERE, "..", "third_party", "zstd", "lib"))   # (the prepared assets' frames: direct_assets)
        dflags = target() + CPU + ["-fPIC", "-I" + POSIX, "-D_GNU_SOURCE"] + [
            # (what the code takes for granted, which newer C++ libraries bring in by themselves and older ones do not)
            "-include", "algorithm", "-include", "cstdio", "-include", "cstdlib", "-include", "cstring", "-include", "functional",
            "-include", "string", "-include", "vector",
            "-std=c++20", "-O2", "-g0", "-DNDEBUG", "-Wno-everything", "-DVK_NO_PROTOTYPES", "-DMKWVK_BUILDING",
            "-I" + os.path.join(dx_eden, "compat"), "-I" + os.path.join(GPU, "compat"), "-I" + dx_eden,
            "-I" + os.path.join(dx_eden, "sirit", "include"), "-I" + os.path.join(dx_eden, "sirit", "src"),
            "-I" + os.path.join(spirv_headers, "include"), "-I" + os.path.join(spirv_headers, "include", "spirv", "unified1"),
            "-I" + os.path.join(vk_headers, "include"), "-I" + os.path.join(vma_headers, "include")]
        ddeps = tree_headers(direct) + b"".join(open(os.path.join(GPU, "api", f), "rb").read() for f in sorted(os.listdir(os.path.join(GPU, "api"))))
        # (the direct back end's shader stamps: the translator it builds and the code that decides the translation's output,
        # so its program caches are made again for a change to them only. In direct_shader.cpp and direct_compute.cpp that
        # code is between "// <stamp NAME>" and "// </stamp NAME>" lines: graphics and shared for the graphics programs,
        # compute and shared for the compute programs)
        dtranslator = sorted(os.path.join(d, f) for d, _, fs in os.walk(dx_eden) for f in fs)

        def stamp_regions(path, names):
            text = open(path, "rb").read().replace(bytes((13, 10)), bytes((10,))).decode()
            out, inside = [], None
            for line in text.split("\n"):
                m = re.match(r"^// <(/?)stamp (\w+)>$", line)
                if m:
                    if (inside is None) == (m.group(1) == "/") or (m.group(1) and m.group(2) != inside):
                        raise SystemExit("%s: the stamp markers do not pair at %r" % (path, line))
                    inside = None if m.group(1) else m.group(2)
                elif inside in names:
                    out.append(line)
            if inside is not None:
                raise SystemExit("%s: the stamp region %s is not closed" % (path, inside))
            return "\n".join(out).encode()

        def shader_stamp(files, regions):
            h = hashlib.sha1(usedforsecurity=False)
            for f in dtranslator + [os.path.join(direct, n) for n in files]:
                if os.path.isfile(f):
                    h.update(os.path.relpath(f, GPU).replace("\\", "/").encode() + open(f, "rb").read().replace(bytes((13, 10)), bytes((10,))))
            for name, which in regions:
                h.update(name.encode() + stamp_regions(os.path.join(direct, name), which))
            return h.hexdigest()[:16]
        shader_cpp = ("direct_shader.cpp", {"graphics", "shared"})
        direct_stamp = "-DHOOKSHOT_DIRECT_SHADER_STAMP=0x%sull" % shader_stamp(
            ("direct_state.h", "direct_spv_late.cpp", "direct_spv_late.h"), (shader_cpp,))
        compute_stamp = "-DHOOKSHOT_DIRECT_COMPUTE_STAMP=0x%sull" % shader_stamp(
            (), (("direct_shader.cpp", {"shared"}), ("direct_compute.cpp", {"compute"})))
        dx_eden_deps = tree_headers(dx_eden, os.path.join(GPU, "compat"))
        for esrc in sorted(os.path.join(d, f) for sub in ("shader_recompiler", os.path.join("sirit", "src"))
                           for d, _, fs in os.walk(os.path.join(dx_eden, sub)) for f in fs if f.endswith(".cpp")):
            direct_jobs.append((cxx, esrc, os.path.join(obj_dir, "dx_eden_" + os.path.relpath(esrc, dx_eden).replace(os.sep, "_")[:-4] + ".o"),
                                dflags, dx_eden_deps))
        for dsrc in sorted(os.path.join(direct, f) for f in os.listdir(direct) if f.endswith(".cpp")):
            direct_jobs.append((cxx, dsrc, os.path.join(obj_dir, "dx_" + os.path.basename(dsrc)[:-4] + ".o"),
                                ["-I" + direct, "-I" + os.path.join(GPU, "api")] + dflags + ["-I" + zstd] +
                                ({"direct_shader.cpp": [direct_stamp], "direct_compute.cpp": [compute_stamp]}.get(os.path.basename(dsrc), [])), ddeps))
        # (zstd's decoder, third_party/zstd, plain C: the prepared assets' frames)
        zflags = target() + CPU + ["-fPIC", "-O2", "-g0", "-DNDEBUG", "-Wno-everything", "-fvisibility=hidden",
                                   "-DZSTD_DISABLE_ASM", "-DZSTD_LEGACY_SUPPORT=0", "-DZSTD_MULTITHREAD=0"]
        zdeps = b"".join(open(os.path.join(d, f), "rb").read() for d in (zstd, os.path.join(zstd, "common"), os.path.join(zstd, "decompress"))
                         for f in sorted(os.listdir(d)) if f.endswith(".h"))
        for f in ("common/debug.c", "common/entropy_common.c", "common/error_private.c", "common/fse_decompress.c", "common/xxhash.c",
                  "common/zstd_common.c", "decompress/huf_decompress.c", "decompress/zstd_ddict.c", "decompress/zstd_decompress.c",
                  "decompress/zstd_decompress_block.c"):
            direct_jobs.append((cc, os.path.join(zstd, *f.split("/")), os.path.join(obj_dir, "dx_zstd_" + os.path.basename(f)[:-2] + ".o"),
                                zflags, zdeps))
        jobs += direct_jobs
    direct_only = bool(os.environ.get("RX_DIRECT_ONLY"))   # (the back end alone, the game's objects as they are)
    if direct_only:
        jobs = list(direct_jobs)
    if a.dry_run:   # (the compile starts here: what it would read, said and checked, and nothing compiled)
        missing = sorted({j[1] for j in jobs if not os.path.isfile(j[1])})
        print("dry run: %d files to compile besides the lift's, %d missing" % (len(jobs), len(missing)), flush=True)
        for f in missing:
            print("    missing: " + f)
        return 1 if missing else 0
    # (a front end's compile timer: HOOKSHOT_TIMES is its record of the earlier builds on this PC. With it,
    # this run says which files it really rebuilds and how long each took before, and the front end shows the rest of the
    # compile as a countdown that follows this PC's own measurements. Run by hand, it prints none of these lines.)
    global GATE
    GATE = None if os.environ.get("HOOKSHOT_MEM_GATE") == "0" else MemoryGate()
    eta_jobs = GATE.expected(a.jobs) if GATE else a.jobs   # (the countdown with the compilers the memory holds, not --jobs)
    counted, expected, pending = os.environ.get("HOOKSHOT_TIMES"), {}, []
    if counted:
        work_jobs, keys = plan_jobs(jobs)
        PLANNED.update(keys)
        estimate = per_file_seconds(work_jobs, times_from(counted))
        pending = list(estimate)
        for job, seconds in zip(work_jobs, estimate):
            expected.setdefault(job[1], []).append(seconds)
        print("@@plan %d %d" % (len(work_jobs), len(jobs)), flush=True)
        print("@@eta %d" % eta_left(pending, eta_jobs), flush=True)
    print("%d files, %d jobs, %s" % (len(jobs), a.jobs, " ".join(f for f in fl if f.startswith("-D") or f.startswith("-O"))),
          flush=True)
    t0, failed = time.time(), []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        # (as each ends, not in order: a front end shows progress from these lines, and the biggest file comes first)
        submitted = {ex.submit(compile_one, job): job for job in jobs}
        for future in concurrent.futures.as_completed(submitted):
            src, secs, err = future.result()
            size = os.path.getsize(src) >> 20
            if err:
                failed.append(submitted[future])
                print("not compiled yet: %s (%d MB, %.0fs), tried again alone at the end:\n%s"
                      % (os.path.basename(src), size, secs, err[-1500:]), flush=True)
            else:
                print("  %-16s %4d MB  %6.0fs  (%.0fs)" % (os.path.basename(src), size, secs, time.time() - t0), flush=True)
            if counted:
                print("@@file %s %.1f %s" % (os.path.basename(src), secs,
                                             "failed" if err else "cached" if secs <= 0 else "new"), flush=True)
                if not err and expected.get(src):
                    pending.remove(expected[src].pop())
                    print("@@eta %d" % eta_left(pending, eta_jobs), flush=True)
    # (a compiler that ran out of memory beside the others dies "due to signal": a player's 24 MB code_021.c, 2026-11-08. The
    # files that failed are compiled again one at a time, with all the memory, before the build gives up)
    still = 0
    for job in failed:
        src, secs, err = compile_one(job)
        size = os.path.getsize(src) >> 20
        if err:
            still += 1
            print("FAILED %s (%d MB, %.0fs, alone too):\n%s" % (os.path.basename(src), size, secs, err), flush=True)
        else:
            print("  %-16s %4d MB  %6.0fs  (%.0fs, alone)" % (os.path.basename(src), size, secs, time.time() - t0), flush=True)
        if counted:
            print("@@file %s %.1f %s" % (os.path.basename(src), secs, "failed" if err else "again"), flush=True)
            if not err and expected.get(src):
                pending.remove(expected[src].pop())
                print("@@eta %d" % eta_left(pending, eta_jobs), flush=True)
    print("compiled in %.0fs, %d failed" % (time.time() - t0, still), flush=True)
    if still:
        return 1
    vk_objs = {j[2] for j in direct_jobs}   # (the back end's objects: not the exe's)
    objs = [j[2] for j in jobs if j[2] not in vk_objs]
    objs += lift2_objs
    rsp = os.path.join(out, "objs.rsp")
    open(rsp, "w", encoding="utf-8").write("\n".join('"%s"' % o.replace("\\", "/") for o in objs))
    if a.lib:
        lib = os.path.join(out, "hookshot_game.lib")
        subprocess.run([toolchain.lld_link(), "/lib", "/out:" + lib, "@" + rsp], check=True)
        print("wrote %s (%d objects, %d MB)" % (lib, len(objs), os.path.getsize(lib) >> 20))
    if a.standalone and not a.only and not direct_only:
        exe = os.path.join(out, EXE)   # (+ hookshot.map: crash offsets -> f_<guest address>)
        t = time.time()
        # (--export-dynamic-symbol: the window's state, which host/posix/win32_posix.c looks up by name)
        r = subprocess.run([cc] + target() + ["-fuse-ld=lld", "-o", exe, "-Wl,-Map=" + exe + ".map", "-Wl,--gc-sections",
                                              "-Wl,--export-dynamic-symbol=hos_posix_*", "@" + rsp] + HOT_ORDER() +
                           (["-fprofile-instr-generate"] if PGO == "gen" else []) +
                           (FFMPEG_LX[1] if FFMPEG_LX else []) + HOST_LIBS,
                           capture_output=True, text=True)
        undefined = sorted({l.strip() for l in (r.stdout + r.stderr).splitlines() if "undefined symbol" in l})
        if r.returncode:
            print("link FAILED (%.0fs), %d undefined symbols" % (time.time() - t, len(undefined)))
            print("\n".join(undefined[:60]) if undefined else (r.stdout + r.stderr)[-3000:])
            return 1
        print("linked %s: %d MB (%.0fs)" % (exe, os.path.getsize(exe) >> 20, time.time() - t))
        import label_syms   # (native-vk: nx2/oot_labels.txt's names for the functions we know, in the symbol table only)
        named, absent = label_syms.apply(exe)
        print("labels: %d functions named%s" % (named, "; no function for " + ", ".join("%08x" % x for x in absent) if absent else ""))
        sdl = os.environ.get("HOOKSHOT_SDL2", "")   # (libSDL2-2.0.so.0 beside it, when the AppImage brings one)
        if sdl and os.path.exists(sdl):
            import shutil
            shutil.copyfile(sdl, os.path.join(out, "libSDL2-2.0.so.0"))
    if direct_jobs:   # (hookshot_direct.so beside the exe, loaded by host/rx_backend.c)
        ddll = os.path.join(out, DIRECT_LIB)
        drsp = os.path.join(out, "direct_objs.rsp")
        open(drsp, "w", encoding="utf-8").write("\n".join('"%s"' % o.replace("\\", "/") for o in [j[2] for j in direct_jobs]))
        t = time.time()
        link = [toolchain.need(toolchain.find("clang++"))] + target() + [
            "-fuse-ld=lld", "-shared", "-o", ddll, "-Wl,-Map=" + ddll + ".map", "-Wl,-Bsymbolic", "-Wl,--gc-sections",
            "-static-libstdc++", "-static-libgcc", "@" + drsp, "-ldl", "-lpthread", "-lm"]
        r = subprocess.run(link, capture_output=True, text=True)
        if r.returncode:
            print("hookshot_direct.so link FAILED\n" + (r.stdout + r.stderr)[-3000:])
            return 1
        if not check_backend_symbols(ddll):
            return 1
        print("linked %s: %d MB (%.0fs)" % (ddll, os.path.getsize(ddll) >> 20, time.time() - t))
    if a.dll and not a.only:
        dll = os.path.join(out, "hookshot_game.dll")   # (+ hookshot_game.map: crash offsets -> f_<guest address>)
        t = time.time()
        r = subprocess.run([cc] + toolchain.native_target() + toolchain.native_link() +
                           ["-shared", "-o", dll, toolchain.link_opt("/map:" + dll[:-4] + ".map"), "@" + rsp],
                           capture_output=True, text=True)
        undefined = sorted({l.strip() for l in (r.stdout + r.stderr).splitlines() if "undefined symbol" in l})
        if r.returncode:
            print("link FAILED (%.0fs), %d undefined symbols" % (time.time() - t, len(undefined)))
            print("\n".join(undefined[:40]) if undefined else (r.stdout + r.stderr)[-3000:])
            return 1
        print("linked %s: %d MB, no undefined symbols (%.0fs)" % (dll, os.path.getsize(dll) >> 20, time.time() - t))
    if not a.only and not a.dry_run:
        removed, freed, kept = prune_object_cache()
        print("objcache: %d objects no build links, older than %g days, removed (%.1f GB); %.1f GB kept"
              % (removed, OBJ_CACHE_KEEP_DAYS, freed / 2**30, kept / 2**30), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

