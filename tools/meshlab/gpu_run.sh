#!/usr/bin/env bash
# One GPU run of the harness under the game's GPU lock (scripts/run-vk.sh's work/run.lock, exclusive), with the hang rules:
# it starts only when the last reset of this boot is 3 minutes old and vulkaninfo works; after a run that lost the device it
# keeps the lock until the reset is 3 minutes old again (no other run starts during the recovery) and saves the kernel log.
#   tools/meshlab/gpu_run.sh <output file> <meshlab args...>   (MESHLAB=<another build of the harness>, MESHLAB_TIMEOUT=<seconds>)
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
lock=$(readlink -f "$root/work/runs")/../run.lock   # (the main checkout's work/: the lock every worktree shares)
out=${1:?output}; shift
last_reset() { journalctl -k -b --no-pager | grep -E 'GPU reset\([0-9]+\) succeeded' | tail -1; }
reset_age() {   # (seconds since the last reset line of this boot; large when none)
    local l
    l=$(last_reset)
    [ -z "$l" ] && { echo 99999; return; }
    echo $(( $(date +%s) - $(date -d "$(echo "$l" | awk '{print $1, $2, $3}')" +%s) ))
}
exec 8>"$lock"
flock -x 8
while (( $(reset_age) < 180 )); do sleep 5; done
vulkaninfo --summary > /dev/null 2>&1 || { echo "vulkaninfo fails: not run" | tee "$out"; exit 4; }
before=$(last_reset)
{ date; echo "meshlab $*"; } > "$out"
# (M6h: a timing series is a result only on a quiet GPU: no game process while the lock is held)
games=$(pgrep -x hookshot | tr '\n' ' ')
if [ -n "$games" ]; then echo "GPU NOT QUIET: hookshot running ($games)" >> "$out"; else echo "quiet GPU: no game process" >> "$out"; fi
timeout "${MESHLAB_TIMEOUT:-300}" "${MESHLAB:-$root/work/meshlab/meshlab}" "$@" 2>&1 | grep -v '^/tmp\|making the mesh stage' >> "$out"
rc=${PIPESTATUS[0]}
sleep 3
if [ "$(last_reset)" != "$before" ] || [ "$rc" = 3 ]; then
    for _ in $(seq 1 60); do [ "$(last_reset)" != "$before" ] && break; sleep 2; done
    echo "DEVICE LOST: $(last_reset)" >> "$out"
    journalctl -k -b --no-pager --since "-180s" > "$out.kernel.txt"
    while (( $(reset_age) < 180 )); do sleep 5; done
    if vulkaninfo --summary > /dev/null 2>&1; then echo "recovered (vulkaninfo works) at $(date)" >> "$out"; else echo "NOT RECOVERED" >> "$out"; fi
fi
cat "$out"
exit "$rc"
