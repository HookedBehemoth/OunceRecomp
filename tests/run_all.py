"""Every game-free check in one run: each tests/test_*.py in its own process, and a byte-compile of every tracked Python file. All of them run even when one fails; the failures are
printed whole at the end, with a table of every check (slowest first), also to $GITHUB_STEP_SUMMARY when it is set.
Exits 1 when anything failed.

    python -I -B tests/run_all.py [--jobs N] [--timeout SECONDS] [--strict] [--no-skip] [name ...]

A name picks the checks whose path contains it. --strict: a check that needs a tool this PC lacks (a compiler, the
SPIR-V or Vulkan headers, spirv-val) fails instead of being skipped (CI installs them all). --no-skip runs the checks on
the skip list below too. Standard library only: no package is installed for the runner itself.
"""
import argparse
import concurrent.futures
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WINDOWS = os.name == "nt"
OS = "windows" if WINDOWS else "linux"

# Arguments some tests need. {name} is a tool or folder found below (find_tools); a check whose tools are missing is
# skipped, or fails with --strict. Tests not listed run with no arguments and their own defaults.
ARGS = {
    "tests/test_direct_spv_late.py": ["--cxx", "{cxx}"],
    "tests/test_multiwait_wake.py": ["--cxx", "{cxx}"],
    "tests/test_oot_wide_save_picture.py": ["--cc", "gcc"],
    "tests/test_frame_rate_cap_step.py": ["--cc", "gcc"],
    "tests/test_reserved_core.py": ["--cxx", "g++"],
    "tests/test_core_number.py": ["--cc", "gcc"],
    "tests/test_prompt_font_bind.py": ["--cc", "gcc"],
    "tests/test_stick_rest.py": ["--cc", "gcc"],
    "tests/test_widescreen_shapes.py": ["--cc", "gcc"],
    "tests/test_nvn_buffer_map.py": ["--cc", "gcc"],
    "tests/test_nvn_host_tables.py": ["--cc", "gcc"],
    "tests/test_audio_voice_biquad.py": ["--cc", "gcc"],
    "tests/test_motion.py": ["--cc", "gcc"],
    "tests/test_nvn_deswizzle.py": ["--cc", "gcc"],
    "tests/test_low_memory_notice.py": ["--cc", "gcc"],
    "tests/test_draw_parameters.py": ["--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"],
    "tests/test_spirv_struct_identity.py": ["--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"],
}

# Python packages some tests import (CI installs them; the runner itself needs none)
MODULES = {
    "tests/test_direct_picture_check.py": ["numpy", "zstandard"],
}

# Checks that cannot run in CI, with why, by OS. Each still runs with --no-skip (or by name).
SKIP = {
    # (a GPU)
    # (Linux only)
    "tests/test_oot_saves.py": ({"windows"}, "the save script's folder discovery is the Linux one (on Windows the runtime's saves are in the game folder: --ounce)"),
}
# (spirv-val: SPIRV-Tools comes with the Linux runner's packages, not on Windows)
for _name in ("tests/test_draw_parameters.py", "tests/test_spirv_struct_identity.py"):
    SKIP[_name] = ({"windows"}, "needs spirv-val: checked on Linux")


def which(*names):
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    return None


def header_dir(env, vendored, probe):
    """A headers folder: the environment variable, else the vendored copy (third_party/<vendored>/include)"""
    if os.environ.get(env):
        return os.environ[env]
    if vendored:
        inc = os.path.join(ROOT, "third_party", vendored, "include")
        if os.path.exists(os.path.join(inc, probe)):
            return inc
    return None


def find_tools():
    return {
        "spirv_headers": header_dir("OUNCE_SPIRV_HEADERS", "SPIRV-Headers", os.path.join("spirv", "unified1", "spirv.h")),
        "vulkan_headers": header_dir("OUNCE_VULKAN_HEADERS", None, os.path.join("vulkan", "vulkan.h")),
        "spirv_val": which("spirv-val"),
        "glslang": which("glslangValidator", "glslang"),
        "cxx": which(os.environ.get("CXX", ""), "clang++", "g++", "c++"),
    }


class Check:
    def __init__(self, name, run, why_skip=None):
        self.name, self.run, self.why_skip = name, run, why_skip
        self.status, self.secs, self.output = "skip", 0.0, ""


def python_check(rel, tools, timeout):
    args, missing = [], []
    for a in ARGS.get(rel, []):
        if a.startswith("{"):
            v = tools.get(a.strip("{}"))
            if not v:
                missing.append(a.strip("{}"))
            args.append(v or "")
        else:
            args.append(a)
    # (a compiler named by the arguments, or the test's own default one, must be on PATH)
    for i, a in enumerate(args[:-1]):
        if a in ("--cc", "--cxx") and not which(args[i + 1]):
            missing.append(args[i + 1])
    missing += [m for m in MODULES.get(rel, []) if importlib.util.find_spec(m) is None]

    def run():
        return subprocess.run([sys.executable, "-I", "-B", os.path.join(ROOT, rel)] + args, cwd=ROOT, timeout=timeout,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    return run, missing


class Result:
    def __init__(self, returncode, stdout):
        self.returncode, self.stdout = returncode, stdout


def compile_all():
    """Every tracked .py file byte-compiled (in memory: nothing written), so a syntax error anywhere fails"""
    r = subprocess.run(["git", "ls-files", "-z", "--", "*.py"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode:
        return Result(r.returncode, r.stdout)
    files = [f for f in r.stdout.decode("utf-8").split("\0") if f]
    bad = []
    for f in files:
        try:
            with open(os.path.join(ROOT, f), "rb") as src:
                compile(src.read(), f, "exec", dont_inherit=True)
        except (SyntaxError, ValueError) as e:
            bad.append("%s: %s" % (f, e))
    text = "%d files byte-compiled\n" % len(files) + "".join(b + "\n" for b in bad)
    return Result(1 if bad else 0, text.encode())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("names", nargs="*")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    p.add_argument("--timeout", type=int, default=600, help="seconds per check (default 600)")
    p.add_argument("--strict", action="store_true", help="a missing tool fails the check instead of skipping it")
    p.add_argument("--no-skip", action="store_true", help="also run the checks on the skip list")
    a = p.parse_args()
    tools = find_tools()

    checks = []
    tests = sorted(d + "/" + f for d in ("tests",) for f in os.listdir(os.path.join(ROOT, d))
                   if f.startswith("test_") and f.endswith(".py"))
    for rel in tests:
        oses, reason = SKIP.get(rel, (set(), ""))
        run, missing = python_check(rel, tools, a.timeout)
        c = Check(rel, run)
        if OS in oses and not a.no_skip and not any(n in rel for n in a.names):
            c.why_skip = reason
        elif missing and not a.strict:
            c.why_skip = "not found here: " + ", ".join(missing)
        elif missing:
            c.run = lambda m=missing: Result(1, ("not found: %s (--strict)\n" % ", ".join(m)).encode())
        checks.append(c)
    checks.append(Check("(byte-compile every tracked .py)", compile_all))
    if a.names:
        checks = [c for c in checks if any(n in c.name for n in a.names)]

    def one(c):
        t = time.time()
        try:
            r = c.run()
            c.status = "pass" if r.returncode == 0 else "FAIL"
            c.output = r.stdout.decode("utf-8", "replace")
        except subprocess.TimeoutExpired as e:
            c.status = "TIMEOUT"
            c.output = (e.stdout or b"").decode("utf-8", "replace") + "\n(stopped after %d s)\n" % e.timeout
        except Exception as e:   # (a check that could not even start is a failure, not a crash of the runner)
            c.status = "FAIL"
            c.output = "%s: %s\n" % (type(e).__name__, e)
        c.secs = time.time() - t
        return c

    to_run = [c for c in checks if not c.why_skip]
    t0 = time.time()
    print("%d checks, %d skipped, %d at once (%s)" % (len(to_run), len(checks) - len(to_run), a.jobs, OS), flush=True)
    with concurrent.futures.ThreadPoolExecutor(max(1, a.jobs)) as ex:
        for c in concurrent.futures.as_completed([ex.submit(one, c) for c in to_run]):
            c = c.result()
            print("%-7s %6.1fs  %s" % (c.status, c.secs, c.name), flush=True)
    wall = time.time() - t0

    failed = [c for c in to_run if c.status != "pass"]
    gh = os.environ.get("GITHUB_ACTIONS") == "true"
    for c in failed:
        print(("::group::" if gh else "\n==== ") + "%s %s" % (c.status, c.name), flush=True)
        print(c.output.rstrip())
        if gh:
            print("::endgroup::")
            print("::error title=%s %s::see the step's log for its output" % (c.status, c.name))

    order = sorted(to_run, key=lambda c: -c.secs) + sorted((c for c in checks if c.why_skip), key=lambda c: c.name)
    passed = sum(c.status == "pass" for c in to_run)
    head = "%d passed, %d failed, %d skipped in %.0f s (%s, %d at once)" % (passed, len(failed), len(checks) - len(to_run),
                                                                         wall, OS, a.jobs)
    print("\n%-8s %8s  %s" % ("result", "time", "check"))
    for c in order:
        print("%-8s %8s  %s%s" % (c.status, "" if c.why_skip else "%.1fs" % c.secs, c.name,
                                  "  (%s)" % c.why_skip if c.why_skip else ""))
    print(head, flush=True)

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        mark = {"pass": "pass", "FAIL": "**FAIL**", "TIMEOUT": "**TIMEOUT**", "skip": "skip"}
        lines = ["### Tests (%s): %s" % (OS, head), "", "| Result | Time | Check | Note |", "|---|---:|---|---|"]
        for c in order:
            note = c.why_skip or ""
            if c.status in ("FAIL", "TIMEOUT"):
                last = [l for l in c.output.strip().splitlines() if l.strip()][-1:] or [""]
                note = "`%s`" % last[0].replace("|", "\\|").replace("`", "'")[:160]
            lines.append("| %s | %s | `%s` | %s |" % (mark[c.status], "" if c.why_skip else "%.1f s" % c.secs, c.name, note))
        with open(summary, "a", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n\n")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
