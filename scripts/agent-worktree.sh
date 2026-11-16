#!/usr/bin/env bash
# One git worktree and build folder per agent, so parallel work does not build each other's uncommitted
# changes or splice into the same lifted code.
#
#   scripts/agent-worktree.sh new <name> [base]   work/wt/<name> on branch agent/<name> (from base, default the
#                                                 main checkout's branch). Its work/ links to the main work/
#                                                 (dump, runs, overlays, venv, locks, objcache) except sa/, which is
#                                                 its own: its build lifts into it (ounce-lift) and builds into
#                                                 work/sa/build, a reflink copy of the main one's (instant on btrfs).
#                                                 Objects are shared through work/objcache (content-keyed), so a
#                                                 host-only change rebuilds in ~40 s.
#   scripts/agent-worktree.sh land <name>         rebase agent/<name> onto the main branch and fast-forward it
#   scripts/agent-worktree.sh drop <name>         remove the worktree and its branch, if the branch has landed
#
# Build inside the worktree as in docs/development.md ("Work in an agent worktree"), run with EXE=work/sa/build/hookshot from it.
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
    mkdir -p "$wt/work/sa"
    for e in "$root"/work/* "$root"/work/.[!.]*; do
        [ -e "$e" ] || continue
        b=$(basename "$e")
        case "$b" in sa|wt) continue ;; esac
        ln -s "$e" "$wt/work/$b"
    done
    [ ! -d "$root/work/sa/build" ] || cp -a --reflink=auto "$root/work/sa/build" "$wt/work/sa/build"
    rm -f "$wt/work/sa/build/"*.lock
    echo "worktree $wt on $branch (from $base); build there, run with EXE=$wt/work/sa/build/hookshot"
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
