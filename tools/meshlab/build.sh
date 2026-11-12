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
