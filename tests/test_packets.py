"""RX_VK_PACKETS (native-vk layer 1 S2): every state op the draws' kept resolutions are made from stamps its group.

The replay (vk_replay.cpp replay()) stamps a group of the NVN state when an op sets it (vk_packets.h pk_touch); a draw keeps its
dynamic state, vertex input, state-list key, program and a stage's textures and storage buffers while the groups they read stand
(vk_draw.cpp PkKeep and the keeps beside it). A state op that sets one of those fields without its stamp would keep a stale resolution:
this checks every assignment to them in replay() is in a case that stamps the field's group, and that the keep's test is what it says.
python -I -B tests/test_packets.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# NvnState fields -> the group a state op that sets them stamps
GROUPS = {
    "viewport": "PKG_DYN", "scissor": "PKG_DYN", "depth_near": "PKG_DYN", "depth_far": "PKG_DYN", "blend": "PKG_DYN",
    "color": "PKG_DYN", "chmask": "PKG_DYN", "ms": "PKG_DYN", "poly": "PKG_DYN", "ds": "PKG_DYN", "stencil_value_mask": "PKG_DYN",
    "stencil_mask": "PKG_DYN", "stencil_ref": "PKG_DYN", "poly_offset": "PKG_DYN", "blend_color": "PKG_DYN", "restart": "PKG_DYN",
    "restart_index": "PKG_DYN", "depth_clamp": "PKG_DYN", "raster_discard": "PKG_DYN",
    "vattr": "PKG_VI", "nvattr": "PKG_VI", "vstream": "PKG_VI",
    "prog": "PKG_PROG", "has_prog": "PKG_PROG",
    "rt": "PKG_RT",
}


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    vk = ROOT / "gpu" / "vk"
    replay_src = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    draw = (vk / "vk_draw.cpp").read_text(encoding="utf-8")
    header = (vk / "vk_packets.h").read_text(encoding="utf-8")
    replay = block(replay_src, "void replay(VkCommandBuffer& cmd, NvnState& s")
    # (the cases of the op switch, each to the next)
    cases = [(m.start(), m.group(1)) for m in re.finditer(r"\n        case (NOP_\w+):", replay)]
    def case_at(pos):
        start = max((c for c in cases if c[0] <= pos), default=None, key=lambda c: c[0])
        if start is None:
            return None, ""
        later = [c[0] for c in cases if c[0] > start[0]]
        # (cases that share a body: the text up to the next case with a body of its own)
        end = later[0] if later else len(replay)
        body = replay[start[0]:end]
        while body.strip().endswith(":") and later:
            later = later[1:]
            end = later[0] if later else len(replay)
            body = replay[start[0]:end]
        return start[1], body
    stores = re.compile(r"\bs\.(\w+)\s*(?:\[[^\]]*\]\s*)*(?:\.\w+\s*)?(?:=[^=]|\+\+|\+=)|memcpy\(&?s\.(\w+)|put\(&s\.(\w+)")
    checked = 0
    for m in stores.finditer(replay):
        field = m.group(1) or m.group(2) or m.group(3)
        group = GROUPS.get(field)
        if not group:
            continue
        op, body = case_at(m.start())
        if op is None:
            raise SystemExit(f"s.{field} is set outside the op switch")
        if f"pk_touch({group})" not in body:
            raise SystemExit(f"{op} sets s.{field} without pk_touch({group})")
        checked += 1
    for op in ("case NOP_BIND_TEX:", "case NOP_SET_TEX_POOL:"):
        if "pk_touch(PKG_TEX)" not in replay[replay.index(op): replay.index(op) + 400]:
            raise SystemExit(f"{op} does not stamp the handles")
    if "pk_touch_all()" not in block(replay_src, "void use_queue_state("):
        raise SystemExit("another state object does not make every kept resolution stale")
    if "pk_touch_all(), pk_rec++" not in block(draw, "void draw_begin_recording("):
        raise SystemExit("a new recording keeps the last one's resolutions")
    # (what each keep reads)
    for name, groups in (("kPkDyn", ("PKG_DYN", "PKG_RT")), ("kPkVi", ("PKG_VI", "PKG_RT")),
                         ("kPkNote", ("PKG_DYN", "PKG_VI", "PKG_RT"))):
        line = next(l for l in draw.splitlines() if l.startswith(f"constexpr uint32_t {name} ="))
        for g in groups:
            if g not in line:
                raise SystemExit(f"{name} does not read {g}")
    body = block(draw, "void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {")
    kept = body[body.index("if (dyn_kept && !pk_check) {"):]
    if "pass().wm_or |= dyn_fast.wm0, pass().wm_n = dyn_fast.wm_n;" not in kept[:400]:
        raise SystemExit("a kept dynamic state leaves the pass without the draw's write masks")
    if "last_dyn_valid && dyn_fast.on && !dump_on()" not in body:
        raise SystemExit("the dynamic state is kept without dynamic_state's own repeat conditions")
    fixture = f'''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do {{ if (!(x)) {{ std::fprintf(stderr, "FAIL:%d %s\\n", __LINE__, #x); std::exit(1); }} }} while (0)
namespace mkwvk {{
struct NvnState {{ int x; }};
{header[header.index("enum PkGroup"):header.index("bool packets_on();")]}
uint64_t pk_clock = 1;
uint64_t pk_gen[PKG_N] = {{1, 1, 1, 1, 1}};
inline void pk_touch(int g) {{ pk_gen[g] = ++pk_clock; }}
{draw[draw.index("constexpr uint32_t pk_bit(int g)"):draw.index("struct PkKeep {")]}
{block(draw, "struct PkKeep {")};
}}
using namespace mkwvk;
int main() {{
    NvnState a{{}}, b{{}};
    int prog1 = 0, prog2 = 0;
    PkKeep k;
    CHECK(!k.same(&a, &prog1, 3, false, kPkDyn));
    k.keep(&a, &prog1, 3, false);
    CHECK(k.same(&a, &prog1, 3, false, kPkDyn));
    CHECK(!k.same(&b, &prog1, 3, false, kPkDyn));      // (another state object)
    CHECK(!k.same(&a, &prog2, 3, false, kPkDyn));      // (another program)
    CHECK(!k.same(&a, &prog1, 4, false, kPkDyn));      // (another topology)
    CHECK(!k.same(&a, &prog1, 3, true, kPkDyn));       // (mesh)
    pk_touch(PKG_TEX);
    CHECK(k.same(&a, &prog1, 3, false, kPkDyn));       // (a group it does not read)
    pk_touch(PKG_VI);
    CHECK(k.same(&a, &prog1, 3, false, kPkDyn));
    CHECK(!k.same(&a, &prog1, 3, false, kPkNote));     // (the state list reads the layouts)
    pk_touch(PKG_RT);
    CHECK(!k.same(&a, &prog1, 3, false, kPkDyn));      // (the targets)
    k.keep(&a, &prog1, 3, false);
    pk_touch(PKG_DYN);
    CHECK(!k.same(&a, &prog1, 3, false, kPkDyn));
    std::printf("PASS: %d state stores stamp their group; a keep stands while its groups do\\n", {checked});
}}
'''
    with tempfile.TemporaryDirectory(prefix="packets-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
