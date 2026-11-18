"""HOS_BENCH (runtime/bench.c, runtime/bench.h): a benchmark run steps the game by exactly 1/60 s a frame and presents unpaced; it
measures from the frame the scene has been ready for a number of frames in a row.

The ready condition counts frames with every part settled in a row and starts again at any frame with one part not; the summary
takes the median and p99 as the back end's frame-time line does (the value at p * n in sorted order) and leaves its input as it
was. In the source: the switch answers the Insert menu's game speed "per frame" (rx_option 0), the real-time step keeps the
whole 1/60 s step with no frame seconds for the patched divides at any frame-rate cap, and the window's acquire waits for nothing.

python -I -B tests/test_bench.py
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r"""
#include "bench.h"
#include <stdio.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void) {
    BenchReady r = {0};
    for (int i = 0; i < 59; i++) CHECK(!bench_ready_frame(&r, BENCH_ALL, 60));
    CHECK(bench_ready_frame(&r, BENCH_ALL, 60));
    BenchReady s = {0};
    for (int i = 0; i < 59; i++) bench_ready_frame(&s, BENCH_ALL, 60);
    CHECK(!bench_ready_frame(&s, BENCH_ALL & ~BENCH_TEXTURES, 60));   /* (one part not settled: the count starts again) */
    for (int i = 0; i < 59; i++) CHECK(!bench_ready_frame(&s, BENCH_ALL, 60));
    CHECK(bench_ready_frame(&s, BENCH_ALL, 60));
    CHECK(!bench_ready_frame(&(BenchReady){0}, BENCH_PLAY | BENCH_SHADERS | BENCH_STREAMING, 1));
    double v[100], scratch[100];
    for (int i = 0; i < 100; i++) v[i] = 100 - i;   /* (100 down to 1) */
    const BenchSummary m = bench_summary(v, 100, scratch);
    CHECK(m.total == 5050 && m.mean == 50.5 && m.median == 51 && m.p99 == 100 && m.max == 100);
    CHECK(v[0] == 100 && v[99] == 1);   /* (the input as it was) */
    double one = 7;
    const BenchSummary o = bench_summary(&one, 1, scratch);
    CHECK(o.median == 7 && o.p99 == 7 && o.mean == 7);
    CHECK(bench_summary(v, 0, scratch).total == 0);
    /* game steps: a 2-then-0 pair is the clock read racing the game thread, not a violation; a fraction, a lasting
       miss or a lasting double step is */
    double st[8] = {1, 1, 2, 0, 1, 1, 1, 1};
    CHECK(bench_steps_off(st, 8) == 0);
    double fr[4] = {1, 1.5, 0.5, 1};
    CHECK(bench_steps_off(fr, 4) == 2);
    double miss[5] = {1, 0, 1, 1, 1};
    CHECK(bench_steps_off(miss, 5) == 3);
    double dbl[4] = {1, 2, 1, 1};
    CHECK(bench_steps_off(dbl, 4) == 2);
    double ok[3] = {1, 1, 1};
    CHECK(bench_steps_off(ok, 3) == 0);
    if (!fails) printf("PASS\n");
    return fails != 0;
}
"""


def body(text, head):
    at = text.index(head)
    end = re.search(r"\n}\n", text[at:])
    return text[at:at + end.end()]


class BenchTests(unittest.TestCase):
    def test_ready_and_summary(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="hookshot-bench-") as tmp:
            tmp = Path(tmp)
            (tmp / "t.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / "t.exe"
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-O1", "-I", str(ROOT / "runtime"), str(tmp / "t.c"),
                                    "-o", str(binary)], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_fixed_step_and_unpaced(self):
        vi = (ROOT / "runtime/nn_vi.c").read_text(encoding="utf-8")
        step = body(vi, "static void realtime_step(int64_t now) {")
        branch = step[step.index("if ((!rx_option(0, 1) && fps_max() <= 60) || hos_bench_on())"):]
        branch = branch[:branch.index("return;")]
        self.assertIn("__atomic_store_n(&hos_frame_seconds_bits, 0, __ATOMIC_RELAXED);", branch)   # (no frame seconds)
        self.assertIn("*slot = 60;", branch)   # (the nominal rate: a whole 1/60 s step)
        acquire = body(vi, "void hos_vi_native_acquire(int active) {")
        self.assertRegex(acquire, r"no_pacing = getenv\(\"HOS_NO_PACING\"\) != NULL \|\| hos_bench_on\(\);")
        self.assertIn("if (hos_bench_on()) hos_bench_present(now);", body(vi, "void hos_vi_native_presented(void) {"))
        rx = (ROOT / "runtime/rx_backend.c").read_text(encoding="utf-8")
        option = body(rx, "int rx_option(int id, int otherwise) {")
        self.assertIn("|| hos_bench_on();", option)
        self.assertIn("if (id == 0 && per_frame) return 0;", option)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
