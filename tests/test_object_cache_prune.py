"""nx2/build.py prune_object_cache: after a build, cached objects that no build folder links any more (link count 1) and
that are older than the keep time go; an object any build's obj folder still links stays whatever its age, and a recent
unlinked one stays too (work/objcache is shared by the main checkout and the agent worktrees). Game-free.

    python -I -B tests/test_object_cache_prune.py
"""
import ast
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
tree = ast.parse((ROOT / "nx2" / "build.py").read_text(encoding="utf-8"))
fn = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "prune_object_cache")

with tempfile.TemporaryDirectory() as w:
    cache, obj = Path(w, "objcache"), Path(w, "sa", "build", "obj")
    ns = {"os": os, "time": __import__("time"), "OBJ_CACHE": str(cache), "OBJ_CACHE_KEEP_DAYS": 3.0}
    exec(compile(ast.Module([fn], []), "build.py", "exec"), ns)
    prune = ns["prune_object_cache"]
    assert prune() == (0, 0, 0)   # (no cache yet)
    cache.mkdir(parents=True)
    obj.mkdir(parents=True)
    now = 1_000_000_000.0
    old, recent = now - 5 * 86400, now - 1 * 86400
    for name, size, when, linked in (("a", 10, old, True), ("b", 20, old, False), ("c", 30, recent, False), ("d", 40, old, True)):
        p = cache / (name * 40 + ".o")
        p.write_bytes(b"x" * size)
        if linked:
            os.link(p, obj / (name + ".o"))
        os.utime(p, (when, when))
    (cache / "system_headers.txt").write_text("stamp\n")
    removed, freed, kept = prune(now)
    assert (removed, freed, kept) == (1, 20, 80), (removed, freed, kept)
    assert sorted(p.name for p in cache.iterdir()) == sorted(["a" * 40 + ".o", "c" * 40 + ".o", "d" * 40 + ".o", "system_headers.txt"])
    os.remove(obj / "a.o")   # (a build folder let it go: unlinked and old now)
    assert prune(now)[0] == 1 and not (cache / ("a" * 40 + ".o")).exists()
    assert prune(now + 3 * 86400)[0] == 1   # (the recent one, once past the keep time)
print("ok: the object cache keeps every linked object and recent ones; unlinked old ones go")
