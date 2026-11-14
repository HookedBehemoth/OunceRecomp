#!/usr/bin/env bash
# A benchmark run (HOS_BENCH, host/bench.c): one scene, headless and muted, at a fixed 1/60 s game step and unpaced; from the
# moment the scene is ready it measures <frames> frames, writes bench.csv in the run's folder and exits. Prints the summary.
#
#   scripts/bench.sh <scene> [frames] [-- NAME=VALUE ...]     the extra variables go to the game (switches)
#   scene: kokiri, hf, market (the chiseled boots of scripts/play.sh), an actor-zoo scene (zoo/<name>.toml: run-vk.sh's ZOO=,
#   with its live.txt) or a folder name under work/overlay
#   NAME=<run name> (default bench-<scene>), READY=<frames settled in a row> (60), SECS=<longest run> (110), EXE=<build>
#   as for run-vk.sh. RUN=timing: exclusive, one game at a time; it waits for any other hookshot to end first.
set -euo pipefail
{
root=$(cd "$(dirname "$0")/.." && pwd)
scene=${1:?scene}; shift
frames=600; if [ $# -gt 0 ] && [ "$1" != "--" ]; then frames=$1; shift; fi
[ "${1:-}" = "--" ] && shift
zoo=
case "$scene" in
  kokiri) overlay=$root/work/overlay/chisel_kokiri ;;
  hf) overlay=$root/work/overlay/chisel_hf ;;
  market) overlay=$root/work/overlay/chisel_hf_market ;;
  *) if [ -f "$root/zoo/$scene.toml" ]; then zoo=$scene; overlay=
     else overlay=$root/work/overlay/$scene; [ -d "$overlay" ] || { echo "no overlay $overlay"; exit 2; }; fi ;;
esac
name=${NAME:-bench-$scene}
# (another game, a run of another checkout or a play session, is waited for: two at once would measure each other)
said=
while pgrep -x hookshot > /dev/null; do
  [ -n "$said" ] || { echo "bench: waiting for another hookshot to end"; said=1; }
  sleep 2
done
ZOO=$zoo OVERLAY=$overlay SHOT_EVERY=${SHOT_EVERY:-1000000} RUN=timing "$root/scripts/run-vk.sh" "$name" "${SECS:-110}" -- \
  HOS_BENCH="$frames,${READY:-60}" "$@"
out=$root/work/runs/$name
rm -rf "$out/user" "$out/shots"   # (saves and caches the run made; no pictures are taken)
grep -h 'bench: ' "$out/log.txt" | grep -v 'not ready' | sed 's/^\[hos [0-9.]* p[0-9]* [^]]*\] //'
[ -f "$out/bench.csv" ] || { echo "bench: no bench.csv: the run did not finish (see $out/log.txt)"; exit 1; }
}
