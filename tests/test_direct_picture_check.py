"""(M6b) scripts/direct_picture_check.py on synthetic pictures, no game data: the same picture passes; a difference that
cancels in the picture's mean (+d on one half, -d on the other), one flat channel, one wrong 16 x 16 tile, a missing state and a
NaN each fail; a cleared state is never lined up with a drawn one. The tool's own selftest, run as a test.
python -I -B tests/test_direct_picture_check.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    r = subprocess.run([sys.executable, "-I", "-B", str(ROOT / "scripts" / "direct_picture_check.py"), "selftest"], text=True,
                       capture_output=True, timeout=300)
    lines = [l for l in r.stdout.splitlines() if l.startswith("selftest:")]
    print("\n".join(lines))
    if r.returncode != 0 or len(lines) < 7 or any(not l.endswith(": ok") for l in lines):
        print(r.stdout + r.stderr)
        print("direct_picture_check: FAILED")
        return 1
    print("direct_picture_check: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
