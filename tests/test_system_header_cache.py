"""nx2/build.py drop_stale_objects: the cached objects are not reused once the system headers (stdio.h) or the compiler's
version change. The note lives in the cache folder, shared by every worktree: a cache with no note keeps its objects and
gets one; the same stamp keeps everything; a new one removes the cached objects and this build's. Game-free, no compiler
(the version call is stubbed).

    python -I -B tests/test_system_header_cache.py
"""
import ast
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
tree = ast.parse((ROOT / "nx2" / "build.py").read_text(encoding="utf-8"))
fn = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "drop_stale_objects")

version = ["clang version 20.1.0"]


class Run:
    def __init__(self, *a, **k):
        self.returncode, self.stdout = 0, version[0] + "\nTarget: x86_64\n"


with tempfile.TemporaryDirectory() as w:
    root = Path(w)
    sysroot, cache, obj = root / "sysroot", root / "objcache", root / "obj"
    (sysroot / "usr" / "include").mkdir(parents=True)
    stdio = sysroot / "usr" / "include" / "stdio.h"
    stdio.write_text("int printf(const char *, ...);\n")
    cache.mkdir()
    obj.mkdir()
    fake_subprocess = type(subprocess)("subprocess")
    fake_subprocess.run, fake_subprocess.SubprocessError = Run, subprocess.SubprocessError
    ns = {"os": os, "hashlib": hashlib, "subprocess": fake_subprocess, "SYSROOT": str(sysroot), "OBJ_CACHE": str(cache)}
    exec(compile(ast.Module([fn], []), "build.py", "exec"), ns)
    drop = ns["drop_stale_objects"]

    def fill():
        (cache / ("a" * 40 + ".o")).write_bytes(b"o")
        (obj / "code_000.o").write_bytes(b"o")
        (obj / "code_000.o.sha1").write_text("a" * 40)

    def objects():
        return sorted(p.name for p in cache.iterdir() if p.name.endswith(".o")) + sorted(p.name for p in obj.iterdir())

    fill()
    drop("clang", [str(obj)])
    note = (cache / "system_headers.txt").read_text()
    assert len(objects()) == 3, "a cache with no note keeps its objects"
    drop("clang", [str(obj)])
    assert len(objects()) == 3 and (cache / "system_headers.txt").read_text() == note, "the same headers keep everything"
    stdio.write_text("int printf(const char *, ...);\nint puts(const char *);\n")
    drop("clang", [str(obj)])
    assert objects() == [] and (cache / "system_headers.txt").read_text() != note, "new headers drop the objects"
    fill()
    note = (cache / "system_headers.txt").read_text()
    version[0] = "clang version 21.0.0"
    drop("clang", [str(obj)])
    assert objects() == [] and (cache / "system_headers.txt").read_text() != note, "a new compiler drops the objects"
print("ok: objects are dropped when the system headers or the compiler change, and only then")
