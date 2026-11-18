# The shader translator (Eden's shader_recompiler, our fork, with the std-based shims of its libraries in compat/) as a
# static library, eden_lib, and eden_dep: its headers and Sirit's, for the code that calls it.
eden_inc = include_directories('compat', '.')
eden_sources = []
subdir('shader_recompiler/backend/spirv')
subdir('shader_recompiler/frontend/ir')
subdir('shader_recompiler/frontend/maxwell')
subdir('shader_recompiler/ir_opt')

eden_lib = static_library('dx_eden', eden_sources,
  cpp_args: renderer_cpp_args,
  include_directories: [host_os_inc, eden_inc, sirit_inc, headers_inc],
  implicit_include_directories: false, pic: true, build_by_default: false)
eden_dep = declare_dependency(include_directories: [eden_inc, sirit_inc])
