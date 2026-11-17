# The replacement mesh stages the renderer draws with (swap.txt), each built from its codes.txt line as
# <code key>_spv.h, array mesh_<code key>_spv.
codes = {}
foreach line : fs.read('codes.txt').strip().split('\n')
  words = line.strip().split()
  if words.length() >= 2 and not words[0].startswith('#')
    codes += {words[0]: words}
  endif
endforeach
foreach line : fs.read('swap.txt').strip().split('\n')
  words = line.strip().split()
  if words.length() == 0 or words[0].startswith('#')
    continue
  endif
  code = words[0]
  if code not in codes
    error('swap.txt: ' + code + ' is not in codes.txt')
  endif
  defines = []
  foreach w : codes[code]
    if w.startswith('-')
      defines += w
    endif
  endforeach
  spv = custom_target(code + '_spv.h',
    input: codes[code][1],
    output: code + '_spv.h',
    depfile: code + '_spv.h.d',
    command: [glslang, '--quiet', '-V', '--target-env', 'vulkan1.3', '-I' + meson.current_source_dir()]
             + defines
             + ['--vn', 'mesh_' + code + '_spv', '--depfile', '@DEPFILE@', '-o', '@OUTPUT@', '@INPUT@'])
  spv_headers += spv
  test(code + '_spv.h matches the checked-in copy', cmp, args: [spv, files(code + '_spv.h')], suite: 'renderer')
endforeach
