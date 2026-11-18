"""tools/dump/byml_write.py writes BYML that its typed reader, and tools/dump/byml.py, read back as the document written.

Synthetic documents only (no dump): every node the writer writes (bool, i32, f32, u32, i64, u64, f64, string, null, array,
dict, empty containers), keys and strings that sort by their UTF-8 bytes, subtrees and 64-bit values that repeat (written
once, so the file is smaller than with copies), and an int that does not fit an i32 refused unless wrapped.

python -I -B ounce/tests/py/byml_write.py
"""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "dump"))
import byml_write as B  # noqa: E402


def doc():
    return {
        "Bool": True, "False": False, "I32": -7, "I32Max": 0x7FFFFFFF, "F32": 1.5, "NegZero": -0.0,
        "U32": B.U32(0xFFFFFFFF), "I64": B.I64(-(1 << 40)), "U64": B.U64(0xFEDCBA9876543210), "F64": B.F64(0.1),
        "Str": "Obj_Pot_A_01", "Empty": "", "Null": None, "Ünïcode": "äöü", "a": "lower after upper",
        "List": [1, 2.0, "x", None, True, B.U64(5), [], {}],
        "Actors": [{"Gyaml": "A", "Translate": [0.0, 100.0, 0.0], "Hash": B.U64(1 << 63)},
                   {"Gyaml": "B", "Translate": [0.0, 100.0, 0.0], "Hash": B.U64(1 << 63)}],
        "Deep": {"x": {"y": {"z": [[[B.F64(-2.5)]]]}}},
    }


class BymlWrite(unittest.TestCase):
    def test_round_trip_typed(self):
        d = doc()
        data = B.dump(d)
        self.assertEqual(data[:4], b"YB\x07\x00")
        self.assertEqual(B.load(data), d)
        self.assertEqual(B.dump(B.load(data)), data)   # (written again: the same bytes)

    def test_plain_reader_agrees(self):
        import types
        if "zs" not in sys.modules:   # (byml.load passes its input through zs, which needs zstandard: not needed here)
            sys.modules["zs"] = types.SimpleNamespace(decompress=lambda data: data)
        import byml   # (the reader the other tools use: plain values)
        plain = byml.load(B.dump(doc()))
        self.assertEqual(plain["U64"], 0xFEDCBA9876543210)
        self.assertEqual(plain["I64"], -(1 << 40))
        self.assertEqual(plain["U32"], 0xFFFFFFFF)
        self.assertEqual(plain["F64"], 0.1)
        self.assertEqual(plain["Actors"][1]["Translate"], [0.0, 100.0, 0.0])
        self.assertEqual(plain["Ünïcode"], "äöü")
        self.assertEqual(struct.pack("<f", plain["NegZero"]), struct.pack("<f", -0.0))

    def test_types_kept(self):
        back = B.load(B.dump(doc()))
        self.assertIs(type(back["I32"]), int)
        self.assertIs(type(back["Bool"]), bool)
        self.assertIs(type(back["U32"]), B.U32)
        self.assertIs(type(back["List"][5]), B.U64)
        self.assertNotEqual(B.U32(5), B.U64(5))

    def test_shared_subtrees(self):
        one = {"Actors": [{"T": [1.0, 2.0, 3.0], "H": B.U64(9)}]}
        many = {"Actors": [{"T": [1.0, 2.0, 3.0], "H": B.U64(9)} for _ in range(50)]}
        self.assertLess(len(B.dump(many)) - len(B.dump(one)), 50 * 8)   # (each copy costs its array slot only)
        self.assertEqual(B.load(B.dump(many)), many)
        # (-0.0 and 0.0 are equal in Python but different floats: not shared)
        z = B.load(B.dump({"a": [0.0], "b": [-0.0]}))
        self.assertEqual(struct.pack("<f", z["b"][0]), struct.pack("<f", -0.0))

    def test_sorted_tables(self):
        data = B.dump({"b": "z", "B": "Z", "é": "a"})
        keys_at = struct.unpack_from("<I", data, 4)[0]
        n = struct.unpack_from("<I", data, keys_at)[0] >> 8
        names = []
        for i in range(n):
            s = keys_at + struct.unpack_from("<I", data, keys_at + 4 + 4 * i)[0]
            names.append(data[s:data.index(b"\0", s)])
        self.assertEqual(names, sorted(names))

    def test_refuses_wide_plain_int(self):
        with self.assertRaises(ValueError):
            B.dump({"x": 1 << 40})
        with self.assertRaises(TypeError):
            B.dump({"x": object()})


if __name__ == "__main__":
    unittest.main()
