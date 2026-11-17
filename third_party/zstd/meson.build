# zstd's decoder, plain C (the prepared assets' frames), as a static library, zstd_lib, and zstd_dep: its headers.
zstd_lib = static_library('dx_zstd', files(
    'lib/common/debug.c', 'lib/common/entropy_common.c', 'lib/common/error_private.c',
    'lib/common/fse_decompress.c', 'lib/common/xxhash.c', 'lib/common/zstd_common.c',
    'lib/decompress/huf_decompress.c', 'lib/decompress/zstd_ddict.c', 'lib/decompress/zstd_decompress.c',
    'lib/decompress/zstd_decompress_block.c'),
  c_args: target_args + ['-O2', '-g0', '-DNDEBUG', '-Wno-everything', '-fvisibility=hidden',
                         '-DZSTD_DISABLE_ASM', '-DZSTD_LEGACY_SUPPORT=0', '-DZSTD_MULTITHREAD=0'],
  implicit_include_directories: false, pic: true, build_by_default: false)
zstd_dep = declare_dependency(include_directories: include_directories('lib'))
