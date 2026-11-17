"""Build the game: work/sa/build/hookshot. Every build lifts first: ounce-lift (ounce/lift, built here with cargo; about
8 s) writes into work/sa the lifted code (one LLVM IR file a partition, lift_<c|h><n>.ll, and lift_answers.ll), the
runtime's tables and the image's description (gen/*.ll) and the image (image/image.bin); `ounce-lift rt64` writes the
guest model (rt64/), which reads no dump. The build compiles those with the native runtime in host/ (TOTKPort's hos/, adapted) and the direct back end (gpu/direct:
hookshot_direct.so). Linking it is also the check that the whole game closes over itself: any unresolved symbol fails
the link.

Objects are cached by their key in work/objcache: a C file's by its text, flags and headers (object_key); a partition's
by its IR text and flags (ir_key), so a partition whose IR did not change is not compiled again.

Usage:  python -I nx2/build.py [--jobs N] [--opt O1] [--standalone]   (--jobs default: HOOKSHOT_JOBS, else by memory)
  --standalone: accepted for the scripts that pass it (the build is always the standalone game)
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
import gamedesc
import kit
import toolchain
import version

HOST = os.path.join(kit.HERE, "..", "host")
LIFTER = os.path.join(kit.ROOT, "ounce", "lift")   # (the lifter's crate; its binary: target/release/ounce-lift)
OPUS = os.path.join(kit.HERE, "..", "third_party", "opus")
OBJ_CACHE = os.path.join(kit.WORK, "objcache")


# (linux) Linux builds: an ELF executable and hookshot_direct.so; the host reaches the system through host/os/os.h
# (os_linux.c), the window and sound on SDL2 (host/posix). HOOKSHOT_SYSROOT=<dir>: compile and link
# against that root (the AppImage's: glibc 2.35, so the game runs on older systems than the one that built it).
if toolchain.windows():
    raise SystemExit("build.py: the host is built for Linux only (RX_TARGET=windows is not supported)")
POSIX = os.path.join(kit.HERE, "..", "host", "posix")   # (the window on SDL2)
OS_DIR = os.path.join(kit.HERE, "..", "host", "os")   # (os.h, the host's one way to the system, and its os_linux.c)
SYSROOT = os.environ.get("HOOKSHOT_SYSROOT", "")
EXE = "hookshot"
DIRECT_LIB = "hookshot_direct.so"


def dynamic_exports(exe):
    """the names exe's dynamic symbol table defines (empty when there is no exe or no nm)"""
    nm = shutil.which("nm") or shutil.which("llvm-nm")
    if not nm or not os.path.exists(exe):
        return set()
    r = subprocess.run([nm, "-D", "--defined-only", exe], capture_output=True, text=True)
    return {l.split()[-1] for l in r.stdout.splitlines() if l.split()}


def check_backend_symbols(so, exe):
    """The back end is dlopened with RTLD_NOW, so one unresolved symbol stops the whole thing
    loading -- and the only sign of that at run time is "the renderer did not start: the window
    stays black", with no symbol named. The link itself does not fail: a shared object is allowed to
    leave symbols for its loader to supply, which is exactly what the host executable does for the
    weak hos_* ones.

    So: anything still undefined here that is not weak, not the C or C++ runtime's and not one the
    executable exports (os.h's os_*: the back end calls the executable's one OS layer) is a mistake,
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
    exported = dynamic_exports(exe)
    bad = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2 or parts[0] != "U":   # ("w" is weak: the host supplies those, or does not)
            continue
        name = parts[1]
        if "@GLIBC" in name or "@CXXABI" in name or "@GLIBCXX" in name or "@GCC" in name:
            continue
        if name in exported:
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


def HOT_ORDER(desc, out):
    """the linker's flags for the game description's link order (hot_order): its functions packed together in that
    order, through a symbol-ordering file written into the build folder"""
    order = desc.link_order()
    if not order:
        return []
    p = os.path.join(out, "hot_order.txt")
    with open(p, "w") as f:
        f.write("".join("f_%08x\n" % a for a in order))
    return ["-Wl,--symbol-ordering-file=" + p, "-Wl,--no-warn-symbol-ordering"]


HOST_LIBS = ["-lpthread", "-ldl", "-lm"]
GPU = os.path.join(kit.HERE, "..", "gpu")
def tree_headers(*roots, skip=()):
    """Every header under these folders, as one dependency blob (Eden's headers sit in many folders); folders named
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


# (what one compiler is counted at where its own need is not worked out: a hot piece (0.8-1.7 GB measured at -O2),
# and the compile countdown's guess of how many the free memory holds. About the median partition's peak)
COMPILER_MB = 1500


def default_jobs():
    """(jobs, why): HOOKSHOT_JOBS, else every CPU (the memory gate, MemoryGate, holds compiles back when memory is short)"""
    if os.environ.get("HOOKSHOT_JOBS"):
        return int(os.environ["HOOKSHOT_JOBS"]), "HOOKSHOT_JOBS"
    cpus = os.cpu_count() or 1
    return cpus, "%d CPUs; compiles wait on the memory gate" % cpus


def cgroup_room_mb():
    """The room left under the tightest memory.max of this process's cgroup and its parents (a capped scope:
    systemd-run -p MemoryMax=...), in MB: memory.max less memory.current; None without a limit"""
    room = None
    try:
        with open("/proc/self/cgroup") as f:
            path = [l.split(":", 2)[2].strip() for l in f if l.startswith("0::")][0].rstrip("/")
    except (OSError, IndexError):
        return None
    while True:
        d = "/sys/fs/cgroup" + path
        try:
            with open(d + "/memory.max") as f:
                v = f.read().strip()
            if v != "max":
                with open(d + "/memory.current") as f:
                    left = (int(v) - int(f.read().strip())) >> 20
                room = left if room is None else min(room, left)
        except (OSError, ValueError):
            pass
        if path in ("", "/"):
            break
        path = os.path.dirname(path)
    return room


def free_mb():
    """The memory free now, in MB: MemAvailable, or less when this build runs in a capped cgroup (cgroup_room_mb);
    None: not known"""
    free = None
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    free = int(line.split()[1]) >> 10
                    break
    except (OSError, ValueError, AttributeError):
        return None
    room = cgroup_room_mb()
    if room is not None:
        free = room if free is None else min(free, room)
    return free


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
    """What compiling a file takes at its peak, in MB, a little over: the runtime's C files ~0.2 GB, the C++ files up to
    ~1 GB, a lifted partition's IR COMPILER_MB"""
    if src.endswith(".ll"):
        return COMPILER_MB
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


def flags(rt64, opt):
    """the guest model's runtime (rt64/a64_rt.c)"""
    return target() + CPU + [
        "-I" + rt64, "-I" + os.path.join(kit.KIT, "rt"), "-Wno-everything",
        "-fno-strict-aliasing", "-mcx16", "-" + opt, "-g0", "-mllvm", "-rotation-max-header-size=0",
        "-ffunction-sections", "-mllvm", "-capture-tracking-max-uses-to-explore=5000"]


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


# ---- the lifted code: ounce-lift's partitions, LLVM IR compiled by clang (after `opt` at a level without DSE)

# (dead context stores: LLVM drops a store a later one overwrites only if it can see that nothing reads the context
# in between. Capture tracking gives up past N uses of the context pointer (a large function has thousands) and then
# assumes the context escaped; DSE stops after so many definitions in a block or steps of its walk. Measured on the
# market's hot functions: context stores after -O2 -40 to -47 %, compile time of the largest 1.6 s -> 3.8 s)
DSE_FLAGS = ["-mllvm", "-capture-tracking-max-uses-to-explore=100000", "-mllvm", "-dse-memoryssa-defs-per-block-limit=100000",
             "-mllvm", "-dse-memoryssa-scanlimit=1000", "-mllvm", "-dse-memoryssa-walklimit=1000",
             "-mllvm", "-dse-memoryssa-otherbb-cost=1"]
_PIPELINES = {}


def part_flags(level, march_flags):
    """clang's flags for a partition's IR at level (O1 cold, the hot level for the hot list's)"""
    return march_flags + ["-" + level, "-g0", "-Wno-everything", "-ffunction-sections",
                          "-mllvm", "-rotation-max-header-size=0"] + DSE_FLAGS


def opt_argv(clang, level, march_flags):
    """the `opt` command a partition's IR goes through before clang's code generation, or None (clang optimises).
    clang's -O1 pipeline has no DSE, so every context store a later one overwrites stayed in the cold partitions: at
    a level whose pipeline lacks `dse`, the IR runs through clang's own pipeline for that level with `dse` after
    `memcpyopt`, where -O2 has it (6 cold partitions: .text -13 %, context stores -33 %, compile time unchanged; -O2
    gave -16 % at 2.25 times the time). The pipeline is asked of the installed clang, so it follows the toolchain"""
    k = (clang, level, tuple(march_flags))
    if k not in _PIPELINES:
        r = subprocess.run([clang, "-" + level] + march_flags + ["-Wno-everything", "-mllvm", "-print-pipeline-passes",
                            "-x", "ir", os.devnull, "-c", "-o", os.devnull], capture_output=True, text=True, check=True)
        p = r.stdout.strip()
        names = set(re.findall(r"[\w-]+", p))
        argv = None
        if "dse" not in names and "memcpyopt" in names:
            assert p.count("memcpyopt,") == 1, "clang -%s: memcpyopt not once in its pipeline" % level
            cpu = [f.replace("-march=", "-mcpu=") for f in march_flags if f.startswith("-march=")]
            argv = [toolchain.need(toolchain.find("opt"))] + cpu + ["-passes=" + p.replace("memcpyopt,", "memcpyopt,dse,")] + \
                [f for f in DSE_FLAGS if f != "-mllvm"]
        _PIPELINES[k] = argv
    return _PIPELINES[k]


# (the lifter writes textual LLVM IR, whose syntax is only kept within one LLVM major)
LLVM_MAJOR = 23


def check_llvm(clang):
    """SystemExit unless clang is of LLVM_MAJOR, the version the lifter's IR text is written for"""
    m = re.search(r"clang version (\d+)\.", subprocess.run([clang, "--version"], capture_output=True, text=True).stdout)
    if not m or int(m.group(1)) != LLVM_MAJOR:
        raise SystemExit("build: %s is LLVM %s, but the lifter's IR is written for LLVM %d: install LLVM %d, or move "
                         "LLVM_MAJOR (nx2/build.py) and check the emitter (ounce/lift/src/emit.rs) against the new "
                         "version's IR syntax" % (clang, m.group(1) if m else "?", LLVM_MAJOR, LLVM_MAJOR))


def ir_key(src, fl, pre):
    """A partition's object key: its flags, the `opt` command (pre) and its IR text"""
    return hashlib.sha1((" ".join(fl + (pre or [])) + "\n").encode() + open(src, "rb").read(),
                        usedforsecurity=False).hexdigest()


def job_key(job):
    """The object key of a compile job: (cc, src, obj, flags, headers) for C and C++, (clang, src, obj, flags, pre) for IR"""
    _, src, _, fl, deps = job
    return ir_key(src, fl, deps) if src.endswith(".ll") else object_key(src, fl, deps)


def lifter():
    """ounce-lift, built (cargo build --release; nothing to do when it is up to date)"""
    cargo = shutil.which("cargo") or os.path.expanduser("~/.cargo/bin/cargo")
    r = subprocess.run([cargo, "build", "--release", "--quiet", "--manifest-path", os.path.join(LIFTER, "Cargo.toml")])
    if r.returncode:
        raise SystemExit("build: ounce-lift did not build (cargo, ounce/lift)")
    return os.path.join(LIFTER, "target", "release", "ounce-lift")


def lift(sa, game, jobs):
    """The lift into sa (ounce-lift): gen/, image/image.bin and the partitions' IR; the partitions' paths"""
    for d in ("gen", "image"):   # (a worktree's work/sa once linked these to the main checkout's)
        if os.path.islink(os.path.join(sa, d)):
            os.remove(os.path.join(sa, d))
    for f in os.listdir(sa):   # (an earlier lift's partitions: the count changes with the code)
        if f.startswith("lift_") and f.endswith(".ll"):
            os.remove(os.path.join(sa, f))
    gen = os.path.join(sa, "gen")
    for f in os.listdir(gen) if os.path.isdir(gen) else []:   # (the C an earlier lift wrote: no reader may take it)
        if f.endswith(".c"):
            os.remove(os.path.join(gen, f))
    t = time.time()
    r = subprocess.run([lifter(), "lift", sa, "--game", game, "--work", kit.WORK, "--root", kit.ROOT, "-j", str(jobs)],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("build: the lift failed:\n" + (r.stdout + r.stderr)[-4000:])
    for line in r.stdout.splitlines():
        if line.startswith(("lift:", "whole pipeline:")):
            print(line, flush=True)
    print("lift: %.0fs" % (time.time() - t), flush=True)
    return sorted((os.path.join(sa, f) for f in os.listdir(sa) if f.startswith("lift_") and f.endswith(".ll")),
                  key=lambda p: -os.path.getsize(p))   # (the biggest first: the compile's long pole starts early)


def compile_ll(job):
    """One partition's IR: through `opt` (pre) when given, then clang; the object cached by ir_key"""
    clang, src, obj, fl, pre = job
    key = PLANNED.get((src, obj)) or ir_key(src, fl, pre)
    stamp = obj + ".sha1"
    if os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key:
        return src, 0.0, None
    cached = os.path.join(OBJ_CACHE, key + ".o")
    if os.path.exists(cached):
        if os.path.exists(obj):
            os.remove(obj)
        os.link(cached, obj)
        open(stamp, "w").write(key)
        return src, 0.0, None
    lease = GATE.enter(src) if GATE else None
    t = time.time()
    try:
        cl = [clang] + fl
        data = open(src, "rb").read()
        if pre:
            with subprocess.Popen(pre + ["-", "-o", "-"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE) as p:
                if GATE:
                    GATE.started(lease, p)
                data, err = p.communicate(data)
            if p.returncode:
                return src, time.time() - t, (err or b"opt exited with status %d" % p.returncode)[-3000:].decode(errors="replace")
            cl += ["-Xclang", "-disable-llvm-passes"]
        with subprocess.Popen(cl + ["-x", "ir", "-", "-c", "-o", obj], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE) as p:
            if GATE:
                GATE.started(lease, p)
            out, err = p.communicate(data)
        if p.returncode:
            return src, time.time() - t, (err or out or b"clang exited with status %d" % p.returncode)[-3000:].decode(errors="replace")
    finally:
        if GATE:
            GATE.leave(lease)
    open(stamp, "w").write(key)
    cache_put(obj, key)
    return src, time.time() - t, None


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
        else:   # (seconds per MB measured on a full build: the hot partitions, the cold ones, the rest)
            out.append(1.3)
    return out


def plan_jobs(jobs):
    """Which jobs this run really compiles, and every job's object key. A job whose object is stamped with its key, or
    whose key is in the object cache (the other target's build), is only linked in: not work, and not in the countdown.
    The keys go to compile_one, which then does not hash every source a second time."""
    work, keys = [], {}
    for cc, src, obj, fl, deps in jobs:
        key = job_key((cc, src, obj, fl, deps))
        keys[(src, obj)] = key
        stamp = obj + ".sha1"
        if os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key:
            continue
        if os.path.exists(os.path.join(OBJ_CACHE, key + ".o")):
            continue
        work.append((cc, src, obj, fl, deps))
    return work, keys


PLANNED = {}   # (src, obj) -> the object key plan_jobs worked out, so compile_one does not hash every source again


def compile_one(job):
    cc, src, obj, fl, deps = job
    if src.endswith(".ll"):   # (a lifted partition)
        return compile_ll(job)
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
    p.add_argument("--jobs", type=int, default=None)   # (default: default_jobs)
    p.add_argument("--opt", default="O1")
    p.add_argument("--standalone", action="store_true")   # (always: kept for the scripts that pass it)
    gamedesc.add_argument(p)
    # (a check with no game: everything up to the compile, without the lift's files; every other file the compile would
    # read must be there, and nothing is compiled)
    p.add_argument("--dry-run", action="store_true")
    a = p.parse_args()
    a.standalone = True
    desc = gamedesc.use(a.game)
    if a.jobs is None:
        a.jobs, why = default_jobs()
        print("build: %d jobs (%s)" % (a.jobs, why), flush=True)
    sa = os.path.join(kit.WORK, "sa")
    gen = os.path.join(sa, "gen")
    out = os.path.join(sa, "build")
    obj_dir = os.path.join(out, "obj")
    os.makedirs(obj_dir, exist_ok=True)
    rt64 = os.path.join(sa, "rt64")
    parts = []
    if os.path.islink(rt64):   # (a worktree's work/sa once linked it to the main checkout's)
        os.remove(rt64)
    if subprocess.run([lifter(), "rt64", rt64, "--root", kit.ROOT]).returncode:   # (the guest model: needs no dump)
        raise SystemExit("build: ounce-lift rt64 failed")
    if not (os.environ.get("RX_DIRECT_ONLY") or a.dry_run):
        parts = lift(sa, os.path.abspath(a.game), a.jobs)
    cc = toolchain.need(toolchain.find("clang"))
    if parts:
        check_llvm(cc)
    drop_stale_objects(cc, [obj_dir])
    fl = flags(rt64, a.opt)
    # (which Hookshot this is, for the log's first line and the Insert menu: given to those two files only, so a new
    # release name rebuilds them and nothing else)
    stamp_def = version.define()
    print("version: %s" % version.describe(), flush=True)
    deps = b"".join(open(os.path.join(rt64, f), "rb").read() for f in sorted(os.listdir(rt64)) if f.endswith(".h"))
    # (the lift's generated IR, compiled as lift_answers is; none without a lift)
    gen_ir = [os.path.join(gen, f + ".ll") for f in ("a64_table", "a64_hooks", "a64_code_slots", "a64_stubs", "hos_image",
                                                     "nvn2_exports")]
    if os.environ.get("RX_DIRECT_ONLY") or a.dry_run:
        gen_ir = []
    srcs = [os.path.join(rt64, "a64_rt.c")]
    host_srcs = desc.host_sources()   # (the game description's; the patch declarations are scanned in them, too)
    host_srcs += [os.path.join(POSIX, f) for f in ("display_sdl.c",)] + \
        [os.path.join(OS_DIR, "os_linux.c"), os.path.join(OS_DIR, "os_guest_linux.c")]
    # the lifted game: each partition at its level (lift_h*: the hot list's, RX_HOT_OPT, default O2; the rest and
    # lift_answers at --opt), through opt_argv's pipeline where clang's has no DSE
    jobs = []
    for s in parts:
        level = os.environ.get("RX_HOT_OPT", "O2") if os.path.basename(s).startswith("lift_h") else a.opt
        jobs.append((cc, s, os.path.join(obj_dir, os.path.basename(s)[:-3] + ".o"), part_flags(level, target() + CPU),
                     opt_argv(cc, level, target() + CPU)))
    jobs += [(cc, s, os.path.join(obj_dir, os.path.basename(s)[:-3] + ".o"), part_flags(a.opt, target() + CPU),
              opt_argv(cc, a.opt, target() + CPU)) for s in gen_ir]
    jobs += [(cc, s, os.path.join(obj_dir, os.path.basename(s)[:-2] + ".o"), fl, deps) for s in srcs]
    # the runtime: ordinary C at -O2, with its own headers (any host/*.h change rebuilds all of it)
    host_fl = target() + CPU + ["-I" + OS_DIR, "-D_GNU_SOURCE"] + \
        ["-I" + HOST, "-I" + rt64, "-Wno-everything", "-O2", "-g0", "-mcx16"] + \
        ["-Werror=implicit-function-declaration",   # (a call without a declaration returns an int: a 64-bit value cut)
         "-Werror=incompatible-pointer-types"]   # (a lock or handle of one kind handed where another is wanted)
    host_deps = deps + b"".join(open(os.path.join(HOST, f), "rb").read() for f in sorted(os.listdir(HOST))
                                if f.endswith(".h"))
    host_deps += open(os.path.join(OS_DIR, "os.h"), "rb").read() + open(os.path.join(OS_DIR, "os_guest.h"), "rb").read()
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
    build_direct = True
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
        dflags = target() + CPU + ["-fPIC", "-D_GNU_SOURCE", "-I" + OS_DIR] + [
            # (what the code takes for granted, which newer C++ libraries bring in by themselves and older ones do not)
            "-include", "algorithm", "-include", "cstdio", "-include", "cstdlib", "-include", "cstring", "-include", "functional",
            "-include", "string", "-include", "vector",
            "-std=c++20", "-O2", "-g0", "-DNDEBUG", "-Wno-everything", "-DVK_NO_PROTOTYPES", "-DMKWVK_BUILDING",
            "-I" + os.path.join(dx_eden, "compat"), "-I" + os.path.join(GPU, "compat"), "-I" + dx_eden,
            "-I" + os.path.join(dx_eden, "sirit", "include"), "-I" + os.path.join(dx_eden, "sirit", "src"),
            "-I" + os.path.join(spirv_headers, "include"), "-I" + os.path.join(spirv_headers, "include", "spirv", "unified1"),
            "-I" + os.path.join(vk_headers, "include"), "-I" + os.path.join(vma_headers, "include")]
        ddeps = tree_headers(direct) + b"".join(open(os.path.join(GPU, "api", f), "rb").read() for f in sorted(os.listdir(os.path.join(GPU, "api"))))
        ddeps += open(os.path.join(OS_DIR, "os.h"), "rb").read() + open(os.path.join(OS_DIR, "os_guest.h"), "rb").read()
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
    if host_srcs:   # (the patch declarations the lifter scanned, against what the compiler made of them)
        r = subprocess.run([lifter(), "records", "--game", a.game, "--root", kit.ROOT] +
                           [j[2] for j in jobs if j[1] in set(host_srcs)], capture_output=True, text=True)
        print((r.stdout + r.stderr).strip(), flush=True)
        if r.returncode:
            return 1
    vk_objs = {j[2] for j in direct_jobs}   # (the back end's objects: not the exe's)
    objs = [j[2] for j in jobs if j[2] not in vk_objs]
    rsp = os.path.join(out, "objs.rsp")
    open(rsp, "w", encoding="utf-8").write("\n".join('"%s"' % o.replace("\\", "/") for o in objs))
    if not direct_only:
        exe = os.path.join(out, EXE)   # (+ hookshot.map: crash offsets -> f_<guest address>)
        t = time.time()
        # (--export-dynamic-symbol: what hookshot_direct.so calls in the exe: os.h's and os_guest.h's os_*, hos_mem_ledger)
        r = subprocess.run([cc] + target() + ["-fuse-ld=lld", "-o", exe, "-Wl,-Map=" + exe + ".map", "-Wl,--gc-sections",
                                              "-Wl,--export-dynamic-symbol=os_*", "-Wl,--export-dynamic-symbol=hos_mem_ledger", "@" + rsp] + HOT_ORDER(desc, out) +
                           (FFMPEG_LX[1] if FFMPEG_LX else []) + HOST_LIBS,
                           capture_output=True, text=True)
        undefined = sorted({l.strip() for l in (r.stdout + r.stderr).splitlines() if "undefined symbol" in l})
        if r.returncode:
            print("link FAILED (%.0fs), %d undefined symbols" % (time.time() - t, len(undefined)))
            print("\n".join(undefined[:60]) if undefined else (r.stdout + r.stderr)[-3000:])
            return 1
        print("linked %s: %d MB (%.0fs)" % (exe, os.path.getsize(exe) >> 20, time.time() - t))
        import label_syms   # (the game's labels for the functions we know, in the symbol table only)
        named, absent = label_syms.apply(exe, desc.labels)
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
        if not check_backend_symbols(ddll, os.path.join(out, EXE)):
            return 1
        print("linked %s: %d MB (%.0fs)" % (ddll, os.path.getsize(ddll) >> 20, time.time() - t))
    if not a.dry_run:
        removed, freed, kept = prune_object_cache()
        print("objcache: %d objects no build links, older than %g days, removed (%.1f GB); %.1f GB kept"
              % (removed, OBJ_CACHE_KEEP_DAYS, freed / 2**30, kept / 2**30), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

