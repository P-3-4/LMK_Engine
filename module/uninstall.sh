#!/system/bin/sh
# ============================================================
#  LMK Engine — Uninstall script
#  Magisk runs this when the user removes the module.
# ============================================================

PID_FILE=/data/local/tmp/lmk_engine.pid
SWAP_FILE=/data/lmk_swap

# Stop daemon
if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE" 2>/dev/null)
    [ -n "$PID" ] && kill "$PID" 2>/dev/null
    rm -f "$PID_FILE"
fi
pkill -f lmk_engine 2>/dev/null

# Disable swap if we created it
if swapon --show 2>/dev/null | grep -q "$SWAP_FILE"; then
    swapoff "$SWAP_FILE" 2>/dev/null
fi
[ -f "$SWAP_FILE" ] && rm -f "$SWAP_FILE"

echo "LMK Engine uninstalled cleanly."
