"""romfs .zs: zstd with the game's dictionaries (work/zsdic, from Pack/ZsDic.pack.zs; ZSDIC=<folder> for another). Imported by the readers."""
import os, zstandard
HERE = os.path.dirname(os.path.abspath(__file__))
DIR = os.environ.get('ZSDIC') or os.path.join(os.path.dirname(os.path.dirname(HERE)), 'work', 'zsdic')
_d = None
def _dicts():
    global _d
    if _d is None:
        _d = {}
        for f in os.listdir(DIR):
            b = open(os.path.join(DIR, f), 'rb').read()
            _d[int.from_bytes(b[4:8], 'little')] = zstandard.ZstdCompressionDict(b)   # (dict id after the magic)
    return _d
def decompress(data):
    if data[:4] != b'\x28\xb5\x2f\xfd': return data
    i = zstandard.get_frame_parameters(data).dict_id
    return zstandard.ZstdDecompressor(dict_data=_dicts()[i] if i else None).decompress(data, max_output_size=1 << 31)
