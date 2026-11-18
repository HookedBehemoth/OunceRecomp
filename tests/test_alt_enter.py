"""Alt+Enter toggles fullscreen and presses no button (Enter is Plus by default, and any button can be bound to it; issue #390).

The bound keys' helper (runtime/nn_hid.c's kmap_down) is compiled alone against fake key states; the window's
event handling (runtime/media/display_sdl.c) is checked to toggle on Alt+Enter, once per press.

python -I -B tests/test_alt_enter.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def case(text, label):
    """The body of one `case label:` of a switch, up to the next case"""
    start = text.index("case %s:" % label)
    end = text.index("    case ", start + 1)
    return text[start:end]


def check_linux():
    src = (ROOT / "runtime/media/display_sdl.c").read_text(encoding="utf-8")
    if "scancode == 40 && (mod & 0x300)" not in src:
        raise SystemExit("display_sdl.c: Alt+Enter no longer toggles fullscreen")


FIXTURE = r'''#include <stdio.h>
#include <stdlib.h>
#define VK_RETURN 0x0D
#define VK_MENU 0x12
#define VK_TAB 0x09
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static int held[256];
static int down(int vk) { return held[vk & 255]; }
@KMAP_DOWN@
int main(void) {
    held[VK_RETURN] = 1;
    CHECK(kmap_down(VK_RETURN, down));            /* Enter alone is Plus */
    held[VK_MENU] = 1;
    CHECK(!kmap_down(VK_RETURN, down));           /* Alt+Enter is not */
    held[VK_TAB] = 1;
    CHECK(kmap_down(VK_TAB, down));               /* Alt does not hide the other keys */
    held[VK_RETURN] = 0;
    CHECK(!kmap_down(VK_RETURN, down));
    held[VK_MENU] = 0;
    CHECK(!kmap_down(VK_MENU, down) && kmap_down(VK_TAB, down));
    printf("PASS: Alt+Enter is fullscreen, not Plus\n");
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    check_linux()
    hid = (ROOT / "runtime/nn_hid.c").read_text(encoding="utf-8")
    fn = block(hid, "static int kmap_down(")
    if "kmap_down(key_bind[i][k], key)" not in hid:
        raise SystemExit("nn_hid.c: the bound keys no longer go through kmap_down")
    with tempfile.TemporaryDirectory(prefix="alt-enter-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture"
        c.write_text(FIXTURE.replace("@KMAP_DOWN@", fn), encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
