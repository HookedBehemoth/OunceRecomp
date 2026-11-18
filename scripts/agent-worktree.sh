#!/usr/bin/env bash
# One git worktree and build folder per agent, so parallel work does not build each other's uncommitted
# changes or splice into the same lifted code.
#
#   scripts/agent-worktree.sh new <name> [base]   work/wt/<name> on branch agent/<name> (from base, default the
#                                                 main checkout's branch). Linked to the main work/: everything in
#                                                 it but sa/ and wt/ (dump, runs, overlays, venv, deps, locks,
#                                                 objcache, builder-cache, hot list). Its own: work/sa. Copied:
#                                                 work/sa/build, a reflink of the main one's (shared extents on
#                                                 btrfs: next to nothing until rebuilt). Its first scripts/build.sh
#                                                 makes work/sa/meson (the renderer's build), gen/ and image/; objects
#                                                 are shared through work/builder-cache (content-keyed), so a build
#                                                 with nothing new to compile takes about 10 s.
#   scripts/agent-worktree.sh land <name>         rebase agent/<name> onto the main branch and fast-forward it
#   scripts/agent-worktree.sh drop <name>         remove the worktree and its branch, if the branch has landed
#
# Build inside the worktree with scripts/build.sh (docs/development.md, "Work in an agent worktree"); run with
# EXE="work/sa/build/<the game's exe>" from it.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cmd=${1:?new|land|drop}; name=${2:?name}
wt=$root/work/wt/$name
branch=agent/$name
main=$(git -C "$root" rev-parse --abbrev-ref HEAD)
case "$cmd" in
new)
    base=${3:-$main}
    [ -e "$wt" ] && { echo "$wt exists"; exit 1; }
    git -C "$root" worktree add -q -b "$branch" "$wt" "$base"
    mkdir -p "$wt/work/sa" "$root/work/builder-cache"   # (the builder's object cache: shared, like objcache)
    for e in "$root"/work/* "$root"/work/.[!.]*; do
        [ -e "$e" ] || continue
        b=$(basename "$e")
        case "$b" in sa|wt) continue ;; esac
        ln -s "$e" "$wt/work/$b"
    done
    [ ! -d "$root/work/sa/build" ] || cp -a --reflink=auto "$root/work/sa/build" "$wt/work/sa/build"
    rm -f "$wt/work/sa/build/"*.lock
    echo "worktree $wt on $branch (from $base); build there, run with EXE=\"$wt/work/sa/build/<the game's exe>\""
    ;;
land)
    git -C "$wt" diff --quiet && git -C "$wt" diff --cached --quiet || { echo "$wt has uncommitted changes"; exit 1; }
    git -C "$wt" rebase -q "$main"
    git -C "$root" merge -q --ff-only "$branch"
    echo "landed $branch on $main: $(git -C "$root" log --oneline -1)"
    ;;
drop)
    git -C "$root" merge-base --is-ancestor "$branch" "$main" || { echo "$branch has not landed on $main"; exit 1; }
    git -C "$root" worktree remove --force "$wt"
    git -C "$root" branch -d "$branch"
    ;;
*) echo "new|land|drop"; exit 2 ;;
esac
