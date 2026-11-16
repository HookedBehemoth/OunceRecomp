"""A Frame rate cap above 60 steps the game by the frames' own time, so the game is not sped up.

The engine's rate is a whole number of display intervals, 60 at most, and it puts 60 back over a higher one: with only the
rate raised, every frame above 60 fps still stepped 1/60 s. The lifter's At probes (ounce-lift scan's At rows) give the
engine's two step divides the host's measured frame time (host/oot_probes.c hos_oot_frame_step, after each FDIV), and host/nn_vi.c realtime_step publishes it while the cap is above
60 and the hook is in the build. At a cap of 60 the bits stay 0 and the lifted code runs as shipped; a game version whose
code differs keeps its own code (both divides or neither) and the build goes on with a message.

python -I -B tests/test_frame_rate_cap_step.py [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import lift_scan  # noqa: E402  (the host's rows as the lifter reads them: ounce-lift scan)

def block(source, marker):
    start = source.index(marker)
    return source[start:start + re.search(r"^}", source[start:], re.M).end()]


def definition(source, marker):
    """a function of host C from its first line: that line when the function is on one line, else up to its closing brace"""
    start = source.index(marker)
    line = source[start:source.index("\n", start)]
    return line if line.rstrip().endswith("}") else block(source, marker)


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


FIXTURE = r'''#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include "patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static uint8_t guest[0x4000];
#define GP(a) ((void *)(guest + (a)))
#define MOD 0x1000u
static void hos_log(const char *fmt, ...) { (void)fmt; }
int rx_option(int id, int otherwise) { (void)id; return otherwise; }
static int hos_bench_on(void) { return 0; }   /* (HOS_BENCH off: test_bench.py checks it on) */
int hos_ini_option(const char *key, int otherwise) { (void)key; return otherwise; }
volatile uint64_t hos_gfx_module = MOD;
volatile int hos_fixed_resolution, hos_dynres_runs;
volatile uint32_t hos_fixed_scale_bits;
volatile float hos_text_scale;
volatile uint64_t hos_step_probe, hos_res_module;
volatile uint32_t hos_frame_seconds_bits;
@HOST@
@PROBES@
/* (the lifted divide, fdiv s8, s1, s0, then the At probe: s8 := hos_oot_frame_step_probe(s1, s8, x19)) */
static float lifted_step(float base, float nominal) {
    float q = base / nominal;
    return hos_oot_frame_step_probe(base, q, hos_step_probe);
}
static int64_t now = 1000000000LL;
static float frames(int n, double seconds) {   /* (n presents that far apart: the seconds the hook is given, 0 when off) */
    float f;
    uint32_t bits;
    for (int i = 0; i < n; i++) now += (int64_t)(seconds * 1e9), realtime_step(now);
    bits = hos_frame_seconds_bits;
    memcpy(&f, &bits, 4);
    return bits ? f : 0.0f;
}
static int near(float a, double b) { return a > b * 0.999 && a < b * 1.001; }
int main(int argc, char **argv) {
    int *limit = (int *)GP(MOD + 0xfc), *rate = (int *)GP(MOD + 0x2c8);
    const char *mode = argc > 1 ? argv[1] : "";
    *limit = 60, *rate = 60;
    hos_step_probe = 0x2000;   /* (the lifted code's frame step sets it) */
    realtime_step(now);
    if (!strcmp(mode, "cap 120")) {
        CHECK(near(frames(72000, 1.0 / 120), 1.0 / 120) && *rate == 60 && *limit == 120);   /* (the rate stays the engine's) */
        CHECK(near(lifted_step(60.0f, (float)*rate), 0.5) && near(lifted_step(30.0f, (float)*rate), 0.25));   /* (half a 60th a frame) */
        CHECK(near(frames(10, 1.0 / 40), 1.0 / 40) && *rate == 40 && near(lifted_step(60.0f, (float)*rate), 1.5));
        CHECK(near(frames(10, 1.0 / 1000), 1.0 / 240));   /* (the shortest step) */
        CHECK(near(frames(10, 0.2), 4.0 / 60));           /* (the longest, as the rate's 15) */
    } else if (!strcmp(mode, "cap 60")) {
        CHECK(frames(72000, 1.0 / 120) == 0.0f && *rate == 60 && *limit == 60);
        /* A long unpaced stretch must repay only bounded debt, including the five-frame smoothing transition. */
        int recovered = 0;
        for (int i = 0; i < 24; i++) {
            CHECK(frames(1, 1.0 / 40) == 0.0f && *rate >= 15 && *rate <= 60);
            if (*rate == 40) { recovered = 1; break; }
        }
        CHECK(recovered && lifted_step(60.0f, (float)*rate) == 1.5f);
    } else {   /* (HOS_FPS_STEP=0: the rate up to the cap) */
        CHECK(frames(10, 1.0 / 120) == 0.0f && *rate == 120 && *limit == 120);
        CHECK(lifted_step(60.0f, 60.0f) == 1.0f);
    }
    printf("PASS: %s\n", mode);
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    sites = {p.pc: p for p in lift_scan.load().AT if p.fn.startswith("hos_oot_frame_step")}
    check(sorted(sites) == [0x80706cc4, 0x83b6eaf8], "the two step divides, one probe each")
    check((sites[0x80706cc4].pc, sites[0x80706cc4].word, sites[0x80706cc4].fn, sites[0x80706cc4].args, sites[0x80706cc4].result)
          == (0x80706cc4, 0x1e201828, "hos_oot_frame_step_probe", ("s1", "s8", "x19"), "s8"),
          "the frame's step: s8 := hos_oot_frame_step_probe(s1, s8, x19) after fdiv s8, s1, s0")
    check((sites[0x83b6eaf8].pc, sites[0x83b6eaf8].word, sites[0x83b6eaf8].fn, sites[0x83b6eaf8].args, sites[0x83b6eaf8].result)
          == (0x83b6eaf8, 0x1e201820, "hos_oot_frame_step", ("s1", "s0"), "s0"),
          "the global step: s0 := hos_oot_frame_step(s1, s0) after fdiv s0, s1, s0")
    oot = (ROOT / "host" / "oot_probes.c").read_text(encoding="utf-8")
    hooks = "\n".join(definition(oot, m) for m in ("PATCH_AT(0x83b6eaf8, word(0x1e201820), after, s0, hos_oot_frame_step,", "PATCH_AT(0x80706cc4, word(0x1e201828), after, s8, hos_oot_frame_step_probe,"))

    vi = (ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8")
    host = "\n".join(block(vi, m) for m in ("static float text_scale(", "static int fps_max(void) {", "static int realtime_rate(", "static void realtime_step("))
    fixture = FIXTURE.replace("@HOST@", host).replace("@PROBES@", hooks)
    with tempfile.TemporaryDirectory(prefix="cap-step-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture.exe"
        c.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "host"), str(c), "-o", str(exe)], check=True)
        for mode, cap, step in (("cap 120", "120", None), ("cap 60", None, None), ("step off", "120", "0")):
            env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_")}
            if cap:
                env["HOS_FPS_MAX"] = cap
            if step:
                env["HOS_FPS_STEP"] = step
            subprocess.run([str(exe), mode], env=env, check=True)


if __name__ == "__main__":
    main()
