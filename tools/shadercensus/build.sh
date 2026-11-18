#!/usr/bin/env bash
# Builds censustool (tools/shadercensus/censustool.cpp: every stage of the shader seed through the translator fork, its static
# profile, RADV's compile) into work/shadercensus/, from the renderer build's translator libraries (work/sa/meson: libdx_eden.a, libdx_sirit.a)
# and the system Vulkan headers and loader.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${CENSUS_OUT:-$root/work/shadercensus}
mkdir -p "$out"
ls "$root"/work/sa/meson/third_party/eden/libdx_eden.a > /dev/null
clang++ -std=c++20 -O1 -g0 -fPIC -D_GNU_SOURCE -include algorithm -include cstdio -include cstdlib -include cstring \
    -include functional -include string -include vector -Wno-everything -DMKWVK_BUILDING -DMKWVK_INCLUDES=2 \
    -I"$root/third_party/eden/compat" -I"$root/third_party/eden" -I"$root/third_party/sirit/include" -I"$root/third_party/sirit/src" -I"$root/runtime/os" \
    -I"$root/third_party/SPIRV-Headers/include" -I"$root/third_party/SPIRV-Headers/include/spirv/unified1" \
    -c "$root/tools/shadercensus/censustool.cpp" -o "$out/censustool.o"
clang++ -o "$out/censustool" "$out/censustool.o" "$root"/work/sa/meson/third_party/eden/libdx_eden.a "$root"/work/sa/meson/third_party/sirit/libdx_sirit.a -lpthread -lvulkan
echo "built $out/censustool"
