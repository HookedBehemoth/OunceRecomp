"""Paths are UTF-8: runtime/ and renderer convert no text with an ANSI code page (CP_ACP and the like): the game's char strings are
UTF-8, and a folder named outside a legacy code page (Japanese, an emoji) must work wherever the host runs.

    python -I -B tests/test_utf8_paths.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# (the game's own sources: not the vendored compiler code)
SOURCES = [ROOT / "runtime", ROOT / "renderer"]
VENDORED = ("third_party/eden/",)
ANSI = re.compile(r"\b(CP_ACP|CP_OEMCP|CP_THREAD_ACP|CP_MACCP|SetFileApisToOEM|SetFileApisToANSI)\b")


def check_no_ansi_code_page():
    bad = []
    for top in SOURCES:
        for p in sorted(top.rglob("*")):
            rel = p.relative_to(ROOT).as_posix()
            if p.suffix not in (".c", ".h", ".cpp", ".hpp") or rel.startswith(VENDORED):
                continue
            for i, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if ANSI.search(line):
                    bad.append("%s:%d: %s" % (rel, i, line.strip()))
    assert not bad, "text converted with the ANSI code page (the game's char strings are UTF-8: CP_UTF8):\n" + "\n".join(bad)


if __name__ == "__main__":
    check_no_ansi_code_page()
    print("ok: no ANSI code page conversions in runtime/ and renderer")
