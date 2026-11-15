#!/usr/bin/env bash
# A new lift of the game and the build on it, for the checkout this copy of the script is in (the main checkout or an agent's
# worktree). Needed after a change to the lift (nx2/lift.py, nx2/lift2/discover.py, the lift-time patch sources); a change
# to the lifter itself needs only a build (the plan holds instructions, not lifted code; scripts/play-refresh.sh lifts again
# by itself when the lift's inputs changed). About 1.5 minutes of lift and 2-5 minutes of build here (6 jobs; the lifted
# partitions come from the object cache when their inputs are unchanged).
#
#   scripts/relift.sh
#
# Stops when a game runs from this checkout's work/sa/build. The hot set: work/hot_funcs_play.txt, else work/hot_funcs.txt
# (one f_<address> a line, from game profiles; it stays in work/): its functions' partitions are built at -O2
# (nx2/lift2/game.py). In the main checkout on native-vk the build is
# scripts/play-refresh.sh's (it writes work/sa/build/BUILD.txt last), else nx2/build.py here. Log: work/play/relift.log in the
# main checkout, work/sa/relift.log in a worktree (its work/play is the main checkout's).
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
common=$(git -C "$root" rev-parse --path-format=absolute --git-common-dir) || exit 1
main=${common%/.git}
bd=$root/work/sa/build
if [ "$root" = "$main" ]; then log=$root/work/play/relift.log; else log=$root/work/sa/relift.log; fi
mkdir -p "$(dirname "$log")" "$bd"
cd "$root" || exit 1
if [ -z "${HOOKSHOT_HEAVY_HELD:-}" ]; then   # (one heavy job on the machine at a time: work/heavy.lock; play-refresh.sh inherits it)
  mkdir -p "$root/work"
  exec env HOOKSHOT_HEAVY_HELD=1 flock "$root/work/heavy.lock" "$0" "$@"
fi

# (a game started from this build: play.sh and run-vk.sh name its path; run-vk.sh's check runs use a copy elsewhere)
if pgrep -af "$bd/hookshot( |$)" > /dev/null; then
    echo "relift: a game runs from $bd: not started"; pgrep -af "$bd/hookshot( |$)"; exit 1
fi
hot=
for f in "$root/work/hot_funcs_play.txt" "$root/work/hot_funcs.txt"; do [ -f "$f" ] && { hot=$f; break; }; done
play=
if [ "$root" = "$main" ] && [ "$(git rev-parse --abbrev-ref HEAD)" = native-vk ]; then
    [ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "relift: the main checkout has uncommitted changes"; exit 1; }
    play=1
fi

start=$(date +%s)
{
    echo "== $(date '+%F %T') relift of $root at $(git rev-parse --short HEAD)${hot:+, hot set $hot ($(wc -l < "$hot") functions)}"
    # (the lift under the build's lock: play.sh waits on it, and a build cannot start on a half-written lift)
    (
        exec 9>"$bd/.build.lock"; flock 9
        rm -f "$bd/BUILD.txt"
        work/venv/bin/python -I -u nx2/lift.py --standalone --write
    ) || { echo "relift: the lift failed"; exit 1; }
    echo "lift: $(( $(date +%s) - start )) s"
} >> "$log" 2>&1 || { echo "relift: the lift failed (see $log)"; exit 1; }

b0=$(date +%s)
if [ -n "$play" ]; then
    # (play-refresh.sh: the build, BUILD.txt last; it writes its own log)
    "$root/scripts/play-refresh.sh" >> "$log" 2>&1 || { echo "relift: the build failed (see $log and work/play/refresh.log)"; exit 1; }
else
    { exec 9>"$bd/.build.lock"; flock 9
      HOOKSHOT_SPIRV_HEADERS=$root/work/deps/SPIRV-Headers HOOKSHOT_VMA_HEADERS=$root/work/deps/VulkanMemoryAllocator \
          work/venv/bin/python -I -u nx2/build.py --standalone --jobs "${HOOKSHOT_JOBS:-12}"; } >> "$log" 2>&1 ||
        { echo "relift: the build failed (see $log)"; exit 1; }
fi
echo "relift: lift and build of $(git rev-parse --short HEAD) in $(( $(date +%s) - start )) s (build $(( $(date +%s) - b0 )) s); log $log" | tee -a "$log"
