#!/usr/bin/env bash
# The game build of the checkout this script is in, into its work/sa/build: the renderer (Meson, build folder
# work/sa/meson), the builder (cargo, ounce/target), then `ounce-builder build`: the lift, the host's C, every
# compile and the link, with the lift's files for the RE tools in work/sa (gen/, facts.tsv, image/). Objects come
# from work/builder-cache when nothing changed (a rebuild takes about 10 s).
#
#   scripts/build.sh [ounce-builder build options...]     e.g. --gen-parts (work/sa/lift_*.ll), -j N
#
# The Vulkan headers come from /usr, the SPIR-V and VMA headers from third_party/ (meson.options' defaults).
# scripts/play-refresh.sh and scripts/relift.sh call this under work/heavy.lock in a memory-capped scope.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
md=work/sa/meson
[ -f "$md/build.ninja" ] || meson setup --native-file renderer/meson/clang.ini "$md"
ninja -C "$md"
cargo=$(command -v cargo || echo "$HOME/.cargo/bin/cargo")
"$cargo" build --release --quiet -p ounce-builder --manifest-path ounce/Cargo.toml
ounce/target/release/ounce-builder build work/sa/build --direct "$md/renderer/ounce_renderer.so" \
  --gen work/sa --progress --game games/oot --work work --root . "$@"
