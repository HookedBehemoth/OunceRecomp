"""Compile runtime/nvn_layout.c with tests/nvn_deswizzle_test.c and check its block-linear to linear copy.

python -I -B tests/test_nvn_deswizzle.py [--cc clang]
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    process_flags = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0

    def run(command):
        result = subprocess.run(command, creationflags=process_flags, capture_output=True,
                                text=True, encoding="utf-8", errors="replace")
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()

    temporary = tempfile.TemporaryDirectory(prefix="deswizzle-")
    folder = Path(temporary.name).resolve()
    try:
        executable = folder / "fixture.exe"
        run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "runtime"), "-I" + str(ROOT / "renderer/api"), "-Wno-unused-parameter", "-Wno-sign-compare",
             str(ROOT / "tests/nvn_deswizzle_test.c"), str(ROOT / "runtime/nvn_layout.c"), "-o", str(executable)])
        run([str(executable)])
    finally:
        temporary.cleanup()


if __name__ == "__main__":
    main()
