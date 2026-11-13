"""nx2/oot_labels.txt is well formed (every address and label once, the label's alphabet) and
nx2/label_syms.py renames a real symbol table: a tiny C program's f_<address> functions get <label>.f_<address>,
an unlisted one keeps its name, a listed address with no function is reported, and a second run changes nothing.
No game data. Skipped parts without a C compiler or objcopy.
python -I -B tests/test_label_syms.py
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import label_syms  # noqa: E402


def main():
    labels = label_syms.read()
    if not labels:
        print("label_syms: FAILED (an empty list)")
        return 1
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    nm = shutil.which("llvm-nm") or shutil.which("nm")
    if not cc or not nm or not (shutil.which("llvm-objcopy") or shutil.which("objcopy")) or sys.platform != "linux":
        print("label_syms: ok (%d labels; the rename not run here)" % len(labels))
        return 0
    with tempfile.TemporaryDirectory() as d:
        src, exe = Path(d) / "t.c", Path(d) / "t"
        src.write_text("int f_80000010(void) { return 1; }\nint f_80000020(void) { return 2; }\n"
                       "int main(void) { return f_80000010() + f_80000020() - 3; }\n")
        subprocess.run([cc, "-O0", "-o", str(exe), str(src)], check=True)
        mine = {0x80000010: "first_thing", 0x80000030: "not_there"}
        n, missing = label_syms.apply(str(exe), mine)
        n2, missing2 = label_syms.apply(str(exe), mine)
        syms = {l.rsplit(" ", 1)[-1] for l in subprocess.run([nm, str(exe)], capture_output=True, text=True, check=True).stdout.splitlines()}
        ok = (n == 1 and n2 == 1 and missing == [0x80000030] and missing2 == [0x80000030] and "first_thing.f_80000010" in syms
              and "f_80000010" not in syms and "f_80000020" in syms and subprocess.run([str(exe)]).returncode == 0)
    if not ok:
        print("label_syms: FAILED (renamed %d then %d, missing %s, symbols %s)" % (n, n2, missing, sorted(s for s in syms if "f_8" in s)))
        return 1
    print("label_syms: ok (%d labels)" % len(labels))
    return 0


if __name__ == "__main__":
    sys.exit(main())
