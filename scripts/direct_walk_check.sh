#!/usr/bin/env bash
# Checks in motion: every earlier check stood still. Two scripted routes from the market
# overlay (Hyrule Field at the castle town's drawbridge), each a run of scripts/run-vk.sh (RUN=timing: the GPU lock):
#
#   scripts/direct_walk_check.sh walk   [run name]   forward from the drawbridge toward Lon Lon Ranch (the stick held up),
#                                                    shots every SHOT_EVERY (default 120) presents
#   scripts/direct_walk_check.sh scenes [run name]   back through the drawbridge and out again, scene changes in a row (the
#                                                    stick held down: each scene starts with Link facing away from the door
#                                                    he came through), the memory check
#
#   HOS_DBG=strict (plus DBG=<more HOS_DBG items>); the verdict fails on a strict
#             stop, on a texture made with undefined content (a load with no data the asset path could not resolve, an
#             ASTC texture with no prepared record: the placeholder), and with no complete run
#   EXE=<build>, EXTRA="NAME=VALUE ..." more settings
#
# vram.txt in the run: seconds, the GPU's VRAM and GTT in use (amdgpu sysfs, the whole device: the desktop's share too),
# sampled every second while the run's game is alive; the verdict prints its steady value per scene window.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
. "$root/scripts/game.sh"
route=${1:?route: walk or scenes}
py=$root/work/venv/bin/python
overlay=$root/work/overlay/chisel_hf_market
case "$route" in
  # (play starts ~20 s after launch; the script's seconds count from the game's first controller read, ~5 s in)
  walk)   input="20:LUP/80000";                                            secs=115 ;;
  scenes) input="20:LDOWN/5000,45:LDOWN/5000,70:LDOWN/5000,95:LDOWN/5000,120:LDOWN/5000,145:LDOWN/5000"; secs=185 ;;
  *) echo "route: walk or scenes"; exit 2 ;;
esac
name=${2:-walk_${route}_$(date +%H%M%S)}
run=$root/work/runs/$name
exe=${EXE:-$game_exe}
args=("HOS_INPUT_SCRIPT=$input" HOS_INPUT_EXCLUSIVE=1 HOS_INPUT_SCRIPT_BACKGROUND=1)
args+=("HOS_DBG=strict${DBG:+,$DBG}")

# (the VRAM sampler: waits for this run's game, then a line a second until it is gone)
card=$(ls -d /sys/class/drm/card*/device 2>/dev/null | while read -r d; do [ -f "$d/mem_info_vram_used" ] && echo "$d" && break; done)
(
  while ! pgrep -f -- "--user $run/user" >/dev/null; do sleep 1; done
  t0=$(date +%s)
  while pid=$(pgrep -f -- "--user $run/user" | head -1) && [ -n "$pid" ]; do
    [ -n "$card" ] && echo "$(( $(date +%s) - t0 )) $(( $(cat "$card/mem_info_vram_used") >> 20 )) $(( $(cat "$card/mem_info_gtt_used") >> 20 ))" >> "$run/vram.txt"
    sleep 1
  done
) &
sampler=$!
trap 'kill $sampler 2>/dev/null || true' EXIT

SHOT_EVERY=${SHOT_EVERY:-120} OVERLAY=$overlay EXE=$exe RUN=timing "$root/scripts/run-vk.sh" "$name" "$secs" -- "${args[@]}" ${EXTRA:-} | tail -1
sleep 2
kill $sampler 2>/dev/null || true
journalctl -k -b --no-pager 2>/dev/null | grep -E 'ring .* timeout|GPU reset\(' | tail -2 || true
"$py" -I "$root/scripts/direct_walk_check.py" "$run" direct "$route"
