# hookshot_direct.so: the direct back end's sources in this folder, linked with the translator (third_party/eden,
# third_party/sirit) and zstd's decoder. api/ holds what the runtime and the back end share.

# (the generated headers' folder: include_directories('.') puts the build folder before the source folder, so
# <shaders/...> finds the SPIR-V headers made there, not the checked-in copies. Meson names it relative to the build
# root, as ninja names the generated headers: an absolute path is another file to ninja, and a change would reach the
# objects one build late. The stamp headers' -include below is named the same way)
direct_inc = include_directories('.')
gen_dir = fs.relative_to(meson.current_build_dir(), meson.project_build_root())
gpu_api_inc = include_directories('api')

# (the checked-in *_spv.h copies: `meson test` keeps them equal to the sources)
cmp = find_program('cmp')
spv_headers = []
subdir('shaders')

direct_sources = files(
  'direct_assets.cpp',
  'direct_check.cpp',
  'direct_cmd.cpp',
  'direct_draw.cpp',
  'direct_dump.cpp',
  'direct_formats.cpp',
  'direct_heap.cpp',
  'direct_image.cpp',
  'direct_lists.cpp',
  'direct_main.cpp',
  'direct_marks.cpp',
  'direct_pass.cpp',
  'direct_pools.cpp',
  'direct_present.cpp',
  'direct_pstats.cpp',
  'direct_query.cpp',
  'direct_spv_late.cpp',
  'direct_state.cpp',
  'direct_sync.cpp',
  'direct_task.cpp',
  'direct_vma.cpp',
  'direct_work.cpp',
)
direct_args = renderer_cpp_args
direct_deps = [eden_dep, zstd_dep]
direct_incs = [direct_inc, gpu_api_inc, host_os_inc, headers_inc]

# (the shader stamps, each force-included into the one file whose program cache it keys; Meson takes no per-file
# flags, so each of those files is a library of its own. The compiler and its flags are in the stamp: they build the
# translator)
cpp = meson.get_compiler('cpp')
toolchain = [cpp.get_id(), cpp.version()] + renderer_cpp_args
stamped = []
foreach kind, src : {'graphics': 'direct_shader.cpp', 'compute': 'direct_compute.cpp'}
  stamp = custom_target('shader_stamp_' + kind + '.h',
    output: 'shader_stamp_' + kind + '.h',
    depfile: 'shader_stamp_' + kind + '.h.d',
    command: [python, '-I', files('meson/shader_stamp.py'), kind, '@OUTPUT@', '@DEPFILE@',
              root / 'third_party/eden', root / 'third_party/sirit', '--'] + toolchain)
  stamped += static_library('dx_' + fs.stem(src), [src, stamp] + spv_headers,
    cpp_args: direct_args + ['-include', gen_dir / 'shader_stamp_' + kind + '.h'],
    include_directories: direct_incs, dependencies: direct_deps,
    implicit_include_directories: false, pic: true, build_by_default: false)
endforeach

direct_so = shared_module('hookshot_direct', direct_sources, spv_headers,
  cpp_args: direct_args, include_directories: direct_incs, dependencies: direct_deps,
  implicit_include_directories: false,
  link_whole: [eden_lib, sirit_lib, zstd_lib] + stamped,
  name_prefix: '',
  link_args: target_args + ['-fuse-ld=lld', '-Wl,-Map=' + meson.current_build_dir() / 'hookshot_direct.so.map',
                            '-Wl,-Bsymbolic', '-Wl,--gc-sections', '-static-libstdc++', '-static-libgcc',
                            '-ldl', '-lpthread', '-lm'])

# (the host dlopens it with RTLD_NOW: an unresolved symbol is a failed build here, not a black window)
exe = get_option('exe') == '' ? '' : root / get_option('exe')
custom_target('hookshot_direct.so.symbols',
  input: direct_so,
  output: 'hookshot_direct.so.symbols',
  command: [python, '-I', files('meson/check_symbols.py'), nm, '@INPUT@', '@OUTPUT@', exe],
  depend_files: exe == '' ? [] : [exe],
  build_by_default: true)
