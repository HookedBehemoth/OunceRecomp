# One heavy job on the machine at a time: work/heavy.lock. Sourced by the scripts that take it, after they set root.
# heavy_lock_or_exec takes the lock and runs the script again under it (with OUNCE_HEAVY_HELD=1), unless the caller says
# it holds it. A caller that holds the lock without saying so (flock work/heavy.lock scripts/relift.sh) would wait on
# itself for ever: that stops here, naming the fix.
heavy_lock=$root/work/heavy.lock

heavy_lock_or_exec() {
    [ -n "${OUNCE_HEAVY_HELD:-}" ] && return 0
    mkdir -p "$root/work"
    local real holders pid
    real=$(realpath -- "$heavy_lock" 2>/dev/null || echo "$heavy_lock")
    # (holders: lslocks marks a waiter's mode with '*')
    holders=" $(lslocks -n -o PID,MODE,PATH 2>/dev/null | awk -v p="$real" '$3 == p && $2 !~ /\*/ { printf "%s ", $1 }')"
    pid=$$
    while [ -n "$pid" ] && [ "$pid" -gt 1 ]; do
        case "$holders" in *" $pid "*)
            echo "$(basename "$0"): a caller (pid $pid) holds $heavy_lock already: run this script without flock (it takes" \
                 "the lock itself), or say so with OUNCE_HEAVY_HELD=1" >&2
            exit 2 ;;
        esac
        pid=$(awk '{ print $4 }' "/proc/$pid/stat" 2>/dev/null)
    done
    exec env OUNCE_HEAVY_HELD=1 flock "$heavy_lock" "$0" "$@"
}
