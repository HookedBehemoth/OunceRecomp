#!/usr/bin/env bash
# scripts/rgp_capture.sh <name> <overlay> [at s, default 40]: one RUN=timing run with RADV's RGP trace armed by a trigger file;
# the .rgp RADV writes to /tmp is moved to work/proto/rgp/ (game-derived: stays on this PC)
cd "$(dirname "$0")/.."
name=${1:?name}; ov=${2:?overlay}; at=${3:-40}
trig=$PWD/work/proto/rgp/$name.trigger; rm -f "$trig"
before=$(ls /tmp/*.rgp 2>/dev/null | wc -l)
RUN=timing EXE=work/sa/build/hookshot OVERLAY=$ov SHOT_EVERY=600 scripts/run-vk.sh "$name" $((at + 20)) -- \
  MESA_VK_TRACE=rgp MESA_VK_TRACE_TRIGGER="$trig" ${EXTRA:-} HOS_INPUT_SCRIPT=16:LUP/12000 HOS_INPUT_EXCLUSIVE=1 &
run=$!
# (the run waits for the exclusive lock first: arm the trigger <at> s after the game started)
while ! pgrep -f "work/runs/$name/" >/dev/null && ! [ -s "work/runs/$name/log.txt" ]; do sleep 1; done
sleep "$at"; touch "$trig"
wait $run
for f in $(ls -t /tmp/*.rgp 2>/dev/null | head -n $(( $(ls /tmp/*.rgp 2>/dev/null | wc -l) - before ))); do mv "$f" work/proto/rgp/; done
ls -la work/proto/rgp/*.rgp 2>/dev/null
grep -iE 'rgp|sqtt|thread trace|trace' work/runs/$name/log.txt | head -5
