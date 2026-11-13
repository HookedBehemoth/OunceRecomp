#!/usr/bin/env bash
# The picture check in one command: the direct back end runs the scene, dumps
# the reference's present at full resolution (the game stepped 1/60 s a frame, HOS_GAME_STEP_PER_FRAME=1, as the reference
# was: the game's clock is then the present's number in every run), the dump is packed and compared pass by pass with the golden
# reference made earlier (work/golden/<scene>) under its noise table (scripts/direct_picture_check.py). Prints the failing passes and a verdict; exit 2 when no pass
# could be compared (all not comparable at one frame), exit 1 when a pass
# fails or is missing, 2 when the run or the dump did not happen.
#
#   scripts/direct_check.sh <title|kokiri|hf|market> [run name]     check the direct back end (EXE= another build)
#   KEEP=1: the raw dump is kept (otherwise deleted once packed); EXTRA="NAME=VALUE ..." more settings for the run
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
scene=${1:?scene: title, kokiri, hf or market}
py=$root/work/venv/bin/python
case "$scene" in
  title)  overlay=;                               present=2400; secs=420 ;;
  kokiri) overlay=$root/work/overlay/chisel_kokiri; present=1800; secs=420 ;;
  hf)     overlay=$root/work/overlay/chisel_hf;     present=1500; secs=420 ;;
  market) overlay=$root/work/overlay/chisel_hf_market; present=1500; secs=420 ;;
  *) echo "scene: title, kokiri, hf or market"; exit 2 ;;
esac
golden=$root/work/golden/$scene
exe=${EXE:-$root/work/sa/build/hookshot}

# one run with its dump packed: <name> <backend settings...>
dump_run() {
  local name=$1; shift
  local run=$root/work/runs/$name
  OVERLAY=$overlay EXE=$exe RUN=timing "$root/scripts/run-vk.sh" "$name" "$secs" -- "$@" ${EXTRA:-} | tail -1
  local dir
  dir=$(ls -d "$run"/shots/dump_"$present" 2>/dev/null || true)
  # (complete: the game went on past the dumped frame; both back ends write the dump before their next presents)
  local last
  last=$(tail -1 "$run/timeline.txt" | awk '{print $2}')
  if [ -z "$dir" ] || (( ${last:-0} <= present + 60 )); then
    echo "check: no complete dump of present $present in $run"; return 2
  fi
  "$py" -I "$root/scripts/direct_picture_check.py" pack "$run/log.txt" "$dir" "$run/packed"
  [ "${KEEP:-0}" = 1 ] || rm -rf "$dir"
}

[ -f "$golden/manifest.json" ] || { echo "check: no reference in $golden"; exit 2; }
name=${2:-check_${scene}_$(date +%H%M%S)}
run=$root/work/runs/$name
dump_run "$name" HOS_DBG=dump:$present,dumpw:4096,dumpraw,dumpdir:$run/shots HOS_FPS_LOG=1 HOS_GAME_STEP_PER_FRAME=1 || exit 2
"$py" -I "$root/scripts/direct_picture_check.py" compare "$golden/run1" "$run/packed" --noise "$golden/noise.json"
