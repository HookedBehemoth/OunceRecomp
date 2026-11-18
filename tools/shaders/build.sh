#!/usr/bin/env bash
# Builds the shader tools into work/shaders/: spvlate (the back end's register-pressure pass as a command:
# renderer/vk_spv_late.cpp) and, from the renderer build's translator libraries (work/renderer: libdx_eden.a, libdx_sirit.a),
# meshtool (one stage through the translator fork, offline).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${SHADERS_OUT:-$root/work/shaders}
mkdir -p "$out"
src=$root/tools/shaders
clang++ -std=c++20 -O2 -Wall -o "$out/spvlate" "$src/spvlate.cpp"
echo "built $out/spvlate"
if ls "$root"/work/renderer/third_party/eden/libdx_eden.a > /dev/null 2>&1; then
        clang++ -std=c++20 -O1 -g0 -fPIC -D_GNU_SOURCE -include algorithm -include cstdio -include cstdlib -include cstring \
        -include functional -include string -include vector -Wno-everything -DVK_NO_PROTOTYPES -DMKWVK_BUILDING -DMKWVK_INCLUDES=2 \
        -I"$root/third_party/eden/compat" -I"$root/third_party/eden" -I"$root/third_party/sirit/include" -I"$root/third_party/sirit/src" -I"$root/runtime/os" \
        -I"$root/third_party/SPIRV-Headers/include" -I"$root/third_party/SPIRV-Headers/include/spirv/unified1" \
        -c "$src/meshtool.cpp" -o "$out/meshtool.o"
    clang++ -o "$out/meshtool" "$out/meshtool.o" "$root"/work/renderer/third_party/eden/libdx_eden.a "$root"/work/renderer/third_party/sirit/libdx_sirit.a -lpthread
    echo "built $out/meshtool"
else
    echo "meshtool: no translator objects (work/renderer: libdx_eden.a, libdx_sirit.a): build the renderer first (scripts/build.sh; the libraries are built on demand: ninja -C work/renderer third_party/eden/libdx_eden.a third_party/sirit/libdx_sirit.a)"
fi
