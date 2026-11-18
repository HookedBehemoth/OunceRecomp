"""runtime/os (os.h, os_linux.c): build tests/os_test.c against it and run it, on all cores and on one.

python -I -B tests/test_os.py [--cc clang]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="os-test-") as folder:
        exe = os.path.join(folder, "os_test")
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "runtime/os"),
                        str(ROOT / "tests/os_test.c"), str(ROOT / "runtime/os/os_linux.c"), "-o", exe, "-lpthread", "-ldl"],
                       check=True)
        subprocess.run([exe], check=True, timeout=300)
        if shutil.which("taskset"):   # (one core: every contended wait sleeps)
            subprocess.run(["taskset", "-c", "0", exe], check=True, timeout=300)


if __name__ == "__main__":
    main()
