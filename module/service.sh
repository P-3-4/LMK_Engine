#!/system/bin/sh
# LMK Engine — Boot Service

BIN=/system/bin/lmk_engine
LOG=/data/local/tmp/lmk_engine.log
PID_FILE=/data/local/tmp/lmk_engine.pid

mkdir -p /data/local/tmp

log() {
    echo "$(date '+%F %T') - $*" >> "$LOG"
}

wait_boot() {
    while [ "$(getprop sys.boot_completed)" != "1" ]; do
        sleep 2
    done
    sleep 5
}

# Checks the PID file directly instead of parsing --status text output.
# (--status formatting is owned by the C source and can change between
# versions — it already had drifted out of sync with the old grep here.)
is_running() {
    [ -f "$PID_FILE" ] || return 1
    PID=$(cat "$PID_FILE" 2>/dev/null)
    [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null
}

start_engine() {
    log "Starting LMK Engine..."
    "$BIN" --start >/dev/null 2>&1
}

# ── sanity check ─────────────────────────────
if [ ! -x "$BIN" ]; then
    log "LMK binary missing or not executable"
    exit 1
fi

wait_boot

if is_running; then
    log "LMK Engine already RUNNING"
else
    log "LMK Engine STOPPED — starting"
    start_engine
    sleep 2

    if is_running; then
        log "LMK Engine started successfully"
    else
        log "FAILED to start LMK Engine"
    fi
fi
