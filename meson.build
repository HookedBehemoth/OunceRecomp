# The renderer, hookshot_direct.so: gpu/direct with its translator fork, built with clang and lld. The game's
# executable is built by the builder (ounce/builder), not here.
project('hookshot-renderer', 'c', 'cpp',
  meson_version: '>= 1.3',
  default_options: ['buildtype=plain', 'warning_level=0', 'b_ndebug=true',
                    'b_lundef=false', 'b_asneeded=false'])

foreach lang : ['c', 'cpp']
  if meson.get_compiler(lang).get_id() != 'clang'
    error('the renderer is built with clang: meson setup --native-file gpu/direct/meson/clang.ini <build dir>')
  endif
endforeach

fs = import('fs')
python = find_program('python3')
glslang = find_program('glslangValidator')
nm = find_program('nm', 'llvm-nm')

root = meson.project_source_root()
# (-U_FILE_OFFSET_BITS: Meson defines it as 64 on every compile, which renames the stdio calls (fopen64))
target_args = ['-U_FILE_OFFSET_BITS', '-march=' + get_option('march'), '-ffp-contract=off']
if get_option('sysroot') != ''
  target_args = ['--sysroot=' + get_option('sysroot')] + target_args
endif

# (the header packages: a package root with include/<probe>)
headers = {}
foreach name, probe : {'vulkan': 'vulkan/vulkan.h', 'spirv': 'spirv/unified1/spirv.h', 'vma': 'vk_mem_alloc.h'}
  dir = root / get_option(name + '_headers')
  if not fs.is_file(dir / 'include' / probe)
    error('no ' + name + ' headers: ' + dir / 'include' / probe + ' is missing (meson configure -D' + name + '_headers=<package root>)')
  endif
  headers += {name: dir}
endforeach

subdir('gpu/direct')
