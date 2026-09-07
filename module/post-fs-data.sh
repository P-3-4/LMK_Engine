#!/system/bin/sh
# Relax kernel lmkd so lmk_engine works better 
MODDIR=${0%/*}

setprop ro.lmk.thrashing_limit 200
setprop ro.lmk.thrashing_limit_decay 30
setprop ro.lmk.swap_free_low_percentage 2
setprop ro.lmk.use_minfree_levels false

setprop ro.lmk.psi_partial_stall_ms 200
setprop ro.lmk.psi_complete_stall_ms 700

echo 150 > /proc/sys/vm/vfs_cache_pressure 2>/dev/null