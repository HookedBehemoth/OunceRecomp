#!/usr/bin/env bash
# Keeps the build scripts/play.sh runs (the main checkout's work/sa/build) at its native-vk head: builds there and writes
# work/sa/build/BUILD.txt (short hash, date, subject) last. Exit 0 only when BUILD.txt names HEAD. Needs a clean native-vk
# checkout; one build at a time (flock on work/sa/build/.build.lock). Log: work/play/refresh.log. The build lifts the game
# first (ounce-lift, about 8 s) and takes every partition whose IR did not change from the object cache. A change the
# commit does not show (the hot list, the dump) needs scripts/relift.sh, which builds even when BUILD.txt names HEAD. Heavy
# work takes work/heavy.lock (one heavy job on the machine at a time) and runs in a memory-capped scope
# (HOOKSHOT_REFRESH_MEM, 18G).
#   scripts/play-refresh.sh [--install-hook]   --install-hook writes .git/hooks/post-merge (a refresh in the background after a merge onto native-vk)
set -uo pipefail
# (the main checkout, also when this copy of the script is run from an agent's worktree)
self=$(cd "$(dirname "$0")" && pwd)
common=$(git -C "$self" rev-parse --path-format=absolute --git-common-dir) || exit 1
root=${common%/.git}
play=$root/work/play; log=$play/refresh.log

if [ "${1:-}" = "--install-hook" ]; then
  hook=$common/hooks/post-merge
  cat > "$hook" <<'HOOK'
#!/bin/sh
# (local; installed by scripts/play-refresh.sh --install-hook; the hooks folder is shared by all worktrees)
top=$(git rev-parse --show-toplevel) || exit 0
[ "$top" = "$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")" ] || exit 0
[ "$(git rev-parse --abbrev-ref HEAD)" = native-vk ] || exit 0
[ -x "$top/scripts/play-refresh.sh" ] || exit 0
mkdir -p "$top/work/play"
setsid nohup "$top/scripts/play-refresh.sh" >> "$top/work/play/refresh.log" 2>&1 < /dev/null &
exit 0
HOOK
  chmod +x "$hook"; echo "installed $hook"; exit 0
fi

fail() { echo "play-refresh: $1; BUILD.txt untouched (see $log)"; exit 1; }
cd "$root" || exit 1
if [ -z "${HOOKSHOT_HEAVY_HELD:-}" ]; then   # (one heavy job at a time: the agents' and this one's; relift.sh holds it already)
  mkdir -p "$root/work"
  exec env HOOKSHOT_HEAVY_HELD=1 flock "$root/work/heavy.lock" "$0" "$@"
fi
capped() {   # (a memory-capped scope where systemd can make one, else plain)
  if systemd-run --user --scope -q true 2>/dev/null; then
    systemd-run --user --scope -q -p MemoryMax="${HOOKSHOT_REFRESH_MEM:-18G}" -p MemorySwapMax=0 "$@"
  else "$@"; fi
}
mkdir -p "$play"
bd=$root/work/sa/build; mkdir -p "$bd"
exec 9>"$bd/.build.lock"; flock 9
[ "$(git rev-parse --abbrev-ref HEAD)" = native-vk ] || fail "the main checkout is not on native-vk"
[ -z "$(git status --porcelain --untracked-files=no)" ] || fail "the main checkout has uncommitted changes"
head=$(git rev-parse --short HEAD)
game=games/oot   # (the game description the lift and the build take: games/oot/game.toml)
if [ "$(head -1 "$bd/BUILD.txt" 2>/dev/null)" = "$head" ]; then echo "play-refresh: work/sa/build already names $head"; exit 0; fi
rm -f "$bd/BUILD.txt"
start=$(date +%s)
{
  echo "== $(date '+%F %T') refresh to $head"
  HOOKSHOT_SPIRV_HEADERS=$root/work/deps/SPIRV-Headers HOOKSHOT_VMA_HEADERS=$root/work/deps/VulkanMemoryAllocator \
    capped work/venv/bin/python -I -u nx2/build.py --game "$game"
} > "$log" 2>&1 || fail "the build failed"
printf '%s\n%s %s\n' "$head" "$(date +%F)" "$(git log -1 --format=%s)" > "$bd/BUILD.txt.new" && mv -f "$bd/BUILD.txt.new" "$bd/BUILD.txt"
echo "play-refresh: work/sa/build is $head ($(( $(date +%s) - start )) s)" | tee -a "$log"
