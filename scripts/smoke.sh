#!/usr/bin/env bash
# Smoke test: one short run that only asks "did anything blow up?" (scripts/smoke.py prints the verdict; read that,
# not the log). Standing in one scene, no input; screenshots hashed against a golden run, rough fps / replay times /
# memory, new warning and error groups.
#
#   scripts/smoke.sh <name> [kokiri|hf] [-- NAME=VALUE ...]     the extra variables go to the game (switches)
#   GOLDEN=1 scripts/smoke.sh <name> [scene]                      record the golden run (default build, no switches)
#   SECS=40 (run length), EXE=<build> as for run-vk.sh. Golden data and known warnings: work/smoke/<scene>/ (game-derived:
#   stays in work/). RUN=timing: exclusive, warm caches; one game at a time.
set -euo pipefail
{
root=$(cd "$(dirname "$0")/.." && pwd)
name=${1:?name}; shift
scene=kokiri; if [ $# -gt 0 ] && [ "$1" != "--" ]; then scene=$1; shift; fi
[ "${1:-}" = "--" ] && shift
case "$scene" in
  kokiri) overlay=$root/work/overlay/chisel_kokiri; export SMOKE_SKIP=${SMOKE_SKIP:-900} ;;
  hf) overlay=$root/work/overlay/chisel_hf_market; export SMOKE_SKIP=${SMOKE_SKIP:-1500} ;;
  *) echo "scene: kokiri or hf"; exit 2 ;;
esac
out=$root/work/runs/$name
gd=$root/work/smoke/$scene
mkdir -p "$gd"
# (the run, in the background; free and shared memory sampled beside it until it ends)
OVERLAY=$overlay SHOT_EVERY=${SHOT_EVERY:-120} RUN=timing "$root/scripts/run-vk.sh" "$name" "${SECS:-40}" -- "$@" \
  > "$root/work/runs/.smoke_$name.out" 2>&1 &
rp=$!
trap 'kill $rp 2>/dev/null || true' EXIT
trap 'exit 130' INT TERM HUP
mem=$root/work/runs/.smoke_$name.mem; : > "$mem"
while kill -0 $rp 2>/dev/null; do
  awk '/MemAvailable/ {a=int($2/1024)} /^Shmem:/ {s=int($2/1024)} END {print a, s}' /proc/meminfo >> "$mem"
  sleep 1
done
wait $rp || true
mv "$root/work/runs/.smoke_$name.out" "$out/run.out"; mv "$mem" "$out/mem.txt"
rm -rf "$out/user"   # (saves and caches the run made)
status=0
"$root/work/venv/bin/python" -I "$root/scripts/smoke.py" "$out" "$gd" ${GOLDEN:+--golden} || status=$?
[ -n "${KEEP_SHOTS:-}" ] || [ -n "${GOLDEN:-}" ] || rm -rf "$out/shots"
exit $status
}
