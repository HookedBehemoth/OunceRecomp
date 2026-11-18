zstd 1.5.7, from https://github.com/facebook/zstd (release v1.5.7, zstd-1.5.7.tar.gz, sha256
eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3), unmodified: lib/zstd.h, lib/zstd_errors.h,
lib/zdict.h, lib/common/ and lib/decompress/. BSD license (LICENSE; zstd is dual BSD or GPLv2, used here under BSD).

The decoder only, built with ZSTD_DISABLE_ASM (third_party/zstd/meson.build). Used by host/nn_zstd.c, host/prompts.c and
renderer/vk_assets.cpp to read zstd frames (the game's compressed files and the prepared assets), in memory.
