"""The hand-written mesh stages the direct back end draws with (gpu/direct/direct_meshrepl.cpp), without a game or a GPU.

gpu/direct/shaders/mesh/swap.txt lists them ("<code key> <shape> <uniform buffers>"). Each must be a line of codes.txt (its
source and defines), have its SPIR-V header beside the sources (<code key>_spv.h) and a line in direct_meshrepl.cpp's table
with the same shape and uniform buffers; and no header or table line is left without a swap.txt line. With
glslangValidator on the PATH each header is the source compiled now (tools/shaders/build_shaders.sh writes them:
glslangValidator -V --target-env vulkan1.3 -I<dir> <defines> --vn mesh_<code key>_spv -o <code key>_spv.h <source>).

    python -I -B tests/test_direct_mesh_repl.py
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MESH = ROOT / "gpu" / "direct" / "shaders" / "mesh"


def lines(path):
    return [l.split() for l in path.read_text().splitlines() if l.strip() and not l.startswith("#")]


def words(text):
    return [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{8}", text)]


def main():
    codes = {l[0]: (l[1], l[2:]) for l in lines(MESH / "codes.txt")}
    swaps = lines(MESH / "swap.txt")
    table = (ROOT / "gpu" / "direct" / "direct_meshrepl.cpp").read_text()
    rows = {m.group(1): (m.group(2), m.group(3), int(m.group(4)), [int(x) for x in m.group(5).split(",")])
            for m in re.finditer(r"^\s*MESH_REPL\(([0-9a-f]{16}), (true|false), (\w+), (\d+), ([0-9, ]+)\),", table, flags=re.M)}
    failed = []
    for code, shape, ubos in swaps:
        if code not in codes:
            failed.append("%s is in swap.txt and not in codes.txt" % code)
            continue
        want = [int(x[1:]) for x in ubos.split(",")]
        row = rows.get(code)
        if row is None:
            failed.append("%s has no MESH_REPL line in direct_meshrepl.cpp" % code)
        elif row != ("true" if shape == "task" else "false", "mesh_%s_spv" % code, len(want), want):
            failed.append("%s: direct_meshrepl.cpp's line (%s) is not swap.txt's (%s, %s)" % (code, row, shape, ubos))
        if '#include <shaders/mesh/%s_spv.h>' % code not in table:
            failed.append("%s: direct_meshrepl.cpp does not include its header" % code)
        header = MESH / ("%s_spv.h" % code)
        if not header.is_file():
            failed.append("%s has no %s" % (code, header.name))
            continue
        glslang = shutil.which("glslangValidator")
        if glslang:
            src, defs = codes[code]
            with tempfile.TemporaryDirectory() as tmp:
                made = Path(tmp) / header.name
                r = subprocess.run([glslang, "-V", "--target-env", "vulkan1.3", "-I" + str(MESH)] + defs +
                                   ["--vn", "mesh_%s_spv" % code, "-o", str(made), str(MESH / src)], capture_output=True, text=True)
                if r.returncode != 0:
                    failed.append("%s does not compile: %s" % (src, r.stdout + r.stderr))
                elif words(made.read_text()) != words(header.read_text()):
                    failed.append("%s is not %s %s compiled now (tools/shaders/build_shaders.sh writes it)" % (header.name, src, " ".join(defs)))
        else:
            print("glslangValidator not found: %s not checked against its source" % header.name)
    listed = {s[0] for s in swaps}
    for code in rows:
        if code not in listed:
            failed.append("direct_meshrepl.cpp has %s, swap.txt does not" % code)
    for h in MESH.glob("*_spv.h"):
        if h.name[:16] not in listed:
            failed.append("%s has no swap.txt line" % h.name)
    for f in failed:
        print("FAIL: " + f)
    if failed:
        return 1
    print("PASS: %d hand-written mesh stages: swap.txt, codes.txt, the table and the headers agree" % len(swaps))
    return 0


if __name__ == "__main__":
    sys.exit(main())
