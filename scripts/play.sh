#!/usr/bin/env bash
# Plays the main checkout's build (work/oot/game, kept at native-vk's head by scripts/play-refresh.sh) with
# `ounce run oot`: window and sound on, your own saves (~/.local/share/ounce/save, docs/saves.md), the log in
# work/oot/runs/play/. Kept for now; type `ounce run oot [--record R] [--set HOS_FS_OVERLAY=<folder>]` yourself.
#   scripts/play.sh [record <name>] [title|kokiri|hf|market|zoo_<scene>|<overlay folder name>] [-- NAME=VALUE ...]
# Takes the GPU lock scripts/run-vk.sh uses and waits for a running build.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# (the main checkout, also when this copy is run from an agent's worktree)
common=$(git -C "$here" rev-parse --path-format=absolute --git-common-dir); root=${common%/.git}
args=()
if [ "${1:-}" = record ]; then args+=(--record "${2:?usage: scripts/play.sh record <name> [scene]}"); shift 2; fi
scene=; if [ $# -gt 0 ] && [ "$1" != "--" ]; then scene=$1; shift; fi
case "$scene" in
  ""|title) overlay= ;;
  kokiri) overlay=$root/work/overlay/chisel_kokiri ;;
  hf) overlay=$root/work/overlay/chisel_hf ;;
  market) overlay=$root/work/overlay/chisel_hf_market ;;
  zoo_*) overlay=$("$here/zoo-overlay.sh" "$scene") ;;   # (an actor-zoo scene, games/oot/zoo/<scene>.toml, made on demand)
  *) overlay=$root/work/overlay/$scene; [ -d "$overlay" ] || { echo "no overlay $overlay"; exit 2; } ;;
esac
[ -n "$overlay" ] && args+=(--set HOS_FS_OVERLAY="$overlay")
[ -n "$overlay" ] && [ -f "$overlay/live.txt" ] && args+=(--set HOS_INPUT_LIVE="$overlay/live.txt")   # (an actor-zoo scene's host commands)
[ "${1:-}" = "--" ] && shift || true
for kv in "$@"; do args+=(--set "$kv"); done
exec 7>"$root/work/run.gate"; flock -x 7
echo "waiting for the GPU lock (an agent's run may be going)..."
exec 8>"$root/work/run.lock"; flock -x 8
exec 7>&-
# (a build in progress holds this lock)
bin=$root/work/oot/game; mkdir -p "$bin"
exec 6>"$bin/.build.lock"
flock -n 6 || { echo "waiting for the build of $(git -C "$root" rev-parse --short native-vk)..."; flock 6; }
exec 6>&-
"$root/ounce/target/release/ounce" run oot ${args[@]+"${args[@]}"} || echo "exited with $?"
