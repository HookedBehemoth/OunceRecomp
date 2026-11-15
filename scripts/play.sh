#!/usr/bin/env bash
# Plays the main checkout's build (work/sa/build, kept at native-vk's head by scripts/play-refresh.sh): direct back end, window and sound on.
#   scripts/play.sh [record <name>] [title|kokiri|hf|market|zoo_<scene>|<overlay folder name under work/overlay>] [-- NAME=VALUE ...]
# Takes the GPU lock scripts/run-vk.sh uses and waits for a running build; saves in Hookshot's own folder
# (~/.local/share/hookshot/save, docs/saves.md), log in work/play/log.txt.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# (the main checkout, also when this copy is run from an agent's worktree)
common=$(git -C "$here" rev-parse --path-format=absolute --git-common-dir); root=${common%/.git}
play=$root/work/play; mkdir -p "$play"
rec=
if [ "${1:-}" = record ]; then
  name=${2:?usage: scripts/play.sh record <name> [scene]}; rec=$root/work/rec/$name; shift 2
  [ -e "$rec" ] && { echo "$rec exists"; exit 2; }
fi
scene=; if [ $# -gt 0 ] && [ "$1" != "--" ]; then scene=$1; shift; fi; [ "${1:-}" = "--" ] && shift || true
[ -n "$rec" ] && args_rec=(HOS_INPUT_RECORD="$rec") || args_rec=()
case "$scene" in
  ""|title) overlay= ;;
  kokiri) overlay=$root/work/overlay/chisel_kokiri ;;
  hf) overlay=$root/work/overlay/chisel_hf ;;
  market) overlay=$root/work/overlay/chisel_hf_market ;;
  zoo_*) overlay=$("$here/zoo-overlay.sh" "$scene") ;;   # (an actor-zoo scene, zoo/<scene>.toml, made on demand)
  *) overlay=$root/work/overlay/$scene; [ -d "$overlay" ] || { echo "no overlay $overlay"; exit 2; } ;;
esac
exec 7>"$root/work/run.gate"; flock -x 7
echo "waiting for the GPU lock (an agent's run may be going)..."
exec 8>"$root/work/run.lock"; flock -x 8
exec 7>&-
# (as scripts/run-vk.sh: the memory a run needs free before it starts, waited for; RUN_MEM_GB=0 skips the check)
need=${RUN_MEM_GB:-14}; said=
while (( need > 0 )) && (( $(awk '/MemAvailable/ {print int($2 / 1048576)}' /proc/meminfo) < need )); do
  [ -n "$said" ] || { echo "waiting for ${need} GB of free memory"; said=1; }
  sleep 2
done
romfs=$(grep -v '^#' "$root/work/dump.txt" | head -1)/romfs
args=(RX_BACKEND=direct ${args_rec[@]+"${args_rec[@]}"})
[ -n "$overlay" ] && args+=(HOS_FS_OVERLAY="$overlay")
[ -n "$overlay" ] && [ -f "$overlay/live.txt" ] && args+=(HOS_INPUT_LIVE="$overlay/live.txt")   # (an actor-zoo scene's host commands)
# (a build in progress holds this lock; the refresh script writes BUILD.txt last)
bin=$root/work/sa/build; mkdir -p "$bin"
exec 6>"$bin/.build.lock"
flock -n 6 || { echo "waiting for the build of $(git -C "$root" rev-parse --short native-vk)..."; flock 6; }
exec 6>&-
label=$(head -1 "$bin/BUILD.txt" 2>/dev/null || echo none); want=$(git -C "$root" rev-parse --short native-vk)
[ "$label" = "$want" ] || echo "STALE: this build is $label, native-vk is $want (a failed or missing build: see work/play/refresh.log)"
[ -x "$bin/hookshot" ] || { echo "no build $bin"; exit 2; }
# (the folder a packaged Hookshot keeps its saves in: one set of saves for every build)
user=${XDG_DATA_HOME:-$HOME/.local/share}/hookshot; mkdir -p "$user"
cd "$play"
echo "playing $label ${scene:-title}; log: $play/log.txt"
env "${args[@]}" "$@" "$bin/hookshot" "$root/work/sa/image/image.bin" --romfs "$romfs" --user "$user" > stdout.txt 2> log.txt || echo "exited with $?"
[ -z "$rec" ] || echo "recorded input: $rec"
journalctl -k -b --no-pager 2>/dev/null | grep -E 'ring .* timeout|GPU reset\(' | tail -2 || true
