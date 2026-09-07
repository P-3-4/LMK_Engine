# lmk_engine — Architecture & Reference Documentation

Version documented: **1.26**

This document explains how `lmk_engine` works internally: its data model, the tiered adaptive-reclaim engine, retention scoring and pinning, ZRAM management, kswapd/VM tuning, external-kill attribution, and a function-level reference.
Its quite a long read, but if you wanna understand the daemon logic, you can go through it.
For install/quick-start, see `README.md`. For version history, see `CHANGELOG.md`.

**Note on device specificity:** several behaviors documented here (screen-state detection, cgroup topology, kernel-version feature gates) were derived against one specific device (see `README.md`). Porting to another device means re-verifying those sections rather than assuming they carry over unmodified.

---

## 1. Overview

`lmk_engine` is a single-binary C daemon that runs continuously in the background (via a service script) and, each tick:

1. Reads system memory state (`/proc/meminfo`, ZRAM sysfs, PSI) into `MemInfo`/`ZramInfo` snapshots.
2. Enumerates running processes (`enumerate_procs`) into a table of `ProcInfo` entries — pid, name, RSS, swap, and a computed `rank`.
3. Tracks true foreground state (`track_foreground`, dumpsys-confirmed, debounced) to credit usage signals correctly and gate foreground-only exemptions.
4. Decides whether to act and how aggressively, via a tiered pressure model (`adaptive_select_tier` → `adaptive_clean`) driven by available RAM, ZRAM fullness, and PSI stall.
5. Before killing anything, tries cheaper interventions first: proactive `memory.high` squeeze (`try_squeeze_then_reclaim`, `pin_squeeze_pass`), then kill (`do_kill`, `zram_pressure_kill`) — while a top-ranked set of apps is protected via `cgroup v2 memory.low` (`oom_pin_retained`) and a short kill-exemption window (`is_actively_retained`).
6. Independently tunes kernel reclaim behavior: pins `kswapd` off the big CPU cluster under normal pressure, and adjusts VM watermark tunables based on available-RAM tier.
7. Attributes deaths it didn't cause — external kills via `logcat` monitoring, and otherwise-unexplained vanishes via presence tracking — so restart/retention stats reflect reality, not just this daemon's own actions.
8. Persists state to disk (`lmk_scores.dat`, `lmk_rank.cache`) atomically (tmp-write + rename) and logs to `lmk_engine.log`.

Everything runs from one process, single-threaded, polling on an adaptive interval (500ms busy, widening to ~1.5s calm).

**Removed since v1.25:** the on-device learning subsystem (`AI_Swap`'s gradient-descent scoring bias) and `oom_score_adj`-based pinning are both gone. See Section 5 and 11 for why, and what replaced them.

---

## 2. Core data structures

### `AppScore` — the persistent per-app retention record
Stored in `g_scores[SCORE_MAX_APPS]` (cap 260 apps), backed by `lmk_scores.dat`, written atomically.

| Field | Purpose |
|---|---|
| `name` | package/process name, lookup key |
| `fg_count` | cumulative foreground ticks — duration proxy |
| `last_fg` / `last_genuine_fg` | last true-foreground timestamp — recency signal. Foreground state is dumpsys-confirmed, not just "not backgrounded" |
| `session_count` | distinct foreground sessions — frequency proxy, decays when unused |
| `restart_count` | times the app was observed alive again shortly after a death, **from any source** — the daemon's own kills, external kills (via `logcat`), or unattributed vanishes. This unification (v1.26) is what makes retention/bounce stats meaningful — earlier it only counted the daemon's own kills |
| `avg_swap_kb` | EMA of ZRAM swap usage, used for kill *ordering* |
| `category` | 0 = native/transient (Zygote children, system binaries — never scored as a real app), 1 = system app, 2 = user app |
| `category_confirmed_at` | when category was confirmed via dumpsys, vs. a heuristic guess |
| `cg_low_set` | whether this app currently has a `memory.low` floor written — cleared on slot loss **or** full app death (v1.26 fix; previously only cleared on slot loss, leaving restarted instances of a dead pinned app believing they were still protected) |
| `dc_kill_count` / `adaptive_kill_count` | historical kill counters by source, for diagnostics |
| `retain_pin_t` | last tick this app was in the actively-retained top-N set (in-memory) |
| `last_seen_t` | last tick this app was observed in the live process table — feeds the unattributed-vanish detector |
| `rss_watch_kb` / `rss_growth_strikes` / `flagged_runaway` | runaway-growth detector state (in-memory) |

Score combines cumulative foreground time, session frequency, and recency in `score_compute()` — a fixed formula, no learned weights.

### `ProcInfo` — one live process, rebuilt every tick
Transient. Carries `pid`, `name`, RSS/swap, computed `Priority prio`, `rank`, `true_fg`, `last_genuine_fg`, bounce-window counters.

### `MemInfo` / `ZramInfo`
Point-in-time snapshots: `MemInfo` has `total_kb`/`avail_kb`/`avail_pct`; `ZramInfo` has `disksize_kb`/`used_kb`/`orig_data_kb`/`compr_data_kb` (actual physical-RAM footprint of the compressed store, distinct from the logical swapped-out page count) and `used_pct`.

### `Priority` enum
```
PRIO_NEVER            never touched (system-critical, exempt services)
PRIO_SEMI_PROTECTED    protected but killable under extreme pressure
PRIO_BACKGROUND        normal background app
PRIO_CACHED            cached/idle app, low cost to kill
PRIO_JUNK              lowest priority, killed first
```
Separate from `rank` (`RANK_EXEMPT` → `RANK_TIER4`), which orders kill *candidates* within a priority band.

### Rank tiers (consolidated, v1.26)
```
RANK_EXEMPT   0   never a kill candidate
RANK_TIER1    1   was FOREGROUND+HOT — recent/active use
RANK_TIER2    2   was WARM+COLD
RANK_TIER3    3   was STALE
RANK_TIER4    4   was BACKGROUND — killed first
```
The old five-tier FOREGROUND/HOT/WARM/COLD/STALE/BACKGROUND split was consolidated into four tiers; `RANK_FREQ_MED_SESSIONS`/`RANK_FREQ_HIGH_SESSIONS` let high session-count apps punch up a tier even without recent use.

---

## 3. Main loop (`run_daemon`)

Each tick, roughly:
1. Read `MemInfo`/`ZramInfo`.
2. `enumerate_procs()` — rebuild the process table.
3. `track_foreground()` — debounced true-foreground detection (see below); updates `AppScore` recency/session fields.
4. `handle_true_foreground_detection()` / `handle_zram_stage_management()` — edge-triggered handling for foreground transitions and ZRAM staged escalation.
5. `logcat_kill_watch()` — drain and attribute any external kills seen since the last tick.
6. `unattrib_check_vanished()` / `unattrib_mark_seen()` — presence-based detection of deaths no kill-path explains.
7. `repin_kswapd_if_needed()` — CPU-cluster pinning maintenance.
8. `apply_avail_tier_tunables()` — VM watermark tuning based on current `avail_pct` tier.
9. `ram_pressure_and_adapt()` → `adaptive_select_tier()` → `adaptive_clean()` — the main tiered decision engine; may call `oom_pin_retained()`, `pin_squeeze_pass()`, `zram_pressure_kill()`, or the deep-clean path.
10. `score_maybe_save()` / `rank_cache_maybe_save()` — dirty-gated, atomic persistence.
11. Adaptive sleep before the next tick.

### True foreground detection

The problem: Android's `oom_adj==ADJ_FOREGROUND` (0) is a much wider net than "the app on screen" — it's assigned to *anything* holding an active foreground service (push handlers, sync, media playback), not just what the user is actually looking at. Using it directly credits "genuine use" to background helpers constantly. Three layers narrow it down:

1. **`confirm_true_foreground(pkg)`** is the ground truth. It runs `dumpsys window windows`, reads the first `mCurrentFocus=` line (authoritative — real input focus), and checks whether it names `pkg`. Fails open (returns true) if the focus line is missing entirely, so a `dumpsys` hiccup doesn't wrongly strip retention credit.
2. **`find_true_foreground()`** scans the process table each tick for the `ADJ_FOREGROUND` holder (skipping native paths and `.persistent` processes) and hands the one candidate to `handle_true_foreground_detection()`, which calls `confirm_true_foreground()` on it and stores the verified result in `g_true_fg_name`.
3. **`is_genuine_fg(name, oom_adj)`** is the actual gate everything else checks. Requires **all** of:
   - `oom_adj == ADJ_FOREGROUND`
   - `is_fg_trackable(name)` — excludes native paths, `.persistent`/`:persistent`, `.providers.` (content providers blip through FOREGROUND during sync bursts — `providers.calendar` alone hit `fg_count=4935` from sync churn before this exclusion), `android.process.*` legacy aliases, plus a small confirmed list of no-launcher transient-focus-stealers found via log evidence: `permissioncontroller`, `cellbroadcastreceiver`, `settings.intelligence`, `apps.turbo` (Device Health Services — flapped into "genuinely reopened" 159× in one 5h18m window, roughly every 50-70s), `adservices.api` (headless Privacy Sandbox service, no UI at all), `chromecast.app` (cast-picker sheet, won 216 pin-slot credits across two full-day logs while never appearing in the persisted USER score section)
   - `name == g_true_fg_name` — must match what `confirm_true_foreground()` actually verified via `dumpsys`, not just share the same `oom_adj`. This is what stops a push-notification helper sitting at `ADJ_FOREGROUND` from stealing credit meant for whatever's on screen.

**Debounce on top** (in `track_foreground()`): `FG_MIN_HOLD_S` requires a few seconds of continuous genuine focus before crediting a session — filters sub-second notification-trampoline blips that briefly touch TOP without real UI. Below the bar, recency/swap still tracks via the lightweight background path so protect-window data doesn't stall while waiting to see if it's a real open. `FG_HOLD_WOBBLE_TOLERANCE_S` tolerates a one-tick `oom_adj` flicker under real memory pressure without resetting the hold timer — previously any single false tick reset it unconditionally, which could starve `last_genuine_fg` from ever refreshing during a long continuous session.

`is_genuine_fg` deliberately dropped an earlier `classify_score_cat(n) != 2` (category) clause — category (NATIVE/SYSAPP/USER) and "is this a real user-facing open" turned out to be orthogonal signals. Conflating them meant any SYSAPP-classified process (e.g. a custom ROM's call recorder) could never register as genuinely foregrounded no matter how much it was actually used.

---

## 4. Adaptive_Clean — tiered memory/ZRAM reclaim

Tier selection (`adaptive_select_tier`) is driven by `avail_pct`, ZRAM `used_pct`, and PSI stall, re-anchored in v1.26 around this device's actual steady state: **ZRAM runs chronically 85–95%+ full under normal operation** — the tiers below describe routine behavior, not rare emergencies.

| Tier | Trigger | Behavior |
|---|---|---|
| **IDLE-DEEP** | Screen off, sustained | ZRAM target-driven compaction toward `IDLE_DEEP_TARGET_PCT` (70%), relaxed kill-age gates |
| **IDLE** | Screen off, not yet deep | Lighter idle housekeeping, `IDLE_MIN_ZRAM_FREE_PCT` (10%) floor |
| **MAINT** | Screen on, memory comfortable | Minimal, low-frequency maintenance |
| **LOW** | `avail_pct` below `FREE_LOW_PCT` (15%) | Light kill pass |
| **HIGH (RAM-critical)** | `avail_pct` below `FREE_CRIT_PCT` (8%) | **v1.26: escalates independently of ZRAM%** — previously a real low-RAM emergency with ZRAM still under its own threshold could leave the daemon passive while the system killed apps itself |
| **T3 / Deep Clean** | ZRAM "stuck" ≥ `ZRAM_STUCK_PCT` (96%) sustained | Staged kill → drop caches → compact, each stage adaptively skipped once pressure is relieved |

### ZRAM pressure ladder (`ZRAM_WARN_PCT` 90 / `ZRAM_TRIM_PCT` 93 / `ZRAM_STUCK_PCT` 96 / `ZRAM_CRIT_PCT` 98)
Re-anchored in v1.26 from an earlier ladder that assumed rare escalation. `graduated_squeeze_target_pct()` returns 0 (no squeeze) at/above `ZRAM_WARN_PCT` — squeezing when ZRAM is already tight is counterproductive, since squeeze pushes reclaimed pages *into* ZRAM, the only swap backend on this device.

### Guardrails
- **Futility detection**: repeated deep-clean attempts that recover negligible ZRAM pause deep-clean for a cooldown window. The campaign timer resets the moment `zram_used_pct` drops back under `ZRAM_TRIM_PCT` (v1.26 fix — previously only reset on full cycle/resize, so stale state from an old campaign could produce a false "not worth it" verdict when real pressure returned later).
- **Chronic bouncer suppression** (`check_bounced`): apps with high `restart_count` or too many kills in a short window are suppressed from further ZRAM-pressure kills.
- **Runaway-growth detection**: sustained RSS growth over multiple windows biases a process toward being killed. Applies in background; foreground processes are never killed while actually in the foreground.
- **PSI event-driven monitor** (new in v1.26): real `lmkd` doesn't poll `/proc/pressure/memory` on a timer — it registers a trigger and `poll()`s the fd, waking instantly when a stall crosses threshold instead of sampling on the main loop's cadence. This layers a fast early-wake path (100ms of `full` stall within a 1s window — `full` maps to visible jank, unlike `some` which fires on any single stalled task) on top of the existing tick-based `psi_mem_avg10()` checks used for tier escalation; it doesn't replace them.
- **Shared pre-kill check** (`should_spare_kill`, new in v1.26): reclaim+ROI mitigation used to live only inside `do_kill()`, but log data showed `do_kill()` accounts for only ~20% of actual kill volume — most kills come from `adaptive_clean()`'s tiered sweeps, which never got a shot at either mitigation. Extracted once and now called at every kill site (`do_kill`, `full_reset_sweep`, `zram_pressure_kill` ×2, `adaptive_clean`, `pre_cycle_kill`) so all of them benefit uniformly. Never spares in the CRITICAL tier — a real RAM emergency takes whatever it can get.

### Per-process kernel reclaim (new in v1.26)
`try_reclaim()` pushes a backgrounded app's anonymous pages into ZRAM via `echo all > /proc/<pid>/reclaim` **without killing the process** — confirmed present and working on this kernel, with an on-device test showing ~20-25% RSS reduction (WhatsApp 284.9MB→216.9MB) while the process stayed alive. This is a genuinely new middle tier between "leave alone" and "kill": squeeze first, reclaim next, only kill if neither freed enough. Rate-limited per app via `RECLAIM_COOLDOWN_S` (600s) since repeated reclaim on an already-squeezed app has diminishing returns.

**Confirmed bugfix in the same feature:** on-device logs showed ZRAM stuck 90-99% for 3+ hours in one session while T3(DEEP) fired 409 times, almost all `killed=0` — because RAM-critical `do_kill()` kept sparing one long-lived process via reclaim (~35 times in that session, never actually killing it), continuously pushing more of its RSS into ZRAM that this device can't swap back out without a genuine kill. Reclaim's "trade RAM for ZRAM" tradeoff only makes sense when ZRAM has headroom to absorb it, so it's now skipped entirely once ZRAM is at/above `ZRAM_WARN_PCT`, letting `do_kill()` fall through to a real kill instead.

---

## 5. Retention scoring & pinning

### Scoring (`score_compute`)
A fixed, deterministic formula, no learned/trained component — the earlier `AI_Swap` gradient-descent bias model was removed as dead weight once the `oom_score_adj` pinning it tuned for was confirmed structurally unwinnable (see below). Four signals, summed and clamped to `[0, SCORE_MAX]` (1000):

1. **Decayed tick score** — `fg_count` (cumulative foreground ticks, capped at `FG_COUNT_MAX`=100000) scaled down by `FG_COUNT_SCALE`=100, then halved once for every 12 hours idle (`idle_h/12` halvings). A long-unused app's usage history decays toward zero rather than staying frozen at its historical peak.
2. **Session frequency bonus** — `session_count` (also halved per idle day), scaled: `session_count * 240 / window_h` where `window_h` is idle hours clamped to `[1, 168]` (1 week), capped at 400. Rewards apps opened often, weighted toward recent activity.
3. **Recency bonus** — flat by idle bucket: 100 if used this tick, 50 if `<4h` idle, 20 if `<24h` idle, 0 beyond that.
4. **Genuine-engagement duration bonus** — separate from the open-count-based frequency bonus above, which treats a 3-second notification-dismiss tap the same as a real 20-minute session. Scales linearly with `avg_session_duration_s` up to `SESSION_DURATION_CAP_S` (600s), capped at `SESSION_DURATION_BONUS_MAX` (150). Speculative weight/cap, not log-calibrated.

### Protect window (`score_protect_window_ram`)
How long a killed-and-relaunched app's *recent-open* memory stays weighted toward protection, scaled by both the app's own score and current pressure. Base window: `FG_PROTECT_BASE` (30 min) + up to `FG_PROTECT_MAX` (6h) scaled proportionally by `score/SCORE_MAX`. Then two independent pressure overrides apply on top:

- Below `FREE_CRIT_PCT` avail: flat `FG_PROTECT_CRIT_S` (20s) — pressure this severe overrides everything.
- Below `FREE_LOW_PCT` avail: `window * FG_PROTECT_LOW_PCT/100` (40%).
- Otherwise, a graduated ZRAM-based curve — plenty of headroom retains longer since swapping out costs nothing spare; tight ZRAM trims back so retention doesn't add to the compression/decompression CPU cost:

| ZRAM band | Multiplier |
|---|---|
| `< WARN_PCT - 15` | 130% |
| `< WARN_PCT` | 100% (no discount) |
| `< TRIM_PCT` | 80% |
| `< STUCK_PCT` | 50% |
| `>= STUCK_PCT` | 20% |

**Fixed in v1.26:** this used to be just the 130%/80% split. Two separate call sites (ZRAMkill, the main T0-T3 sweep) each additionally multiplied the *result* by their own extra `ZRAM_STUCK_PROTECT_PCT` (10%) whenever ZRAM was stuck-full, added later by someone unaware this function already applied its own ZRAM discount — the two stacked multiplicatively (0.80×0.10=0.08), collapsing the intended 6h window to ~8% of itself under the device's *typical* ZRAM level (median 88%, not a rare tail case). Folded into the single graduated curve above so there's only ever one ZRAM-based multiplier, regardless of caller. Percentages are shape-only, speculative pending on-device A/B validation.

### Ranking (`app_rank` / `compute_proc_rank`)
`RANK_EXEMPT` → `RANK_TIER4` (see §2), computed per app per tick, then held stable by a promote/demote state machine so ties don't thrash the ordering every cycle:

- **Candidate tier**: `true_fg` or (`recency_trusted` and age `< RANK_HOT_S` 60s) → TIER1; `recency_trusted` and age `< RANK_COLD_S` 1h → TIER2; else TIER3. An app that has *never* earned trusted recency can't land TIER2 via the raw-recency path at all — only real session-frequency history can promote it (below), since TIER2 membership without trusted recency was previously bought nothing but wasted capacity (root-caused: many always-on daemons — HAL processes, `iwlan`, `qcrilam`, etc. — were fighting over a capped number of TIER2 slots on background-service noise alone, each promoted then immediately capacity-bumped back out, forever).
- **Frequency floor**: an app with `session_count >= RANK_FREQ_HIGH_SESSIONS` (20) can punch up to TIER1's neighbor, or `>= RANK_FREQ_MED_SESSIONS` (8) up to TIER2 — but only if `avg_session_duration_s >= MIN_GENUINE_SESSION_S` (20s), so 20 quick notification-glances don't earn the same floor as 20 real sessions.
- **Promotion** (tier number decreasing) is immediate for a genuine `true_fg`/`recency_trusted` reopen. An *untrusted* TIER3→TIER2 promotion is debounced (`UNTRUSTED_PROMO_DEBOUNCE_S`, 30s) — must hold candidacy that long before it sticks, closing the same always-on-daemon flapping case above at the promotion side too. A promotion back into a tier this app was *just* capacity-demoted out of is separately held off for `CAPACITY_PROMO_DEBOUNCE_S` (30s) unless `true_fg`.
- **Demotion** (tier number increasing) is dwell-gated: `TIER1_DWELL_S` (300s) / `TIER2_DWELL_S` (420s) minimum time in the current tier before a demotion is allowed to land, scaled by `score_dwell_mult_pct(score)` — 200% at score ≥800 (daily-driver apps get double the grace), 160% at ≥500, 130% at ≥200, 100% baseline. `tier_entered_t` refreshes on every tick `true_fg` holds (fixed in v1.26 — it previously only updated on the *first* entry into a tier, so a continuously-used app's dwell clock was measuring from session *start*, not session *end*: a 5-minute real session had already exhausted its 120s dwell budget the instant it backgrounded, while a 20-second glance still had ~100s of grace left — backwards from intent).
- **RANK1_HOLD_S** (3s): TIER1 promotion itself requires `true_fg` held continuously that long before it counts for *ranking* purposes (a separate, longer hold than `FG_MIN_HOLD_S` gates for *scoring* credit) — stops a transient dumpsys-confirmed focus (a permission dialog, a notification-trampoline activity) from buying a full TIER1 promotion and its dwell protection off one blip.

### Kill ordering
`cmp_killable_ram()` and `cmp_killable_zram()` are the qsort comparators that decide which candidate dies first once a tier has selected who's eligible. Both start from rank (higher/staler rank first) and priority, then diverge on secondary signal since the two kill paths optimize for different things:

- **RAM kill** (`cmp_killable_ram`): rank → priority → score (lowest first) → `avg_swap_kb` (highest swap-holder preferred — already mostly out of RAM, cheapest to fully evict) → `restart_count` (highest first — a proven bouncer is lower-value to keep around) → `effective_fg_recency()` (oldest first) → RSS (largest first, tie-break fallback).
- **ZRAM kill** (`cmp_killable_zram`): rank → blended swap (avg_swap_kb weighted 70%, live swap_kb 30%, for stable ordering against momentary spikes — highest first, since ZRAM pressure specifically wants to relieve whatever's contributing most to it) → score (lowest first) → `restart_count` (**lowest** first here — opposite of RAM kill, since ZRAM pressure-kill deliberately avoids re-attacking known bouncers, see below) → recency → RSS.

### Kill mitigation & ROI gating (`should_spare_kill`)
Runs before every kill site commits (§4's "Shared pre-kill check"). Never spares in a critical-tier kill. Otherwise, in order:
1. If the app was squeezed or kernel-reclaimed within `SQUEEZE_KILL_GRACE_S` (45s), spare it — give a just-applied mitigation a chance to actually show results before escalating.
2. Try `try_squeeze_then_reclaim()` — squeeze via `memory.high` first; a real reprieve here keeps the app warm instead of forcing a cold relaunch.
3. **Kill ROI gate**: if `restart_count >= KILL_ROI_RC_THRESHOLD` (5, a confirmed thrashy bouncer) *and* `rss_kb < KILL_ROI_MIN_RSS_KB` (15MB), spare it — not worth spending an expensive relaunch on a candidate barely holding any RAM.

### Bounce detection & cooldown (`check_bounced`, `kill_cooldown_for`)
Every kill funnels through `record_kill()` regardless of source (this daemon's own, `ExternalKill`, or `Unattributed`), which stamps `g_kill_hist`. `check_bounced()` checks on the *next* sighting of that name: if it reappears within `RESTART_WINDOW_S` (240s) of its own kill, `restart_count` increments and the cooldown recalculates. **Exception**: a kill tagged `scheduled` (i.e. from `full_reset_sweep()`, §9) never counts toward `restart_count` — a prompt relaunch after a deliberate scheduled flush is expected, not instability, and letting it count would misclassify a healthy app as a confirmed bouncer over repeated 12h cycles.

Cooldown itself (`kill_cooldown_for`): `RESTART_CD_BASE_S` (30s) + `restart_count * RESTART_CD_SCALE_S` (20s per bounce), capped at `RESTART_CD_MAX_S` (180s) normally — but once `restart_count >= BOUNCE_SUPPRESS_RC` (20, a confirmed chronic bouncer), the cap raises to `BOUNCE_KILL_CD_MAX_S` (30 min) instead of flattening out, so a proven thrasher gets real backoff instead of being re-attacked every 3 minutes.

### Runaway-growth detection (`check_runaway_growth`)
Samples RSS every `GROWTH_WINDOW_S` (10 min) for any process over `GROWTH_MIN_BASE_KB` (50MB, filters noise). Growth ≥`GROWTH_STRIKE_PCT` (12%) in a window counts a strike; any window below that resets strikes to zero. `GROWTH_STRIKES_FLAG` (6 consecutive strikes, ~1h of sustained growth) flags the process as a runaway and biases it toward being killed; re-alerts hourly (`GROWTH_REALERT_S`) while still growing. Applies only in background — a foreground process is never killed for this while actually in use.

### Retention pinning (`oom_pin_retained`) — `cgroup v2 memory.low`, not `oom_score_adj`
**Why the mechanism changed:** `oom_score_adj` pinning was confirmed structurally unwinnable — AMS's `computeOomAdjLocked`/`ProcessList.setOomAdj` runs on a privileged, continuous, event-driven cycle with no public suppression hook, and extended on-device measurement (`PinHealth`) showed every write evicted within 1–2 seconds, 0% survival across every app/rank/priority tested over many hours. All the machinery built around fighting that (eviction-detection re-reads, backoff/streak/give-up logic) was removed as pure overhead.

**Current mechanism:**
- Top-N apps by score get a `memory.low` floor written to their cgroup, sized to current RSS. This is *best-effort* per cgroup v2 docs — the kernel can still reclaim below it under severe global pressure, which is expected, not a bug.
- The real surviving benefit: being selected top-N also sets `retain_pin_t`, which grants an 8-second exemption from the daemon's *own* opportunistic kills (`is_actively_retained()`, checked at 5 call sites). This exemption never depended on the `memory.low` write surviving.
- `PIN_SETTLE_S` (10s) gate — the daemon was previously writing pins during AMS's own 1–9s `oom_adj`-finalization window for a foreground transition, losing that race almost every time. Pins now wait until after settle.
- Fixed-size slot model: `TIER1_PIN_MAX` (4) + `TIER2_PIN_MAX` (3) = 7 real pins, not dynamically scaled by headroom.
- `pin_squeeze_pass()` (v1.26, new) — pinned apps were previously fully exempt from reclaim, letting cold memory sit protected indefinitely. Pinned apps can now still be squeezed via `memory.high`, just not killed.
- Stale-protection fix: `cg_low_set` now clears on full app death, not just slot loss — previously a restarted instance of a dead pinned app could inherit a phantom "still protected" state.

### `memory.low` / `memory.high` orthogonality
Per cgroup v2 docs: `memory.low` protects an app from *other* cgroups' pressure (best-effort, kernel-enforced). `memory.high` is a self-directed throttle the daemon writes to push a background app to reclaim its own pages — it never invokes the OOM killer. Squeezing a `memory.low`-protected app is valid and standard; they aren't in tension.

---

## 6. ZRAM management

- `zram_setup()`/`zram_find()`: locates and sizes the ZRAM device to `ZRAM_SIZE_PCT` (50% of physical RAM — lowered from an earlier 90% after steady-state data showed the larger pool didn't help and cost more compressed-store RAM).
- `zram_read_stats()`: reads `used_pct`, `disksize_kb`, `orig_data_kb`, `compr_data_kb`.
- `zram_pressure_kill()`: targeted kill pass when ZRAM crosses `ZRAM_TRIM_PCT`, rate-limited and batch-capped.
- `zram_cycle()`: full swapoff/swapon defragmentation cycle. **Effectively dead on this device**: its gate requires ~50% of total RAM free simultaneously — structurally unreachable under the exact pressure profile that would motivate a cycle in the first place (a confirmed catch-22). The passive/natural-window attempt in `adaptive_clean()` remains as a free fallback; a full fix (lower the margin, or formally retire the function) is an open product decision.
- `zram_try_deferred_resize()`: handles resize when the device couldn't be set to target size at boot.

---

## 7. kswapd pinning & VM tuning (new in v1.26)

Root-caused via log correlation that kswapd CPU spikes track **kill/process-teardown churn** (~96% correlation with an external kill in the preceding 30s), not sustained ZRAM pressure level — static ZRAM% and its rate of change are both weak predictors.

- `detect_little_cluster()`: groups CPU cores by `cpuinfo_max_freq`; the lowest-frequency group is treated as the "little" cluster. Fully generic — no hardcoded core IDs, so it should adapt to other big.LITTLE layouts.
- `pin_kswapd_to_little_cores()` / `unpin_kswapd()` / `repin_kswapd_if_needed()`: pins `kswapd` to little cores during normal pressure (so it doesn't compete with foreground UI rendering on the cores Android uses for that — pinning to *big* cores instead was tried and rejected as actively worse, since most spikes coincide with active app-launch events). Unpins to all cores as ZRAM approaches `ZRAM_CRIT_PCT`.
- `KSWAPD_PIN_DWELL_S` (15s) exists as a constant but **the dwell gate is not yet wired into the actual pin/unpin decision** — known gap, confirmed causing rapid flap cycles right at the pressure boundary in on-device logs.
- `apply_avail_tier_tunables()`: three `avail_pct` tiers (≥15% / 10–14% / <10%) drive `extra_free_kbytes` and `watermark_scale_factor`, gated by `VM_TUNE_DWELL_S` (15s) to prevent write-storms at tier boundaries. `avail_pct` proved a tighter kswapd-CPU predictor than ZRAM% alone. Tuning magnitudes are speculative pending A/B data.
- `drop_clean_caches()`: page-cache-only drop (never dentries/inodes) on screen-on/unlock and on entering a low-avail tier, floor-rate-limited.

---

## 8. External-kill & unattributed-death attribution (new in v1.26)

Needed because a meaningful share of process deaths on this device were never the daemon's own doing — AMS, the Phantom Process Killer (before it was disabled via `device_config`), and other actors all kill processes independently, and without attribution, `restart_count`/bounce stats only reflected a fraction of what was actually happening.

- `logcat_watch_init()` / `logcat_drain_and_attribute()` / `logcat_kill_watch()`: taps both the system buffer and AMS's structured events buffer. Matching is field-structured (`"Process X (pid Y) has died"`, `"Killing PID:X"`), **not** raw substring search — an earlier substring-based version had a confirmed collision bug (`"com.android.se"` matching both `com.android.settings` and unrelated Java stack-trace text), which this structured approach avoids by construction.
- `unattrib_check_vanished()` / `unattrib_mark_seen()`: presence-based detection using `AppScore.last_seen_t`. If a tracked app vanishes from the process table with no kill-path record at all, it's logged as `Unattributed:` and still feeds `record_kill()` so restart accounting stays consistent.
  - Skips `category == 0` (native/transient) entries — covers Zygote's real, transient `<pre-initialized>` cmdline value for a forked-but-not-yet-specialized child, which would otherwise generate a false vanish report the moment the child execs into its real name.
- `record_kill()` is now called uniformly from every attribution path (the daemon's own kills, `ExternalKill` matches, and `Unattributed` detections), so `restart_count`/`check_bounced()` reflect deaths from any source.

---

## 9. Scheduled full-flush sweep (new in v1.26)

`full_reset_sweep()` — a deliberate clean-slate flush, not a pressure response. Fires once both gates clear: `RESET_GATE_S` (12h) since boot or the last sweep, **and** `IDLE_DEEP_S` (30 min) of continuous screen-off. When it fires, it kills everything except `PRIO_NEVER` (never-kill list, exempt services, launchers, IME, widget providers, current foreground) and `RANK_EXEMPT` (HAL/native) — bypassing pin, protect-window, and cooldown entirely. Not a literal ZRAM/swap reset (`swapoff` is confirmed impossible on this device, see §6) — it reclaims ZRAM indirectly by killing whatever's holding pages in it. Kills from this path are tagged (`KillRecord.scheduled`) so `check_bounced()` doesn't mistake a scheduled flush for pressure-driven bounce churn.

## 10. Authoritative package classification (new in v1.26)

`classify_score_cat()`'s curated-prefix fallback defaults unrecognized packages to USER category, which misclassifies vendor/OEM system packages it doesn't have a rule for (`com.google.android.gms.*`, `.iwlan`, `.projection.gearhead`, `pixel.*`, etc.). Rather than growing the curated list indefinitely, any unconfirmed package gets queued (`classify_queue_maybe`/`_requeue`, capped at `SYSCHECK_QUEUE_MAX` = 128) and confirmed later, a few at a time, only during deep idle (`CLASSIFY_IDLE_S` = 240s screen-off) via `classify_dumpsys_drain()` reading the package manager's own authoritative `flags=[ ... SYSTEM ... ]` line. `category_reconfirm_sweep()` periodically re-checks confirmed categories (`CATEGORY_RECONFIRM_S`, currently 3 days, speculative pending more log data). This runs only during idle specifically because it was previously calling `dumpsys` every main-loop tick — an expensive `popen()` call — before being gated to idle-only.

**Confirmed bugfix in the same feature:** `score_alloc_slot()`'s eviction pick previously chose purely by `score_compute()`, which is ~0 for any always-resident-but-never-foreground process (`gms.persistent`, `digitalkey.timesync`, etc.) — exactly the entries that took a real `dumpsys` round-trip to classify. These zero-score confirmed entries were the first evicted whenever the table needed a slot, then silently recreated from scratch (`confirmed=false`) the next time they were seen, repeatedly re-running the classify pipeline for the same answer. `EVICT_CONFIRMED_BONUS` (80) now protects `category_confirmed` entries from casual eviction, so an unconfirmed/fresh entry is evicted first when scores are otherwise similarly low. The eviction path is also no longer silent — it logs which entry was evicted and why.

## 11. Removed subsystems (historical context)

- **`AI_Swap` learned-weight scoring**: an on-device gradient-descent model that blended a trained bias into `score_compute()`. Removed because the pinning mechanism it was tuning for (`oom_score_adj`) was confirmed structurally unwinnable — see §5. No remnants confirmed via full-file audit.
- **`oom_score_adj` pinning + eviction-detection/backoff machinery**: replaced by `cgroup v2 memory.low` (§5).

---

## 12. Function reference

Grouped by subsystem. Signatures are internal (`static`) unless noted.

**Logging & status**
`log_rotate`, `_log`, `print_status`, `explain_pkg`, `usage`.

**Memory & process info**
`read_meminfo`, `enumerate_procs`, `proc_cmdline`, `proc_oom_adj`, `proc_mem_stats`.

**Classification**
`classify`, `classify_score_cat`, `is_hal_process`, `is_bg_service_proc`, `is_rank_exempt`, `is_launcher_like`, `is_ime_like`, `is_media_player`, `is_structurally_headless`, `name_matches`, `classify_queue_maybe`/`_requeue`/`_try`/`_apply`, `classify_dumpsys_drain`, `category_reconfirm_sweep`.

**Ranking & scoring**
`app_rank`, `compute_proc_rank`, `score_lookup`, `score_compute`, `score_dwell_mult_pct`, `score_get`, `score_protect_window`, `score_protect_window_ram`, `is_actively_retained`, `score_touch`, `score_touch_bg`, `score_lookup_or_create`, `lookup_or_create_score_stub`, `check_runaway_growth`, `check_proc_runaway`, `effective_fg_recency`, `enforce_tier_caps`, `rank_name`.

**Persistence**
`score_save_full`, `score_save_incremental`, `score_save`, `score_compact`, `score_load`, `score_maybe_save`, `score_alloc_slot`, `score_hash_rebuild`, `rank_cache_save`, `rank_cache_load`, `rank_cache_maybe_save`, `write_pid`.

Persistence is journal-based (new in v1.26): `score_save()` used to rewrite the full `lmk_scores.dat` table (up to `SCORE_MAX_APPS` entries) on every dirty cycle, even for a single changed entry. Changed entries are now appended to `lmk_scores.journal` instead; the full base file is only rewritten periodically — after `SCORE_JOURNAL_CONSOLIDATE_N` (400) journal-appended entries — or when the table structurally changes (compaction/pruning). `score_load()` replays the journal on top of the base file at startup, so no data is lost between consolidations.

**Foreground tracking**
`track_foreground`, `find_true_foreground`, `confirm_true_foreground`, `is_fg_trackable`, `is_genuine_fg`, `update_fg_history`, `get_last_fg`, `handle_true_foreground_detection`.

**Kill mechanics & retention**
`record_kill`, `kill_cooldown_for`, `recently_killed`, `check_bounced`, `track_proc_bounce_window`, `do_kill`, `should_spare_kill`, `cmp_killable_ram`, `cmp_killable_zram`, `cmp_adaptive_clean`, `oom_pin_retained`, `was_prev_pinned`, `full_reset_sweep`.

**Squeeze (`memory.high`)**
`try_reclaim`, `try_squeeze_then_reclaim`, `pin_squeeze_pass`, `graduated_squeeze_target_pct`, `has_trim_candidate`, `proactive_squeeze_pass`, `proactive_squeeze`.

**cgroup v2**
`cgroup_path_for_pid`, `cgroup_path_for_pid_fresh`, `cgroup_path_cache_invalidate`, `cgroup_write`.

**ZRAM**
`zram_sysfs_rd`, `zram_sysfs_wr`, `zram_orig_data_kb`, `zram_find`, `zram_read_stats`, `zram_setup`, `zram_cycle`, `zram_try_deferred_resize`, `zram_pressure_kill`.

**kswapd / CPU / VM tuning**
`find_kswapd_pid`, `kswapd_note_disruptive_event`, `detect_little_cluster`, `pin_kswapd_to_little_cores`, `unpin_kswapd`, `repin_kswapd_if_needed`, `cpu_track_sample`, `vm_tune_write_int`, `apply_avail_tier_tunables`, `drop_clean_caches`.

**PSI / swappiness**
`psi_check_available`, `psi_mem_avg10`, `psi_monitor_init`, `update_swappiness`.

**Screen/idle**
`is_screen_off`, `is_doze_active`, `lmkd_minfree_cleanup`, `update_idle_detection_state`, `wait_for_next_tick`, `read_uptime_s`, `wait_for_boot_settle`.

**Swap file**
`swap_create`, `swap_delete`.

**Widget provider protection**
`parse_widget_xml`, `load_active_widget_pkgs`, `is_active_widget_provider`.

**External-kill & unattributed attribution**
`logcat_watch_init`, `pkg_name_matches_line`, `logcat_drain_and_attribute`, `logcat_kill_watch`, `unattrib_check_vanished`, `unattrib_mark_seen`.

**Adaptive clean & top-level orchestration**
`adaptive_select_tier`, `adaptive_kill_eligible`, `adaptive_run_compaction`, `adaptive_clean`, `handle_zram_stage_management`, `ram_pressure_and_adapt`, `signal_handler`, `log_startup_banner`, `daemon_init`, `fix_lmkd_psi_stall`, `run_daemon`, `daemon_is_running`, `kill_daemon`, `main`.

---

## 13. Key configuration constants

| Constant | Value | Meaning |
|---|---|---|
| `FREE_HIGH_PCT` / `FREE_LOW_PCT` / `FREE_CRIT_PCT` | 25 / 15 / 8 | RAM `avail_pct` thresholds gating kill activity; `FREE_CRIT_PCT` independently escalates tier regardless of ZRAM% (v1.26) |
| `PIN_ALLOW_FLOOR_PCT` | 4 | Below this `avail_pct`, new pin *selection* is skipped (release still always runs) — fighting kernel reclaim with a fresh `memory.low` write at the true bottom is counterproductive |
| `ZRAM_SIZE_PCT` | 50 | Target ZRAM size as % of physical RAM (lowered from 90 in v1.26) |
| `ZRAM_WARN_PCT` / `_TRIM_PCT` / `_STUCK_PCT` / `_CRIT_PCT` | 90 / 93 / 96 / 98 | Re-anchored v1.26 ZRAM pressure ladder — describes routine operation on this device, not rare escalation |
| `IDLE_MIN_ZRAM_FREE_PCT` / `IDLE_DEEP_TARGET_PCT` | 10 / 70 | Re-anchored idle-tier ZRAM targets |
| `TIER1_PIN_MAX` / `TIER2_PIN_MAX` | 4 / 3 | Real `memory.low` pin slots (7 total) |
| `PIN_SETTLE_S` | 10 | Delay before pinning a newly-foregrounded app, to avoid racing AMS's own `oom_adj` finalization |
| `KSWAPD_PIN_DWELL_S` | 15 | Intended min time between kswapd re-pins — constant exists, **not yet wired into the actual gate** (known open item) |
| `VM_TUNE_DWELL_S` | 15 | Debounce for `avail_pct`-tier VM tunable writes |
| `SCORE_MAX_APPS` | 260 | Tracked-app cap in `lmk_scores.dat` (raised from 200 after observing peak live count) |
| `SCORE_MAX` | 1000 | Final `score_compute()` clamp |
| `FG_PROTECT_BASE` / `FG_PROTECT_MAX` | 1800 (30 min) / 21600 (6h) | Protect-window base and score-scaled ceiling |
| `FG_PROTECT_LOW_PCT` / `FG_PROTECT_CRIT_S` | 40% / 20s | Protect-window override under LOW/CRITICAL avail_pct |
| `RANK_HOT_S` / `RANK_COLD_S` | 60 / 3600 | Age thresholds for TIER1/TIER2 candidacy |
| `RANK_FREQ_MED_SESSIONS` / `RANK_FREQ_HIGH_SESSIONS` | 8 / 20 | session_count floors that punch a tier up regardless of recency |
| `MIN_GENUINE_SESSION_S` | 20 | Min avg session duration for the frequency floor to apply at all |
| `TIER1_DWELL_S` / `TIER2_DWELL_S` | 300 / 420 | Min time in tier before demotion is allowed, before score scaling |
| `RANK1_HOLD_S` | 3 | Continuous true_fg required before a TIER1 promotion counts for ranking |
| `UNTRUSTED_PROMO_DEBOUNCE_S` / `CAPACITY_PROMO_DEBOUNCE_S` | 30 / 30 | Debounce windows preventing rapid re-promotion at capacity/trust boundaries |
| `RESTART_WINDOW_S` | 240 | Window after a kill in which a reappearance counts as a bounce |
| `RESTART_CD_BASE_S` / `RESTART_CD_SCALE_S` / `RESTART_CD_MAX_S` | 30 / 20 / 180 | Kill cooldown formula and normal cap |
| `BOUNCE_SUPPRESS_RC` / `BOUNCE_KILL_CD_MAX_S` | 20 / 1800 (30 min) | restart_count bar for "confirmed chronic bouncer" and its raised cooldown cap |
| `SQUEEZE_KILL_GRACE_S` | 45 | Grace period after a squeeze/reclaim before the same app is kill-eligible again |
| `KILL_ROI_MIN_RSS_KB` / `KILL_ROI_RC_THRESHOLD` | 15360 (15MB) / 5 | Below this RSS *and* at/above this restart_count, a kill is skipped as not worth the relaunch cost |
| `GROWTH_WINDOW_S` / `GROWTH_MIN_BASE_KB` | 600 / 51200 (50MB) | Runaway-growth sample window and minimum tracked RSS |
| `GROWTH_STRIKE_PCT` / `GROWTH_STRIKES_FLAG` | 12% / 6 | Growth-per-window strike threshold and consecutive strikes to flag |
| `RECLAIM_COOLDOWN_S` | 600 (10 min) | Min time between two per-process kernel-reclaim attempts on the same app |
| `SCORE_JOURNAL_CONSOLIDATE_N` | 400 | Journal-appended entries before the full score file is rewritten |
| `RESET_GATE_S` | 43200 (12h) | Min time since boot/last sweep before another scheduled full-flush is eligible |
| `IDLE_DEEP_S` | 1800 (30 min) | Continuous screen-off required before the scheduled full-flush can fire |
| `CLASSIFY_IDLE_S` | 240 (4 min) | Screen-off required before dumpsys-based classification starts draining its queue |
| `SYSCHECK_QUEUE_MAX` | 128 | Cap on unconfirmed packages queued for dumpsys classification |
| `CATEGORY_RECONFIRM_S` | 259200 (3 days, speculative) | Interval for re-checking already-confirmed package categories |
| `EVICT_CONFIRMED_BONUS` | 80 | Score bonus protecting `category_confirmed` entries from casual slot eviction |

Full constants are documented inline in `src/lmk_engine.c` — this table covers the ones most relevant to tuning behavior for a different device.

---

## 14. CLI usage

```
lmk_engine --start          start the daemon
lmk_engine --stop           stop the daemon
lmk_engine --status         show memory/ZRAM/swap/pin status
lmk_engine --log            tail the live log
lmk_engine --swap <MB>      create & enable a file swap
lmk_engine --delswap        remove file swap
lmk_engine --explain <pkg>  dump everything the daemon knows about one app (score, category, restart history, exemption status) — read-only, safe to run alongside a live daemon
lmk_engine --features       print full feature list
```

---

## 15. Known limitations (as of v1.26)

- kswapd unpin/repin has no dwell gate wired in yet — confirmed flapping at the ZRAM pressure boundary in on-device logs.
- `avail_pct`-tiered VM tunable magnitudes (`extra_free_kbytes`/`watermark_scale_factor` values) are speculative, not yet A/B tested.
- `zram_cycle()` is effectively dead on this device [Infinix X683] (structurally unreachable gate) — a decision on lowering the margin or formally retiring the function is pending.
- A handful of low-priority classification/noise items remain open in the unattributed-death detector (junk-tier one-shot processes accounting for most of its volume; not a correctness bug, just noise worth filtering by default eventually).
- Events-buffer `logcat` tag names used for external-kill attribution are standard AOSP but not independently verified against every device — recommend a raw capture to confirm before relying on them elsewhere.

Tbh though if your phone is shit, this Daemon will struggle to keep apps open, for reducing the struggles or smth you can either increase the zram percentage and build using actions, or add a swap file using the terminal commands `lmk_engine --swap <amount of swap needed in megabytes>`
e.g `lmk_engine --swap 1024`
commands must be run with root
Just wanted to add this here again in case 🙂
Thanks for reading 
