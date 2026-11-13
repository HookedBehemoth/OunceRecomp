"""(M6f; from work/proto/scenecycle/make_maps.py) The scene cycle's stops, "<banc> <startpos> -" per line, from the game's
StartPos table (Banc/BancBasedResource/StartPos.byml.zs of the dump named in work/dump.txt). Per banc: its entrance
(EntrancePos, the banc's own name, or the first plain one), and for maps with more spawns one more area. Child maps,
age-free maps, "!age", adult maps. The list is game-derived: it is written under work/ and never committed.

    python -I scripts/scene_cycle_maps.py [out]   (default work/scenecycle/maps.txt)
"""
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / "work" / "tools"))
import byml  # noqa: E402  (work/tools: the dump readers)
import zs  # noqa: E402

dump = [l for l in (root / "work" / "dump.txt").read_text().split("\n") if l and not l.startswith("#")][0]
d = byml.load(zs.decompress((Path(dump) / "romfs" / "Banc" / "BancBasedResource" / "StartPos.byml.zs").read_bytes()))
skip = ("SubEvent", "Air", "Restart", "Dream", "Defeat", "Sage", "Get_", "BossDefeat", "Ending", "Song", "Bolero",
        "Requiem", "Minuet", "Serenade", "Nocturne", "Prelude", "Warp")


def plain(n):
    return n and not any(s in n for s in skip)


child, other, adult = [], [], []
for b in sorted(d):
    if b.startswith("Demo_") or b in ("GerudoFortress_Adult", "GerudoValley_Adult"):   # (an NPC dialogue holds warps)
        continue
    names = [n for n in d[b] if plain(n)] or [n for n in d[b] if n]
    if not names:
        continue
    first = "EntrancePos" if "EntrancePos" in names else (b if b in names else names[0])
    stops = [first] + [n for n in names if n != first][:1 if len(names) >= 4 else 0]
    out = child if b.endswith("_Child") else adult if "_Adult" in b else other
    out += [f"{b} {s} -" for s in stops]
dst = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "work" / "scenecycle" / "maps.txt"
dst.parent.mkdir(parents=True, exist_ok=True)
dst.write_text("\n".join(child + other + ["!age"] + adult) + "\n")
print(len(child), len(other), len(adult), dst)
