#!/usr/bin/env bash
# Builds meshlab (the mesh harness: tools/meshlab/meshlab.cpp) into work/meshlab/, apart from the game; system Vulkan headers
# and loader. And the replacement mesh stages (gpu/direct/shaders/mesh/*.mesh) to SPIR-V when given --shaders <out dir>.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$root/work/meshlab
mkdir -p "$out"
src=$root/tools/meshlab
clang++ -std=c++20 -O2 -g -Wall -Wno-missing-field-initializers -o "$out/meshlab" "$src/meshlab.cpp" "$src/spv_dump.cpp" -lvulkan
echo "built $out/meshlab"
# meshtool (offline: the translator fork's objects from the standalone build, work/sa/build/obj/dx_eden_*.o)
if [ "${1:-}" != "--harness-only" ] && ls "$root"/work/sa/build/obj/dx_eden_*.o > /dev/null 2>&1; then
    d=$root/gpu/direct
    clang++ -std=c++20 -O1 -g0 -fPIC -D_GNU_SOURCE -I"$root/host/posix" -include algorithm -include cstdio -include cstdlib -include cstring \
        -include functional -include string -include vector -Wno-everything -DVK_NO_PROTOTYPES -DMKWVK_BUILDING -DMKWVK_INCLUDES=2 \
        -I"$d/eden/compat" -I"$root/gpu/compat" -I"$d/eden" -I"$d/eden/sirit/include" -I"$d/eden/sirit/src" \
        -I"$root/work/deps/SPIRV-Headers/include" -I"$root/work/deps/SPIRV-Headers/include/spirv/unified1" -I"$root/gpu/vk" \
        -c "$src/meshtool.cpp" -o "$out/meshtool.o"
    clang++ -o "$out/meshtool" "$out/meshtool.o" "$root"/work/sa/build/obj/dx_eden_*.o -lpthread
    echo "built $out/meshtool"
fi
