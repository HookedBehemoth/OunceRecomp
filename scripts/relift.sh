#!/usr/bin/env bash
# The build again, by force, for the checkout this copy of the script is in (the main checkout or an agent's worktree):
# every build lifts the game first (ounce-lift, about 8 s; nx2/build.py), so this is a build that does not stop early
# because work/sa/build/BUILD.txt already names HEAD. Use it after a change the commit does not show: the dump, the
# toolchain.
#
#   scripts/relift.sh
#
# Stops when a game runs from this checkout's work/sa/build. In the main checkout on native-vk the build is
# scripts/play-refresh.sh's (it writes work/sa/build/BUILD.txt last), else nx2/build.py here. Log: work/play/relift.log in
# the main checkout, work/sa/relift.log in a worktree.
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
game=games/oot
start=$(date +%s)
echo "== $(date '+%F %T') relift of $root at $(git rev-parse --short HEAD), game $game" >> "$log"
if [ "$root" = "$main" ] && [ "$(git rev-parse --abbrev-ref HEAD)" = native-vk ]; then
    [ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "relift: the main checkout has uncommitted changes"; exit 1; }
    (exec 9>"$bd/.build.lock"; flock 9; rm -f "$bd/BUILD.txt")
    # (play-refresh.sh: the build, BUILD.txt last; it writes its own log)
    "$root/scripts/play-refresh.sh" >> "$log" 2>&1 || { echo "relift: the build failed (see $log and work/play/refresh.log)"; exit 1; }
else
    { exec 9>"$bd/.build.lock"; flock 9
      HOOKSHOT_SPIRV_HEADERS=$root/work/deps/SPIRV-Headers HOOKSHOT_VMA_HEADERS=$root/work/deps/VulkanMemoryAllocator \
          work/venv/bin/python -I -u nx2/build.py --game "$game"; } >> "$log" 2>&1 ||
        { echo "relift: the build failed (see $log)"; exit 1; }
fi
echo "relift: lift and build of $(git rev-parse --short HEAD) in $(( $(date +%s) - start )) s; log $log" | tee -a "$log"
