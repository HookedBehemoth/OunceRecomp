"""(M6b) The direct back end's layer 0 window scheme, as tests/test_layer0_windows.py checks the frozen back end's.

Builds tests/direct_layer0_windows_test.cpp against gpu/direct/layer0_windows.h (its windows a granule short of 4 GB: the
longest whole range, the last granule of a stretch in one window only, pieces cut at a window's end, and the rest as the
original), checks that the copy differs from gpu/vk/layer0_windows.h only where it says so, and checks in the source that
gpu/direct/direct_pools.cpp binds through the pieces (cut at the window's end), imports outside its lock, publishes a chunk's
bits only after its binds were waited for, and reads no environment.
python -I -B tests/test_direct_layer0_windows.py
"""
import difflib
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class DirectLayer0WindowTests(unittest.TestCase):
    def test_arithmetic(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("no C++ compiler")
        with tempfile.TemporaryDirectory(prefix="hookshot-direct-layer0-") as tmp:
            binary = Path(tmp) / "direct_layer0_windows_test"
            build = subprocess.run(
                [compiler, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(ROOT / "gpu" / "direct"),
                 str(ROOT / "tests" / "direct_layer0_windows_test.cpp"), "-o", str(binary)],
                cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_copy_differs_only_where_it_says(self):
        frozen = (ROOT / "gpu" / "vk" / "layer0_windows.h").read_text(encoding="utf-8").splitlines()
        copy = (ROOT / "gpu" / "direct" / "layer0_windows.h").read_text(encoding="utf-8").splitlines()
        changed = [l for l in difflib.unified_diff(frozen, copy, lineterm="", n=0)
                   if l[:1] in "+-" and not l.startswith(("+++", "---"))]
        allowed = ("// (gpu/direct", "// size-06409", "// one window holds", "constexpr uint64_t kWindow", "constexpr uint64_t kMaxRange",
                   "if (k > 0 && k - 1 < kWindows) out.push_back({k - 1, window_offset(a) + kStride",
                   "const uint64_t prev = window_offset(a) + kStride;", "if (k > 0 && k - 1 < kWindows && prev < kWindow)")
        for l in changed:
            self.assertTrue(any(a in l for a in allowed), "an undocumented difference from gpu/vk/layer0_windows.h: " + l)
        self.assertTrue(any("kWindow = (4ull << 30) - (64ull << 10)" in l for l in changed))

    def test_pools(self):
        pools = (ROOT / "gpu" / "direct" / "direct_pools.cpp").read_text(encoding="utf-8")
        self.assertNotIn("getenv", pools)   # (no switch but HOS_DBG, and that is read in direct_main.cpp)
        # every bind goes through range_places (pieces cut at the window's end); chunk_places only names the windows to make
        add = block(pools, "void add_binds(")
        self.assertIn("l0::range_places(lo, hi, ps)", add)
        claimed = block(pools, "void bind_claimed(")
        cp = claimed[claimed.index("l0::chunk_places("):]
        self.assertIn("window_make(pl[i].window)", cp[:cp.index("\n", cp.index("for (int i = 0; i < n; i++)"))])
        # imports outside mu; binds submitted and waited for before the bits; the bits before the chunk's state
        self.assertLess(claimed.index("dma_buf_memory("), claimed.index("std::lock_guard lk(mu)"))
        self.assertLess(claimed.index("submit_binds(binds)"), claimed.index("bits->set(c)"))
        self.assertLess(claimed.index("bits->set(c)"), claimed.index("cstate[c].store(CH_BOUND"))
        self.assertIn("vkWaitForFences", block(pools, "VkResult submit_binds("))
        # the importer claims only what is still queued; the host's discard check counts chunks being imported
        self.assertIn("compare_exchange_strong(st, CH_IMPORTING", block(pools, "void worker_main("))
        self.assertIn("CH_IMPORTING", block(pools, "bool layer0_bound("))
        # a span: whole in the window of its start, or not given
        span = block(pools, "bool layer0_span(")
        self.assertIn("l0::fits_window(gpu, size)", span)
        self.assertLess(span.index("l0::fits_window(gpu, size)"), span.index("bind_claimed(claimed)"))


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
