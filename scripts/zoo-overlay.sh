#!/usr/bin/env bash
# The overlay of an actor-zoo scene (zoo/<name>.toml): work/overlay/<name>, made by tools/dump/make_zoo.py from the user's
# dump when it is missing or older than its description or the tool. Prints the overlay's folder.
#   scripts/zoo-overlay.sh <name>
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
common=$(git -C "$here" rev-parse --path-format=absolute --git-common-dir); root=${common%/.git}
name=${1:?usage: scripts/zoo-overlay.sh <name>}
scene=$here/zoo/$name.toml; out=$root/work/overlay/$name
[ -f "$scene" ] || { echo "no scene $scene" >&2; exit 2; }
if [ ! -f "$out/.zoo" ] || [ "$scene" -nt "$out/.zoo" ] || [ "$here/tools/dump/make_zoo.py" -nt "$out/.zoo" ]; then
  "$root/work/venv/bin/python" -I "$here/tools/dump/make_zoo.py" "$scene" "$out" >&2
fi
echo "$out"
