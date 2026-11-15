#!/usr/bin/env bash
# Replays an input recording (HOS_INPUT_RECORD, docs/testing.md "Record and replay a session") through scripts/run-vk.sh:
# headless and muted, from the recording's copy of the save folder, with the settings its header names. The run is
# work/runs/replay-<name>; run-vk.sh's variables (EXE, RUN, SHOT_EVERY, ...) pass through.
#
#   scripts/replay.sh <name> <seconds> [-- NAME=VALUE ...]      <name>: work/rec/<name>, or a recording's folder
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
name=${1:?name}; seconds=${2:?seconds}; shift 2; [ "${1:-}" = "--" ] && shift
dir=$name; [ -d "$dir" ] || dir=$root/work/rec/$name
dir=$(cd "$dir" && pwd)
[ -f "$dir/input.rec" ] || { echo "no $dir/input.rec"; exit 2; }
read -r -a settings <<< "$(head -1 "$dir/input.rec" | grep -oE 'HOS_[A-Z_]+=[^ ]+' | tr '\n' ' ')"
USER_FROM="$dir/user" exec "$root/scripts/run-vk.sh" "replay-$(basename "$dir")" "$seconds" -- \
    "${settings[@]}" HOS_INPUT_REPLAY="$dir/input.rec" "$@"
