# Sirit, the SPIR-V assembler the translator writes with, as a static library, sirit_lib, and sirit_dep: its headers.
sirit_inc = include_directories('include', 'src')
sirit_sources = []
subdir('src')
sirit_lib = static_library('dx_sirit', sirit_sources,
  cpp_args: renderer_cpp_args,
  include_directories: [sirit_inc, headers_inc],
  implicit_include_directories: false, pic: true, build_by_default: false)
sirit_dep = declare_dependency(include_directories: sirit_inc)
