# Changelog

All notable changes to this project are documented here.

## v1.25 (current)
- **Screen-off detection fix**: `is_screen_off()` now matches both `Asleep` and `Dozing` states — this was blocking IDLE-DEEP tier, the learning trainer, ZRAM compaction during idle, and fast-track MAINT→IDLE transitions
- Added `last_bg` tracking so warm-relaunch checks use real elapsed background time
- Reduced unnecessary disk writes by only marking scores dirty on meaningful events, not every tick
- Deterministic tie-breaking when choosing which apps to pin
- Decoupled foreground-count accumulation from score clamping
- Graduated `oom_adj` values across pinned apps by rank instead of a flat value
- Rate-limited logging for silent pin-write failures
- Converted proc file reads to raw syscalls for consistency and performance
- Learning model improvements: regularization, per-feature normalization, class-imbalance weighting, sample-age decay, accuracy logging
- Incremental save/compaction for the on-disk score file

**Known issue carried forward:** foreground-count tracking saturates near its cap for most apps, making one of the learning signals redundant for now — fix planned for a future version.

## v1.24 and earlier
- Learning subsystem wired up so learned weights actually influence scoring
- Fixed learning trainer to train on recent samples instead of oldest
- Exempted persistent system services (e.g. background sync services) from kill/retention scoring — these get auto-relaunched by Android regardless, so killing them was pointless and wasteful
- Debounced foreground-app detection to avoid flapping
- Exempt processes no longer counted against the pin budget
- Pin log now shows readable app names instead of just PIDs

## v1.00–v1.23
- Initial builds and incremental fixes establishing the core kill-tier system, retention scoring, and Magisk packaging
