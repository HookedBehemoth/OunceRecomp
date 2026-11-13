#!/usr/bin/env bash
# The replacement stages (mesh, and since M6h fragment: the stage from the extension) to SPIR-V, one file a game mesh stage code key: <out>/<code>.spv (what meshlab --repl reads and
# what the in-game swap would load). <src>/codes.txt lists "<code key> <source> <defines...>"; built as gpu/direct/shaders/*.comp
# are (glslangValidator, Vulkan 1.3), with the sources' folder on the include path.
#   tools/meshlab/build_shaders.sh [<src dir> (gpu/direct/shaders/mesh)] <out dir>
set -euo pipefail
if [ $# -ge 2 ]; then src=$1; out=$2; else src=$(cd "$(dirname "$0")/../.." && pwd)/gpu/direct/shaders/mesh; out=${1:?out dir}; fi
mkdir -p "$out"
n=0
while read -r code file defs; do
    case "$code" in ""|\#*) continue ;; esac
    # shellcheck disable=SC2086
    glslangValidator -V --target-env vulkan1.3 -I"$src" $defs -o "$out/$code.spv" "$src/$file" > "$out/$code.log" 2>&1 \
        || { cat "$out/$code.log"; echo "FAILED: $code ($file $defs)"; exit 1; }
    # (fragment stages: every fma() NoContraction, as the translator's: tools/meshlab/spv_exact_fma.py)
    case "$file" in *.frag) python3 -I "$(dirname "$0")/spv_exact_fma.py" "$out/$code.spv" >> "$out/$code.log" ;; esac
    spirv-val --target-env vulkan1.3 --scalar-block-layout "$out/$code.spv" || { echo "spirv-val FAILED: $code"; exit 1; }
    n=$((n + 1))
done < "$src/codes.txt"
echo "built $n replacement stages into $out"
