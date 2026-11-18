#!/usr/bin/env bash
# Builds the shader tools into work/shaders/: spvlate (the back end's register-pressure pass as a command:
# renderer/vk_spv_late.cpp) and, from the standalone build's translator objects (work/sa/build/obj/dx_eden_*.o),
# meshtool (one stage through the translator fork, offline).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${SHADERS_OUT:-$root/work/shaders}
mkdir -p "$out"
src=$root/tools/shaders
clang++ -std=c++20 -O2 -Wall -o "$out/spvlate" "$src/spvlate.cpp"
echo "built $out/spvlate"
if ls "$root"/work/sa/build/obj/dx_eden_*.o > /dev/null 2>&1; then
    d=$root/gpu/direct
    clang++ -std=c++20 -O1 -g0 -fPIC -D_GNU_SOURCE -include algorithm -include cstdio -include cstdlib -include cstring \
        -include functional -include string -include vector -Wno-everything -DVK_NO_PROTOTYPES -DMKWVK_BUILDING -DMKWVK_INCLUDES=2 \
        -I"$d/eden/compat" -I"$root/gpu/compat" -I"$d/eden" -I"$d/eden/sirit/include" -I"$d/eden/sirit/src" \
        -I"$root/work/deps/SPIRV-Headers/include" -I"$root/work/deps/SPIRV-Headers/include/spirv/unified1" \
        -c "$src/meshtool.cpp" -o "$out/meshtool.o"
    clang++ -o "$out/meshtool" "$out/meshtool.o" "$root"/work/sa/build/obj/dx_eden_*.o -lpthread
    echo "built $out/meshtool"
else
    echo "meshtool: no translator objects (work/sa/build/obj/dx_eden_*.o): build the game first"
fi
