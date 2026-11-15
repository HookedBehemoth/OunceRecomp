# tools/re

Searches for reverse engineering in the main executable's flat image (`work/re/main.bin`, made from the dump's exefs; `MAIN_BIN=<file>` for
another). Offsets are file offsets (virtual address minus the 0x80020000 base). Run with `work/venv/bin/python`.

| script | what it is |
|---|---|
| `disasm.py <hex_off> [before] [after]` | AArch64 disassembly around an offset, with ADRP+ADD string references named (needs `capstone`) |
| `xref.py <hex_off> ...` or `-s <string> ...` | code that references an address or a string through ADRP+ADD |
| `callers.py <hex_off> ...` | BL/B callers of a function and 8-byte pointers to it |
