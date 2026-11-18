"""The direct back end's shader stamps: a hash of the translator (third_party/eden, third_party/sirit) and of the code that decides the
translation's output, which keys the program caches. Code inside vk_shader.cpp and vk_compute.cpp counts when it
sits between "// <stamp NAME>" and "// </stamp NAME>" lines: graphics and shared for the graphics programs, compute and
shared for the compute programs.

    shader_stamp.py graphics|compute <out header> <depfile> <translator folder>... -- <toolchain>

Paths are hashed relative to their folder (a translator file as <folder name>/<path in it>, a back end file by its name),
so moving the folders does not change the stamp.

<toolchain> names the compiler and the flags that build the translator, which the stamp also hashes. It writes `#define OUNCE_RENDERER_SHADER_STAMP` (graphics) or `OUNCE_RENDERER_COMPUTE_STAMP` (compute) and a depfile naming
every file and folder the stamp read, so a new translator file makes it again.
"""
import hashlib
import os
import re
import sys

DIRECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (kind -> the define, the files hashed whole besides the translator, and the stamp regions of each file)
KINDS = {
    "graphics": ("OUNCE_RENDERER_SHADER_STAMP", ("vk_state.h", "vk_spv_late.cpp", "vk_spv_late.h"),
                 (("vk_shader.cpp", {"graphics", "shared"}),)),
    "compute": ("OUNCE_RENDERER_COMPUTE_STAMP", (),
                (("vk_shader.cpp", {"shared"}), ("vk_compute.cpp", {"compute"}))),
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


def translator(roots):
    """(name, path) of every file under the translator's folders but the build's meson.build files, named
    <folder name>/<path in it>; and the folders walked"""
    dirs = []
    files = []
    for root in roots:
        root = os.path.abspath(root)
        for d, _, fs in os.walk(root):
            dirs.append(d)
            files += [(os.path.join(os.path.basename(root), os.path.relpath(os.path.join(d, f), root)), os.path.join(d, f))
                      for f in fs if f != "meson.build"]
    return sorted(files), dirs


def stamp(kind, roots, toolchain=""):
    """the stamp's 16 hex digits and the paths it read"""
    _, whole, regions = KINDS[kind]
    files, dirs = translator(roots)
    read = list(dirs)
    h = hashlib.sha1(usedforsecurity=False)
    h.update(b"toolchain " + toolchain.encode() + b"\n")
    for name, f in files + [(n, os.path.join(DIRECT, n)) for n in whole]:
        if os.path.isfile(f):
            h.update(name.replace("\\", "/").encode() + lf(f))
            read.append(f)
    for name, which in regions:
        h.update(name.encode() + stamp_regions(os.path.join(DIRECT, name), which))
        read.append(os.path.join(DIRECT, name))
    return h.hexdigest()[:16], read


def main():
    kind, out, dep = sys.argv[1:4]
    rest = sys.argv[4:]
    cut = rest.index("--")
    digits, read = stamp(kind, rest[:cut], " ".join(rest[cut + 1:]))
    text = "#define %s 0x%sull\n" % (KINDS[kind][0], digits)
    # (unchanged text keeps the header's time: the one file that includes it is not compiled again)
    if not os.path.exists(out) or open(out).read() != text:
        open(out, "w").write(text)
    with open(dep, "w") as f:
        f.write("%s: %s\n" % (out.replace(" ", "\\ "), " ".join(p.replace(" ", "\\ ") for p in read)))


if __name__ == "__main__":
    main()
