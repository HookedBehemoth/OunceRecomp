#!/usr/bin/env bash
# Builds difftest (tools/shaders/difftest: the differential of two translator fronts, with irexec) into work/difftest/,
# from the renderer build's translator libraries as tools/shaders/irexec/build.sh does. DIFFTEST_LIBS: another build
# folder's third_party (default work/renderer/third_party).
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${DIFFTEST_OUT:-$root/work/difftest}
libs=${DIFFTEST_LIBS:-$root/work/renderer/third_party}
mkdir -p "$out"
ls "$libs/eden/libdx_eden.a" > /dev/null
flags=(-std=c++20 -O2 -g0 -fPIC -D_GNU_SOURCE -include algorithm -include cstdio -include cstdlib -include cstring
       -include functional -include string -include vector -Wno-everything -DVK_NO_PROTOTYPES -DMKWVK_BUILDING -DMKWVK_INCLUDES=2
       -I"$root/third_party/eden/compat" -I"$root/third_party/eden" -I"$root/third_party/sirit/include" -I"$root/third_party/sirit/src"
       -I"$root/runtime/os" -I"$root/third_party/SPIRV-Headers/include" -I"$root/third_party/SPIRV-Headers/include/spirv/unified1")
objs=()
for f in "$root/tools/shaders/irexec/irexec.cpp" "$root/tools/shaders/difftest/difftest.cpp"; do
    o=$out/$(basename "${f%.cpp}").o
    clang++ "${flags[@]}" -c "$f" -o "$o" &
    objs+=("$o")
done
wait
clang++ -o "$out/difftest" "${objs[@]}" "$libs/eden/libdx_eden.a" "$libs/sirit/libdx_sirit.a" -lpthread
echo "built $out/difftest"
