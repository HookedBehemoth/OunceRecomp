#!/usr/bin/env python3
"""(M4f) Per-program draw and dispatch counts of one frame: the frozen back end's RX_VK_DRAW_STATS file against the direct
back end's HOS_DBG=drawlog:<present> trace (docs/direct/m4f-report.md). Frozen counts can miss draws (its frame boundary
is the texture frame number), so programs only in the frozen file are the reliable list.

    scripts/direct_trace_compare.py <frozen draw_stats_N.txt> <direct log.txt>
"""
import re, sys, collections
fz, dl = sys.argv[1], sys.argv[2]
F = {}
for line in open(fz, errors='replace'):
    m = re.match(r'(draw|mesh|comp) ([0-9a-f]{16})\s+(\d+) \| (.*)', line)
    if m: F[m.group(2)] = (m.group(1), int(m.group(3)), m.group(4).strip())
D = collections.Counter(); K = {}; skip = collections.Counter()
for line in open(dl, errors='replace'):
    if 'drawlog' not in line: continue
    m = re.search(r'drawlog dispatch .* prog ([0-9a-f]{16})', line)
    if m: D[m.group(1)] += 1; K[m.group(1)] = 'comp'; continue
    m = re.search(r'kind (\d+) rt \d+ (\d+x\d+) c0 (-?\d+): (drawn[a-z ]*) prog ([0-9a-f]{16})', line)
    if m: D[m.group(5)] += 1; K[m.group(5)] = m.group(2) + ' k' + m.group(1); continue
    m = re.search(r'skipped: ([^;]*);', line)
    if m: skip[m.group(1)] += 1
print('direct skipped draws in the frame:', dict(skip))
fd = {k for k, v in F.items() if v[0] != 'comp'}; fc = {k for k, v in F.items() if v[0] == 'comp'}
dd = {k for k in D if K[k] != 'comp'}; dc = {k for k in D if K[k] == 'comp'}
print('frozen draw programs %d, direct %d, both %d' % (len(fd), len(dd), len(fd & dd)))
print('frozen compute programs %d, direct %d, both %d' % (len(fc), len(dc), len(fc & dc)))
print('-- drawn in frozen only:')
for k in sorted(fd - dd, key=lambda k: -F[k][1]): print('  ', k, F[k][0], F[k][1], '|', F[k][2][:110])
print('-- drawn in direct only:')
for k in sorted(dd - fd, key=lambda k: -D[k]): print('  ', k, D[k], K[k])
print('-- dispatched in frozen only:', ' '.join(sorted(fc - dc)))
print('-- dispatched in direct only:', ' '.join(sorted(dc - fc)))
print('-- both, draw counts differ:')
for k in sorted(fd & dd):
    if F[k][1] != D[k]: print('  ', k, 'frozen', F[k][1], 'direct', D[k], K[k], '|', F[k][2][:80])
