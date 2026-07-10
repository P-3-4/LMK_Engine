#!/system/bin/sh
# ============================================================
#  LMK Engine — Magisk Module Installer  (customize.sh)
#  Author : P34   |   Version : 1.25
# ============================================================

SKIPUNZIP=1   # We handle extraction ourselves

# ── Banner ──────────────────────────────────────────────────
ui_print ""
ui_print "╔══════════════════════════════════════╗"
ui_print "║          LMK Engine 1.25 by P34       ║"
ui_print "║     A module to boost smoothness      ║"
ui_print "║          on low ram devices           ║"
ui_print "╚══════════════════════════════════════╝"
ui_print ""

# ── Arch check ──────────────────────────────────────────────
ui_print "- Checking architecture…"
case "$ARCH" in
  arm64)
    ui_print "  ✓ arm64 detected (MT6768 compatible)"
    ;;
  arm)
    ui_print "  ✓ arm (32-bit) detected"
    ;;
  *)
    ui_print "! Unsupported architecture: $ARCH"
    abort "   LMK Engine requires arm/arm64."
    ;;
esac

# ── Extract module files ─────────────────────────────────────
# NOTE: SKIPUNZIP=1 means Magisk extracts NOTHING automatically —
# every file the module needs at runtime, including uninstall.sh,
# must be copied to $MODPATH explicitly right here. (uninstall.sh
# was missing from this list for a long time, which silently meant
# Magisk never ran it on module removal — the daemon and the swap
# file were never being cleaned up.)
ui_print "- Extracting module files…"
unzip -o "$ZIPFILE" 'system/*'      -d "$MODPATH" >&2
unzip -o "$ZIPFILE" 'service.sh'    -d "$MODPATH" >&2
unzip -o "$ZIPFILE" 'module.prop'   -d "$MODPATH" >&2
unzip -o "$ZIPFILE" 'uninstall.sh'  -d "$MODPATH" >&2

# ── Verify binary ────────────────────────────────────────────
BIN="$MODPATH/system/bin/lmk_engine"
if [ ! -f "$BIN" ] || [ ! -s "$BIN" ]; then
    ui_print ""
    ui_print "! Binary not found or empty at system/bin/lmk_engine"
    ui_print "  Compile it, then reflash the module."
    ui_print ""
    # Install anyway — service.sh will detect missing binary safely
else
    ui_print "  ✓ Binary OK"
fi

# ── Permissions ──────────────────────────────────────────────
ui_print "- Setting permissions…"
set_perm "$MODPATH/system/bin/lmk_engine" root root 0755 \
    u:object_r:system_file:s0
chmod +x "$MODPATH/service.sh"
chmod +x "$MODPATH/uninstall.sh" 2>/dev/null

# ── Disable stock lmkd? ──────────────────────────────────────
#  On AOSP GSI, stock lmkd is managed by init. We don't stop it
#  here because our daemon works alongside it; stopping lmkd
#  requires vendored init changes that vary per device.
ui_print "- Note: LMK Engine coexists with stock lmkd."
ui_print "  It provides extra protection on top of the kernel LMK."

# ── Done ─────────────────────────────────────────────────────
ui_print ""
ui_print "✓ LMK Engine installed!"
ui_print "  Reboot to activate."
ui_print ""
ui_print "  Log : /data/local/tmp/lmk_engine.log"
ui_print "  PID : /data/local/tmp/lmk_engine.pid"
ui_print ""
