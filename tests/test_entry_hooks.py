"""nx2/entry_hooks.py reads host/*.c's ENTRY_HOOK, ENTRY_HOOK_SYM (resolved through image.json's exports) and
SPLICE_AFTER declarations; the lifter calls the hooks at the functions' entries (nx2/lift2/game.py, emit_llvm). A name
the exports lack is reported and left out. (The hook's call at the entry is compiled and run by the lifter's own
test: tests/test_lift2_fp.py.)

python -I -B tests/test_entry_hooks.py
"""
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import entry_hooks  # noqa: E402

HOST = r'''#include <stdint.h>
typedef struct { uint64_t x[32]; } Ctx;
#define ENTRY_HOOK(addr) int entry_##addr(Ctx *c)
#define ENTRY_HOOK_SYM(sym) int entry_sym_##sym(Ctx *c)
ENTRY_HOOK(0x80001000) { return 0; }
ENTRY_HOOK_SYM(the_copy) { c->x[0] = 999; return 1; }
ENTRY_HOOK_SYM(not_exported) { return 1; }
#define SPLICE_AFTER(addr, anchor) void splice_##addr(Ctx *c)
#define SPLICE_AFTER_EACH(addr, anchor) void splice_##addr(Ctx *c)
ENTRY_HOOK(0x80004000) { return 0; }
SPLICE_AFTER_EACH(0x80004000, "c->x[23] + 56,c->x[10]);") { c->x[10] += 1; }
SPLICE_AFTER(0x80005000, "say \"a\\b\"") { }
'''


def main():
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        gen, host, image = d / "sa" / "gen", d / "host", d / "sa" / "image"
        for p in (gen, host, image):
            p.mkdir(parents=True)
        (host / "hooks.c").write_text(HOST)
        (image / "image.json").write_text(json.dumps({"exports": {"the_copy": 0x80002000}}))
        hooks, missing = entry_hooks.wanted(str(gen), str(host))
        assert hooks == {0x80001000: "entry_0x80001000", 0x80002000: "entry_sym_the_copy",
                         0x80004000: "entry_0x80004000"}, hooks
        assert missing == ["not_exported"], missing
        splices = entry_hooks.wanted_splices(str(host))
        assert splices[0x80004000] == ("c->x[23] + 56,c->x[10]);", True), splices
        assert splices[0x80005000] == ('say "a\\b"', False), splices
    print("ok")


if __name__ == "__main__":
    main()
