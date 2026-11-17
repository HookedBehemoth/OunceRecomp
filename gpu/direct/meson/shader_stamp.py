"""The direct back end's shader stamps: a hash of the translator (gpu/direct/eden) and of the code that decides the
translation's output, which keys the program caches. Code inside direct_shader.cpp and direct_compute.cpp counts when it
sits between "// <stamp NAME>" and "// </stamp NAME>" lines: graphics and shared for the graphics programs, compute and
shared for the compute programs.

    shader_stamp.py graphics|compute <out header> <depfile>

writes `#define HOOKSHOT_DIRECT_SHADER_STAMP` (graphics) or `HOOKSHOT_DIRECT_COMPUTE_STAMP` (compute) and a depfile naming
every file and folder the stamp read, so a new translator file makes it again. nx2/build.py uses stamp() too.
"""
import hashlib
import os
import re
import sys

DIRECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPU = os.path.dirname(DIRECT)
EDEN = os.path.join(DIRECT, "eden")

# (kind -> the define, the files hashed whole besides the translator, and the stamp regions of each file)
KINDS = {
    "graphics": ("HOOKSHOT_DIRECT_SHADER_STAMP", ("direct_state.h", "direct_spv_late.cpp", "direct_spv_late.h"),
                 (("direct_shader.cpp", {"graphics", "shared"}),)),
    "compute": ("HOOKSHOT_DIRECT_COMPUTE_STAMP", (),
                (("direct_shader.cpp", {"shared"}), ("direct_compute.cpp", {"compute"}))),
}


def lf(path):
    return open(path, "rb").read().replace(b"\r\n", b"\n")


def stamp_regions(path, names):
    out, inside = [], None
    for line in lf(path).decode().split("\n"):
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


def translator():
    """every file under gpu/direct/eden, and the folders walked"""
    dirs = []
    files = []
    for d, _, fs in os.walk(EDEN):
        dirs.append(d)
        files += [os.path.join(d, f) for f in fs]
    return sorted(files), dirs


def stamp(kind):
    """the stamp's 16 hex digits and the paths it read"""
    _, whole, regions = KINDS[kind]
    files, dirs = translator()
    read = list(dirs)
    h = hashlib.sha1(usedforsecurity=False)
    for f in files + [os.path.join(DIRECT, n) for n in whole]:
        if os.path.isfile(f):
            h.update(os.path.relpath(f, GPU).replace("\\", "/").encode() + lf(f))
            read.append(f)
    for name, which in regions:
        h.update(name.encode() + stamp_regions(os.path.join(DIRECT, name), which))
        read.append(os.path.join(DIRECT, name))
    return h.hexdigest()[:16], read


def main():
    kind, out, dep = sys.argv[1:4]
    digits, read = stamp(kind)
    text = "#define %s 0x%sull\n" % (KINDS[kind][0], digits)
    # (unchanged text keeps the header's time: the one file that includes it is not compiled again)
    if not os.path.exists(out) or open(out).read() != text:
        open(out, "w").write(text)
    with open(dep, "w") as f:
        f.write("%s: %s\n" % (out.replace(" ", "\\ "), " ".join(p.replace(" ", "\\ ") for p in read)))


if __name__ == "__main__":
    main()
