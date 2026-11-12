#!/usr/bin/env bash
# The replacement mesh stages to SPIR-V, one file a game mesh stage code key: <out>/<code>.spv (what meshlab --repl reads and
# what the in-game swap would load). <src>/codes.txt lists "<code key> <source> <defines...>"; built as gpu/direct/shaders/*.comp
# are (glslangValidator, Vulkan 1.3), with the sources' folder on the include path.
#   tools/meshlab/build_shaders.sh <src dir> <out dir>
set -euo pipefail
src=${1:?source dir}; out=${2:?out dir}
mkdir -p "$out"
n=0
while read -r code file defs; do
    case "$code" in ""|\#*) continue ;; esac
    # shellcheck disable=SC2086
    glslangValidator -V --target-env vulkan1.3 -S mesh -I"$src" $defs -o "$out/$code.spv" "$src/$file" > "$out/$code.log" 2>&1 \
        || { cat "$out/$code.log"; echo "FAILED: $code ($file $defs)"; exit 1; }
    spirv-val --target-env vulkan1.3 --scalar-block-layout "$out/$code.spv" || { echo "spirv-val FAILED: $code"; exit 1; }
    n=$((n + 1))
done < "$src/codes.txt"
echo "built $n replacement mesh stages into $out"
