# The renderer, ounce_renderer.so: renderer/ with the shader translator (third_party/eden, third_party/sirit), built
# with clang and lld. The game's
# executable is built by the builder (ounce/builder), not here.
project('ounce-renderer', 'c', 'cpp',
  meson_version: '>= 1.3',
  default_options: ['buildtype=plain', 'warning_level=0', 'b_ndebug=true',
                    'b_lundef=false', 'b_asneeded=false'])

foreach lang : ['c', 'cpp']
  if meson.get_compiler(lang).get_id() != 'clang'
    error('the renderer is built with clang: meson setup --native-file renderer/meson/clang.ini <build dir>')
  endif
endforeach

fs = import('fs')
windows = host_machine.system() == 'windows'
python = find_program('python3', 'python')
glslang = find_program('glslangValidator')
nm = find_program('nm', 'llvm-nm', required: not windows)

root = meson.project_source_root()
# (-U_FILE_OFFSET_BITS: Meson defines it as 64 on every compile, which renames the stdio calls (fopen64))
target_args = ['-U_FILE_OFFSET_BITS', '-march=' + get_option('march'), '-ffp-contract=off']
if windows
  # (Windows: clang for the MSVC target, against the static C runtime as the game executable is; the CRT's POSIX
  # names without its deprecation, and <windows.h> without min/max macros)
  target_args += ['-fms-runtime-lib=static', '-D_CRT_SECURE_NO_WARNINGS', '-D_CRT_NONSTDC_NO_DEPRECATE', '-DNOMINMAX',
                  '-D_USE_MATH_DEFINES']
endif
if get_option('sysroot') != ''
  target_args = ['--sysroot=' + get_option('sysroot')] + target_args
endif

# (what the code takes for granted, which newer C++ libraries bring in by themselves and older ones do not)
std_includes = []
foreach h : ['algorithm', 'cstdio', 'cstdlib', 'cstring', 'functional', 'string', 'vector']
  std_includes += ['-include', h]
endforeach
renderer_cpp_args = target_args + ['-D_GNU_SOURCE'] + std_includes + [
  '-std=c++20', '-O2', '-g0', '-DNDEBUG', '-Wno-everything', '-DVK_NO_PROTOTYPES', '-DMKWVK_BUILDING']
# (the C++ library, linked statically: the compiler's libstdc++, or a release's libc++, whose sysroot's libstdc++ is
# too old for <format>; its symbols are kept local to the renderer)
libcxx = get_option('libcxx')
if libcxx != ''
  renderer_cpp_args = ['-nostdinc++', '-isystem', libcxx / 'include/c++/v1'] + renderer_cpp_args
  cxx_link_args = ['-nostdlib++', libcxx / 'lib/libc++.a', libcxx / 'lib/libc++abi.a', '-Wl,--exclude-libs,ALL', '-static-libgcc']
elif windows
  cxx_link_args = []   # (the static C++ library comes with -fms-runtime-lib=static)
else
  cxx_link_args = ['-static-libstdc++', '-static-libgcc']
endif

# (the header packages, each a package root with include/<probe>; options relative to the source root)
header_dirs = []
foreach pkg : [['spirv', 'spirv/unified1/spirv.h'], ['vulkan', 'vulkan/vulkan.h'], ['vma', 'vk_mem_alloc.h']]
  dir = get_option(pkg[0] + '_headers')
  if not fs.is_file(root / dir / 'include' / pkg[1])
    error('no ' + pkg[0] + ' headers: ' + root / dir / 'include' / pkg[1] + ' is missing (meson configure -D' + pkg[0] + '_headers=<package root>)')
  endif
  header_dirs += dir / 'include'
  if pkg[0] == 'spirv'
    header_dirs += dir / 'include/spirv/unified1'
  endif
endforeach
headers_inc = include_directories(header_dirs)
host_os_inc = include_directories('runtime/os')

subdir('third_party/zstd')
subdir('third_party/sirit')
subdir('third_party/eden')
subdir('renderer')
