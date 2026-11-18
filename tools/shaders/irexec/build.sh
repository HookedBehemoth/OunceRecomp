#!/usr/bin/env bash
# Builds irexec (tools/shaders/irexec: the post-pass IR interpreter and its command) into work/irexec/, from the
# renderer build's translator libraries (work/sa/meson: libdx_eden.a, libdx_sirit.a), as tools/shadercensus/build.sh
# builds censustool. IREXEC_LIBS: another build folder's third_party (default work/sa/meson/third_party).
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${IREXEC_OUT:-$root/work/irexec}
libs=${IREXEC_LIBS:-$root/work/sa/meson/third_party}
mkdir -p "$out"
ls "$libs/eden/libdx_eden.a" > /dev/null
flags=(-std=c++20 -O2 -g0 -fPIC -D_GNU_SOURCE -include algorithm -include cstdio -include cstdlib -include cstring
       -include functional -include string -include vector -Wno-everything -DVK_NO_PROTOTYPES -DMKWVK_BUILDING -DMKWVK_INCLUDES=2
       -I"$root/third_party/eden/compat" -I"$root/third_party/eden" -I"$root/third_party/sirit/include" -I"$root/third_party/sirit/src"
       -I"$root/runtime/os" -I"$root/third_party/SPIRV-Headers/include" -I"$root/third_party/SPIRV-Headers/include/spirv/unified1")
src=$root/tools/shaders/irexec
objs=()
for f in ${IREXEC_SOURCES:-$src/irexec.cpp $src/main.cpp}; do
    o=$out/$(basename "${f%.cpp}").o
    clang++ "${flags[@]}" -c "$f" -o "$o" &
    objs+=("$o")
done
wait
clang++ -o "$out/irexec" "${objs[@]}" "$libs/eden/libdx_eden.a" "$libs/sirit/libdx_sirit.a" -lpthread
echo "built $out/irexec"
