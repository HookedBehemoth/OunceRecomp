"""meshlab cap_poke: a copy of a meshcap capture with words of one draw's uniform buffers changed, so that the harness
(meshlab run --tasks, which runs the captured task stage again) or cpu_draw.py sees the draw with other inputs: for example
the task stage's culling switched off, which makes a draw emit every meshlet it has.

    cap_poke.py <frame.bin> <out.bin> <seq> <stage>.c<buffer>.<byte offset>=<u32> ...
      stage: 6 task, 7 mesh, 1 fragment (NVN's numbers, as the capture has them).   Example: 6.c3.1920=0

A uniform buffer's bytes are one blob of the capture, shared by every draw that captured the same buffer: those draws change
with it. The output is as large as the input and as local: it holds game data and stays under work/.
"""
import shutil
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture import Cap


def main():
    src, dst, seq = sys.argv[1], sys.argv[2], int(sys.argv[3])
    c = Cap(src)
    d = [x for x in c.draws if x['seq'] == seq and x['mesh']][0]
    pokes = []
    for a in sys.argv[4:]:
        k, v = a.split('=')
        stage, buf, off = k.split('.')
        u = [u for u in d['ubos'] if u['stage'] == int(stage) and u['index'] == int(buf[1:])][0]
        _, at, n = c.blobs[u['blob']]
        if int(off) + 4 > n: raise SystemExit('%s: past the captured %d bytes' % (a, n))
        pokes.append((at + int(off), int(v, 0), a))
    shutil.copyfile(src, dst)
    with open(dst, 'r+b') as f:
        for at, v, a in pokes:
            f.seek(at)
            old = struct.unpack('<I', f.read(4))[0]
            f.seek(at)
            f.write(struct.pack('<I', v))
            print('%s: %#x -> %#x' % (a, old, v))


if __name__ == '__main__':
    main()
