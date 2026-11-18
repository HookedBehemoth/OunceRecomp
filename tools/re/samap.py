"""An offset in the hookshot executable -> lifted function (f_<guest address>) + offset, from the link map work/sa/build/hookshot.map.

Usage:  python -I tools/re/samap.py <hex offset> [...]
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit

rows = []
for line in open(os.path.join(kit.WORK, "sa", "build", "hookshot.map"), errors="replace"):
    m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})", line)
    if m:
        rows.append((int(m.group(2), 16), m.group(1)))
rows.sort()
keys = [r[0] for r in rows]
for a in sys.argv[1:]:
    t = 0x140000000 + int(a, 16)
    i = bisect.bisect_right(keys, t) - 1
    print("%s: %s+%#x" % (a, rows[i][1], t - rows[i][0]))
