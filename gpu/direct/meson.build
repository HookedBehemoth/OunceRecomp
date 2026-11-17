# hookshot_direct.so. The objects are linked in a fixed order (translator, renderer, zstd; each sorted by path), so
# the library comes out the same from the same sources.

gpu = root / 'gpu'
direct = gpu / 'direct'
eden = direct / 'eden'
zstd = root / 'third_party' / 'zstd' / 'lib'

# (what the code takes for granted, which newer C++ libraries bring in by themselves and older ones do not)
std_includes = []
foreach h : ['algorithm', 'cstdio', 'cstdlib', 'cstring', 'functional', 'string', 'vector']
  std_includes += ['-include', h]
endforeach
cpp_flags = target_args + ['-D_GNU_SOURCE', '-I' + root / 'host' / 'os'] + std_includes + [
  '-std=c++20', '-O2', '-g0', '-DNDEBUG', '-Wno-everything', '-DVK_NO_PROTOTYPES', '-DMKWVK_BUILDING',
  '-I' + eden / 'compat', '-I' + gpu / 'compat', '-I' + eden,
  '-I' + eden / 'sirit' / 'include', '-I' + eden / 'sirit' / 'src',
  '-I' + headers['spirv'] / 'include', '-I' + headers['spirv'] / 'include' / 'spirv' / 'unified1',
  '-I' + headers['vulkan'] / 'include', '-I' + headers['vma'] / 'include']
# (the renderer's own files: the generated SPIR-V headers' folder first, so <shaders/...> finds them there. Generated
# headers are named relative to the build folder, as ninja names them: an absolute path is another file to ninja, and a
# change would reach the objects one build late)
gen_dir = fs.relative_to(meson.current_build_dir(), meson.project_build_root())
direct_flags = ['-I' + gen_dir, '-I' + direct, '-I' + gpu / 'api'] + cpp_flags + ['-I' + zstd]

# (the checked-in *_spv.h copies serve nx2/build.py; the tests keep them equal to the sources)
cmp = find_program('cmp')
spv_headers = []
subdir('shaders')

stamps = {}
foreach kind : ['graphics', 'compute']
  stamps += {kind: custom_target('shader_stamp_' + kind + '.h',
    output: 'shader_stamp_' + kind + '.h',
    depfile: 'shader_stamp_' + kind + '.h.d',
    command: [python, '-I', files('meson/shader_stamp.py'), kind, '@OUTPUT@', '@DEPFILE@'])}
endforeach
stamped = {'direct_shader.cpp': 'graphics', 'direct_compute.cpp': 'compute'}

eden_sources = files(
  'eden/shader_recompiler/backend/spirv/emit_spirv.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_barriers.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_bitwise_conversion.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_composite.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_control_flow.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_convert.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_image.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_image_atomic.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_integer.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_logical.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_memory.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_mesh.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_select.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_shared_memory.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_special.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_undefined.cpp',
  'eden/shader_recompiler/backend/spirv/emit_spirv_warp.cpp',
  'eden/shader_recompiler/backend/spirv/spirv_emit_context.cpp',
  'eden/shader_recompiler/frontend/ir/basic_block.cpp',
  'eden/shader_recompiler/frontend/ir/condition.cpp',
  'eden/shader_recompiler/frontend/ir/ir_emitter.cpp',
  'eden/shader_recompiler/frontend/ir/microinstruction.cpp',
  'eden/shader_recompiler/frontend/ir/opcodes.cpp',
  'eden/shader_recompiler/frontend/ir/patch.cpp',
  'eden/shader_recompiler/frontend/ir/post_order.cpp',
  'eden/shader_recompiler/frontend/ir/program.cpp',
  'eden/shader_recompiler/frontend/ir/type.cpp',
  'eden/shader_recompiler/frontend/ir/value.cpp',
  'eden/shader_recompiler/frontend/maxwell/control_flow.cpp',
  'eden/shader_recompiler/frontend/maxwell/decode.cpp',
  'eden/shader_recompiler/frontend/maxwell/indirect_branch_table_track.cpp',
  'eden/shader_recompiler/frontend/maxwell/opcodes.cpp',
  'eden/shader_recompiler/frontend/maxwell/structured_control_flow.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/atomic_operations_global_memory.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/atomic_operations_shared_memory.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/attribute_memory_to_physical.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/barrier_operations.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/bitfield_extract.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/bitfield_insert.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/branch_indirect.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/common_funcs.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/condition_code_set.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/double_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/double_compare_and_set.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/double_fused_multiply_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/double_min_max.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/double_multiply.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/double_set_predicate.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/exit_program.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/find_leading_one.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_compare.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_compare_and_set.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_conversion_floating_point.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_conversion_integer.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_fused_multiply_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_min_max.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_multi_function.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_multiply.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_range_reduction.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_set_predicate.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/floating_point_swizzled_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_fused_multiply_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_helper.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_multiply.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_set.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/half_floating_point_set_predicate.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/impl.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_add_three_input.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_compare.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_compare_and_set.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_floating_point_conversion.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_funnel_shift.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_minimum_maximum.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_popcount.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_scaled_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_set_predicate.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_shift_left.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_shift_right.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_short_multiply_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/integer_to_integer_conversion.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/internal_stage_buffer_entry_read.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/load_constant.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/load_effective_address.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/load_store_attribute.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/load_store_local_shared.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/load_store_memory.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/logic_operation.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/logic_operation_three_input.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/matrix_multiply_nx2.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/move_predicate_to_register.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/move_register.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/move_register_to_predicate.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/move_special_register.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/not_implemented.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/output_geometry.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/pixel_load.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/predicate_set_predicate.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/predicate_set_register.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/register_map_nx2.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/select_source_with_predicate.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/surface_atomic_operations.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/surface_load_store.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/texture_fetch.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/texture_fetch_swizzled.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/texture_gather.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/texture_gather_swizzled.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/texture_gradient.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/texture_load.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/texture_load_swizzled.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/texture_mipmap_level.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/texture_query.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/video_helper.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/video_minimum_maximum.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/video_multiply_add.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/video_set_predicate.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/vote.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/impl/warp_shuffle.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate/translate.cpp',
  'eden/shader_recompiler/frontend/maxwell/translate_program.cpp',
  'eden/shader_recompiler/ir_opt/collect_shader_info_pass.cpp',
  'eden/shader_recompiler/ir_opt/conditional_barrier_pass.cpp',
  'eden/shader_recompiler/ir_opt/constant_propagation_pass.cpp',
  'eden/shader_recompiler/ir_opt/dead_code_elimination_pass.cpp',
  'eden/shader_recompiler/ir_opt/dual_vertex_pass.cpp',
  'eden/shader_recompiler/ir_opt/geometry_compaction_pass.cpp',
  'eden/shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp',
  'eden/shader_recompiler/ir_opt/identity_removal_pass.cpp',
  'eden/shader_recompiler/ir_opt/layer_pass.cpp',
  'eden/shader_recompiler/ir_opt/lower_fp16_to_fp32.cpp',
  'eden/shader_recompiler/ir_opt/lower_fp64_to_fp32.cpp',
  'eden/shader_recompiler/ir_opt/lower_int64_to_int32.cpp',
  'eden/shader_recompiler/ir_opt/position_pass.cpp',
  'eden/shader_recompiler/ir_opt/rescaling_pass.cpp',
  'eden/shader_recompiler/ir_opt/ssa_rewrite_pass.cpp',
  'eden/shader_recompiler/ir_opt/texture_pass.cpp',
  'eden/shader_recompiler/ir_opt/vendor_workaround_pass.cpp',
  'eden/shader_recompiler/ir_opt/verification_pass.cpp',
  'eden/sirit/src/instructions/annotation.cpp',
  'eden/sirit/src/instructions/arithmetic.cpp',
  'eden/sirit/src/instructions/atomic.cpp',
  'eden/sirit/src/instructions/barrier.cpp',
  'eden/sirit/src/instructions/bit.cpp',
  'eden/sirit/src/instructions/constant.cpp',
  'eden/sirit/src/instructions/conversion.cpp',
  'eden/sirit/src/instructions/debug.cpp',
  'eden/sirit/src/instructions/derivatives.cpp',
  'eden/sirit/src/instructions/extension.cpp',
  'eden/sirit/src/instructions/flow.cpp',
  'eden/sirit/src/instructions/function.cpp',
  'eden/sirit/src/instructions/group.cpp',
  'eden/sirit/src/instructions/image.cpp',
  'eden/sirit/src/instructions/logical.cpp',
  'eden/sirit/src/instructions/memory.cpp',
  'eden/sirit/src/instructions/misc.cpp',
  'eden/sirit/src/instructions/type.cpp',
  'eden/sirit/src/sirit.cpp',
  'eden/sirit/src/stream.cpp',
)
direct_sources = [
  'direct_assets.cpp',
  'direct_check.cpp',
  'direct_cmd.cpp',
  'direct_compute.cpp',
  'direct_draw.cpp',
  'direct_dump.cpp',
  'direct_formats.cpp',
  'direct_heap.cpp',
  'direct_image.cpp',
  'direct_lists.cpp',
  'direct_main.cpp',
  'direct_marks.cpp',
  'direct_meshrepl.cpp',
  'direct_pass.cpp',
  'direct_pools.cpp',
  'direct_present.cpp',
  'direct_pstats.cpp',
  'direct_query.cpp',
  'direct_shader.cpp',
  'direct_spv_late.cpp',
  'direct_state.cpp',
  'direct_sync.cpp',
  'direct_task.cpp',
  'direct_vma.cpp',
  'direct_work.cpp',
]

eden_lib = static_library('dx_eden', eden_sources,
  cpp_args: cpp_flags, implicit_include_directories: false, pic: true, build_by_default: false)

objects = [eden_lib.extract_all_objects(recursive: false)]
foreach src : direct_sources
  args = direct_flags
  deps = spv_headers
  if src in stamped
    args += ['-include', gen_dir / 'shader_stamp_' + stamped[src] + '.h']
    deps += stamps[stamped[src]]
  endif
  lib = static_library('dx_' + fs.stem(src), [src] + deps,
    cpp_args: args, implicit_include_directories: false, pic: true, build_by_default: false)
  objects += lib.extract_objects(src)
endforeach

# (zstd's decoder, plain C: the prepared assets' frames)
zstd_lib = static_library('dx_zstd', files(
    zstd / 'common/debug.c', zstd / 'common/entropy_common.c', zstd / 'common/error_private.c',
    zstd / 'common/fse_decompress.c', zstd / 'common/xxhash.c', zstd / 'common/zstd_common.c',
    zstd / 'decompress/huf_decompress.c', zstd / 'decompress/zstd_ddict.c', zstd / 'decompress/zstd_decompress.c',
    zstd / 'decompress/zstd_decompress_block.c'),
  c_args: target_args + ['-O2', '-g0', '-DNDEBUG', '-Wno-everything', '-fvisibility=hidden',
                         '-DZSTD_DISABLE_ASM', '-DZSTD_LEGACY_SUPPORT=0', '-DZSTD_MULTITHREAD=0'],
  implicit_include_directories: false, pic: true, build_by_default: false)
objects += zstd_lib.extract_all_objects(recursive: false)

direct_so = shared_module('hookshot_direct',
  objects: objects,
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
