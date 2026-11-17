#!/usr/bin/env bash
# The game build of the checkout this script is in, into its work/sa/build: the renderer (Meson, build folder
# work/sa/meson), the builder (cargo, ounce/target), then `hookshot-builder build`: the lift, the host's C, every
# compile and the link, with the lift's files for the RE tools in work/sa (gen/, facts.tsv, image/). Objects come
# from work/builder-cache when nothing changed (a rebuild takes about 10 s).
#
#   scripts/build.sh [hookshot-builder build options...]     e.g. --gen-parts (work/sa/lift_*.ll), -j N
#
# The Vulkan, SPIR-V and VMA headers are meson.options' defaults (/usr, work/deps/...: scripts/linux/deps.sh).
# scripts/play-refresh.sh and scripts/relift.sh call this under work/heavy.lock in a memory-capped scope.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
md=work/sa/meson
[ -f "$md/build.ninja" ] || meson setup --native-file gpu/direct/meson/clang.ini "$md"
ninja -C "$md"
cargo=$(command -v cargo || echo "$HOME/.cargo/bin/cargo")
"$cargo" build --release --quiet -p hookshot-builder --manifest-path ounce/Cargo.toml
ounce/target/release/hookshot-builder build work/sa/build --direct "$md/gpu/direct/hookshot_direct.so" \
  --gen work/sa --progress --game games/oot --work work --root . "$@"
