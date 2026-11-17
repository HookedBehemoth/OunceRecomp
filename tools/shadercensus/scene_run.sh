#!/usr/bin/env bash
# One game run for the shader census (scripts/run-vk.sh) under the GPU lock, with these hang rules:
# it takes the gate and the lock exclusively, starts only when the last reset of this boot is 3 minutes old and vulkaninfo
# works, and after a run that lost the device saves the kernel log into the run's folder and keeps the lock until the reset is
# 3 minutes old again.
#   OVERLAY=work/overlay/<scene> EXE=<build> tools/shadercensus/scene_run.sh <run name> <seconds> -- NAME=VALUE...
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
main=$(readlink -f "$root/work/runs")/..
name=${1:?name}; seconds=${2:?seconds}; shift 2; [ "${1:-}" = "--" ] && shift
last_reset() { journalctl -k -b --no-pager | grep -E 'GPU reset\([0-9]+\) succeeded' | tail -1; }
reset_age() {
    local l
    l=$(last_reset)
    [ -z "$l" ] && { echo 99999; return; }
    echo $(( $(date +%s) - $(date -d "$(echo "$l" | awk '{print $1, $2, $3}')" +%s) ))
}
exec 7>"$main/run.gate"; flock -x 7
exec 8>"$main/run.lock"; flock -x 8
exec 7>&-
while (( $(reset_age) < 180 )); do sleep 5; done
vulkaninfo --summary > /dev/null 2>&1 || { echo "vulkaninfo fails: not run"; exit 4; }
before=$(last_reset)
echo "run $name: started $(date)"
"$root/scripts/run-vk.sh" "$name" "$seconds" -- "$@"
rc=$?
sleep 3
out=$main/runs/$name
if [ "$(last_reset)" != "$before" ] || grep -qi "device lost\|VK_ERROR_DEVICE_LOST" "$out/log.txt" 2>/dev/null; then
    for _ in $(seq 1 60); do [ "$(last_reset)" != "$before" ] && break; sleep 2; done
    echo "DEVICE LOST: $(last_reset)" | tee -a "$out/lost.txt"
    journalctl -k -b --no-pager > "$out/kernel.txt"
    while (( $(reset_age) < 180 )); do sleep 5; done
    vulkaninfo --summary > /dev/null 2>&1 && echo "recovered at $(date)" | tee -a "$out/lost.txt" || echo "NOT RECOVERED" | tee -a "$out/lost.txt"
fi
echo "run $name: ended $(date) rc $rc"
exit $rc
