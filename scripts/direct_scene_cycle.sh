#!/usr/bin/env bash
# (M6f; promoted from work/proto/scenecycle/cycle.sh) The scene cycle for the direct back end: one boot into Kokiri
# (chisel_kokiri), then the stops of the maps list in turn through "!warp <banc> <startpos> -" lines on HOS_INPUT_LIVE
# (host/nn_hid.c, host/hos_main.c: the log's "scene: ready <banc>"). At each stop: pan right 1 s, walk 2 s, pan left 1 s,
# then wait for the back end's next 600-present block and note the stop's state in <run>/stops.txt (a line per stop:
# the device-local heap's driver usage and VMA blocks, live images and their MB and the store's share, images held only
# by agl's LRU cache, the VRAM eviction patch's free VRAM and evictions, the whole device's VRAM from sysfs) and its
# last shot in <run>/stops/. Child maps, age-free maps, the age switch ("!cheat 999"), adult maps.
#
#   scripts/direct_scene_cycle.sh <name> [NAME=VALUE ...]     (RX_BACKEND=direct HOS_DBG=strict,strictall by default;
#                                                              the arguments are added to the run's environment)
#   STOPS=N (N stops spread over the list), LOOPS=k (the selection k times), MAPS=<file> (default
#   work/scenecycle/maps.txt, made by scripts/scene_cycle_maps.py: game-derived, never committed), EXE=, BACKEND=frozen
#   RUN=timing (the GPU lock: a stop may lose the device). Summary: grep . <run>/stops.txt; <run>/cycle.txt the misses.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
name=${1:?name}; shift
maps=${MAPS:-work/scenecycle/maps.txt}
[ -f "$maps" ] || work/venv/bin/python -I scripts/scene_cycle_maps.py "$maps" >/dev/null
mapfile -t all < "$maps"
stops=${STOPS:-${#all[@]}}; loops=${LOOPS:-1}
pick=()
if (( stops >= ${#all[@]} )); then pick=("${all[@]}")
else for ((i = 0; i < stops; i++)); do pick+=("${all[$(( i * ${#all[@]} / stops ))]}"); done
  for a in "${all[@]}"; do [ "$a" = "!age" ] && { printf '%s\n' "${pick[@]}" | grep -qx '!age' || pick=("${pick[@]:0:stops/2}" "!age" "${pick[@]:stops/2}"); }; done
fi
seq=(); for ((l = 0; l < loops; l++)); do seq+=("${pick[@]}"); done
run_dir=$root/work/runs/$name
live=$root/work/scenecycle/$name.live; mkdir -p "$(dirname "$live")"; : > "$live"
log=$run_dir/log.txt
args=(HOS_INPUT_LIVE="$live" HOS_INPUT_EXCLUSIVE=1 HOS_INPUT_SCRIPT_BACKGROUND=1 HOS_LOG_TIME=1)
[ "${BACKEND:-direct}" = direct ] && args+=(RX_BACKEND=direct HOS_DBG=strict,strictall)
RUN=${RUN:-timing} OVERLAY=${OVERLAY:-work/overlay/chisel_kokiri} EXE=${EXE:-work/sa/build/hookshot} SHOT_EVERY=${SHOT_EVERY:-120} \
  scripts/run-vk.sh "$name" "${SECONDS_MAX:-3600}" -- "${args[@]}" "$@" &
run=$!
card=$(ls -d /sys/class/drm/card*/device 2>/dev/null | while read -r d; do [ -f "$d/mem_info_vram_used" ] && echo "$d" && break; done)
ready() { local n; n=$(grep -c '] scene: ready ' "$log" 2>/dev/null); echo "${n:-0}"; }
want=
ready_for() { local n; n=$(grep -c "] scene: ready ${want} " "$log" 2>/dev/null); echo "${n:-0}"; }
wait_ready() {   # <count before> <timeout s>
  local t=0
  while (( $(if [ -n "$want" ]; then ready_for; else ready; fi) <= $1 )); do
    kill -0 $run 2>/dev/null || return 2
    sleep 0.25; t=$((t + 1)); (( t > $2 * 4 )) && return 1
    (( t > 24 && t % 6 == 0 )) && echo "0:A/150" >> "$live"   # (a dialogue or an event holds the warp: A every 1.5 s)
  done
  return 0
}
blocks() { local n; n=$(grep -c '] direct: memory (MB)' "$log" 2>/dev/null); echo "${n:-0}"; }
note() {   # <stop index> <stop>: after the next 600-present block
  local b0 t=0; b0=$(blocks)
  while (( $(blocks) <= b0 )) && (( t < 80 )) && kill -0 $run 2>/dev/null; do sleep 0.25; t=$((t + 1)); done
  local mem img cache ev dev
  mem=$(grep -o 'heap [0-9]* (device-local, [0-9]*): driver usage [0-9]* of budget [0-9]*, VMA blocks [0-9]*' "$log" | tail -1 | sed -E 's/.*driver usage ([0-9]+) of budget ([0-9]+), VMA blocks ([0-9]+)/usage \1 budget \2 vma \3/')
  img=$(grep -oE 'direct: images: .*' "$log" | tail -1 | sed -E 's/.*; ([0-9]+) live .*\), ([0-9]+) MB \(assets: ([0-9]+) images, ([0-9]+) MB.*/live \1 \2MB store \3 \4MB/')
  cache=$(grep -oE "held only by textures in agl's LRU cache \(M6f\): [0-9]+ images, [0-9]+ MB" "$log" | tail -1 | sed -E 's/.*: ([0-9]+) images, ([0-9]+) MB/cache \1 \2MB/')
  ev=$(grep -oE 'VRAM eviction patch: free VRAM -?[0-9]+ MB .*' "$log" | tail -1 | sed -E 's/.*free VRAM (-?[0-9]+) MB.*cache ([0-9]+), evicted by the patch ([0-9]+).*/free \1MB cached \2 evicted \3/')
  dev=$([ -n "$card" ] && echo "device $(( $(cat "$card/mem_info_vram_used") >> 20 ))MB" || echo "")
  echo "stop $1 $2: $mem; $img; $cache; $ev; $dev" >> "$run_dir/stops.txt"
  local shot; shot=$(ls "$run_dir/shots"/vk_*.png 2>/dev/null | tail -1)
  mkdir -p "$run_dir/stops"; [ -n "$shot" ] && cp "$shot" "$run_dir/stops/$(printf %03d "$1")_${2%% *}.png"
}
sleep 30
i=0
for b in "${seq[@]}"; do
  kill -0 $run 2>/dev/null || { echo "cycle: the game is gone before stop $i ($b)" >> "$run_dir/cycle.txt"; break; }
  if [ "$b" = "!age" ]; then want=; n=$(ready); else want=${b%% *}; n=$(ready_for); fi
  send() { if [ "$b" = "!age" ]; then echo "!cheat 999" >> "$live"; else echo "!warp $b" >> "$live"; fi; }
  send
  if ! wait_ready "$n" 20; then
    echo "cycle: $b not ready in 20 s, sent again" >> "$run_dir/cycle.txt"; send
    if ! wait_ready "$n" 40; then echo "cycle: $b not ready" >> "$run_dir/cycle.txt"; i=$((i + 1)); continue; fi
  fi
  echo "0:RRIGHT/1000,1.1:LUP/2000,3.2:RLEFT/1000" >> "$live"
  sleep 4.4
  note "$i" "$b"
  i=$((i + 1))
done
kill $run 2>/dev/null; wait $run
journalctl -k -b --no-pager 2>/dev/null | grep -E 'ring .* timeout|GPU reset\(' | tail -2 || true
echo "cycle $name: $(grep -c . "$run_dir/stops.txt" 2>/dev/null) stops noted, $(grep -c 'not ready' "$run_dir/cycle.txt" 2>/dev/null) not ready, device lost $(grep -ci 'DEVICE_LOST\|device lost' "$log")"
