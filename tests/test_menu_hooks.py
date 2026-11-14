"""The game's menus' mouse (host/oot_menu.c) hooks two functions of the lifted game at their heads: a menu screen's handler of
a pane getting the focus (f_802ed660: hos_oot_menu_focus) and a menu screen's update (f_802f1380: hos_oot_menu_tick).

Parked (nx2/oot_parked.py) while the present lifter is retired: oot_patches.apply makes neither, so the menus do not
follow the mouse until the hooks are ported. The parked patch_menu_focus and patch_menu_tick still put each call first in
its function, once, and declare it; a file without the function is left alone, and a file patched before is given back
as it is.

python -I -B tests/test_menu_hooks.py
"""
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_parked  # noqa: E402
import oot_patches  # noqa: E402

FOCUS = "    hos_oot_menu_focus(c, c->x[0], c->x[1], c->x[30]);"
TICK = "    hos_oot_menu_tick(c, c->x[0]);"
BODY = "    c->x[19]=c->x[0];\n    c->x[8]=recomp_load64(c,c->x[19] + 56);\nreturn;\n}\n"


def lifted(*heads):
    return '#include "a64.h"\n' + "".join(head + BODY for head in heads)


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def hooked(patch, head, call, decl):
    out = patch(lifted(head))
    check(out is not None and out.count(call) == 1, "the hook is not in its function once: %s" % call.strip())
    check(head + call in out, "the hook is not first in its function: %s" % call.strip())
    check(decl in out, "the lifted file does not declare the hook: %s" % decl)
    check(BODY in out, "the function's body was changed")
    check(patch(out) == out, "a file patched before was patched again: %s" % call.strip())
    check(patch(lifted(head.replace("f_802", "f_803"))) is None, "a file without the function was patched")
    return out


def applied():
    with tempfile.TemporaryDirectory() as gen:
        path = os.path.join(gen, "code_000.c")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(lifted(oot_parked.MENU_FOCUS_HEAD, oot_parked.MENU_TICK_HEAD))
        stdout = sys.stdout
        sys.stdout = open(os.devnull, "w")
        try:
            oot_patches.apply(gen)
        finally:
            sys.stdout.close()
            sys.stdout = stdout
        with open(path, encoding="utf-8", newline="") as f:
            return f.read()


def main():
    hooked(oot_parked.patch_menu_focus, oot_parked.MENU_FOCUS_HEAD, FOCUS,
           "extern void hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr);")
    hooked(oot_parked.patch_menu_tick, oot_parked.MENU_TICK_HEAD, TICK,
           "extern void hos_oot_menu_tick(Ctx *c, uint64_t screen);")
    out = applied()
    check(FOCUS not in out and TICK not in out, "oot_patches.apply made a parked hook")
    print("PASS: the parked menus' hooks go first in their functions, once; the lift applies neither")


if __name__ == "__main__":
    main()
