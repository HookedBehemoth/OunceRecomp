#!/usr/bin/env bash
# A test run of the standalone build on Linux (scripts/run-vk.ps1's counterpart): muted, headless unless WINDOW=1,
# screenshots every SHOT_EVERY presents, a frame-time CSV, stopped after SECONDS. Output in work/runs/<name>.
#
#   scripts/run-vk.sh <name> [seconds] [-- extra NAME=VALUE ...]
#   OVERLAY=work/overlay/<dir>   patched game files read first (HOS_FS_OVERLAY: the chiseled boot)
#   USER_FROM=<folder>           copied into the run's own user folder first (a save to start from)
#   EXE=<path>                   another build (default work/sa/build/hookshot; its caches are beside it)
#   WINDOW=1 SOUND=1 SHOT_EVERY=300
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
name=${1:?name}; seconds=${2:-90}; shift $(( $# >= 2 ? 2 : 1 )) || true
[ "${1:-}" = "--" ] && shift
out=$root/work/runs/$name
rm -rf "$out"; mkdir -p "$out/shots" "$out/user"
[ -n "${USER_FROM:-}" ] && cp -r "$USER_FROM"/. "$out/user/"
if pgrep -x hookshot >/dev/null; then echo "another hookshot runs"; exit 1; fi
romfs=$(grep -v '^#' "$root/work/dump.txt" | head -1)/romfs
env_args=(RX_BACKEND=vulkan RX_VK_SHOTS="$out/shots" RX_VK_SHOT_EVERY="${SHOT_EVERY:-300}"
          RX_VK_FRAME_LOG="$out/frames.csv" HOOKSHOT_NO_UPDATE_CHECK=1
          RX_VK_MESH_NOCULL=2c56dc1006942f74   # (RDNA2: that task shader's depth-grid walk hangs the GPU: docs/development.md)
          RX_VK_TASK_MESH_FALLBACK=${TASK_MESH_FALLBACK:-1})   # (RDNA2: native task shaders hang the async ring under faster timing: docs/native-vk.md)
[ "${WINDOW:-0}" = 1 ] || env_args+=(HOS_HEADLESS=1)
[ "${SOUND:-0}" = 1 ] || env_args+=(HOS_AUDIO=0)
[ -n "${OVERLAY:-}" ] && env_args+=(HOS_FS_OVERLAY="$(cd "$OVERLAY" && pwd)")
exe=$(realpath "${EXE:-$root/work/sa/build/hookshot}")
cd "$out"
t0=$(date +%s)
env "${env_args[@]}" "$@" "$exe" "$root/work/sa/image/image.bin" --romfs "$romfs" \
    --user "$out/user" > stdout.txt 2> log.txt &
pid=$!
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
    wait $pid 2>/dev/null || true; while pgrep -x hookshot >/dev/null; do sleep 0.5; done   # (gone before the next run)
    how="stopped after $seconds s"
else wait $pid && how="exited with 0" || how="exited with $?"; fi
echo "run '$name': $(( $(date +%s) - t0 )) s, $how, peak RSS $peak MB"
