# The game's executable, from its description (games/oot/game.toml's exe): sourced by the scripts, after they set
# root. GAME_EXE is its name (it may hold spaces: quote it), game_exe the build's path in work/sa/build, game_running
# whether one runs (its process name, cut to 15 characters by the kernel).
game_toml=${GAME_DIR:-$root/games/oot}/game.toml
GAME_EXE=$(sed -n 's/^exe = "\(.*\)"$/\1/p' "$game_toml")
[ -n "$GAME_EXE" ] || { echo "no exe in $game_toml" >&2; exit 2; }
game_exe=$root/work/sa/build/$GAME_EXE
game_running() { pgrep -x -- "${GAME_EXE:0:15}" > /dev/null; }
