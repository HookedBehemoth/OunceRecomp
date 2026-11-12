#!/usr/bin/env bash
# (M6b) The picture check in one command (docs/direct/m6b-report.md, section 3): the direct back end runs the scene, dumps
# the reference's present at full resolution (the game stepped 1/60 s a frame, HOS_GAME_STEP_PER_FRAME=1, as the reference
# was: the game's clock is then the present's number in every run), the dump is packed and compared pass by pass with the frozen back end's golden
# reference under its noise table (scripts/direct_picture_check.py). Prints the failing passes and a verdict; exit 2 when no pass
# could be compared (all not comparable at one frame), exit 1 when a pass
# fails or is missing, 2 when the run or the dump did not happen.
#
#   scripts/direct_check.sh <title|kokiri|hf|market> [run name]     check the direct back end (EXE= another build)
#   GOLDEN=<n> scripts/direct_check.sh <scene>                      make work/golden/<scene> anew: n (>= 3) frozen runs,
#                                                                   packed, their noise table, the manifest (the commit, the
#                                                                   present, how to make it again)
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

if [ -n "${GOLDEN:-}" ]; then
  (( GOLDEN >= 3 )) || { echo "GOLDEN: at least 3 frozen runs (the noise needs pairs)"; exit 2; }
  rm -rf "$golden"; mkdir -p "$golden"
  packs=()
  for ((i = 1; i <= GOLDEN; i++)); do
    name=golden_${scene}_$i
    dump_run "$name" RX_VK_DUMP=$present RX_VK_DUMP_W=4096 RX_VK_DUMP_RAW=1 RX_VK_DUMP_PROGS=pass HOS_FPS_LOG=1 HOS_GAME_STEP_PER_FRAME=1 || exit 2
    mv "$root/work/runs/$name/packed" "$golden/run$i"
    cp "$root/work/runs/$name/log.txt" "$golden/run$i/log.txt"
    packs+=("$golden/run$i")
  done
  "$py" -I "$root/scripts/direct_picture_check.py" noise "$golden/noise.json" "${packs[@]}"
  cat > "$golden/manifest.json" <<EOF
{"scene": "$scene", "present": $present, "overlay": "${overlay#$root/}", "commit": "$(git -C "$root" rev-parse HEAD)",
 "made": "$(date -Iseconds)", "reference": "run1", "runs": $GOLDEN, "back end": "frozen (RX_BACKEND unset)",
 "how": "GOLDEN=$GOLDEN scripts/direct_check.sh $scene (frozen runs: RX_VK_DUMP=$present RX_VK_DUMP_W=4096 RX_VK_DUMP_RAW=1 RX_VK_DUMP_PROGS=pass HOS_GAME_STEP_PER_FRAME=1; packed by scripts/direct_picture_check.py pack; noise.json by its noise command)",
 "note": "game-derived pictures: never committed, never leave the PC"}
EOF
  du -sh "$golden"
  exit 0
fi

[ -f "$golden/manifest.json" ] || { echo "check: no reference in $golden (GOLDEN=3 scripts/direct_check.sh $scene makes it)"; exit 2; }
name=${2:-check_${scene}_$(date +%H%M%S)}
run=$root/work/runs/$name
dump_run "$name" RX_BACKEND=direct HOS_DBG=dump:$present,dumpw:4096,dumpraw,dumpdir:$run/shots HOS_FPS_LOG=1 HOS_GAME_STEP_PER_FRAME=1 || exit 2
"$py" -I "$root/scripts/direct_picture_check.py" compare "$golden/run1" "$run/packed" --noise "$golden/noise.json"
