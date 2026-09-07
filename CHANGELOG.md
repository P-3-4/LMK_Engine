# Changelog

All notable changes to this project are documented here.

## v1.26 (current)
- **Reworked App Pin Logic**: Pinning switched from `oom_score_adj` to `cgroup v2 memory.low`**: the old mechanism was confirmed unwinnable — AMS overwrites it in 1-2s, 0% survival on-device — so it's gone, along with the `AI_Swap` learned-weight model that was tuning it
- **Improved App Rank Logic**: Consolidated the six-tier rank system down to four and added dwell timers for different ranks
- **Improved App Classification Logic**: Added authoritative package classification via `dumpsys package` which classified the apps when the device goes idle.
- Switched score persistence from full-table rewrites to a journal — changed entries get appended instead of rewriting the whole file, with a full consolidation only every 400 entries or on a structural change
- **Added CGSqueeze Logic**: The Daemon now squeezes apps to reduce their consumed space without killing them. Pinned apps can now also be squeezed via `memory.high` to release RAM 
- kswapd is now pinned to little clusters under normal pressure, released as ZRAM nears critical
- ZRAM amount is now configured to be 50 percent of physical RAM size to reduce possible CPU overhead from continuous compression and decompression [and also because the Daemon has become so fking efficient even with little ZRAM]
- Configured The Daemon to treat mostly full ZRAM as normal working conditions — the device runs 85-95%+ full as normal operation.
- Fixed stale pin state surviving a full app death instead of clearing
- Fixed a demote-then-instantly-repromote flapping loop at the tier capacity boundary [apps going back and forth continuously between ranks]
- Fixed tier-cap slot counting per-process instead of per-package [PIDs change if an app is killed, so now the daemon detects the package name instead]
- Added ZRAM Flush - clears up ZRAM after every 12 hours
- Fixed foreground detection crediting usage to any app holding a foreground service, not just what's actually on screen [it now uses three layers of confirmation to detect if an app is actually foreground, not just by the oom-adj number]
- Added External Kill in logs to catch external kills outside the daemon.
- Also added vanish detection in case any app disappears from rank without any kill logs
- Fixed a leftover hardcoded rule blocking an entire package namespace from ever counting as foreground
- Fixed two protect-window discounts stacking silently, shrinking a multi-hour window down to ~8% of intended
- Replaced the flat squeeze-target cutoff with a graduated curve
- Added an independent low-RAM escalation path so a real emergency can't slip through when ZRAM% alone looks fine
- Removed a ZRAM-cycle pre-kill path that could never reach its own target and was killing real apps trying to
- Added CPU tuning based on available RAM headroom, a better predictor than ZRAM% alone
- Added detection for deaths that leave no trace from any source
- Restart counts now reflect kills from every source, not just this daemon's own
- Fixed a substring bug fabricating/misattributing kill records when one package name matched inside another
- Fixed squeeze-release jank on reopening a previously-squeezed app
- Fixed a stale timer that could produce a false "not worth it" verdict on ZRAM cleanup
- Fixed a PID-reuse gap in cgroup path caching
- Atomic writes for on-disk state so a crash mid-save can't corrupt it
- Added a scheduled full-flush sweep — every 12h+ (after 30 min continuous screen-off), kills everything except never-kill/exempt apps, bypassing pin/protect-window/cooldown entirely, as a deliberate clean slate rather than a pressure response
- Fixed that same classification table silently evicting confirmed entries first, which forced repeatedly re-running the classify pipeline for the same answer on always-resident processes
- Switched `score_lookup()` from a linear scan to a hash-indexed lookup
- Added `lmk_engine --explain <pkg>` — dumps everything the daemon knows about one app (score, category, restart history, exemption status) without a manual log dig
- Added a per-process kernel reclaim step (`echo all > /proc/pid/reclaim`) as a new middle tier between squeeze and kill — pushes a backgrounded app's pages into ZRAM without killing it (~20-25% RSS reduction confirmed on-device), rate-limited per app
- Fixed that same reclaim path fighting the daemon's own ZRAM-pressure relief — it was repeatedly sparing one long-lived process via reclaim instead of ever actually killing it, feeding ZRAM that can't be swapped back out on this device; now skipped once ZRAM is already tight
- Switched the PSI stall check from pure timer-polling to an event-driven trigger, so a real stall wakes the daemon immediately instead of waiting for the next tick
- Consolidated the kill-mitigation logic (reclaim + ROI check) that used to live only inside the daemon's own opportunistic kill path into one shared check used at every kill site — most actual kill volume was going through paths that never got a chance to try mitigation first

A lot of changes 😏
Details in `docs/ARCHITECTURE.md`.

## v1.25
- **Screen-off detection fix**: `is_screen_off()` now matches both `Asleep` and `Dozing` states — this was blocking idle-tier processing, ZRAM compaction during idle, and fast maintenance→idle transitions
- Added background-time tracking so warm-relaunch checks use real elapsed background time
- Reduced unnecessary disk writes by only marking scores dirty on meaningful events, not every tick
- Deterministic tie-breaking when choosing which apps to pin
- Decoupled foreground-count accumulation from score clamping
- Rate-limited logging for silent pin-write failures
- Converted proc file reads to raw syscalls for consistency and performance
- Incremental save/compaction for the on-disk score file

## v1.24 and earlier
- Fixed a training bug where the (since-removed) learning subsystem trained on the oldest samples instead of the most recent ones
- Exempted persistent system services from kill/retention scoring — these get auto-relaunched by Android regardless
- Debounced foreground-app detection to avoid flapping
- Exempt processes no longer counted against the pin budget
- Pin log now shows readable app names instead of just PIDs

## v1.00–v1.23
- Initial builds and incremental fixes establishing the core kill-tier system, retention scoring, and Magisk packaging
