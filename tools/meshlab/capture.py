# reader of a meshcap frame.bin (gpu/direct/direct_meshcap_format.h), CPU only
import mmap, struct, sys
class Cap:
    def __init__(s, path):
        f = open(path, 'rb'); s.m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        m = s.m; at = 16; s.blobs = {}; s.draws = []; s.progs = {}; s.others = []
        while at < len(m):
            t, _, n = struct.unpack_from('<IIQ', m, at); at += 16; end = at + n
            if t == 2:
                i, _, g, b = struct.unpack_from('<IIQQ', m, at); s.blobs[i] = (g, at + 24, b)
            elif t == 1:
                key, c0, c1, c2, tc, task, tt, tpw, pb, nubo, npushed, nst, msz = struct.unpack_from('<Q3QQ8I', m, at)
                s.progs[key] = dict(codes=(c0, c1, c2), task_code=tc, task_threads=tt, payload_words=tpw, nubo=nubo, at=at)
            elif t in (3, 5):
                d = {}
                (d['key'], d['cb'], d['lr'], d['present'], d['seq'], d['cb_seq'], d['kind'], d['task'], d['indirect'], d['gated'],
                 d['x'], d['y'], d['z']) = struct.unpack_from('<4Q6I3i', m, at)
                d['w'], d['h'], d['n'], d['samples'] = struct.unpack_from('<4I', m, at + 104)
                d['push'] = m[at + 164:at + 420]
                d['header'] = struct.unpack_from('<16I', m, at + 936)
                nubo, nssbo, nmap, npay = struct.unpack_from('<4I', m, at + 1000)
                p = at + 1016; d['ubos'] = []; d['ssbos'] = []
                for i in range(nubo):
                    st, ix, _, by, dev, g, bl, _ = struct.unpack_from('<BBHIQQII', m, p); p += 32
                    d['ubos'].append(dict(stage=st, index=ix, bytes=by, dev=dev, guest=g, blob=bl))
                if t == 3:
                    for i in range(nssbo):
                        st, sl, _, bl, g, sz = struct.unpack_from('<BBHIQQ', m, p); p += 24
                        d['ssbos'].append(dict(stage=st, slot=sl, blob=bl, guest=g, size=sz))
                    d['map'] = struct.unpack_from('<%dI' % nmap, m, p); p += 4 * nmap
                    d['payload'] = struct.unpack_from('<%dI' % npay, m, p)
                d['mesh'] = t == 3
                s.draws.append(d)
            at = end
    def blob(s, i):
        g, o, b = s.blobs[i]; return s.m[o:o + b]
    def read(s, guest, n):
        "guest memory from the captured blobs (None where not captured)"
        for i, (g, o, b) in s.blobs.items():
            if g and g <= guest and guest + n <= g + b: return s.m[o + guest - g:o + guest - g + n]
        return None
    def u32(s, guest):
        b = s.read(guest, 4); return None if b is None else struct.unpack('<I', b)[0]
    def u64(s, guest):
        b = s.read(guest, 8); return None if b is None else struct.unpack('<Q', b)[0]
