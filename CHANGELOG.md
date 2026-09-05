# Changelog

All notable changes to this project are documented here.

## v1.26 (current)

The biggest arc since the initial rewrite — the old learned-weight retention model (`AI_Swap`) is gone, pinning moved to `cgroup v2`, and a large chunk of this cycle was spent finding out the device's *actual* steady-state (chronically high ZRAM, not a rare emergency) and re-calibrating everything around that instead of a "rare escalation" assumption that never matched reality.

**Retention & pinning**
- Replaced `oom_score_adj` pinning (unwinnable — AMS overwrites it within 1–2s, confirmed 0% survival) with `cgroup v2 memory.low`, best-effort per kernel docs. The daemon's own opportunistic kills additionally respect a short exemption window for the current top-ranked set.
- `PIN_SETTLE_S` gate added — the daemon was previously writing pins in the same window AMS was still finalizing `oom_adj` for the same transition, losing the race almost every time.
- New `pin_squeeze_pass()` — pinned apps were previously never squeezed at all (fully exempt from reclaim), which let cold memory sit protected indefinitely and contributed to chronic ZRAM pressure.
- Fixed a stale-protection bug where an app's pin state could persist after full app death (not just losing its slot), causing a restarted instance to be squeezed while the daemon still believed it was protected.

**ZRAM & pressure model**
- Full ZRAM tier ladder (WARN/TRIM/STUCK/CRIT) re-anchored after real-device data showed the old thresholds assumed rare emergencies — this device runs ZRAM chronically 85–95%+ full as normal operation.
- Fixed a protect-window bug where two independently-added discount multipliers (from different versions) were stacking silently, collapsing the intended multi-hour foreground-protect window down to ~8% of its value under typical conditions.
- Unified squeeze targeting (`memory.high`) into one graduated curve instead of a flat target with a hard cutoff, so proactive reclaim doesn't silently disable itself for large chunks of a normal day.
- Added an independent RAM-critical escalation path — previously, kill-tier escalation was gated almost entirely on ZRAM%, so a real low-RAM emergency with ZRAM still comfortably below its threshold could leave the daemon passive while the system killed priority apps itself.
- Removed a ZRAM-cycle pre-kill path that was confirmed structurally unable to reach its own target on this device's RAM budget (chasing free RAM equal to ~50% of total physical RAM) while still killing real, in-use apps trying to get there.

**Scheduler / CPU**
- kswapd little-core pinning — detects the device's low-frequency CPU cluster generically (by `cpuinfo_max_freq`, no hardcoded core IDs) and pins `kswapd` there during normal pressure, releasing to all cores as ZRAM approaches critical. Root-caused via log correlation that kswapd CPU spikes track kill/process-teardown churn, not sustained ZRAM level.
- avail_pct-tiered kernel VM tuning (`extra_free_kbytes`, `watermark_scale_factor`) — avail-RAM headroom turned out to be a tighter predictor of kswapd CPU cost than ZRAM% alone.

**External-kill & attribution tracking**
- Expanded `logcat` monitoring to a second tap (AMS's structured events buffer) alongside the original system-buffer tap, to catch kills the original tap missed entirely.
- Added presence-based "unattributed vanish" detection — catches apps that disappear with no record from any kill path at all (raw kernel OOM, another root actor, anything with zero log trace).
- `restart_count` now reflects kills from every source (the daemon's own, external, and unattributed), not just kills the daemon itself performed — needed for retention/bounce stats to mean anything.
- Fixed a substring-collision bug in kill-source attribution that could misattribute or fabricate kill records for packages whose name was a literal substring of another package or of unrelated log text.

**Other fixes**
- Fixed synchronous-reclaim jank on reopening a previously-squeezed app (release of the `memory.high` throttle was gated behind an unrelated hold-timer, delaying it by several seconds after a genuine reopen).
- Fixed a stale futility-timer bug in ZRAM pressure-kill campaign tracking that could produce a false "not worth trying" verdict right when real pressure returned after a calm gap.
- Fixed a PID-reuse gap in cgroup path caching (a live PID number is never reused, so comparing process start-time now reliably catches it).
- Fixed a false "unexplained death" log line for a real but transient Zygote process-naming artifact.
- General code-quality pass: atomic file writes for on-disk state (crash/kill mid-write no longer risks corrupting the score file), bounded string copies throughout, one uninitialized-memory fix, zero remaining leaks/dead code confirmed via repeated audit.

**Known open items**
- `avail_pct`-tiered VM tuning magnitudes are speculative — not yet A/B tested on-device.
- kswapd unpin/repin has no dwell gate yet — can flap rapidly right at the pressure boundary.
- A handful of low-priority classification/noise-filtering items in the unattributed-death detector remain open; see `docs/ARCHITECTURE.md` for current status.

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
