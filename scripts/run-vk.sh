#!/usr/bin/env bash
# A test run of the standalone build on Linux (scripts/run-vk.ps1's counterpart): muted, headless unless WINDOW=1,
# screenshots every SHOT_EVERY presents, a frame-time CSV, stopped after SECONDS. Output in work/runs/<name>.
#
#   scripts/run-vk.sh <name> [seconds] [-- extra NAME=VALUE ...]
#   OVERLAY=work/overlay/<dir>   patched game files read first (HOS_FS_OVERLAY: the chiseled boot)
#   ZOO=<scene>                  the actor-zoo scene zoo/<scene>.toml as the overlay (made on demand: scripts/zoo-overlay.sh)
#   USER_FROM=<folder>           copied into the run's own user folder first (a save to start from)
#   EXE=<path>                   another build (default work/sa/build/hookshot; its caches are beside it)
#   WINDOW=1 SOUND=1 SHOT_EVERY=300
#   RUN=timing|check             the script takes the GPU lock itself; do not wrap it in flock then:
#     timing  exclusive (flock -x work/run.lock): fps, launch time, anything that may hang the GPU (a reset kills
#             every run beside it)
#     check   shared (flock -s work/run.lock) plus one of RUN_SLOTS (default 1) slots: counters, screenshots,
#             censuses. Runs on a private reflink copy of the build's folder, so the caches beside the executable
#             are not raced (cache updates made by the run are not kept)
#   (RUN unset: as before; the caller holds `flock work/run.lock`, and no other hookshot may run)
set -euo pipefail
# (the whole script is read before any of it runs, so editing this file while runs use it cannot break them: bash
# reads a script as it goes, and an edit made under a running copy shifted it into a syntax error, orphaning its game)
{
root=$(cd "$(dirname "$0")/.." && pwd)
name=${1:?name}; seconds=${2:-90}; shift $(( $# >= 2 ? 2 : 1 )) || true
[ "${1:-}" = "--" ] && shift
mode=${RUN:-}
case "$mode" in
  "") ;;
  # (work/run.gate makes a waiting timing run block new check runs, so back-to-back checks cannot starve it: a timing
  # run holds the gate until it has the lock; a check run passes the gate only to take its shared lock, and does not
  # hold the shared lock while it waits for a slot.)
  timing)
    exec 7>"$root/work/run.gate"; flock -x 7
    exec 8>"$root/work/run.lock"; flock -x 8
    exec 7>&- ;;
  check)
    # (one at a time by default: with layer 0 each game holds ~10 GB of guest memory in a memfd shared with the GPU,
    # which RSS does not show; two runs and the desktop ran a 31 GB machine out of memory, and systemd-oomd killed the
    # terminal. RUN_SLOTS=2 where there is room.)
    slots=${RUN_SLOTS:-1}; got=
    while [ -z "$got" ]; do
      exec 7>"$root/work/run.gate"; flock -x 7
      exec 8>"$root/work/run.lock"; flock -s 8
      exec 7>&-
      for ((i = 0; i < slots; i++)); do
        exec 9>"$root/work/run.slot.$i"
        if flock -n 9; then got=$i; break; fi
        exec 9>&-
      done
      [ -n "$got" ] || { exec 8>&-; sleep 1; }
    done ;;
  *) echo "RUN=$mode: timing or check"; exit 2 ;;
esac
# (RUN_MEM_GB, default 14: the memory a run needs free before it starts, waited for; 0 skips the check)
need=${RUN_MEM_GB:-14}; said=
while (( need > 0 )) && (( $(awk '/MemAvailable/ {print int($2 / 1048576)}' /proc/meminfo) < need )); do
  [ -n "$said" ] || { echo "run '$name': waiting for ${need} GB of free memory"; said=1; }
  sleep 2
done
out=$root/work/runs/$name
rm -rf "$out"; mkdir -p "$out/shots" "$out/user"
[ -n "${USER_FROM:-}" ] && cp -r "$USER_FROM"/. "$out/user/"
if [ "$mode" != check ] && pgrep -x hookshot >/dev/null; then echo "another hookshot runs"; exit 1; fi
romfs=$(grep -v '^#' "$root/work/dump.txt" | head -1)/romfs
env_args=(RX_VK_SHOTS="$out/shots" RX_VK_SHOT_EVERY="${SHOT_EVERY:-300}")
[ "${WINDOW:-0}" = 1 ] || env_args+=(HOS_HEADLESS=1)
[ "${SOUND:-0}" = 1 ] || env_args+=(HOS_AUDIO=0)
[ -n "${ZOO:-}" ] && OVERLAY=$("$root/scripts/zoo-overlay.sh" "$ZOO")
[ -n "${OVERLAY:-}" ] && env_args+=(HOS_FS_OVERLAY="$(cd "$OVERLAY" && pwd)")
# (an actor-zoo overlay's live.txt: host commands the scene needs, e.g. its time of day)
[ -n "${OVERLAY:-}" ] && [ -f "$OVERLAY/live.txt" ] && env_args+=(HOS_INPUT_LIVE="$(cd "$OVERLAY" && pwd)/live.txt")
exe=$(realpath "${EXE:-$root/work/sa/build/hookshot}")
if [ "$mode" = check ]; then   # (a private copy of the build's files: the executable, the renderer, the caches)
  mkdir -p "$out/bin"
  find "$(dirname "$exe")" -maxdepth 1 -type f ! -name '*.lock' -exec cp --reflink=auto -p {} "$out/bin/" \;
  exe=$out/bin/$(basename "$exe")
fi
cd "$out"
t0=$(date +%s)
env "${env_args[@]}" "$@" "$exe" "$root/work/sa/image/image.bin" --romfs "$romfs" \
    --user "$out/user" > stdout.txt 2> log.txt &
pid=$!
# (the game dies with this script: killed or interrupted, it would otherwise keep running, holding the GPU lock and a
# slot through the descriptors it inherited)
trap 'kill -9 $pid 2>/dev/null || true' EXIT
trap 'exit 130' INT TERM HUP
peak=0
for ((t = 0; t < seconds; t++)); do
    kill -0 $pid 2>/dev/null || break
    rss=$(awk '/VmRSS/ {print int($2/1024)}' /proc/$pid/status 2>/dev/null || echo 0)
    (( rss > peak )) && peak=$rss
    # (timeline.txt: seconds since the start, the last "<n> presents" the renderer logged, RSS in MB)
    pr=$(grep -o '\] vk: [0-9]* presents' log.txt 2>/dev/null | tail -1 | grep -o '[0-9]*' || true)
    echo "$t ${pr:-0} $rss" >> timeline.txt
    sleep 1
done
if kill -0 $pid 2>/dev/null; then kill $pid; sleep 2; kill -9 $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
    # (gone before the next run; with RUN=check other runs' hookshots are not waited for)
    if [ "$mode" = check ]; then while kill -0 $pid 2>/dev/null; do sleep 0.5; done
    else while pgrep -x hookshot >/dev/null; do sleep 0.5; done; fi
    how="stopped after $seconds s"
else wait $pid && how="exited with 0" || how="exited with $?"; fi
if [ "$mode" = check ]; then
  [ -f "$out/bin/hookshot_frames.csv" ] && cp "$out/bin/hookshot_frames.csv" "$out/frames_trace.csv"
  rm -rf "$out/bin"
fi
echo "run '$name': $(( $(date +%s) - t0 )) s, $how, peak RSS $peak MB${mode:+ (RUN=$mode)}"
exit
}
