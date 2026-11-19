# The renderer's own compute shaders as SPIR-V headers (<name>_spv.h, array <name>_spv), made in the build folder.
foreach name : ['report_resolve', 'task_prep', 'upscale']
  spv = custom_target(name + '_spv.h',
    input: name + '.comp',
    output: name + '_spv.h',
    depfile: name + '_spv.h.d',
    command: [glslang, '--quiet', '-V', '--target-env', 'vulkan1.3', '--vn', name + '_spv',
              '--depfile', '@DEPFILE@', '-o', '@OUTPUT@', '@INPUT@'])
  spv_headers += spv
  test(name + '_spv.h matches the checked-in copy', cmp, args: [spv, files(name + '_spv.h')], suite: 'renderer')
endforeach
