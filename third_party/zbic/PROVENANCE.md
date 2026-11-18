# zbic

ZBIC is the NSO segment compression of Switch firmware 22.0.0+ (NSO flags bit 7, "UseZbicCompression"): zstd
frames with the magic `ZBIC` (0x4349425A) and entropy tables written with binary interpolative coding.

- `zstd.c`, `zstd.h`, `zstd_errors.h`: Atmosphere-NX's single-file zstd with `ZSTD_ZBIC_SUPPORT`, as copied into
  <https://github.com/kinnay/zbic> (`src/`, master, fetched 2026-11-04), unmodified. zstd is BSD-or-GPLv2;
  the ZBIC parts come from Atmosphere-NX (GPLv2). `LICENSE` is that repository's GPLv2 text.

It is built twice by `ounce/dump` (the dump reader in `ounce-builder`): with ZBIC support to unpack the dump's
modules, and without it for the `.nup` package's compressed members. None of it is linked into the game.
