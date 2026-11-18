#!/usr/bin/env bash
# Ocarina of Time's build of the checkout this script is in: builds `ounce` (cargo, ounce/target), then runs
# `ounce build oot` (the renderer in work/renderer, the game in work/oot/: game/, lift/, obj/, build.log). Kept for the
# scripts that call it; type `ounce build <game>` yourself.
#
#   scripts/build.sh [ounce build options...]     e.g. --gen-parts, -j N
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
cargo=$(command -v cargo || echo "$HOME/.cargo/bin/cargo")
"$cargo" build --release --quiet -p ounce --manifest-path ounce/Cargo.toml
exec ounce/target/release/ounce build oot "$@"
