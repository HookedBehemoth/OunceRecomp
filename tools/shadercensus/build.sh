#!/usr/bin/env bash
# Builds censustool (tools/shadercensus/censustool.cpp: every stage of the shader seed through the translator fork, its static
# profile, RADV's compile) into work/shadercensus/, from the standalone build's translator objects (work/sa/build/obj/dx_eden_*.o)
# and the system Vulkan headers and loader.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${CENSUS_OUT:-$root/work/shadercensus}
mkdir -p "$out"
d=$root/gpu/direct
ls "$root"/work/sa/build/obj/dx_eden_*.o > /dev/null
clang++ -std=c++20 -O1 -g0 -fPIC -D_GNU_SOURCE -I"$root/host/posix" -include algorithm -include cstdio -include cstdlib -include cstring \
    -include functional -include string -include vector -Wno-everything -DMKWVK_BUILDING -DMKWVK_INCLUDES=2 \
    -I"$d/eden/compat" -I"$root/gpu/compat" -I"$d/eden" -I"$d/eden/sirit/include" -I"$d/eden/sirit/src" \
    -I"$root/work/deps/SPIRV-Headers/include" -I"$root/work/deps/SPIRV-Headers/include/spirv/unified1" -I"$root/gpu/vk" \
    -c "$root/tools/shadercensus/censustool.cpp" -o "$out/censustool.o"
clang++ -o "$out/censustool" "$out/censustool.o" "$root"/work/sa/build/obj/dx_eden_*.o -lpthread -lvulkan
echo "built $out/censustool"
