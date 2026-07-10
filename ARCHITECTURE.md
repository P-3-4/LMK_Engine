# lmk_engine — Architecture & Reference Documentation

Version documented: **1.25**

This document explains how `lmk_engine` works internally: its data model, its two major subsystems (`Adaptive_Clean` and `AI_Swap`), the main loop, ZRAM handling, configuration knobs, and a function-level reference. For install/quick-start, see `README.md`. For version history, see `CHANGELOG.md`.

---

## 1. High-level overview

`lmk_engine` is a single-binary C daemon that runs continuously in the background (via a Magisk service script) and periodically:

1. Reads system memory state (`/proc/meminfo`, ZRAM sysfs, PSI) and builds a snapshot (`MemInfo`, `ZramInfo`).
2. Enumerates running processes (`enumerate_procs`) into a table of `ProcInfo` entries — pid, name, `oom_adj`, RSS, swap, and a computed `rank`/`score`.
3. Classifies each process (`classify`) into a `Priority` tier, and separately assigns a rank (`app_rank`) used for kill ordering.
4. Decides whether to act, and how aggressively, based on tiered pressure thresholds (`Adaptive_Clean`) and ZRAM-specific thresholds.
5. Selects victims and kills them (`do_kill`, `zram_pressure_kill`, `pre_cycle_kill`), while actively pinning/protecting apps it has learned you care about (`oom_pin_retained`, `AI_Swap`).
6. Persists learned state to disk (`lmk_scores.dat`, `lmk_rank.cache`) and logs kill outcomes (`lmk_learn.log`) for on-device learning.

Everything runs from one process, single-threaded, polling on an adaptive interval — there is no separate learning process; training happens inline during idle periods.

---

## 2. Core data structures

### `AppScore` — the persistent per-app learning record
Stored in `g_scores[SCORE_MAX_APPS]` (cap 200 apps), backed by `lmk_scores.dat`.

| Field | Purpose |
|---|---|
| `name` | package/process name, lookup key |
| `fg_count` | cumulative foreground ticks — duration proxy (Signal 1). Capped at `FG_COUNT_MAX` (100,000 ticks ≈ 17 days), compressed by `FG_COUNT_SCALE` before entering `score_compute()` |
| `last_fg` | last time this app was in true foreground — feeds the recency signal (Signal 3) |
| `last_bg` | *(1.25)* timestamp of the tick the app left true foreground — used only for `warm_relaunch` detection, never refreshed while foreground, unlike `last_fg` |
| `was_true_fg` | *(1.25)* previous-tick foreground state, for edge-detecting the fg→bg transition that sets `last_bg` |
| `session_count` | distinct foreground sessions — frequency proxy (Signal 2), decays by halving daily when unused |
| `restart_count` | times the app was observed alive again shortly after being killed — drives cooldown scaling and bounce suppression |
| `avg_swap_kb` | EMA of ZRAM swap usage, used for kill *ordering*, not score |
| `category` | 0 = native daemon, 1 = system app, 2 = user app |
| `dc_kill_count` / `adaptive_kill_count` | historical kill counters by source, for diagnostics |
| `retain_pin_t` | last tick this app was actively pinned (in-memory only) |
| `rss_watch_kb` / `rss_watch_t` / `rss_growth_strikes` / `flagged_runaway` / `runaway_alert_t` | runaway-growth detector state (in-memory only) |

Three signals combine in `score_compute()`: cumulative foreground time, session frequency, and recency — plus, since v1.24, a bounded bias term from the on-device learning model (`g_learn_w`).

### `ProcInfo` — one live process, rebuilt every tick
Transient — populated fresh each cycle by `enumerate_procs()`. Carries `pid`, `name`, `oom_adj`, `rss_kb`, `swap_kb`, computed `Priority prio`, `last_fg`, `score`, `restart_count`, `avg_swap_kb`, bounce-window counters, and `rank`.

### `MemInfo` / `ZramInfo`
Point-in-time snapshots of system RAM (`total_kb`, `free_kb`, `avail_kb`, `avail_pct`, swap) and ZRAM device state (`disksize_kb`, `used_kb`, `orig_data_kb`, `used_pct`), respectively.

### `Priority` enum — classification, not kill-worthiness ranking
```
PRIO_NEVER            never touched (system-critical, exempt services)
PRIO_SEMI_PROTECTED    protected but killable under extreme pressure
PRIO_BACKGROUND        normal background app
PRIO_CACHED            cached/idle app, low cost to kill
PRIO_JUNK              lowest priority, killed first
```
This is separate from `rank` (RANK_EXEMPT → RANK_STALE), which is what actually orders kill *candidates* within a priority band using AI_Swap's learned scoring.

---

## 3. Main loop (`run_daemon`)

Roughly, each tick:
1. Read `MemInfo` and `ZramInfo`.
2. `enumerate_procs()` to rebuild the process table.
3. `track_foreground()` — debounced (2-tick confirm) detection of the true foreground app, updating `AppScore.last_fg`/`last_bg`/`session_count` and `g_true_fg_name`.
4. Check screen state (`is_screen_off`, `is_doze_active`) to gate idle-only behavior.
5. Run `adaptive_clean()` — the main tiered decision engine — which internally may call `oom_pin_retained()`, `zram_pressure_kill()`, `pre_cycle_kill()`, or the T3 deep-clean path.
6. `score_maybe_save()` / `rank_cache_maybe_save()` — dirty-gated persistence.
7. `ai_learn_train_maybe()` — during IDLE-DEEP only, retrains `g_learn_w` from recent `lmk_learn.log` rows.
8. Adaptive sleep before the next tick — polling interval widens when the system is calm and tightens under pressure (`ADAPTIVE_INTVL_LOW_S` / `_MED_S` / `_HIGH_S`).

---

## 4. Adaptive_Clean — tiered memory/ZRAM reclaim

Escalates through tiers rather than applying one fixed kill policy. Tier selection is driven by `avail_pct` (from `MemInfo`), ZRAM `used_pct` trend, and PSI memory pressure (`psi_mem_avg10`) when available.

| Tier | Trigger | Behavior |
|---|---|---|
| **IDLE-DEEP** | Screen off ≥ 30 min continuous | Intense sweep: relaxed kill-age gates, ZRAM compaction, learning trainer runs here |
| **IDLE** | Screen off, not yet 30 min | Lighter idle housekeeping |
| **MAINT** | Screen on, memory comfortable | Minimal, low-frequency maintenance |
| **LOW** | `avail_pct` mildly below `FREE_LOW_PCT` (15%) | `ADAPTIVE_KILLS_LOW` (3) max kills, 45s interval |
| **MED** | ZRAM/RAM pressure clearly rising | `ADAPTIVE_KILLS_MED` (7) max kills, 25s interval |
| **HIGH** | Heavy pressure | `ADAPTIVE_KILLS_HIGH` (12) max kills, 12s interval |
| **T3 / Deep Clean** | ZRAM "stuck" ≥ `ZRAM_STUCK_PCT` (85%) for `ZRAM_STUCK_S` (60s) | Staged: kill → `drop_caches` → `compact_memory` → ZRAM compact, each stage adaptively skipped once pressure is relieved |

Guardrails layered on top of the tier system:
- **Futility detection** (`DEEPCLEAN_FUTILE_PCT`/`_STRIKES`/`_PAUSE_S`): if deep cleans repeatedly recover ≤2% of ZRAM, the engine pauses deep-clean attempts for 3 minutes rather than burning CPU on cleans that aren't helping.
- **Chronic bouncer suppression** (`check_bounced`): apps with high `restart_count` or that were killed too many times inside a short window are suppressed from further ZRAM-pressure kills unless swap usage crosses an override threshold.
- **Runaway-growth detection** (`check_runaway_growth`): watches RSS growth over a 10-minute window; a process sustaining >12% growth per window for 6 consecutive windows (~1h) is flagged and biased toward being killed. Applies to background *and* (since 1.24) foreground processes, though foreground processes are never killed while actually in the foreground.
- **ZRAM cycle-impossibility guard**: if `orig_data_kb` already exceeds a safe percentage of total ZRAM, a full swapoff/cycle is skipped as structurally unsafe rather than attempted and failed.

---

## 5. AI_Swap — retention scoring & learning

### Scoring (`score_compute`)
Combines three signals into a single bounded score (`0..SCORE_MAX`):
1. **Engagement volume** — compressed `fg_count`
2. **Frequency** — `session_count`, decayed daily
3. **Recency** — derived from `last_fg`

Since v1.24, a bounded bias term (± `LEARN_SCORE_BIAS_MAX`) from the trained logistic model `g_learn_w` is blended in on top of these three signals.

### Ranking (`app_rank`)
Produces the ordinal rank used for kill ordering: `RANK_EXEMPT` (never touched) down through `RANK_HOT`, `RANK_WARM`, `RANK_COLD`, to `RANK_STALE` (kill first). Rank blends recency with `session_count` so a frequently-used-but-not-recently-opened app doesn't rank as low as a genuine one-off.

### Retention pinning (`oom_pin_retained`)
Selects the top-N apps by effective score each tick and writes a protective `oom_score_adj` value to them directly, scaled by available RAM/ZRAM headroom rather than a flat step function. Includes:
- **Pin-set stickiness** (`PIN_STICKY_BONUS`): previously-pinned apps get a bonus so near-ties don't cause the pinned set to thrash tick to tick.
- **Deterministic tie-break** (1.25): exact ties after the stickiness bonus fall back to name comparison instead of `/proc` scan order, which itself shifts tick to tick.
- **Eviction detection** (1.25 groundwork): tracks the `oom_adj` value written for each previously-pinned app (`g_prev_pinned_adj`) so a future check can detect `system_server` silently overwriting a pin.

### On-device learning (`ai_learn_log_resolved`, `ai_learn_train_maybe`)
Kill outcomes are logged to `lmk_learn.log` per app per event. During IDLE-DEEP, `ai_learn_train_maybe()` runs a plain gradient-descent pass (no external ML libraries — just `libm`) over the most recent `LEARN_MAX_SAMPLES` rows (tail-read as of 1.24, fixing a bug where training silently used only the oldest rows before log rotation) to update `g_learn_w`, which `score_compute()` then reads back.

### Persistent service exemption
`SERVICE_EXEMPT` (classified to `PRIO_NEVER` in `classify()`) covers processes Android will unconditionally relaunch regardless of what the engine does — e.g. carrier RCS messaging services and GMS search/interactor components. Rather than fight an unwinnable kill-relaunch loop, these are exempted outright, which also frees up retention-pin budget for real user apps.

---

## 6. ZRAM management

- `zram_setup()` / `zram_find()`: locates and initializes the ZRAM device, sized to `ZRAM_SIZE_PCT` (70%) of physical RAM, with a 3-attempt retry.
- `zram_read_stats()`: reads live `used_pct`, `disksize_kb`, `orig_data_kb`.
- `zram_pressure_kill()`: targeted kill pass when ZRAM crosses `ZRAM_TRIM_PCT`/`ZRAM_CRIT_PCT`, rate-limited to once per `ZRAM_KILL_INTVL_S` (5s) and capped at `ZRAM_KILL_BATCH` (6) kills per call.
- `zram_cycle()`: attempts a full swapoff/swapon cycle to defragment ZRAM — gated by `ZRAM_CYCLE_MARGIN_KB` (200MB safety buffer) and skipped entirely when `g_cycle_impossible` is set (device-specific: this device's `orig_data_kb` structurally exceeds the safe threshold, so a cycle is never actually attempted here).
- `zram_try_deferred_resize()`: handles resizing the ZRAM device when it couldn't be set to the target size at boot.

---

## 7. Screen state & idle detection

- `is_screen_off()`: parses `dumpsys power`'s `mWakefulness` field. **Device-specific fix (1.25):** this GSI/vendor combination reports `Dozing` and never progresses to `Asleep`, so the match was broadened to include both. This one fix unblocked IDLE-DEEP, the learning trainer, relaxed idle kill-age gates, ZRAM compaction during idle, and fast MAINT→IDLE transitions on screen lock — all of which were silently dead code on this device before 1.25.
- `is_doze_active()`: separate check for actual Doze mode (distinct from plain screen-off).

If porting to another device, verify your own `dumpsys power` output before assuming this detection works unmodified — the wording of wakefulness states varies across vendor skins and Android versions.

---

## 8. Function reference

Grouped by subsystem. Signatures are internal (`static`) unless noted.

**Logging & status**
`log_rotate`, `_log`, `print_status`, `usage` — logging, log rotation at `LOG_MAX` (2MB), CLI status/help output.

**Memory & process info**
`read_meminfo`, `enumerate_procs`, `proc_cmdline`, `proc_oom_adj`, `proc_mem_stats` — read `/proc` for system and per-process memory data.

**Classification**
`classify`, `classify_score_cat`, `is_hal_process`, `is_bg_service_proc`, `is_rank_exempt`, `is_launcher_like`, `is_ime_like`, `is_media_player`, `name_matches` — determine `Priority`/category from process name against the `NEVER_KILL`, `LAUNCHERS`, `SERVICE_EXEMPT`, `IME_PKGS`, `MEDIA_PLAYERS` lists.

**Ranking & scoring (AI_Swap)**
`app_rank`, `score_lookup`, `score_compute`, `score_get`, `score_protect_window`, `score_protect_window_ram`, `is_actively_retained`, `score_touch`, `score_touch_bg`, `check_runaway_growth`.

**Persistence**
`score_save`, `score_compact`, `score_load`, `score_maybe_save`, `rank_cache_save`, `rank_cache_load`, `rank_cache_maybe_save`, `write_pid`.

**Learning**
`ai_learn_log_resolved`, `ai_learn_train_maybe`.

**Foreground tracking**
`track_foreground`, `find_true_foreground`, `is_genuine_fg`, `update_fg_history`, `get_last_fg`.

**Kill mechanics**
`record_kill`, `kill_cooldown_for`, `recently_killed`, `check_bounced`, `do_kill`, `cmp_killable_ram`, `cmp_killable_zram`, `cmp_adaptive_clean`, `oom_pin_retained`, `was_prev_pinned`, `pre_cycle_kill`.

**ZRAM**
`zram_sysfs_rd`, `zram_sysfs_wr`, `zram_orig_data_kb`, `zram_find`, `zram_read_stats`, `zram_setup`, `zram_cycle`, `zram_try_deferred_resize`, `zram_pressure_kill`.

**PSI / swappiness**
`psi_check_available`, `psi_mem_avg10`, `update_swappiness`.

**Screen/idle**
`is_screen_off`, `is_doze_active`, `lmkd_minfree_cleanup`.

**Swap file**
`swap_create`, `swap_delete`.

**Widget provider protection**
`parse_widget_xml`, `load_active_widget_pkgs`, `is_active_widget_provider`.

**Adaptive clean & top-level orchestration**
`adaptive_clean`, `proactive_trim`, `zram_pressure_kill`, `signal_handler`, `run_daemon`, `main`.

---

## 9. Key configuration constants

| Constant | Value | Meaning |
|---|---|---|
| `FREE_HIGH_PCT` / `FREE_LOW_PCT` / `FREE_CRIT_PCT` | 25 / 15 / 8 | RAM `avail_pct` thresholds gating kill activity |
| `ZRAM_SIZE_PCT` | 70 | Target ZRAM size as % of physical RAM |
| `ZRAM_WARN_PCT` / `_TRIM_PCT` / `_CRIT_PCT` | 78 / 83 / 92 | Escalating ZRAM pressure tiers |
| `ZRAM_STUCK_PCT` / `_STUCK_S` | 85 / 60 | Threshold + dwell time before deep clean triggers |
| `DEEPCLEAN_KILL_MAX` | 24 | Max kills per deep-clean pass |
| `SCORE_MAX_APPS` | 200 | Tracked-app cap in `lmk_scores.dat` |
| `SCORE_MAX` | 1000 | Final `score_compute()` clamp |
| `FG_COUNT_MAX` / `FG_COUNT_SCALE` | 100000 / 100 | Raw fg_count ceiling and compression, decoupled from `SCORE_MAX` (1.25) |
| `SCORE_SAVE_INTVL` | 15s | Persistence check interval (dirty-gated, not unconditional) |
| `GROWTH_WINDOW_S` / `_STRIKE_PCT` / `_STRIKES_FLAG` | 600 / 12 / 6 | Runaway-growth detection window, threshold, strikes-to-flag |
| `BOUNCE_SUPPRESS_RC` / `_WINDOW_S` / `_WINDOW_KILLS` | 20 / 60 / 3 | Chronic bouncer suppression thresholds |

Full constants are documented inline in `src/lmk_engine.c` — this table covers the ones most relevant to tuning behavior for a different device.

---

## 10. CLI usage

```
lmk_engine --start          start the daemon
lmk_engine --stop           stop the daemon
lmk_engine --status         show memory/ZRAM/swap status
lmk_engine --log            tail the live log
lmk_engine --swap <MB>      create & enable a file swap
lmk_engine --delswap        remove file swap
lmk_engine --features       print full feature list
```

---

## 11. Known limitations (as of v1.25)

See `CHANGELOG.md` for the full list of fixed-vs-open items. Notable open items at time of writing:
- `warm_relaunch`/`g_scores_dirty` fixes are implemented but **not yet on-device verified**.
- Pin-eviction detection tracks the data needed (`g_prev_pinned_adj`) but doesn't yet compare it against live `oom_adj` to confirm survival.
- Learning model has no L2 regularization, per-feature normalization, or class-imbalance weighting yet.
- `RETAIN_TOP_N` is a smooth-scaled value (per 1.25 changes) but further dynamic tuning is planned.
