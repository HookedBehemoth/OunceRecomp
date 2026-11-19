# ounce_renderer.so: the direct back end's sources in this folder, linked with the translator (third_party/eden,
# third_party/sirit) and zstd's decoder. api/ holds what the runtime and the back end share.

# (the generated headers' folder: include_directories('.') puts the build folder before the source folder, so
# <shaders/...> finds the SPIR-V headers made there, not the checked-in copies. Meson names it relative to the build
# root, as ninja names the generated headers: an absolute path is another file to ninja, and a change would reach the
# objects one build late. The stamp headers' -include below is named the same way)
vk_inc = include_directories('.')
gen_dir = fs.relative_to(meson.current_build_dir(), meson.project_build_root())
gpu_api_inc = include_directories('api')

# (the checked-in *_spv.h copies: `meson test` keeps them equal to the sources)
cmp = find_program('cmp')
spv_headers = []
subdir('shaders')

vk_sources = files(
  'vk_assets.cpp',
  'vk_check.cpp',
  'vk_cmd.cpp',
  'vk_draw.cpp',
  'vk_dump.cpp',
  'vk_formats.cpp',
  'vk_pool.cpp',
  'vk_image.cpp',
  'vk_lists.cpp',
  'vk_main.cpp',
  'vk_marks.cpp',
  'vk_pass.cpp',
  'vk_pools.cpp',
  'vk_present.cpp',
  'vk_pstats.cpp',
  'vk_query.cpp',
  'vk_spv_late.cpp',
  'vk_state.cpp',
  'vk_sync.cpp',
  'vk_task.cpp',
  'vk_vma.cpp',
  'vk_work.cpp',
)
vk_args = renderer_cpp_args
vk_deps = [eden_dep, zstd_dep]
vk_incs = [vk_inc, gpu_api_inc, host_os_inc, headers_inc]

# (the shader stamps, each force-included into the one file whose program cache it keys; Meson takes no per-file
# flags, so each of those files is a library of its own. The compiler and its flags are in the stamp: they build the
# translator)
cpp = meson.get_compiler('cpp')
toolchain = [cpp.get_id(), cpp.version()] + renderer_cpp_args
stamped = []
foreach kind, src : {'graphics': 'vk_shader.cpp', 'compute': 'vk_compute.cpp'}
  stamp = custom_target('shader_stamp_' + kind + '.h',
    output: 'shader_stamp_' + kind + '.h',
    depfile: 'shader_stamp_' + kind + '.h.d',
    command: [python, '-I', files('meson/shader_stamp.py'), kind, '@OUTPUT@', '@DEPFILE@',
              root / 'third_party/eden', root / 'third_party/sirit', '--'] + toolchain)
  stamped += static_library('dx_' + fs.stem(src), [src, stamp] + spv_headers,
    cpp_args: vk_args + ['-include', gen_dir / 'shader_stamp_' + kind + '.h'],
    include_directories: vk_incs, dependencies: vk_deps,
    implicit_include_directories: false, pic: true, build_by_default: false)
endforeach

vk_so = shared_module('ounce_renderer', vk_sources, spv_headers,
  cpp_args: vk_args, include_directories: vk_incs, dependencies: vk_deps,
  implicit_include_directories: false,
  link_whole: [eden_lib, sirit_lib, zstd_lib] + stamped,
  name_prefix: '',
  link_args: target_args + ['-fuse-ld=lld', '-Wl,-Map=' + meson.current_build_dir() / 'ounce_renderer.so.map',
                            '-Wl,-Bsymbolic', '-Wl,--gc-sections', '-static-libstdc++', '-static-libgcc',
                            '-ldl', '-lpthread', '-lm'])

# (the host dlopens it with RTLD_NOW: an unresolved symbol is a failed build here, not a black window)
exe = get_option('exe') == '' ? '' : root / get_option('exe')
custom_target('ounce_renderer.so.symbols',
  input: vk_so,
  output: 'ounce_renderer.so.symbols',
  command: [python, '-I', files('meson/check_symbols.py'), nm, '@INPUT@', '@OUTPUT@', exe],
  depend_files: exe == '' ? [] : [exe],
  build_by_default: true)
