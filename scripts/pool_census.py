"""Read a pool census (a RX_POOL_CENSUS=1 build run with HOS_POOL_CENSUS=<file>): the lifted functions that read or
write NVN memory pool pages, by pool kind, with counts and bytes.

    python -I scripts/pool_census.py <census file> [--since <earlier dump>] [--exe work/sa/census/hookshot] [--top 40]

The run writes <file> every HOS_POOL_CENSUS_SEC seconds and keeps each dump as <file>.001, .002, ...; --since
subtracts an earlier dump, so a phase (gameplay without the load) is the difference of two. Each hit is a place (the
lifted function the access is in, from the executable's symbols) and its guest caller (x30 at the access): for a leaf
such as memcpy (sdk f_8575cdc0) the caller is the call site that matters, so such places are reported by caller.

Pool kinds are the NVN flags. Reads from pools the CPU can see matter most for a renderer that keeps pools in device
memory (a CPU read of BAR memory runs at ~4 MB/s on the RX 6950 XT): each read site is a call site to change or a pool
to keep in system memory.
"""
import argparse
import bisect
import collections
import os
import subprocess
import sys

FLAGS = [(0x1, "cpu-none"), (0x2, "cpu-uncached"), (0x4, "cpu-cached"), (0x8, "gpu-none"), (0x10, "gpu-uncached"),
         (0x20, "gpu-cached"), (0x40, "shader"), (0x80, "compressible"), (0x100, "physical"), (0x200, "virtual"),
         (0x80000000, "native-image")]
LEAVES = {"f_8575cdc0": "memcpy", "entry_sym_memcpy": "memcpy", "entry_sym_memmove": "memmove", "entry_sym_memset": "memset",
          "entry_sym___memcpy_aarch64": "memcpy", "entry_sym___memcpy_aarch64_simd": "memcpy"}   # (places reported by their caller)


def kind(flags):
    return "+".join(n for b, n in FLAGS if flags & b) or "0"


def symbols(exe):
    out = subprocess.run([os.environ.get("NM") or "nm", "-n", "--defined-only", exe], capture_output=True, text=True).stdout
    addrs, names, start = [], [], None
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            if p[2] == "__executable_start":
                start = int(p[0], 16)
            if p[1] in "tTwW":
                addrs.append(int(p[0], 16))
                names.append(p[2])
    return addrs, names, start or 0


def read(path):
    pools, hits, t = {}, {}, 0.0
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "time":
            t = float(p[1])
        elif p[0] == "pool":
            pools[int(p[1])] = (int(p[2], 16), int(p[3], 16), int(p[4], 16))
        elif p[0] == "hit":
            key = (int(p[1], 16), p[2], int(p[3], 16))
            hits[key] = (int(p[4]), int(p[5], 16), int(p[6]), int(p[7]))
        elif p[0] == "dropped" and int(p[1]):
            print("note: %s hits did not fit the table (%s)" % (p[1], path))
    return pools, hits, t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("census")
    ap.add_argument("--since", help="an earlier dump of the same run: report the difference")
    ap.add_argument("--exe", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                  "work", "sa", "census", "hookshot"))
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()

    pools, hits, t1 = read(a.census)
    t0 = 0.0
    if a.since:
        _, before, t0 = read(a.since)
        for k, (pool, flags, n, b) in before.items():
            if k in hits:
                p, f, n1, b1 = hits[k]
                hits[k] = (p, f, n1 - n, b1 - b)
    seconds = max(t1 - t0, 1e-9)

    addrs, names, start = symbols(a.exe)
    if not addrs:
        sys.exit("no symbols in %s" % a.exe)
    guest = sorted((int(n[2:], 16), n) for n in names if n.startswith("f_") and all(c in "0123456789abcdef" for c in n[2:]))
    gstarts = [g for g, _ in guest]

    def place_name(off):
        i = bisect.bisect_right(addrs, start + off) - 1
        return names[i] if i >= 0 else "?"

    def caller_name(lr):
        if lr >= 0x87FFFFFF:
            return "?"
        i = bisect.bisect_right(gstarts, lr) - 1
        return "%s+%#x" % (guest[i][1], lr - guest[i][0]) if i >= 0 else "%#x" % lr

    agg = collections.defaultdict(lambda: [0, 0, set()])   # (function, via, direction, kind) -> count, bytes, pools
    by_kind = collections.defaultdict(lambda: [0, 0])
    for (off, d, lr), (pool, flags, n, b) in hits.items():
        if n <= 0:
            continue
        fn = place_name(off)
        if fn in LEAVES:
            fn, via = caller_name(lr), LEAVES[fn]
        else:
            via = ""
        k = kind(flags)
        e = agg[(fn, via, d, k)]
        e[0] += n
        e[1] += b
        e[2].add(pool)
        by_kind[(k, d)][0] += n
        by_kind[(k, d)][1] += b

    print("%.1f s of the run (%s)" % (seconds, "%.0f-%.0f s" % (t0, t1)))
    sizes = collections.Counter()
    for base, size, flags in pools.values():
        sizes[kind(flags)] += size
    print("pools: %d registered" % len(pools))
    for k, s in sizes.most_common():
        print("  %-40s %8.1f MB" % (k, s / 2**20))
    print("\naccesses by pool kind:")
    for (k, d), (n, b) in sorted(by_kind.items(), key=lambda x: -x[1][1]):
        print("  %-40s %s %12d accesses %10.1f MB %8.2f MB/s" % (k, d, n, b / 2**20, b / 2**20 / seconds))
    for d, title in (("r", "READS (each a call site a device-memory pool makes slow)"), ("w", "WRITES")):
        rows = sorted(((k, v) for k, v in agg.items() if k[2] == d), key=lambda x: -x[1][1])
        print("\n%s: %d sites" % (title, len(rows)))
        for (fn, via, _, k), (n, b, pis) in rows[:a.top]:
            print("  %-26s %-7s %-34s %11d x %9.2f MB %8.3f MB/s  %d pools" % (fn, via, k, n, b / 2**20, b / 2**20 / seconds, len(pis)))


if __name__ == "__main__":
    main()
