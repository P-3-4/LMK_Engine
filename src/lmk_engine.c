/*
 * lmk_engine.c — Android Userspace Memory Manager
 * Version      : 1.25
 *
 * Changes in 1.25 (over 1.24):
 *  - is_screen_off() now also matches "Dozing" in dumpsys power's
 *    mWakefulness field, not just "Asleep". On this Android 16 GSI /
 *    Android 10 vendor combo the device reports "Dozing" and never
 *    progresses to "Asleep", so screen-off was never detected — IDLE-DEEP
 *    tier (and the learning trainer gated behind it) never fired.
 *  - New AppScore.last_bg field, set only on the exact tick a genuinely
 *    foregrounded app leaves true foreground. proactive_trim()'s
 *    warm_relaunch check now reads elapsed time from last_bg instead of
 *    last_fg: last_fg is refreshed to "now" by track_foreground() in the
 *    same tick a relaunch is detected, so the old (now - last_fg) check
 *    always read ~0s and treated every launch as a warm relaunch.
 *  - score_touch()/score_touch_bg() no longer force g_scores_dirty=true
 *    on every single tick. Continuous per-tick last_fg/fg_count refresh
 *    stays in memory as before, but only genuinely save-worthy events
 *    (new tracked app, new session boundary, meaningful avg_swap_kb
 *    move) mark scores dirty now — score_save() was firing its full
 *    200-entry rewrite almost every 15s cycle regardless of real change.
 *  - oom_pin_retained() tie-break: when two candidates have identical
 *    effective score (including PIN_STICKY_BONUS), selection now falls
 *    back to a deterministic name comparison instead of "first match in
 *    table scan order" — scan order shifts tick to tick as /proc listing
 *    order changes, which was still thrashing the pin set on exact ties
 *    even after 1.24's stickiness bonus.
 *  - fg_count accumulation ceiling decoupled from the final score clamp:
 *    new FG_COUNT_MAX (100000 ticks, ~17 days of cumulative foreground
 *    time) replaces the old SCORE_MAX (1000 ticks, ~4h) as the raw
 *    counter cap, with FG_COUNT_SCALE compressing it back down so
 *    Signal-1's contribution to score_compute() keeps the same effective
 *    range as before. Previously nearly every tracked app saturated
 *    fg_count within hours, collapsing Signal 1 into a redundant copy
 *    of the recency signal for the vast majority of entries.
 *
 * Changes in 1.24 (over 1.23):
 *  - AI_Swap learning now actually affects kill decisions: score_compute()
 *    blends the trained g_learn_w logistic model into a bounded bias
 *    (+/-LEARN_SCORE_BIAS_MAX) on top of the existing score. Previously
 *    the model trained and logged weights every 12h but nothing read
 *    them back — zero effect on real behavior.
 *  - ai_learn_train_maybe() now trains on the most recent LEARN_MAX_SAMPLES
 *    rows (circular tail-read) instead of the oldest: the log holds far
 *    more rows than the training cap before rotating, so training was
 *    silently stuck on stale data.
 *  - oom_pin_retained() pin-set stickiness: previously-pinned apps get a
 *    fixed score bonus (PIN_STICKY_BONUS) when reselecting the top-N each
 *    tick, so ties/near-ties at the score ceiling no longer thrash the
 *    pinned set every ~30s on scan-order alone.
 *  - New SERVICE_EXEMPT list (webview sandboxed renderers, carrier :rcs,
 *    GMS quicksearch interactor) routed to PRIO_NEVER in classify(): fixes
 *    both service processes crowding out real apps for retention pin
 *    slots, and the multi-thousand restart_count kill/relaunch loop on
 *    these same processes (Android always relaunches them regardless).
 *  - check_runaway_growth() is now watched for foreground processes too
 *    (previously excluded), so a heavy app that balloons RSS while
 *    actively in use (e.g. an emulator) is already flagged the instant it
 *    backgrounds instead of needing another full growth window to notice.
 *    Foreground processes are still never killed or reprioritized while
 *    actually in the foreground..
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <math.h>

/* ================================================================
 *  VERSION
 * ================================================================ */
#define LMK_VERSION   "1.25"
#define LMK_AUTHOR    "P34"

/* ================================================================
 *  MEMORY PRESSURE THRESHOLDS
 * ================================================================ */
#define FREE_HIGH_PCT    25   /* stop killing above this % avail   */
#define FREE_LOW_PCT     15   /* start killing below this %        */
#define FREE_CRIT_PCT     8   /* aggressive kill tier              */

/* ================================================================
 *  ZRAM SETTINGS
 * ================================================================ */
#define ZRAM_SIZE_PCT    70   /* target ZRAM size as % of physical RAM (was 90; lowered
                                * 1.22 — less ZRAM pressure/CPU decompress overhead)     */
#define ZRAM_WARN_PCT    78   /* early-warning: proactive page drop    */
#define ZRAM_TRIM_PCT    83   /* pressure kill tier                    */
#define ZRAM_CRIT_PCT    92   /* try full cycle                        */

/* Minimum rate at which zram_pressure_kill() is called (seconds). */
#define ZRAM_KILL_INTVL_S    5

/* Minimum avail buffer (kB) above orig_data_size to attempt a cycle. */
#define ZRAM_CYCLE_MARGIN_KB  (200 * 1024)   /* 200 MB safety buffer   */

/* Maximum ZRAM pressure kills per invocation of zram_pressure_kill(). */
#define ZRAM_KILL_BATCH       6

/* ================================================================
 *  ZRAM STUCK / DEEP CLEAN
 * ================================================================ */
#define ZRAM_STUCK_PCT            85   /* "stuck" threshold (%)           */
#define ZRAM_STUCK_S              60   /* seconds at stuck level → action */
#define ZRAM_DEEPCLEAN_CD_S      180   /* min seconds between deep cleans */
#define ZRAM_COMPACT_WAIT_MS    1500   /* ms to wait after zram compact   */
#define ZRAM_CYCLE_IMPOSSIBLE_PCT 75   /* orig_data > this % RAM → no cyc */

/* Deep-clean reclaim-by-kill stage (improved in 1.17) */
#define DEEPCLEAN_KILL_MAX         24   /* max procs killed per deep clean */
#define DEEPCLEAN_KILL_CD_S        20   /* relaxed per-app cooldown floor  */
#define DEEPCLEAN_KILL_TARGET_PCT  25   /* stop once this % of ZRAM freed  */

/* Emergency kill: when ZRAM ≥ STUCK and all blocked by cooldown, a
 * second pass runs with this reduced cooldown floor (seconds). */
#define ZRAM_EMERG_CD_S           10
#define ZRAM_EMERG_KILL_MAX        2

/* ================================================================
 *  CHRONIC BOUNCER SUPPRESSION (improved in 1.14)
 * ================================================================ */
#define BOUNCE_SUPPRESS_RC        20        /* rc ≥ this → suppress ZRAM kill */
#define BOUNCE_SUPPRESS_SWAP_KB   (150*1024) /* override: >150 MB swap = kill  */
#define BOUNCE_WINDOW_S           60        /* time window for bounce count    */
#define BOUNCE_WINDOW_KILLS       3         /* >3 kills in window → suppress   */

/* ================================================================
 *  DEEP-CLEAN FUTILITY DETECTION
 * ================================================================ */
#define DEEPCLEAN_FUTILE_PCT       2    /* ≤2% recovered = futile            */
#define DEEPCLEAN_FUTILE_STRIKES   2    /* strikes before pause              */
#define DEEPCLEAN_FUTILE_PAUSE_S  180   /* 3-minute pause after futile burst */

/* ================================================================
 *  ADAPTIVE CLEAN (tiered, trend-aware)
 * ================================================================ */
#define ADAPTIVE_INTVL_LOW_S      45   /* interval when ZRAM mildly elevated  */
#define ADAPTIVE_INTVL_MED_S      25   /* interval when ZRAM clearly rising   */
#define ADAPTIVE_INTVL_HIGH_S     12   /* interval under heavy pressure       */
#define ADAPTIVE_KILLS_LOW         3   /* max kills at low tier               */
#define ADAPTIVE_KILLS_MED         7   /* max kills at medium tier            */
#define ADAPTIVE_KILLS_HIGH       12   /* max kills at high tier              */
#define ADAPTIVE_TARGET_LOW_PCT   5    /* free 5% of ZRAM disksize            */
#define ADAPTIVE_TARGET_MED_PCT   10   /* free 10%                            */
#define ADAPTIVE_TARGET_HIGH_PCT  25   /* free 25%                            */
#define ADAPTIVE_TREND_WINDOW_S   60   /* seconds to measure ZRAM rise        */
#define ADAPTIVE_RISE_THRESH_PCT   4   /* pct rise over window = "rising"     */
#define ADAPTIVE_INTVL_MAINT_S   120   /* maintenance kill interval (normal)  */
#define ADAPTIVE_KILLS_MAINT       1   /* only 1 stale kill at maintenance    */

/* ================================================================
 *  RANK CACHE  (smarter ZRAM kill ordering across restarts)
 *  Saves avg_swap_kb + computed rank for all running apps every
 *  RANK_CACHE_SAVE_S seconds.  Loaded at startup to pre-populate
 *  avg_swap_kb in AppScore entries so cmp_killable_zram immediately
 *  targets the highest-swap apps even after a daemon restart.
 * ================================================================ */
#define RANK_CACHE_FILE  "/data/local/tmp/lmk_rank.cache"
#define RANK_CACHE_SAVE_S  12   /* write cache every 12 s (was 30s, 1.22)   */

/* ================================================================
 *  IDLE MODE  (in adaptive_clean)
 *  When the phone has had no foreground-app change for IDLE_DETECT_S
 *  seconds (screen likely off / phone sitting idle), run a gentle
 *  COLD+STALE sweep up to IDLE_KILLS_MAX kills every IDLE_INTVL_S.
 *  Only fires when RAM is not already under pressure (pressure tiers
 *  take precedence).
 * ================================================================ */
#define IDLE_DETECT_S     300   /* seconds of no fg change = idle      */
#define IDLE_INTVL_S      120   /* idle clean interval (s) (was 90, 1.22)     */
#define IDLE_KILLS_MAX    2     /* max kills per idle sweep (was 3, 1.22)     */
#define IDLE_MIN_ZRAM_FREE_PCT 20  /* don't idle-kill if ZRAM already >80% */
/* Age-based kill gates within IDLE tier */
#define IDLE_KILL_HOT_AGE_S    (14*60) /* rank2 (HOT): 14 min since last fg (was 7m, 1.22) */
#define IDLE_KILL_LOWER_AGE_S  (6*60)  /* rank3-5: 6 min since last fg (was 3m, 1.22)       */

/* Intense idle clean: device screen-off ≥30 min — best opportunity to
 * sweep harder and run compaction since nothing is user-visible. */
#define IDLE_DEEP_S            1800  /* 30 min continuous screen-off       */
#define IDLE_DEEP_KILLS_MAX    8     /* generous kill budget                */
#define IDLE_DEEP_INTVL_S      300   /* run at most every 5 min             */
#define IDLE_DEEP_TARGET_PCT   15    /* push ZRAM down toward 15%           */
#define IDLE_DEEP_HOT_AGE_S    60    /* relaxed age gate — already idle 30m */
#define IDLE_DEEP_LOWER_AGE_S  20

#define LOG_RATELIMIT_S           30   /* noisy-state logs: once per 30 s    */
/* ================================================================
 *  APP RANKING (NEW in 1.17)
 *  rank 0 = exempt  (HAL / native daemon / never-kill)
 *  rank 1 = FOREGROUND  (currently oom_adj == 0)
 *  rank 2 = HOT         (< 60 s since last foreground)
 *  rank 3 = WARM        (60 s – 10 min)
 *  rank 4 = COLD        (10 min – 1 hr)
 *  rank 5 = STALE       (> 1 hr or never used)
 *  Higher rank number → kill first.
 * ================================================================ */
#define RANK_EXEMPT      0
#define RANK_FOREGROUND  1
#define RANK_HOT         2
#define RANK_WARM        3
#define RANK_COLD        4
#define RANK_STALE       5

#define RANK_FREQ_MED_SESSIONS  8   /* session_count floor: STALE->COLD  (1.23) */
#define RANK_FREQ_HIGH_SESSIONS 20  /* session_count floor: STALE/COLD->WARM (1.23) */
#define RANK_HOT_S       60          /*  <60 s → HOT                   */
#define RANK_WARM_S      (10*60)     /*  <10 min → WARM                */
#define RANK_COLD_S      (60*60)     /*  <1 hr → COLD; else STALE      */

/* ================================================================
 *  FILE SWAP
 * ================================================================ */
#define SWAP_FILE "/data/lmk_swap"

/* ================================================================
 *  OOM ADJ BANDS
 * ================================================================ */
#define ADJ_FOREGROUND    0
#define ADJ_VISIBLE_MAX   200
#define ADJ_SERVICE_MAX   899
#define ADJ_CACHED_MAX    999
#define ADJ_UNKNOWN       1001

/* ================================================================
 *  FOREGROUND PROTECTION WINDOW (seconds)
 * ================================================================ */
#define FG_PROTECT_BASE    90   /* base window for zero-score apps   */
#define FG_PROTECT_MAX    300   /* cap                               */
/* Reduced windows under memory pressure */
#define FG_PROTECT_LOW_PCT  40  /* LOW state: % of computed window   */
#define FG_PROTECT_CRIT_S   20  /* CRITICAL state: flat floor (s)    */

/* ================================================================
 *  APP RETENTION (oom pinning)
 * ================================================================ */
#define RETAIN_TOP_N       3    /* number of apps to pin per tick    */
#define RETAIN_OOM_ADJ   700    /* legacy floor / least-protected pinned slot (kept for --features/log text) */
#define RETAIN_OOM_ADJ_TOP  200 /* 1.25: most-protected (top-ranked) pinned slot */
#define RETAIN_OOM_ADJ_STEP 100 /* 1.25: oom_adj step per rank down from TOP toward the RETAIN_OOM_ADJ floor */
#define RETAIN_FG_AGE_S  600    /* only pin if used within 10 min    */
#define RETAIN_HEADROOM_PCT 35  /* legacy step threshold, kept for --features/log text */
#define RETAIN_BONUS_N      2   /* legacy step bonus, kept for --features/log text */
#define RETAIN_MAX_N        8   /* ceiling on scaled retain count regardless of headroom */
#define RETAIN_PROTECT_GRACE_S 8
/* T3 critical-RAM override sparing — keep a couple of rank-2 (HOT) apps
 * retained even when overriding AI_Swap retention, so the user still has
 * light multitasking (e.g. switching back to the last app or two) instead
 * of every background app getting wiped in one critical sweep. */
#define T3_SPARE_HOT_N      2

/* ================================================================
 *  PROACTIVE LAUNCH TRIM
 * ================================================================ */
#define LAUNCH_TRIM_AVAIL_PCT     FREE_HIGH_PCT  /* only trim if avail below this   */
#define LAUNCH_TRIM_HEAVY_SWAP_KB (150 * 1024)   /* learned avg_swap_kb → "heavy"   */
#define LAUNCH_TRIM_TARGET_PCT    FREE_HIGH_PCT  /* normal trim target               */
#define LAUNCH_TRIM_HEAVY_TARGET_PCT (FREE_HIGH_PCT + 10) /* heavy-app trim target   */
#define LAUNCH_TRIM_WARM_SKIP_S   90  /* skip RAM trim if relaunched within this   */

/* ================================================================
 *  SESSION TRACKING
 * ================================================================ */
#define SESSION_GAP_S     60   /* seconds of absence = new session  */

/* ================================================================
 *  RESTART RESILIENCE
 * ================================================================ */
#define RESTART_WINDOW_S    35
#define RESTART_CD_BASE_S   30
#define RESTART_CD_SCALE_S  20
#define RESTART_CD_MAX_S   180

/* ================================================================
 *  KILL HISTORY / COOLDOWN
 * ================================================================ */
#define KILL_HIST_SIZE   32

/* ================================================================
 *  PROCESS PROTECTION LISTS (generic AOSP)
 * ================================================================ */
static const char * const NEVER_KILL[] = {
    "zygote","zygote64","system_server","surfaceflinger",
    "android.hardware","audioserver","cameraserver","installd","vold","netd",
    "logd","servicemanager","hwservicemanager","vndservicemanager",
    "lmk_engine","init","ueventd",
    "com.android.systemui",
    "android.process.media",
    "com.android.providers.media",
    "com.android.contacts",
    "com.android.calendar",
    "com.android.deskclock",
    NULL
};

static const char * const LAUNCHERS[] = {
    "com.android.launcher","com.android.launcher2","com.android.launcher3",
    "com.google.android.apps.nexuslauncher",
    "com.miui.home","com.huawei.android.launcher",
    "com.sec.android.app.launcher","com.teslacoilsw.launcher",
    "com.microsoft.launcher",
    NULL
};

/* Input method editors (keyboards) — must never be killed; losing the
 * active IME causes input field freezes / fallback-keyboard flicker. */
/* Service helper processes Android relaunches unconditionally no matter
 * how many times they're killed (webview sandboxes, RCS/carrier services,
 * GMS quicksearch interactor). Killing them just burns battery and steals
 * retention pin slots from real user apps for no benefit — fully exempt
 * them instead. (1.24) */
static const char * const SERVICE_EXEMPT[] = {
    "sandboxed_process",               /* webview/chrome sandboxed renderers */
    ":rcs",                            /* carrier RCS messaging service      */
    "googlequicksearchbox:interactor", /* GMS quicksearch interactor         */
    NULL
};

static const char * const IME_PKGS[] = {
    "inputmethod.latin",               /* AOSP LatinIME / OpenBoard */
    "com.google.android.inputmethod",  /* Gboard */
    "com.touchtype.swiftkey",          /* SwiftKey */
    "com.samsung.android.honeyboard",
    "com.baidu.input",
    "com.sohu.inputmethod",
    "com.iflytek.inputmethod",
    "com.komoxo.babelkeyboard",
    NULL
};

static const char * const MEDIA_PLAYERS[] = {
    "spotify","music","youtube","player","audio","podcast","radio",
    "tidal","deezer","soundcloud","pandora","amazon","apple",
    "vlc","audioplayer","media","song","fm",
    NULL
};

/* ================================================================
 *  GLOBALS
 * ================================================================ */
static volatile bool g_running      = true;
static bool          g_zram_active  = false;
static char          g_zram_dev[128] = {0};
static char          g_zram_sys[128] = {0};
static long          g_total_ram_kb  = 0;
static time_t        g_last_kill_t   = 0;
static FILE         *g_log           = NULL;

/* True foreground app tracking (NEW in 1.11), for proactive launch trim */
static char          g_true_fg_name[256] = {0};
static char          g_pending_fg_name[256] = {0};
static int           g_pending_fg_streak = 0;
#define FG_DEBOUNCE_TICKS 2 /* candidate must persist this many ticks before accepted */

/* Deferred ZRAM resize flag (NEW in 1.13): set when zram_setup() adopts
 * ZRAM at the wrong disksize because it was too full to safely swapoff.
 * zram_try_deferred_resize() checks each tick and resizes once ZRAM is
 * below 50% used and avail RAM can absorb the swapoff. */
static bool          g_zram_needs_resize = false;

/* Widget providers (dynamic, refreshed every 5 min) */
static char   g_wpkg[64][128];
static int    g_wpkg_count = 0;
static time_t g_wpkg_ts    = 0;

/* ================================================================
 *  KILL HISTORY (per-app cooldown + restart detection)
 * ================================================================ */
typedef struct {
    char   name[256];
    time_t killed_at;
    bool   bounced;
    /* AI_Swap learning snapshot (1.23) — features at kill time, resolved
     * against `bounced` once RESTART_WINDOW_S has elapsed. */
    int    lrn_score, lrn_rank, lrn_restart_count, lrn_session_count;
    long   lrn_avg_swap_kb;
    bool   lrn_logged;
} KillRecord;
static KillRecord g_kill_hist[KILL_HIST_SIZE];
static int        g_kill_hist_idx = 0;

/* ================================================================
 *  FUTILITY TRACKING
 * ================================================================ */
static int    g_zram_pct_at_start = 0;
static time_t g_zram_kill_start_t = 0;
static time_t g_zram_pause_until  = 0;
#define FUTILITY_WINDOW_S   90
#define FUTILITY_PAUSE_S   120
#define FUTILITY_MIN_DROP    2

/* Rate-limit for zram_pressure_kill() */
static time_t g_zram_kill_last_t  = 0;

/* Rate-limit for oom_pin_retained() to suppress per-tick log spam */
static time_t g_retain_log_t      = 0;
static time_t g_pin_fail_log_t    = 0; /* NEW 1.25: rate-limit for pin-write failure log */
static long   g_pin_write_fail_count = 0; /* NEW 1.25: running count this run          */

/* Adaptive clean trend tracking */
static int    g_adaptive_prev_zram_pct = 0;
static time_t g_adaptive_trend_t       = 0;

/* Idle mode tracking — updated when true-foreground app changes */
static time_t g_last_fg_change_t = 0;
static time_t g_screen_off_since = 0;  /* 0 = screen currently on */

/* Latest ZRAM used% snapshot, refreshed once per main-loop tick — lets
 * AI_Swap retention (score_protect_window_ram) use spare ZRAM headroom
 * resourcefully instead of only reacting to RAM avail_pct. (NEW 1.22) */
static int g_zram_used_pct = 0;

/* Rank cache save timestamp */
static time_t g_rank_cache_last_save = 0;

/* Boot widget-settle: kill paths are suppressed until this timestamp to give
 * AppWidgetService time to write appwidgets.xml after sys.boot_completed. */
static time_t g_widget_settle_until = 0;
#define WIDGET_SETTLE_S  45   /* seconds to wait after boot_completed */

/* Stuck-ZRAM tracking for deep clean */
static time_t g_zram_stuck_since  = 0;   /* when ZRAM first hit STUCK_PCT */
static time_t g_last_deepclean    = 0;   /* timestamp of last deep clean  */
static bool   g_cycle_impossible  = false; /* set once when cycle can't run */

/* ================================================================
 *  NEW IN 1.10: uptime, log rate-limits, deep-clean futility
 * ================================================================ */
#define START_TIME_FILE  "/data/local/tmp/lmk_engine.start"

static time_t g_start_time           = 0;  /* daemon start epoch           */

/* Log rate-limit timestamps (prevent 4+ lines/s when ZRAM saturated) */
static time_t g_log_stuck_t          = 0;  /* last ZRAMstuck cd-active log */
static time_t g_log_crit_t           = 0;  /* last ZRAMcrit log            */
static time_t g_log_precycle_t       = 0;  /* last PreCycle need-X log     */
static time_t g_log_zramcd_t         = 0;  /* last ZRAMcd cooldown log     */

/* Deep-clean futility */
static int    g_deepclean_fail_cnt   = 0;  /* consecutive ≤2% cleans       */
static time_t g_deepclean_pause_until = 0; /* pause end timestamp          */

/* ================================================================
 *  APP USAGE SCORING  (persistent)
 *
 *  Three signals are learned and combined in score_compute():
 *   1. fg_count  — cumulative foreground ticks (duration proxy).
 *                  Decays by halving every 12 h of non-use.
 *   2. session_count — distinct foreground sessions (frequency proxy).
 *                  Decays: halves every day of non-use (NEW 1.14).
 *   3. Recency bonus — derived from last_fg at compute time.
 *
 *  avg_swap_kb tracks the EMA of ZRAM swap usage for each app and
 *  is used by kill ordering (not directly by score_compute).
 *
 *  restart_count tracks how often an app bounces after being killed,
 *  scaling its kill cooldown and deprioritising it in RAM kills.
 * ================================================================ */
#define SCORE_FILE          "/data/local/lmk_scores.dat"
#define SCORE_MAX_APPS      200
#define SCORE_MAX           1000   /* final score_compute() clamp only    */
/* 1.25: fg_count's raw accumulation ceiling, decoupled from SCORE_MAX.
 * At one tick/~15s, the old shared cap (1000) saturated fg_count after
 * only ~4h of cumulative foreground time, after which Signal 1 in
 * score_compute() became identical for every heavily-used app regardless
 * of true usage volume. FG_COUNT_SCALE compresses the raw counter back
 * down before it's added into the score so Signal 1's effective range
 * (0..SCORE_MAX) is unchanged — only the saturation point moves out to
 * ~17 days of cumulative foreground time instead of ~4 hours. */
#define FG_COUNT_MAX        100000
#define FG_COUNT_SCALE      100
#define SCORE_SAVE_INTVL    15    /* persist every 15 s (was 180s, 1.22)   */
#define SCORE_AVG_ALPHA_PCT  25   /* EMA weight for new sample (25%)  */

/* ── Runaway memory-growth detector (NEW 1.22) ──────────────────────
 * Catches long-lived background daemons/services that never settle and
 * instead keep climbing in RSS forever (e.g. a rogue root-app daemon).
 * Native system daemons like system_server are excluded via classify()
 * (PRIO_NEVER / is_rank_exempt) before this ever sees them.           */
#define GROWTH_WINDOW_S        600    /* sample window: 10 min             */
#define GROWTH_MIN_BASE_KB     51200  /* ignore <50MB procs (noise)        */
#define GROWTH_STRIKE_PCT      12     /* % growth in a window = 1 strike   */
#define GROWTH_STRIKES_FLAG    6      /* consecutive strikes (~1h) → flag  */
#define GROWTH_REALERT_S       3600   /* re-log a still-growing flag hourly*/

typedef struct {
    char   name[256];
    int    fg_count;       /* cumulative foreground ticks               */
    time_t last_fg;        /* last foreground timestamp                 */
    int    restart_count;  /* times seen alive <RESTART_WINDOW_S post-kill */
    long   avg_swap_kb;    /* EMA of swap usage in kB                  */
    int    session_count;  /* distinct foreground sessions (v3)         */
    int    category;       /* 0=native daemon, 1=system app, 2=user app */
    int    dc_kill_count;  /* times killed by deep clean (v5)           */
    int    adaptive_kill_count; /* times killed by adaptive clean (v5)  */
    time_t retain_pin_t;   /* last tick this app was actively pinned.
                             * In-memory only — not persisted. */
    time_t last_bg;        /* NEW 1.25: timestamp of the tick this app left
                             * true foreground. In-memory only — not
                             * persisted. Used for warm_relaunch detection;
                             * unlike last_fg it does NOT get refreshed
                             * again while foreground. */
    bool   was_true_fg;    /* NEW 1.25: true-foreground state as of the
                             * previous tick, for edge detection above. */
    /* Runaway-growth watch (NEW 1.22) — in-memory only, not persisted */
    long   rss_watch_kb;      /* RSS sampled at start of current window   */
    time_t rss_watch_t;       /* when the current window started         */
    int    rss_growth_strikes;/* consecutive windows with sustained growth*/
    bool   flagged_runaway;   /* confirmed runaway — bias toward killing  */
    time_t runaway_alert_t;   /* last time we logged the alert            */
} AppScore;

static AppScore g_scores[SCORE_MAX_APPS];
static int      g_score_count     = 0;
static time_t   g_score_last_save = 0;
static bool     g_scores_dirty    = false; /* NEW 1.23: skip save when untouched */
static time_t   g_score_last_compact = 0;  /* NEW 1.23: periodic prune timestamp */
#define SCORE_COMPACT_INTVL_S (24*60*60)  /* run compaction once a day        */
#define SCORE_STALE_PRUNE_S   (30*24*60*60) /* drop entries untouched 30 days */

/* Foreground history – last time each app was in the foreground */
#define FG_HISTORY_SIZE 64
typedef struct { char name[256]; time_t last_seen; } FgHistory;
static FgHistory g_fg_history[FG_HISTORY_SIZE];
static int       g_fg_history_count = 0;

/* ================================================================
 *  TYPES
 * ================================================================ */
typedef enum {
    PRIO_NEVER,
    PRIO_SEMI_PROTECTED,
    PRIO_BACKGROUND,
    PRIO_CACHED,
    PRIO_JUNK
} Priority;

typedef struct {
    pid_t    pid;
    char     name[256];
    long     oom_adj, rss_kb, swap_kb;
    Priority prio;
    time_t   last_fg;
    int      score;
    int      restart_count;
    long     avg_swap_kb;
    int      bounce_count;      /* NEW 1.14: kills in last BOUNCE_WINDOW_S */
    time_t   bounce_first;      /* first kill in the window               */
    int      rank;              /* NEW 1.17: RANK_* (0=exempt … 5=stale)  */
} ProcInfo;

typedef struct {
    long total_kb, free_kb, avail_kb;
    long swap_total_kb, swap_free_kb;
    int  free_pct, avail_pct;
} MemInfo;

typedef struct {
    char dev[128];
    char sys[128];
    bool active;
    long disksize_kb;
    long used_kb;
    long orig_data_kb;
    int  used_pct;
} ZramInfo;

/* ================================================================
 *  LOGGING
 * ================================================================ */
#define LOG_FILE "/data/local/tmp/lmk_engine.log"
#define LOG_MAX  (2 * 1024 * 1024)

static void log_rotate(void) {
    if (!g_log) return;
    fseek(g_log, 0, SEEK_END);
    if (ftell(g_log) > LOG_MAX) {
        fclose(g_log);
        char bak[320]; snprintf(bak, sizeof(bak), "%s.old", LOG_FILE);
        rename(LOG_FILE, bak);
        g_log = fopen(LOG_FILE, "w");
    }
}
static void _log(const char *tag, const char *fmt, va_list ap) {
    FILE *fp = g_log ? g_log : stderr;
    log_rotate();
    time_t now = time(NULL); struct tm *t = localtime(&now);
    char ts[24]; strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", t);
    fprintf(fp, "[%s][%s] ", ts, tag);
    vfprintf(fp, fmt, ap); fputc('\n', fp); fflush(fp);
}
#define DEFLOG(fn,tag) \
    static void fn(const char *fmt,...){ \
        va_list ap;va_start(ap,fmt);_log(tag,fmt,ap);va_end(ap);}
DEFLOG(logi,"INFO ")
DEFLOG(logw,"WARN ")
DEFLOG(loge,"ERROR")
DEFLOG(logk,"KILL ")

/* ================================================================
 *  MEMINFO
 * ================================================================ */
static int read_meminfo(MemInfo *m) {
    FILE *f = fopen("/proc/meminfo","r"); if (!f) return -1;
    memset(m, 0, sizeof(*m));
    char line[128]; long v;
    while (fgets(line, sizeof(line), f)) {
        if      (sscanf(line,"MemTotal: %ld kB",&v)==1)     m->total_kb=v;
        else if (sscanf(line,"MemFree: %ld kB",&v)==1)      m->free_kb=v;
        else if (sscanf(line,"MemAvailable: %ld kB",&v)==1) m->avail_kb=v;
        else if (sscanf(line,"SwapTotal: %ld kB",&v)==1)    m->swap_total_kb=v;
        else if (sscanf(line,"SwapFree: %ld kB",&v)==1)     m->swap_free_kb=v;
    }
    fclose(f);
    if (m->total_kb > 0) {
        m->free_pct  = (int)(m->free_kb  * 100 / m->total_kb);
        m->avail_pct = (int)(m->avail_kb * 100 / m->total_kb);
    }
    if (!g_total_ram_kb && m->total_kb) g_total_ram_kb = m->total_kb;
    return 0;
}

/* ================================================================
 *  PROCESS HELPERS
 * ================================================================ */
static bool name_matches(const char *name, const char * const *list) {
    for (int i = 0; list[i]; i++) if (strstr(name, list[i])) return true;
    return false;
}
static bool is_launcher_like(const char *n) { return name_matches(n, LAUNCHERS); }
static bool is_ime_like(const char *n)      { return name_matches(n, IME_PKGS); }
static bool is_media_player(const char *n)  { return name_matches(n, MEDIA_PLAYERS); }

/* ================================================================
 *  HAL / NATIVE DAEMON DETECTION  (NEW 1.17)
 * ================================================================ */

/* Forward declaration — full definition is in SCORE CATEGORY section below */
static int classify_score_cat(const char *name);

/* Returns true if the process is a hardware abstraction layer (HAL)
 * or low-level vendor daemon that should never be kill-targeted. */
static bool is_hal_process(const char *name) {
    if (!name || !name[0]) return false;
    /* Absolute-path native binaries */
    if (name[0] == '/') return true;
    /* HIDL / AIDL HAL service names */
    if (strncmp(name, "android.hardware.", 17) == 0) return true;
    if (strncmp(name, "android.hidl.",     13) == 0) return true;
    if (strncmp(name, "android.frameworks.",19) == 0) return true;
    if (strncmp(name, "android.system.",   15) == 0) return true;
    if (strncmp(name, "vendor.",             7) == 0) return true;
    /* MTK-specific HAL patterns */
    if (strstr(name, "mediatek"))  return true;
    if (strstr(name, ".hal"))      return true;
    if (strstr(name, "ccci"))      return true;
    return false;
}

/* Returns true if this is a background service sub-process component
 * (colon in the name → spawned by the main process as a private process). */
static bool is_bg_service_proc(const char *name) {
    return name && strchr(name, ':') != NULL;
}

/* Returns true if the process should be exempt from the ranking system
 * entirely (won't be ranked or targeted by recents-dismiss). */
static bool is_rank_exempt(const char *name) {
    if (!name || !name[0]) return true;
    if (is_hal_process(name))    return true;
    if (is_bg_service_proc(name)) return true;
    if (name_matches(name, NEVER_KILL)) return true;
    if (is_launcher_like(name))  return true;
    if (is_ime_like(name))       return true;
    /* System apps (cat 0,1) are exempt from ranking-based kills */
    int cat = classify_score_cat(name);
    return (cat < 2);
}

/* Compute the app rank for a process given its current oom_adj, the last
 * time it was in the foreground, and its session_count (frequency proxy).
 * Frequency blend (1.23): a frequently-used-but-not-recent app gets a
 * floor on how low its rank can fall, so it isn't treated like a true
 * one-off app just because it hasn't been opened in a while. */
static int app_rank(long oom_adj, time_t last_fg, int session_count) {
    if (oom_adj == ADJ_FOREGROUND) return RANK_FOREGROUND;
    if (last_fg == 0)              return RANK_STALE;
    time_t age = time(NULL) - last_fg;
    int rank;
    if (age < RANK_HOT_S)  rank = RANK_HOT;
    else if (age < RANK_WARM_S) rank = RANK_WARM;
    else if (age < RANK_COLD_S) rank = RANK_COLD;
    else rank = RANK_STALE;

    if (session_count >= RANK_FREQ_HIGH_SESSIONS && rank > RANK_WARM)
        rank = RANK_WARM;
    else if (session_count >= RANK_FREQ_MED_SESSIONS && rank > RANK_COLD)
        rank = RANK_COLD;
    return rank;
}

static int proc_cmdline(pid_t pid, char *buf, size_t sz) {
    char path[48]; snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    int n = (int)read(fd, buf, sz-1); close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    for (int i = 0; i < n; i++) if (!buf[i]) { buf[i] = '\0'; break; }
    return 0;
}
/* 1.25: raw open()/read()/close() instead of fopen/fscanf — avoids a
 * stdio buffer allocation per call, same style as proc_cmdline() above.
 * Called for every tracked process every ~15s tick, so this adds up on
 * a low-RAM device even though each individual call is cheap. */
static long proc_oom_adj(pid_t pid) {
    char path[48]; snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", pid);
    int fd = open(path, O_RDONLY); if (fd < 0) return ADJ_UNKNOWN;
    char buf[16];
    int n = (int)read(fd, buf, sizeof(buf)-1); close(fd);
    if (n <= 0) return ADJ_UNKNOWN;
    buf[n] = '\0';
    return atol(buf);
}
static void proc_mem_stats(pid_t pid, long *rss, long *swap) {
    char path[48]; snprintf(path, sizeof(path), "/proc/%d/status", pid);
    *rss = 0; *swap = 0;
    int fd = open(path, O_RDONLY); if (fd < 0) return;
    char buf[4096];
    int n = (int)read(fd, buf, sizeof(buf)-1); close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        long v;
        if      (sscanf(line, "VmRSS: %ld kB", &v)  == 1) *rss  = v;
        else if (sscanf(line, "VmSwap: %ld kB", &v) == 1) *swap = v;
        line = nl ? nl + 1 : NULL;
    }
}

/* ================================================================
 *  ZRAM SYSFS HELPERS
 * ================================================================ */
static long zram_sysfs_rd(const char *sys, const char *attr) {
    char path[256]; snprintf(path, sizeof(path), "%s/%s", sys, attr);
    FILE *f = fopen(path,"r"); if (!f) return -1;
    long v = -1; fscanf(f,"%ld",&v); fclose(f); return v;
}
static void zram_sysfs_wr(const char *sys, const char *attr, const char *val) {
    char path[256]; snprintf(path, sizeof(path), "%s/%s", sys, attr);
    FILE *f = fopen(path,"w"); if (!f) { loge("sysfs write failed: %s", path); return; }
    fputs(val, f); fclose(f);
}

static long zram_orig_data_kb(void) {
    if (!g_zram_sys[0]) return -1;
    char path[256]; snprintf(path, sizeof(path), "%s/mm_stat", g_zram_sys);
    FILE *f = fopen(path,"r"); if (!f) return -1;
    unsigned long long orig = 0;
    fscanf(f, "%llu", &orig);
    fclose(f);
    return (long)(orig / 1024);
}

static bool zram_find(char *dev, size_t dsz, char *sys, size_t ssz) {
    static const char * const C[] = {"/dev/block/zram0","/dev/zram0",NULL};
    for (int i = 0; C[i]; i++) {
        struct stat st;
        if (stat(C[i], &st) == 0 && S_ISBLK(st.st_mode)) {
            snprintf(dev, dsz, "%s", C[i]);
            const char *b = strrchr(C[i], '/'); b = b ? b+1 : C[i];
            snprintf(sys, ssz, "/sys/block/%s", b);
            return true;
        }
    }
    if (system("modprobe zram 2>/dev/null") == 0) {
        struct stat st;
        for (int w = 0; w < 10; w++) {
            if (stat("/sys/block/zram0", &st) == 0) {
                snprintf(dev, dsz, "/dev/block/zram0");
                snprintf(sys, ssz, "/sys/block/zram0");
                return true;
            }
            usleep(200000);
        }
    }
    return false;
}

static void zram_read_stats(ZramInfo *z) {
    memset(z, 0, sizeof(*z));
    if (!g_zram_dev[0]) return;
    snprintf(z->dev, sizeof(z->dev), "%s", g_zram_dev);
    snprintf(z->sys, sizeof(z->sys), "%s", g_zram_sys);
    FILE *f = fopen("/proc/swaps","r");
    if (f) {
        char line[256]; fgets(line, sizeof(line), f);
        while (fgets(line, sizeof(line), f))
            if (strstr(line, g_zram_dev)) { z->active = true; break; }
        fclose(f);
    }
    long ds = zram_sysfs_rd(g_zram_sys, "disksize");
    z->disksize_kb = (ds > 0) ? ds / 1024 : 0;
    MemInfo mi; read_meminfo(&mi);
    z->used_kb = mi.swap_total_kb - mi.swap_free_kb;
    if (z->used_kb < 0) z->used_kb = 0;
    long orig = zram_orig_data_kb();
    z->orig_data_kb = (orig > 0) ? orig : (z->used_kb * 4 / 10);
    if (z->disksize_kb > 0)
        z->used_pct = (int)((z->used_kb * 100) / z->disksize_kb);
    else if (mi.swap_total_kb > 0)
        z->used_pct = (int)((z->used_kb * 100) / mi.swap_total_kb);
    else
        z->used_pct = 0;
}

static void zram_setup(void) {
    if (!g_zram_dev[0] || !g_zram_sys[0]) return;
    if (g_total_ram_kb <= 0) {
        MemInfo mi;
        if (read_meminfo(&mi) != 0 || mi.total_kb <= 0) {
            loge("ZRAM: cannot determine RAM size"); return;
        }
        g_total_ram_kb = mi.total_kb;
    }

    long target_bytes = (long)g_total_ram_kb * 1024LL * ZRAM_SIZE_PCT / 100;
    logi("ZRAM: target %ldMB (%d%% of %ldMB RAM)",
         target_bytes/1024/1024, ZRAM_SIZE_PCT, g_total_ram_kb/1024);

    bool already_active = false;
    FILE *f = fopen("/proc/swaps","r");
    if (f) {
        char line[256]; fgets(line, sizeof(line), f);
        while (fgets(line, sizeof(line), f))
            if (strstr(line, g_zram_dev)) { already_active = true; break; }
        fclose(f);
    }

    long current_bytes = 0;
    long ds = zram_sysfs_rd(g_zram_sys, "disksize");
    if (ds > 0) current_bytes = ds;

    bool within_range = (current_bytes > 0) &&
                        (labs(current_bytes - target_bytes) <= target_bytes / 50);

    if (already_active && within_range) {
        g_zram_active = true;
        logi("ZRAM: already active at %ldMB (within range) — adopted",
             current_bytes/1024/1024);
        return;
    }

    if (already_active && !within_range) {
        logi("ZRAM: active but size %ldMB != target %ldMB — resizing",
             current_bytes/1024/1024, target_bytes/1024/1024);
        ZramInfo z; zram_read_stats(&z);
        if (z.used_pct > 70) {
            logw("ZRAM: too full (%d%%) to safely resize — adopting as-is; "
                 "will resize automatically when ZRAM drops to ≤50%%",
                 z.used_pct);
            g_zram_active       = true;
            g_zram_needs_resize = true;
            return;
        }
        if (swapoff(g_zram_dev) < 0) {
            loge("ZRAM: swapoff failed: %s — adopting as-is", strerror(errno));
            g_zram_active = true;
            return;
        }
        usleep(150000);
        already_active = false;
    }

    if (!already_active) {
        bool setup_ok = false;
        for (int attempt = 0; attempt < 3 && !setup_ok; attempt++) {
            if (attempt > 0) {
                logw("ZRAM: setup retry %d/2 …", attempt);
                usleep(1000000);
            }
            zram_sysfs_wr(g_zram_sys, "reset", "1\n");
            usleep(100000);
            int cpus = sysconf(_SC_NPROCESSORS_CONF);
            if (cpus < 1) cpus = 4;
            char buf[16]; snprintf(buf, sizeof(buf), "%d\n", cpus);
            zram_sysfs_wr(g_zram_sys, "max_comp_streams", buf);
            zram_sysfs_wr(g_zram_sys, "comp_algorithm", "lz4\n");
            usleep(50000);
            char sval[32]; snprintf(sval, sizeof(sval), "%ld\n", target_bytes);
            zram_sysfs_wr(g_zram_sys, "disksize", sval);
            usleep(100000);
            char cmd[200];
            snprintf(cmd, sizeof(cmd), "mkswap %s >/dev/null 2>&1", g_zram_dev);
            if (system(cmd) != 0) {
                loge("ZRAM: mkswap failed (attempt %d)", attempt + 1); continue;
            }
            if (swapon(g_zram_dev, 0) < 0) {
                loge("ZRAM: swapon failed (attempt %d): %s", attempt + 1, strerror(errno));
                continue;
            }
            setup_ok = true;
        }
        if (!setup_ok) { loge("ZRAM: all attempts failed"); return; }
        g_zram_active = true;
        logi("ZRAM: active on %s (%ldMB)", g_zram_dev, target_bytes/1024/1024);
    }
}

static void zram_cycle(void) {
    if (!g_zram_dev[0] || !g_zram_active) return;
    MemInfo mi; read_meminfo(&mi);
    ZramInfo z; zram_read_stats(&z);

    long need_kb = z.orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
    if (mi.avail_kb < need_kb) {
        logi("ZRAMcycle: need %ldMB free (orig=%ldMB + margin), have %ldMB — skip",
             need_kb/1024, z.orig_data_kb/1024, mi.avail_kb/1024);
        return;
    }

    logi("ZRAMcycle: flushing (used=%ldMB orig=%ldMB avail=%ldMB)…",
         z.used_kb/1024, z.orig_data_kb/1024, mi.avail_kb/1024);

    if (swapoff(g_zram_dev) < 0) {
        loge("ZRAMcycle: swapoff: %s", strerror(errno)); return;
    }
    usleep(100000);
    zram_sysfs_wr(g_zram_sys, "reset", "1\n");
    usleep(50000);
    long target_bytes = (long)g_total_ram_kb * 1024LL * ZRAM_SIZE_PCT / 100;
    char sval[32]; snprintf(sval, sizeof(sval), "%ld\n", target_bytes);
    zram_sysfs_wr(g_zram_sys, "disksize", sval);
    char cmd[200]; snprintf(cmd, sizeof(cmd), "mkswap %s >/dev/null 2>&1", g_zram_dev);
    if (system(cmd) != 0) { loge("ZRAMcycle: mkswap failed"); return; }
    if (swapon(g_zram_dev, 0) < 0) { loge("ZRAMcycle: swapon failed"); return; }
    logi("ZRAMcycle: done, ZRAM fresh (%ldMB)", target_bytes/1024/1024);
    g_zram_pct_at_start = 0;
    g_zram_kill_start_t = 0;
    g_zram_pause_until  = 0;
}

static void zram_try_deferred_resize(MemInfo *mi) {
    if (!g_zram_active || !g_zram_dev[0] || !g_zram_sys[0]) return;

    ZramInfo z; zram_read_stats(&z);
    if (z.used_pct > 50) return;
    long need_kb = z.orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
    if (mi->avail_kb < need_kb) return;

    long target_bytes = (long)g_total_ram_kb * 1024LL * ZRAM_SIZE_PCT / 100;
    logi("ZRAMresize: conditions met (ZRAM=%d%% avail=%ldMB orig=%ldMB) — "
         "resizing %ldMB→%ldMB",
         z.used_pct, mi->avail_kb/1024, z.orig_data_kb/1024,
         z.disksize_kb/1024, target_bytes/1024/1024);

    if (swapoff(g_zram_dev) < 0) {
        loge("ZRAMresize: swapoff failed: %s — will retry", strerror(errno));
        return;
    }
    usleep(150000);
    zram_sysfs_wr(g_zram_sys, "reset", "1\n");
    usleep(100000);
    char sval[32]; snprintf(sval, sizeof(sval), "%ld\n", target_bytes);
    zram_sysfs_wr(g_zram_sys, "disksize", sval);
    usleep(100000);
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "mkswap %s >/dev/null 2>&1", g_zram_dev);
    if (system(cmd) != 0) {
        loge("ZRAMresize: mkswap failed — ZRAM disabled");
        g_zram_active = false; g_zram_needs_resize = false; return;
    }
    if (swapon(g_zram_dev, 0) < 0) {
        loge("ZRAMresize: swapon failed: %s — ZRAM disabled", strerror(errno));
        g_zram_active = false; g_zram_needs_resize = false; return;
    }
    g_zram_needs_resize = false;
    g_zram_pct_at_start = 0;
    g_zram_kill_start_t = 0;
    g_zram_pause_until  = 0;
    logi("ZRAMresize: complete — ZRAM now %ldMB (%d%% of RAM)",
         target_bytes/1024/1024, ZRAM_SIZE_PCT);
}

/* ================================================================
 *  PSI (Pressure Stall Information) HELPERS  — from reference engine
 *  Reads /proc/pressure/memory for real kernel memory pressure signal.
 * ================================================================ */
static bool g_psi_available = false;

static void psi_check_available(void) {
    FILE *f = fopen("/proc/pressure/memory", "r");
    if (f) { g_psi_available = true; fclose(f); }
    else    { g_psi_available = false;
              logw("PSI: /proc/pressure/memory unavailable"); }
}

/* Returns memory "some" avg10 PSI stall %, or -1.0 on error. */
static double psi_mem_avg10(void) {
    if (!g_psi_available) return -1.0;
    FILE *f = fopen("/proc/pressure/memory", "r");
    if (!f) return -1.0;
    char line[128]; double avg10 = -1.0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "some", 4) == 0) {
            sscanf(line, "some avg10=%lf", &avg10); break;
        }
    }
    fclose(f); return avg10;
}

#define PSI_MEM_ESCALATE_PCT  8.0   /* avg10 > 8%  → bump adaptive tier +1 */
#define PSI_MEM_URGENT_PCT   25.0   /* avg10 > 25% → bump adaptive tier +2  */

/* ================================================================
 *  DYNAMIC SWAPPINESS — from reference engine concept
 *  Higher ZRAM usage → lower swappiness to preserve headroom.
 * ================================================================ */
static void update_swappiness(int zram_pct) {
    int target;
    if      (zram_pct < 60) target = 100;
    else if (zram_pct < 78) target =  80;
    else if (zram_pct < 83) target =  60;
    else if (zram_pct < 92) target =  40;
    else                    target =  20;
    static int s_last = -1;
    if (target == s_last) return;
    FILE *f = fopen("/proc/sys/vm/swappiness", "w");
    if (f) { fprintf(f, "%d\n", target); fclose(f); }
    logi("Swappiness: ZRAM=%d%% → %d", zram_pct, target);
    s_last = target;
}

/* ================================================================
 *  SLEEP / DOZE DETECTION — from reference engine
 * ================================================================ */
static bool is_screen_off(void) {
    FILE *p = popen("dumpsys power 2>/dev/null | grep -o 'mWakefulness=[A-Za-z]*'", "r");
    if (!p) return false;
    char buf[64] = {0};
    fgets(buf, sizeof(buf), p); pclose(p);
    /* 1.25: this device reports "Dozing" and never progresses to
     * "Asleep", so match both. */
    return strstr(buf, "Asleep") != NULL || strstr(buf, "Dozing") != NULL;
}

static bool is_doze_active(void) {
    FILE *p = popen("dumpsys deviceidle get deep 2>/dev/null", "r");
    if (!p) return false;
    char buf[32] = {0};
    fgets(buf, sizeof(buf), p); pclose(p);
    int n = (int)strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
    return strcmp(buf, "IDLE") == 0;
}

/* ================================================================
 *  LMKD MINFREE CLEANUP — from reference engine fmiop()
 *  Prevents stock LMKD from overriding our kill thresholds.
 * ================================================================ */
static void lmkd_minfree_cleanup(void) {
    if (access("/system/bin/resetprop", X_OK) != 0 &&
        access("/data/adb/magisk/resetprop", X_OK) != 0) return;
    system("resetprop -d sys.lmk.minfree_levels 2>/dev/null; "
           "resetprop lmkd.reinit 1 2>/dev/null");
    logi("LMKD: cleared minfree_levels, reinit sent");
}

/* ================================================================
 *  FILE SWAP
 * ================================================================ */
static void swap_create(int mb) {
    if (mb <= 0) { fprintf(stderr,"Swap size must be >0 MB\n"); return; }
    struct stat st;
    if (stat(SWAP_FILE, &st) == 0) {
        long cur_mb = (long)st.st_size / 1024 / 1024;
        if (cur_mb != mb) {
            swapoff(SWAP_FILE); unlink(SWAP_FILE);
            logi("Swap: removed old file (%ldMB)", cur_mb);
        } else {
            FILE *f = fopen("/proc/swaps","r");
            if (f) {
                char line[256]; bool found = false;
                fgets(line, sizeof(line), f);
                while (fgets(line, sizeof(line), f))
                    if (strstr(line, SWAP_FILE)) { found = true; break; }
                fclose(f);
                if (found) { printf("Swap already active: %s (%dMB)\n", SWAP_FILE, mb); return; }
            }
            goto do_swapon;
        }
    }
    printf("Creating swap file %s (%dMB)…\n", SWAP_FILE, mb);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "dd if=/dev/zero of=%s bs=1048576 count=%d 2>/dev/null", SWAP_FILE, mb);
    if (system(cmd) != 0) { fprintf(stderr,"dd failed\n"); return; }
    chmod(SWAP_FILE, 0600);
    snprintf(cmd, sizeof(cmd), "mkswap %s >/dev/null 2>&1", SWAP_FILE);
    if (system(cmd) != 0) { fprintf(stderr,"mkswap failed\n"); unlink(SWAP_FILE); return; }
do_swapon:
    if (swapon(SWAP_FILE, 0) < 0) { perror("swapon"); return; }
    printf("Swap active: %s (%dMB)\n", SWAP_FILE, mb);
}

static void swap_delete(void) {
    struct stat st;
    if (stat(SWAP_FILE, &st) != 0) { printf("No swap file at %s\n", SWAP_FILE); return; }
    if (swapoff(SWAP_FILE) < 0 && errno != EINVAL)
        fprintf(stderr,"swapoff: %s\n", strerror(errno));
    if (unlink(SWAP_FILE) == 0) printf("Swap removed: %s\n", SWAP_FILE);
    else perror(SWAP_FILE);
}

/* ================================================================
 *  ACTIVE WIDGET PROVIDERS
 * ================================================================ */
#define APPWIDGET_XML     "/data/system/appwidgets.xml"
#define APPWIDGET_XML_U0  "/data/system/users/0/appwidgets.xml"
#define WIDGET_RELOAD_S    300
#define WIDGET_RETRY_S      15   /* short retry until first successful load   */

/* Parse widget package names from an open XML file into g_wpkg[]. */
static void parse_widget_xml(FILE *f) {
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while ((p = strstr(p, "pkg=\"")) != NULL) {
            p += 5; char *end = strchr(p, '"'); if (!end) break;
            int len = (int)(end - p);
            if (len <= 0 || len >= 128) { p = end+1; continue; }
            bool dup = false;
            for (int j = 0; j < g_wpkg_count && !dup; j++)
                dup = (strncmp(g_wpkg[j], p, len) == 0 && g_wpkg[j][len] == '\0');
            if (!dup && g_wpkg_count < 64) {
                strncpy(g_wpkg[g_wpkg_count], p, len);
                g_wpkg[g_wpkg_count][len] = '\0';
                g_wpkg_count++;
            }
            p = end + 1;
        }
    }
}

static void load_active_widget_pkgs(void) {
    time_t now = time(NULL);
    int intvl = (g_wpkg_count > 0) ? WIDGET_RELOAD_S : WIDGET_RETRY_S;
    if (g_wpkg_ts > 0 && now - g_wpkg_ts < intvl) return;
    g_wpkg_ts = now;

    /* Candidate XML paths: system-level + per-user (users/0 .. users/9) */
    const char *primary_paths[] = {
        APPWIDGET_XML, APPWIDGET_XML_U0, NULL
    };
    /* Also try users/1..9 for secondary profiles */
    char extra[10][64];
    const char *extra_ptrs[11]; int ne = 0;
    for (int u = 1; u <= 9; u++) {
        snprintf(extra[ne], sizeof(extra[ne]),
                 "/data/system/users/%d/appwidgets.xml", u);
        struct stat st;
        if (stat(extra[ne], &st) == 0) { extra_ptrs[ne] = extra[ne]; ne++; }
    }
    extra_ptrs[ne] = NULL;

    /* Try all paths until at least one loads */
    bool loaded_any = false;
    int tmp_count = 0;
    char tmp_pkgs[64][128];

    /* Inline parse into tmp buffer first so we don't clobber on partial read */
    for (int pi = 0; primary_paths[pi]; pi++) {
        FILE *f = fopen(primary_paths[pi], "r");
        if (!f) continue;
        /* parse into g_wpkg temporarily */
        int save = g_wpkg_count;
        g_wpkg_count = tmp_count;
        /* copy tmp back */
        memcpy(g_wpkg, tmp_pkgs, sizeof(char) * tmp_count * 128);
        parse_widget_xml(f);
        fclose(f);
        tmp_count = g_wpkg_count;
        memcpy(tmp_pkgs, g_wpkg, sizeof(char) * tmp_count * 128);
        g_wpkg_count = save;
        loaded_any = true;
    }
    for (int pi = 0; extra_ptrs[pi]; pi++) {
        FILE *f = fopen(extra_ptrs[pi], "r");
        if (!f) continue;
        int save = g_wpkg_count;
        g_wpkg_count = tmp_count;
        memcpy(g_wpkg, tmp_pkgs, sizeof(char) * tmp_count * 128);
        parse_widget_xml(f);
        fclose(f);
        tmp_count = g_wpkg_count;
        memcpy(tmp_pkgs, g_wpkg, sizeof(char) * tmp_count * 128);
        g_wpkg_count = save;
        loaded_any = true;
    }

    /* Fallback: cmd appwidget list (Android 7+) */
    if (!loaded_any || tmp_count == 0) {
        FILE *cmd = popen("cmd appwidget list 2>/dev/null", "r");
        if (cmd) {
            char buf[256];
            while (fgets(buf, sizeof(buf), cmd)) {
                /* Lines look like: "package=com.example.widget ..." */
                char *kw = strstr(buf, "package=");
                if (!kw) kw = strstr(buf, "pkg=");
                if (!kw) continue;
                kw += (kw[1] == 'k') ? 4 : 8; /* skip "pkg=" or "package=" */
                /* Strip to first space or newline */
                char pkg[128]; int n = 0;
                while (kw[n] && kw[n] != ' ' && kw[n] != '\n' && n < 127) {
                    pkg[n] = kw[n]; n++;
                }
                pkg[n] = '\0';
                if (n > 0 && tmp_count < 64) {
                    bool dup = false;
                    for (int j = 0; j < tmp_count && !dup; j++)
                        dup = strcmp(tmp_pkgs[j], pkg) == 0;
                    if (!dup) { strncpy(tmp_pkgs[tmp_count++], pkg, 127); loaded_any = true; }
                }
            }
            pclose(cmd);
        }
    }

    if (!loaded_any) {
        logi("Widget: no source readable – retry in %ds", WIDGET_RETRY_S);
        return;
    }

    /* Commit: only update the live list once we have a result */
    g_wpkg_count = tmp_count;
    memcpy(g_wpkg, tmp_pkgs, sizeof(char) * tmp_count * 128);
    if (g_wpkg_count > 0)
        logi("Widget: %d active provider(s) loaded", g_wpkg_count);
}

static bool is_active_widget_provider(const char *name) {
    for (int i = 0; i < g_wpkg_count; i++)
        if (g_wpkg[i][0] && strstr(name, g_wpkg[i])) return true;
    return false;
}

/* ================================================================
 *  FOREGROUND HISTORY
 * ================================================================ */
static void update_fg_history(const char *name, time_t now) {
    if (!name[0]) return;
    for (int i = 0; i < g_fg_history_count; i++) {
        if (strcmp(g_fg_history[i].name, name) == 0) {
            g_fg_history[i].last_seen = now; return;
        }
    }
    if (g_fg_history_count < FG_HISTORY_SIZE) {
        strncpy(g_fg_history[g_fg_history_count].name, name,
                sizeof(g_fg_history[0].name)-1);
        g_fg_history[g_fg_history_count].last_seen = now;
        g_fg_history_count++;
    } else {
        int oldest = 0;
        for (int i = 1; i < FG_HISTORY_SIZE; i++)
            if (g_fg_history[i].last_seen < g_fg_history[oldest].last_seen)
                oldest = i;
        strncpy(g_fg_history[oldest].name, name, sizeof(g_fg_history[0].name)-1);
        g_fg_history[oldest].last_seen = now;
    }
}

static time_t get_last_fg(const char *name) {
    for (int i = 0; i < g_fg_history_count; i++)
        if (strcmp(g_fg_history[i].name, name) == 0)
            return g_fg_history[i].last_seen;
    return 0;
}

/* ================================================================
 *  KILL HISTORY + COOLDOWN
 * ================================================================ */
static int kill_cooldown_for(const char *name) {
    for (int i = 0; i < g_score_count; i++) {
        if (strcmp(g_scores[i].name, name) == 0) {
            int rc = g_scores[i].restart_count;
            int cd = RESTART_CD_BASE_S + rc * RESTART_CD_SCALE_S;
            return cd > RESTART_CD_MAX_S ? RESTART_CD_MAX_S : cd;
        }
    }
    return RESTART_CD_BASE_S;
}

static bool recently_killed(const char *name) {
    time_t now = time(NULL);
    int cooldown = kill_cooldown_for(name);
    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        if (g_kill_hist[i].name[0] &&
            strcmp(g_kill_hist[i].name, name) == 0 &&
            (now - g_kill_hist[i].killed_at) < cooldown)
            return true;
    }
    return false;
}

/* Forward decls: defined later, needed by record_kill()'s learning snapshot */
static AppScore *score_lookup(const char *name);
static int score_compute(const AppScore *s);

static void record_kill(ProcInfo *p) {
    time_t now = time(NULL);
    const char *name = p->name;
    AppScore *as = score_lookup(name);
    int sc = 0, si = 0, ri = 0, rk = p->rank; long avgsw = 0;
    if (as) { sc = score_compute(as); si = as->session_count;
              ri = as->restart_count; avgsw = as->avg_swap_kb; }

    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        if (strcmp(g_kill_hist[i].name, name) == 0) {
            g_kill_hist[i].killed_at = now;
            g_kill_hist[i].bounced   = false;
            g_kill_hist[i].lrn_logged = false;
            g_kill_hist[i].lrn_score = sc; g_kill_hist[i].lrn_rank = rk;
            g_kill_hist[i].lrn_restart_count = ri; g_kill_hist[i].lrn_session_count = si;
            g_kill_hist[i].lrn_avg_swap_kb = avgsw;
            return;
        }
    }
    strncpy(g_kill_hist[g_kill_hist_idx].name, name, 255);
    g_kill_hist[g_kill_hist_idx].name[255]   = '\0';
    g_kill_hist[g_kill_hist_idx].killed_at   = now;
    g_kill_hist[g_kill_hist_idx].bounced     = false;
    g_kill_hist[g_kill_hist_idx].lrn_logged  = false;
    g_kill_hist[g_kill_hist_idx].lrn_score = sc; g_kill_hist[g_kill_hist_idx].lrn_rank = rk;
    g_kill_hist[g_kill_hist_idx].lrn_restart_count = ri;
    g_kill_hist[g_kill_hist_idx].lrn_session_count = si;
    g_kill_hist[g_kill_hist_idx].lrn_avg_swap_kb = avgsw;
    g_kill_hist_idx = (g_kill_hist_idx + 1) % KILL_HIST_SIZE;
}

/* ================================================================
 *  AI_SWAP LEARNING (NEW 1.23) — self-contained, C-only, no external
 *  libs/process. Every kill's features are snapshotted in record_kill()
 *  (lrn_* fields); once RESTART_WINDOW_S resolves the bounce outcome,
 *  ai_learn_log_resolved() appends one line to LEARN_LOG_FILE. Once a
 *  day, gated to run only during IDLE-DEEP (tier==-3, so training never
 *  competes with foreground responsiveness), ai_learn_train_maybe() reads
 *  the log and fits a small logistic-regression model via plain gradient
 *  descent. Weights are logged only — not yet fed back into scoring
 *  (future work item).
 * ================================================================ */
#define LEARN_LOG_FILE       "/data/local/lmk_learn.log"
#define LEARN_LOG_MAX_BYTES  (256*1024)  /* rotate (truncate) above this size */
#define LEARN_TRAIN_INTVL_S  (12*60*60)  /* retrain at most every 12h (was 24h) */
#define LEARN_GD_EPOCHS       200
#define LEARN_GD_LR            0.05
#define LEARN_NFEAT            5   /* score, rank, restart_count, session_count, avg_swap_kb */
#define LEARN_MAX_SAMPLES     2000 /* bounded read of the log for training    */
#define LEARN_MIN_SAMPLES       20 /* skip training below this many rows      */
#define LEARN_SCORE_BIAS_MAX    60 /* cap on learned bias folded into score (1.24) */

static double g_learn_w[LEARN_NFEAT + 1] = {0}; /* [0]=bias, learned weights  */
static time_t g_learn_last_train = 0;

/* Append one resolved (features, outcome) row per kill once its bounce
 * window has closed. Cheap: scans the fixed-size KILL_HIST_SIZE ring. */
static void ai_learn_log_resolved(void) {
    time_t now = time(NULL);
    bool any = false;
    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        KillRecord *k = &g_kill_hist[i];
        if (!k->name[0] || k->lrn_logged) continue;
        if ((now - k->killed_at) < RESTART_WINDOW_S) continue;
        any = true;
        break;
    }
    if (!any) return;

    struct stat st;
    if (stat(LEARN_LOG_FILE, &st) == 0 && st.st_size > LEARN_LOG_MAX_BYTES) {
        FILE *tf = fopen(LEARN_LOG_FILE, "w");
        if (tf) fclose(tf);
        logi("Learn: %s rotated (exceeded %dKB)", LEARN_LOG_FILE, LEARN_LOG_MAX_BYTES/1024);
    }

    FILE *f = fopen(LEARN_LOG_FILE, "a");
    if (!f) return;
    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        KillRecord *k = &g_kill_hist[i];
        if (!k->name[0] || k->lrn_logged) continue;
        if ((now - k->killed_at) < RESTART_WINDOW_S) continue;
        /* score rank restart_count session_count avg_swap_kb bounced */
        fprintf(f, "%d %d %d %d %ld %d\n",
                k->lrn_score, k->lrn_rank, k->lrn_restart_count,
                k->lrn_session_count, k->lrn_avg_swap_kb, k->bounced ? 1 : 0);
        k->lrn_logged = true;
    }
    fclose(f);
}

/* Once/day, IDLE-DEEP only: fit logistic-regression weights over the
 * logged (features, bounced) rows via plain batch gradient descent.
 * No Python, no external libs — this is the entire "model". */
static void ai_learn_train_maybe(int tier) {
    if (tier != -3) return; /* IDLE-DEEP gate: never competes with fg work */
    time_t now = time(NULL);
    if (now - g_learn_last_train < LEARN_TRAIN_INTVL_S) return;
    g_learn_last_train = now;

    FILE *f = fopen(LEARN_LOG_FILE, "r");
    if (!f) return;

    static double feat[LEARN_MAX_SAMPLES][LEARN_NFEAT];
    static int    label[LEARN_MAX_SAMPLES];
    int n = 0;
    long total = 0;
    char line[256];
    /* 1.24: circular tail-read fix. The log holds far more rows than
     * LEARN_MAX_SAMPLES before rotating, so reading straight from the
     * head (as before) always trained on the oldest samples and never
     * saw the most recent ~8000 rows. Read the whole file but overwrite
     * the buffer circularly, so it ends up holding only the most recent
     * LEARN_MAX_SAMPLES rows regardless of total log size. */
    while (fgets(line, sizeof(line), f)) {
        int sc, rk, rc, si, bounced; long avgsw;
        if (sscanf(line, "%d %d %d %d %ld %d", &sc, &rk, &rc, &si, &avgsw, &bounced) == 6) {
            int idx = (int)(total % LEARN_MAX_SAMPLES);
            feat[idx][0] = sc   / 100.0;
            feat[idx][1] = rk   / 5.0;
            feat[idx][2] = rc   / 10.0;
            feat[idx][3] = si   / 20.0;
            feat[idx][4] = avgsw / 100000.0;
            label[idx]   = bounced;
            total++;
            if (n < LEARN_MAX_SAMPLES) n++;
        }
    }
    fclose(f);
    if (n < LEARN_MIN_SAMPLES) {
        logi("Learn: only %d samples logged, skipping today's training", n);
        return;
    }

    double w[LEARN_NFEAT + 1] = {0};
    for (int e = 0; e < LEARN_GD_EPOCHS; e++) {
        double grad[LEARN_NFEAT + 1] = {0};
        for (int i = 0; i < n; i++) {
            double z = w[0];
            for (int k = 0; k < LEARN_NFEAT; k++) z += w[k+1] * feat[i][k];
            double pred = 1.0 / (1.0 + exp(-z));
            double err  = pred - label[i];
            grad[0] += err;
            for (int k = 0; k < LEARN_NFEAT; k++) grad[k+1] += err * feat[i][k];
        }
        for (int k = 0; k <= LEARN_NFEAT; k++)
            w[k] -= LEARN_GD_LR * grad[k] / n;
    }
    memcpy(g_learn_w, w, sizeof(w));
    logi("Learn: retrained on %d samples — w[bias=%.3f score=%.3f rank=%.3f "
         "restart=%.3f sess=%.3f swap=%.3f]",
         n, w[0], w[1], w[2], w[3], w[4], w[5]);
}

static void check_bounced(const char *name) {
    time_t now = time(NULL);
    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        if (g_kill_hist[i].name[0] &&
            strcmp(g_kill_hist[i].name, name) == 0 &&
            !g_kill_hist[i].bounced &&
            (now - g_kill_hist[i].killed_at) < RESTART_WINDOW_S) {
            g_kill_hist[i].bounced = true;
            for (int j = 0; j < g_score_count; j++) {
                if (strcmp(g_scores[j].name, name) == 0) {
                    g_scores[j].restart_count++;
                    g_scores_dirty = true;
                    logi("Resilient: %s restart_count=%d (cd=%ds)",
                         name, g_scores[j].restart_count,
                         kill_cooldown_for(name));
                    break;
                }
            }
            break;
        }
    }
}

/* ================================================================
 *  SCORE CATEGORY CLASSIFIER
 * ================================================================ */
static int classify_score_cat(const char *name) {
    if (name[0] == '/') return 0;
    static const char * const NATIVE_NAMES[] = {
        "zygote","zygote64","webview_zygote","system_server",
        "magiskd","logcat","top","su","lmk_engine",
        "<pre-initialized>",
        NULL
    };
    for (int i = 0; NATIVE_NAMES[i]; i++)
        if (!strcmp(name, NATIVE_NAMES[i])) return 0;
    if (strncmp(name, "media.", 6) == 0) return 0;

    if (strncmp(name, "com.android.",  12) == 0) return 1;
    if (strncmp(name, "android.",       8) == 0) return 1;
    if (strncmp(name, "org.lineageos.", 14) == 0) return 1;
    if (strncmp(name, "org.protonaosp.",15)== 0) return 1;
    if (strncmp(name, "io.chaldeaprjkt.",16)== 0) return 1;
    if (strncmp(name, "co.aospa.",      9) == 0) return 1;
    if (strncmp(name, "me.phh.",         7) == 0) return 1;

    return 2;
}

/* ================================================================
 *  APP USAGE SCORING
 * ================================================================ */
static AppScore *score_lookup(const char *name) {
    for (int i = 0; i < g_score_count; i++)
        if (strcmp(g_scores[i].name, name) == 0) return &g_scores[i];
    return NULL;
}

/*
 * score_compute() — multi-signal importance score (0..SCORE_MAX).
 * Improved in 1.14: session_count decays every day of non-use.
 */
static int score_compute(const AppScore *s) {
    if (!s || s->last_fg == 0) return 0;
    time_t now = time(NULL);
    long idle_h = (now - s->last_fg) / 3600;
    long idle_d = idle_h / 24;

    /* Signal 1: decayed tick score (1.25: fg_count now accumulates up to
     * FG_COUNT_MAX, so scale it back down to keep this signal's range the
     * same as before — see FG_COUNT_MAX/FG_COUNT_SCALE comment above). */
    int raw_ticks = s->fg_count < FG_COUNT_MAX ? s->fg_count : FG_COUNT_MAX;
    int ticks = raw_ticks / FG_COUNT_SCALE;
    if (ticks > SCORE_MAX) ticks = SCORE_MAX;
    int steps = (int)(idle_h / 12);
    for (int i = 0; i < steps && ticks > 0; i++) ticks >>= 1;

    /* Signal 2: session frequency bonus with decay per day */
    int session_count = s->session_count;
    for (int d = 0; d < idle_d && session_count > 0; d++) session_count >>= 1;
    int freq_bonus = 0;
    if (session_count > 0) {
        long window_h = idle_h > 168 ? 168 : (idle_h < 1 ? 1 : idle_h);
        freq_bonus = (int)((long)session_count * 240 / window_h);
        if (freq_bonus > 400) freq_bonus = 400;
    }

    /* Signal 3: recency bonus */
    int recency = 0;
    if      (idle_h == 0)  recency = 100;
    else if (idle_h < 4)   recency =  50;
    else if (idle_h < 24)  recency =  20;

    int sc = ticks + freq_bonus + recency;

    /* 1.24: fold in the AI_Swap learned weights as a bounded bias.
     * g_learn_w[] predicts P(bounce | features) from past kills; a high
     * predicted bounce probability means "killing this tends to backfire"
     * so nudge its importance score up, and vice versa. Rank isn't known
     * at this call site, so that feature term is left at 0 (neutral) —
     * a rough blend beats the previous zero effect the trained model had. */
    {
        double z = g_learn_w[0]
                 + g_learn_w[1] * (sc / 100.0)
                 + g_learn_w[3] * (s->restart_count / 10.0)
                 + g_learn_w[4] * (s->session_count / 20.0)
                 + g_learn_w[5] * (s->avg_swap_kb / 100000.0);
        double pred = 1.0 / (1.0 + exp(-z));
        int bias = (int)((pred - 0.5) * 2.0 * LEARN_SCORE_BIAS_MAX);
        sc += bias;
    }

    return sc > SCORE_MAX ? SCORE_MAX : (sc < 0 ? 0 : sc);
}

static int score_get(const char *name) {
    AppScore *s = score_lookup(name);
    return s ? score_compute(s) : 0;
}

static int score_protect_window_ram(const char *name, int avail_pct) {
    int sc  = score_get(name);
    int win = FG_PROTECT_BASE + sc;
    if (win > FG_PROTECT_MAX) win = FG_PROTECT_MAX;

    if (avail_pct < FREE_CRIT_PCT)  return FG_PROTECT_CRIT_S;
    if (avail_pct < FREE_LOW_PCT)   return win * FG_PROTECT_LOW_PCT / 100;

    /* ZRAM-resourceful retention (NEW 1.22): plenty of spare ZRAM capacity
     * and comfortable RAM → retain a bit longer since swapping the app
     * out costs nothing we can't spare. ZRAM getting tight → trim back
     * so we don't feed the compression/decompression CPU cost further. */
    if (g_zram_used_pct > 0 && g_zram_used_pct < (ZRAM_WARN_PCT - 15))
        win = win * 130 / 100;
    else if (g_zram_used_pct >= ZRAM_TRIM_PCT)
        win = win * 80 / 100;
    if (win > FG_PROTECT_MAX) win = FG_PROTECT_MAX;

    return win;
}

static int score_protect_window(const char *name) {
    return score_protect_window_ram(name, FREE_HIGH_PCT);
}

static bool is_actively_retained(const char *name) {
    AppScore *s = score_lookup(name);
    return s && s->retain_pin_t > 0 &&
           (time(NULL) - s->retain_pin_t) < RETAIN_PROTECT_GRACE_S;
}

static void score_touch(const char *name, time_t now, long swap_kb) {
    if (!name[0]) return;
    AppScore *s = score_lookup(name);
    if (!s) {
        if (g_score_count >= SCORE_MAX_APPS) {
            int min_idx = 0, min_sc = score_compute(&g_scores[0]);
            for (int i = 1; i < g_score_count; i++) {
                int sc = score_compute(&g_scores[i]);
                if (sc < min_sc) { min_sc = sc; min_idx = i; }
            }
            s = &g_scores[min_idx];
        } else {
            s = &g_scores[g_score_count++];
        }
        memset(s, 0, sizeof(*s));
        strncpy(s->name, name, 255); s->name[255] = '\0';
        s->category = classify_score_cat(name);
        g_scores_dirty = true;
    }

    bool new_session = (s->last_fg == 0 ||
                        (now - s->last_fg) >= SESSION_GAP_S);
    if (new_session && s->session_count < 99999) {
        s->session_count++;
        g_scores_dirty = true;   /* session boundary is worth persisting */
    }

    /* last_fg/fg_count keep refreshing every tick in memory as before —
     * other logic (protect windows, retention, etc.) depends on last_fg
     * tracking true real-time recency. But that per-tick refresh alone no
     * longer forces a disk write (1.25): it was making g_scores_dirty
     * true on almost every 15s cycle regardless of any real change. */
    s->last_fg = now;
    if (s->fg_count < FG_COUNT_MAX) s->fg_count++;

    if (swap_kb >= 0) {
        long old_avg = s->avg_swap_kb;
        s->avg_swap_kb = (s->avg_swap_kb * (100 - SCORE_AVG_ALPHA_PCT) +
                          swap_kb * SCORE_AVG_ALPHA_PCT) / 100;
        /* Only a meaningful swap move is worth persisting; EMA noise
         * every tick isn't. */
        if (labs(s->avg_swap_kb - old_avg) >= 1024) g_scores_dirty = true;
    }
}

/* Lightweight touch for background-visible launches (foreground services,
 * perceptible adj 1-200 that were never truly user-foregrounded — e.g. a
 * music/location/notification service spun up by the system). Updates
 * recency only; does NOT inflate fg_count/session_count, which must stay
 * a measure of genuine user engagement for AI_Swap scoring. */
static void score_touch_bg(const char *name, time_t now, long swap_kb) {
    if (!name[0]) return;
    AppScore *s = score_lookup(name);
    if (!s) return; /* don't create new score entries for bg-only activity */
    s->last_fg = now;   /* in-memory recency refresh only, see score_touch() */
    if (swap_kb >= 0) {
        long old_avg = s->avg_swap_kb;
        s->avg_swap_kb = (s->avg_swap_kb * (100 - SCORE_AVG_ALPHA_PCT) +
                          swap_kb * SCORE_AVG_ALPHA_PCT) / 100;
        if (labs(s->avg_swap_kb - old_avg) >= 1024) g_scores_dirty = true;
    }
}

/* Evaluate one process against its persisted growth-watch window and
 * update strike/flag state. Called once per tick from enumerate_procs()
 * for every non-exempt process — cheap (a handful of int ops). */
static void check_runaway_growth(const char *name, time_t now, long rss_kb) {
    if (!name[0] || rss_kb < GROWTH_MIN_BASE_KB) return;
    AppScore *s = score_lookup(name);
    if (!s) return;

    if (s->rss_watch_t == 0) {
        s->rss_watch_kb = rss_kb;
        s->rss_watch_t  = now;
        return;
    }
    if (now - s->rss_watch_t < GROWTH_WINDOW_S) return;

    long base = s->rss_watch_kb > 0 ? s->rss_watch_kb : 1;
    long grew_pct = ((rss_kb - base) * 100) / base;

    if (grew_pct >= GROWTH_STRIKE_PCT) {
        s->rss_growth_strikes++;
    } else {
        s->rss_growth_strikes = 0;
        s->flagged_runaway    = false;
    }

    if (s->rss_growth_strikes >= GROWTH_STRIKES_FLAG) {
        if (!s->flagged_runaway || (now - s->runaway_alert_t) >= GROWTH_REALERT_S) {
            logw("[GROWTH] '%s' looks like a RUNAWAY: %ldMB -> %ldMB over ~%dm "
                 "(%d consecutive growth windows) — deprioritizing for kill",
                 name, base/1024, rss_kb/1024,
                 (int)((now - (s->rss_watch_t - GROWTH_WINDOW_S)) / 60),
                 s->rss_growth_strikes);
            s->runaway_alert_t = now;
        }
        s->flagged_runaway = true;
    }

    /* Slide the window forward regardless of outcome */
    s->rss_watch_kb = rss_kb;
    s->rss_watch_t  = now;
}

static void score_save(void) {
    if (!g_scores_dirty) return; /* NEW 1.23: nothing changed since last write */

    FILE *f = fopen(SCORE_FILE, "w");
    if (!f) { loge("Scores: cannot write"); return; }

    fprintf(f, "# lmk usage scores v5\n");

    static const char * const SEC_TAG[]  = { "NATIVE", "SYSAPP", "USER" };
    static const char * const SEC_DESC[] = {
        "native daemons  (kernel / HAL / bionic)",
        "system app processes  (com.android.* / lineageos / OEM)",
        "user-installed apps"
    };

    int saved = 0;
    for (int cat = 0; cat < 3; cat++) {
        bool wrote_hdr = false;
        for (int i = 0; i < g_score_count; i++) {
            AppScore *s = &g_scores[i];
            if (s->fg_count == 0) continue;
            int c = s->category;
            if (c < 0 || c > 2) c = classify_score_cat(s->name);
            if (c != cat) continue;
            if (!wrote_hdr) {
                fprintf(f, "#\n# [%s] %s\n", SEC_TAG[cat], SEC_DESC[cat]);
                wrote_hdr = true;
            }
            /* v5: name fg_count last_fg restart_count avg_swap_kb session_count
             *         category dc_kill_count adaptive_kill_count */
            fprintf(f, "%s %d %ld %d %ld %d %d %d %d\n",
                    s->name, s->fg_count, (long)s->last_fg,
                    s->restart_count, s->avg_swap_kb,
                    s->session_count, c,
                    s->dc_kill_count, s->adaptive_kill_count);
            saved++;
        }
    }
    fclose(f);
    g_scores_dirty = false;
    logi("Scores: saved %d entries (v5)", saved);
}

/* Periodic compaction (NEW 1.23): drop entries that haven't been touched
 * in SCORE_STALE_PRUNE_S (uninstalled/abandoned apps), keeping the file
 * and in-memory table from growing forever with dead data. Runs at most
 * once every SCORE_COMPACT_INTVL_S. */
static void score_compact(void) {
    time_t now = time(NULL);
    if (now - g_score_last_compact < SCORE_COMPACT_INTVL_S) return;
    g_score_last_compact = now;

    int kept = 0, pruned = 0;
    for (int i = 0; i < g_score_count; i++) {
        AppScore *s = &g_scores[i];
        if (s->last_fg != 0 && (now - s->last_fg) > SCORE_STALE_PRUNE_S) {
            pruned++;
            continue;
        }
        if (kept != i) g_scores[kept] = *s;
        kept++;
    }
    if (pruned > 0) {
        g_score_count  = kept;
        g_scores_dirty = true;
        logi("Scores: compacted, pruned %d stale entr%s (%d remain)",
             pruned, pruned == 1 ? "y" : "ies", kept);
    }
}

static void score_load(void) {
    FILE *f = fopen(SCORE_FILE,"r");
    if (!f) { logi("Scores: no data file – starting fresh"); return; }
    char line[512]; int loaded = 0;
    while (fgets(line, sizeof(line), f) && g_score_count < SCORE_MAX_APPS) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char pkg[256]; int cnt; long ts; int rc; long avg_sw; int ses; int cat;
        int dckills, adpkills;
        int fields = sscanf(line, "%255s %d %ld %d %ld %d %d %d %d",
                            pkg, &cnt, &ts, &rc, &avg_sw, &ses, &cat,
                            &dckills, &adpkills);
        if (fields < 3 || cnt <= 0) continue;
        if (fields < 5) { rc = 0; avg_sw = 0; }
        if (fields < 6) { ses = 0; }
        if (fields < 7) { cat = classify_score_cat(pkg); }
        if (fields < 8) { dckills = 0; }
        if (fields < 9) { adpkills = 0; }
        if (cat < 0 || cat > 2) cat = classify_score_cat(pkg);
        AppScore *s = &g_scores[g_score_count++];
        strncpy(s->name, pkg, 255); s->name[255] = '\0';
        s->fg_count          = cnt < FG_COUNT_MAX ? cnt : FG_COUNT_MAX;
        s->last_fg           = (time_t)ts;
        s->restart_count     = rc;
        s->avg_swap_kb       = avg_sw;
        s->session_count     = ses;
        s->category          = cat;
        s->dc_kill_count     = dckills;
        s->adaptive_kill_count = adpkills;
        loaded++;
    }
    fclose(f);
    logi("Scores: loaded %d entries", loaded);
}

static void score_maybe_save(void) {
    time_t now = time(NULL);
    if (now - g_score_last_save >= SCORE_SAVE_INTVL) {
        score_compact(); /* NEW 1.23: rarely runs (24h gate), cheap to check */
        score_save(); g_score_last_save = now;
    }
}

/* ================================================================
 *  RANK CACHE  — lightweight per-app swap + rank snapshot
 *  Format: name avg_swap_kb rank
 *  Purpose: warm up avg_swap_kb for background (non-fg) apps so
 *  cmp_killable_zram sorts correctly immediately after restart.
 * ================================================================ */
static void rank_cache_save(ProcInfo *tbl, int cnt) {
    FILE *f = fopen(RANK_CACHE_FILE, "w");
    if (!f) return;
    time_t now = time(NULL);
    fprintf(f, "# lmk rank cache ts=%ld\n", (long)now);
    for (int i = 0; i < cnt; i++) {
        ProcInfo *p = &tbl[i];
        if (!p->name[0]) continue;
        if (classify_score_cat(p->name) < 2) continue; /* user apps only */
        if (is_rank_exempt(p->name)) continue;
        /* Use live swap_kb blended with stored avg */
        long swap = p->swap_kb;
        AppScore *as = score_lookup(p->name);
        if (as && as->avg_swap_kb > 0)
            swap = (as->avg_swap_kb * 80 + p->swap_kb * 20) / 100;
        fprintf(f, "%s %ld %d\n", p->name, swap, p->rank);
    }
    fclose(f);
}

static void rank_cache_load(void) {
    FILE *f = fopen(RANK_CACHE_FILE, "r");
    if (!f) return;
    char line[300]; int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char pkg[256]; long avg_sw; int rank;
        if (sscanf(line, "%255s %ld %d", pkg, &avg_sw, &rank) < 2) continue;
        /* Find or create a minimal score entry to hold avg_swap_kb */
        AppScore *s = score_lookup(pkg);
        if (!s) {
            if (g_score_count >= SCORE_MAX_APPS) continue;
            s = &g_scores[g_score_count++];
            memset(s, 0, sizeof(*s));
            strncpy(s->name, pkg, 255); s->name[255] = '\0';
            s->category = classify_score_cat(pkg);
        }
        /* Only overwrite avg_swap_kb if the score entry has none */
        if (s->avg_swap_kb == 0 && avg_sw > 0)
            s->avg_swap_kb = avg_sw;
        loaded++;
    }
    fclose(f);
    if (loaded > 0) logi("RankCache: loaded %d entries (avg_swap warm-up)", loaded);
}

static void rank_cache_maybe_save(ProcInfo *tbl, int cnt) {
    time_t now = time(NULL);
    if (now - g_rank_cache_last_save >= RANK_CACHE_SAVE_S) {
        rank_cache_save(tbl, cnt);
        g_rank_cache_last_save = now;
    }
}

/* ================================================================
 *  PROCESS CLASSIFICATION
 * ================================================================ */
static Priority classify(const ProcInfo *p) {
    if (name_matches(p->name, NEVER_KILL))    return PRIO_NEVER;
    if (name_matches(p->name, SERVICE_EXEMPT))return PRIO_NEVER;
    if (is_launcher_like(p->name))            return PRIO_NEVER;
    if (is_ime_like(p->name))                 return PRIO_NEVER;
    if (is_active_widget_provider(p->name))   return PRIO_NEVER;
    if (p->oom_adj <= 0)                      return PRIO_NEVER;
    if (p->oom_adj <= ADJ_VISIBLE_MAX)        return PRIO_SEMI_PROTECTED;
    if (is_media_player(p->name))             return PRIO_SEMI_PROTECTED;
    if (p->oom_adj <= ADJ_SERVICE_MAX)        return PRIO_BACKGROUND;
    if (p->oom_adj <= ADJ_CACHED_MAX)         return PRIO_CACHED;
    return PRIO_JUNK;
}

/* ================================================================
 *  PROCESS ENUMERATION
 * ================================================================ */
static int enumerate_procs(ProcInfo *tbl, int maxn) {
    DIR *dir = opendir("/proc"); if (!dir) return -1;
    int cnt = 0; struct dirent *de;
    while ((de = readdir(dir)) && cnt < maxn) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = (pid_t)atol(de->d_name); if (pid <= 1) continue;
        ProcInfo *p = &tbl[cnt]; memset(p, 0, sizeof(*p)); p->pid = pid;
        if (proc_cmdline(pid, p->name, sizeof(p->name)) < 0) continue;
        if (!p->name[0]) continue;
        p->oom_adj       = proc_oom_adj(pid);
        proc_mem_stats(pid, &p->rss_kb, &p->swap_kb);
        p->prio          = classify(p);
        p->last_fg       = get_last_fg(p->name);
        check_bounced(p->name);
        AppScore *as     = score_lookup(p->name);
        /* Fall back to persisted last_fg if fg_history has no entry
         * (covers daemon restart — fixes recents-dismiss and retention) */
        if (p->last_fg == 0 && as) p->last_fg = as->last_fg;
        p->score         = as ? score_compute(as) : 0;
        p->restart_count = as ? as->restart_count : 0;
        p->avg_swap_kb   = as ? as->avg_swap_kb : 0;
        /* Ranking (NEW 1.17) */
        if (is_rank_exempt(p->name))
            p->rank = RANK_EXEMPT;
        else
            p->rank = app_rank(p->oom_adj, p->last_fg, as ? as->session_count : 0);
        /* Runaway-growth watch — generalized in 1.23 to any non-foreground,
         * non-HAL/system, non-pinned process (was scene-daemon-shaped:
         * PRIO_NEVER-only gate). Reuses the same window/strike/threshold
         * constants; no parallel tracker. A confirmed runaway gets bumped
         * to PRIO_JUNK so it's first in line for Adaptive_Clean. */
        /* 1.24: track growth even while foreground (was excluded, which
         * let heavy foreground apps like emulators/games balloon RSS for
         * hours without ever tripping a strike, since the window only
         * advanced once they finally backgrounded). Foreground processes
         * are still never killed or trimmed here — this only primes the
         * flag so the instant such an app backgrounds it's already
         * PRIO_JUNK and first in line for Adaptive_Clean, instead of
         * needing another full growth window to notice. */
        if (p->prio != PRIO_NEVER && p->rank != RANK_EXEMPT &&
            !is_actively_retained(p->name)) {
            check_runaway_growth(p->name, time(NULL), p->rss_kb);
            AppScore *gs = score_lookup(p->name);
            if (gs && gs->flagged_runaway && p->oom_adj != ADJ_FOREGROUND)
                p->prio = PRIO_JUNK;
        }
        /* bounce window tracking */
        p->bounce_count  = 0;
        p->bounce_first  = 0;
        time_t now = time(NULL);
        for (int i = 0; i < KILL_HIST_SIZE; i++) {
            if (g_kill_hist[i].name[0] &&
                strcmp(g_kill_hist[i].name, p->name) == 0 &&
                (now - g_kill_hist[i].killed_at) < BOUNCE_WINDOW_S) {
                if (p->bounce_count == 0) p->bounce_first = g_kill_hist[i].killed_at;
                p->bounce_count++;
            }
        }
        cnt++;
    }
    closedir(dir); return cnt;
}

/* ================================================================
 *  FOREGROUND TRACKING + BOUNCE DETECTION
 * ================================================================ */
/* Shared by track_foreground() for both the current-tick classification
 * and the fg->bg transition edge detection (1.25). */
static bool is_genuine_fg(const char *n, long oom_adj) {
    return oom_adj == ADJ_FOREGROUND &&
           !(n[0] == '/' || strstr(n, ".persistent") ||
             classify_score_cat(n) != 2);
}

static void track_foreground(ProcInfo *tbl, int cnt) {
    time_t now = time(NULL);
    for (int i = 0; i < cnt; i++) {
        const char *n = tbl[i].name;
        bool genuine_fg = is_genuine_fg(n, tbl[i].oom_adj);

        /* 1.25: edge-detect the exact tick this app leaves true foreground
         * and stamp last_bg once. Unlike last_fg (refreshed every tick
         * while foreground), last_bg freezes until the next transition,
         * so "now - last_bg" measured on relaunch reflects the real
         * time the app spent backgrounded rather than reading ~0s. */
        AppScore *s = score_lookup(n);
        if (s) {
            if (s->was_true_fg && !genuine_fg) s->last_bg = now;
            s->was_true_fg = genuine_fg;
        }

        if (tbl[i].oom_adj != ADJ_FOREGROUND) {
            /* Background-visible (perceptible/visible svc, 1-200): app was
             * NOT user-foregrounded — likely a foreground service, sync,
             * media playback, or notification. Recency-only touch. */
            if (tbl[i].oom_adj <= ADJ_VISIBLE_MAX) {
                update_fg_history(n, now);
                score_touch_bg(n, now, tbl[i].swap_kb);
            }
            continue;
        }
        if (!genuine_fg) {
            update_fg_history(n, now);
            score_touch_bg(n, now, tbl[i].swap_kb);
            continue;
        }
        update_fg_history(n, now);
        score_touch(n, now, tbl[i].swap_kb);
    }
}

/* ================================================================
 *  TRUE FOREGROUND DETECTION
 * ================================================================ */
static bool find_true_foreground(ProcInfo *tbl, int cnt, char *out, size_t outsz) {
    for (int i = 0; i < cnt; i++) {
        if (tbl[i].oom_adj != ADJ_FOREGROUND) continue;
        const char *n = tbl[i].name;
        /* Skip absolute-path native binaries (e.g. Termux bash) */
        if (n[0] == '/') continue;
        /* Skip persistent background services (GMS persistent, etc.) */
        if (strstr(n, ".persistent")) continue;
        /* Only user-installed apps count as true foreground */
        if (classify_score_cat(n) != 2) continue;
        strncpy(out, n, outsz - 1);
        out[outsz - 1] = '\0';
        return true;
    }
    return false;
}

/* ================================================================
 *  OOM PINNING — ACTIVE APP RETENTION
 * ================================================================ */
/* 1.24: pin-set stickiness. Previously the top-N was greedily reselected
 * every tick with no memory of the prior pin set, so ties at the score
 * ceiling (broken only by table/PID scan order) made the pinned set
 * thrash every ~30s. Remembering last cycle's names and giving them a
 * bonus means a previously-pinned app keeps its slot unless clearly
 * outranked, not just tied. */
#define PIN_STICKY_BONUS   60
static char    g_prev_pinned[RETAIN_MAX_N][64];
static int     g_prev_pinned_adj[RETAIN_MAX_N]; /* NEW 1.25: adj we wrote for each, for eviction detection */
static int     g_prev_pinned_n = 0;
static time_t  g_pin_evict_log_t = 0; /* NEW 1.25: rate-limit for eviction log */

static bool was_prev_pinned(const char *name) {
    for (int i = 0; i < g_prev_pinned_n; i++)
        if (strcmp(g_prev_pinned[i], name) == 0) return true;
    return false;
}

static void oom_pin_retained(ProcInfo *tbl, int cnt, int avail_pct) {
    if (avail_pct < FREE_CRIT_PCT) return;

    time_t now0 = time(NULL);
    /* 1.25: pin survival verification. Android's activitymanager can
     * silently reconcile a process's oom_score_adj on its own state
     * transitions (e.g. a service binding change), quietly undoing our
     * pin without lmk_engine ever killing anything or being told about
     * it. Compare each app we pinned last cycle against its current,
     * freshly-read adj and flag any mismatch. */
    for (int j = 0; j < g_prev_pinned_n; j++) {
        for (int i = 0; i < cnt; i++) {
            if (strcmp(tbl[i].name, g_prev_pinned[j]) != 0) continue;
            if (tbl[i].oom_adj != g_prev_pinned_adj[j] &&
                now0 - g_pin_evict_log_t >= LOG_RATELIMIT_S) {
                g_pin_evict_log_t = now0;
                logw("Retain: pin evicted by system for [%s] "
                     "expected_adj=%d actual_adj=%ld",
                     g_prev_pinned[j], g_prev_pinned_adj[j], tbl[i].oom_adj);
            }
            break;
        }
    }

    /* Smooth retain-count scaling (1.23): tracks continuous headroom instead
     * of a single step at RETAIN_HEADROOM_PCT. More free RAM and lower ZRAM
     * pressure both push retain_n up toward RETAIN_MAX_N; either constraint
     * tightening pulls it back down toward the RETAIN_TOP_N floor. */
    int headroom = avail_pct - FREE_CRIT_PCT;
    if (headroom < 0) headroom = 0;
    int zram_room = 100 - g_zram_used_pct; /* higher = less ZRAM pressure */
    if (zram_room < 0) zram_room = 0;
    int extra = (headroom * zram_room) / 2000;
    int retain_n = RETAIN_TOP_N + extra;
    if (retain_n > RETAIN_MAX_N) retain_n = RETAIN_MAX_N;

    time_t now = time(NULL);
    bool   done[2048];
    int    max_done = cnt < 2048 ? cnt : 2048;
    memset(done, 0, (size_t)max_done * sizeof(bool));

    int pinned = 0;
    char pinned_names[512] = "";
    char local_pinned_name[RETAIN_MAX_N][64];
    int  local_pinned_adj[RETAIN_MAX_N];
    int  local_pinned_count = 0;
    for (int k = 0; k < retain_n; k++) {
        int best = -1;
        for (int i = 0; i < cnt; i++) {
            if (done[i]) continue;
            ProcInfo *p = &tbl[i];
            if (p->prio <= PRIO_SEMI_PROTECTED) continue;
            if (p->score <= 0) continue;
            if (p->last_fg == 0 || (now - p->last_fg) > RETAIN_FG_AGE_S) continue;
            int eff = p->score + (was_prev_pinned(p->name) ? PIN_STICKY_BONUS : 0);
            int best_eff = best >= 0
                ? tbl[best].score + (was_prev_pinned(tbl[best].name) ? PIN_STICKY_BONUS : 0)
                : -1;
            /* 1.25: on an exact tie, break deterministically by name instead
             * of silently keeping "first found in scan order" — table scan
             * order shifts tick to tick as /proc listing order changes,
             * which kept thrashing the pin set on ties even with the
             * stickiness bonus above. */
            if (best == -1 || eff > best_eff ||
                (eff == best_eff && strcmp(p->name, tbl[best].name) < 0))
                best = i;
        }
        if (best < 0) break;
        done[best] = true;

        ProcInfo *p = &tbl[best];
        /* 1.25: graduated protection by rank within the pin set instead of
         * a flat value for every slot — the top-scoring pinned app gets
         * the most protection, tapering down to the old flat floor
         * (RETAIN_OOM_ADJ) for the lowest-ranked pinned slot. */
        int adj = RETAIN_OOM_ADJ_TOP + k * RETAIN_OOM_ADJ_STEP;
        if (adj > RETAIN_OOM_ADJ) adj = RETAIN_OOM_ADJ;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", p->pid);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%d\n", adj); fclose(f); pinned++;
            AppScore *as = score_lookup(p->name);
            if (as) as->retain_pin_t = now;

            size_t used = strlen(pinned_names);
            if (used < sizeof(pinned_names) - 1) {
                snprintf(pinned_names + used, sizeof(pinned_names) - used,
                         "%s%s", used ? "," : "", p->name);
            }
            if (local_pinned_count < RETAIN_MAX_N) {
                strncpy(local_pinned_name[local_pinned_count], p->name,
                        sizeof(local_pinned_name[0]) - 1);
                local_pinned_name[local_pinned_count][sizeof(local_pinned_name[0]) - 1] = 0;
                local_pinned_adj[local_pinned_count] = adj;
                local_pinned_count++;
            }
        } else {
            /* 1.25: this previously failed silently (process raced and
             * died between selection and write, or a permission edge
             * case) with zero visibility into how often it happens. */
            g_pin_write_fail_count++;
            if (now - g_pin_fail_log_t >= LOG_RATELIMIT_S) {
                g_pin_fail_log_t = now;
                logw("Retain: pin write failed for [%s] pid=%d errno=%d "
                     "(%ld total this run)",
                     p->name, p->pid, errno, g_pin_write_fail_count);
            }
        }
    }

    /* Remember this cycle's pin set (name + adj) for next tick's
     * stickiness bonus and pin-eviction verification (1.25: carries adj
     * now, so this replaces the old strtok-based reparse of pinned_names,
     * which only had names). */
    g_prev_pinned_n = local_pinned_count;
    for (int i = 0; i < local_pinned_count; i++) {
        strncpy(g_prev_pinned[i], local_pinned_name[i], sizeof(g_prev_pinned[0]) - 1);
        g_prev_pinned[i][sizeof(g_prev_pinned[0]) - 1] = 0;
        g_prev_pinned_adj[i] = local_pinned_adj[i];
    }

    if (pinned > 0) {
        if (now - g_retain_log_t >= LOG_RATELIMIT_S) {
            g_retain_log_t = now;
            logi("Retain: pinned %d app(s) [%s] oom_adj=%d..%d (avail=%d%%)",
                 pinned, pinned_names, RETAIN_OOM_ADJ_TOP, RETAIN_OOM_ADJ, avail_pct);
        }
    }
}

/* ================================================================
 *  KILL ORDERING
 * ================================================================ */
/* RAM kill ordering: rank desc (stale first), then prio, score, swap, age, rss */
static int cmp_killable_ram(const void *a, const void *b) {
    const ProcInfo *pa = a, *pb = b;
    /* Primary: higher rank (less important) killed first */
    if (pa->rank != pb->rank)
        return pb->rank - pa->rank;
    if (pa->prio != pb->prio)
        return (int)pa->prio - (int)pb->prio;
    if (pa->score != pb->score)
        return pa->score - pb->score;
    if (pa->avg_swap_kb != pb->avg_swap_kb)
        return (pa->avg_swap_kb > pb->avg_swap_kb) ? -1 : 1;
    if (pa->restart_count != pb->restart_count)
        return pb->restart_count - pa->restart_count;
    if (pa->last_fg != pb->last_fg)
        return (pa->last_fg > pb->last_fg) ? 1 : -1;
    return (int)(pb->rss_kb - pa->rss_kb);
}

/* New comparator for ZRAM kills: rank desc, blended-swap desc, score, rc, last_fg.
 * Uses avg_swap_kb (EMA) blended 70/30 with live swap_kb for stable ordering. */
static int cmp_killable_zram(const void *a, const void *b) {
    const ProcInfo *pa = a, *pb = b;
    /* Primary: higher rank (stale/cold) first */
    if (pa->rank != pb->rank)
        return pb->rank - pa->rank;
    /* Secondary: blended swap — avg gives persistent signal, live gives current */
    long sw_a = pa->avg_swap_kb > 0
                ? (pa->avg_swap_kb * 70 + pa->swap_kb * 30) / 100
                : pa->swap_kb;
    long sw_b = pb->avg_swap_kb > 0
                ? (pb->avg_swap_kb * 70 + pb->swap_kb * 30) / 100
                : pb->swap_kb;
    if (sw_a != sw_b) return (sw_b > sw_a) ? 1 : -1;
    /* Tertiary: lowest score first */
    if (pa->score != pb->score)
        return pa->score - pb->score;
    /* Quaternary: lower restart_count preferred (more stable) */
    if (pa->restart_count != pb->restart_count)
        return pa->restart_count - pb->restart_count;
    /* Quinary: older last_fg first */
    if (pa->last_fg != pb->last_fg)
        return (pa->last_fg > pb->last_fg) ? 1 : -1;
    /* Fallback: larger RSS */
    return (int)(pb->rss_kb - pa->rss_kb);
}

/* ================================================================
 *  RAM PRESSURE KILL
 * ================================================================ */
static long do_kill(ProcInfo *tbl, int cnt, long target_kb,
                    Priority min_prio, bool is_critical, int avail_pct) {
    qsort(tbl, cnt, sizeof(ProcInfo), cmp_killable_ram);
    long freed = 0;
    time_t now = time(NULL);
    for (int i = 0; i < cnt && freed < target_kb; i++) {
        ProcInfo *p = &tbl[i];
        if (p->prio < min_prio)                  continue;
        if (p->oom_adj <= ADJ_VISIBLE_MAX)        continue;
        if (!is_critical && is_actively_retained(p->name)) {
            logi("Retained: sparing %s (pinned by retention)", p->name);
            continue;
        }
        if (!is_critical && p->last_fg > 0 &&
            (now - p->last_fg) < score_protect_window_ram(p->name, avail_pct)) {
            logi("Sparing %s (score=%d win=%ds age=%lds)",
                 p->name, p->score,
                 score_protect_window_ram(p->name, avail_pct),
                 now - p->last_fg);
            continue;
        }
        if (!is_critical && recently_killed(p->name)) {
            logi("Cooldown: skipping %s", p->name); continue;
        }
        /* In CRITICAL, allow PRIO_SEMI_PROTECTED if they hold substantial RAM */
        if (is_critical && p->prio == PRIO_SEMI_PROTECTED && p->rss_kb < 100*1024)
            continue;
        if (kill(p->pid, SIGKILL) == 0) {
            freed += p->rss_kb;
            record_kill(p);
            logk("RAM pid=%-6d prio=%d sc=%-4d rc=%-2d fg_age=%-4lds rss=%4ldMB sw=%4ldMB [%s]",
                 p->pid, p->prio, p->score, p->restart_count,
                 p->last_fg ? (now - p->last_fg) : -1,
                 p->rss_kb/1024, p->avg_swap_kb/1024, p->name);
            usleep(30000);
        }
    }
    return freed;
}

/* zram_deep_clean() removed in 1.18 — logic merged into Adaptive[T3] */

/* ================================================================
 *  ZRAM PRESSURE KILL (improved with new comparator)
 * ================================================================ */
static void zram_pressure_kill(ProcInfo *tbl, int cnt, int zram_used_pct) {
    if (zram_used_pct < ZRAM_TRIM_PCT) return;

    time_t now = time(NULL);

    if ((now - g_zram_kill_last_t) < ZRAM_KILL_INTVL_S) return;
    if (g_zram_pause_until > 0 && now < g_zram_pause_until) return;

    if (g_zram_kill_start_t == 0) {
        g_zram_kill_start_t = now;
        g_zram_pct_at_start = zram_used_pct;
    } else {
        long campaign_s = now - g_zram_kill_start_t;
        if (campaign_s >= FUTILITY_WINDOW_S) {
            int drop = g_zram_pct_at_start - zram_used_pct;
            if (drop < FUTILITY_MIN_DROP) {
                logi("ZRAMkill: futile (%d%%→%d%% in %lds) — pausing %ds",
                     g_zram_pct_at_start, zram_used_pct,
                     campaign_s, FUTILITY_PAUSE_S);
                g_zram_pause_until  = now + FUTILITY_PAUSE_S;
                g_zram_kill_start_t = 0;
                return;
            }
            g_zram_kill_start_t = now;
            g_zram_pct_at_start = zram_used_pct;
        }
    }

    g_zram_kill_last_t = now;
    qsort(tbl, cnt, sizeof(ProcInfo), cmp_killable_zram);
    long freed_swap = 0;
    int  kills = 0, skipped_cd = 0;

    for (int i = 0; i < cnt && kills < ZRAM_KILL_BATCH; i++) {
        ProcInfo *p = &tbl[i];
        if (p->rank == RANK_EXEMPT)                  continue;
        if (p->prio < PRIO_BACKGROUND)              continue;
        if (p->oom_adj <= ADJ_VISIBLE_MAX)           continue;
        if (p->swap_kb < 256)                        continue;
        if (is_actively_retained(p->name))           continue;
        if (p->last_fg > 0 &&
            (now - p->last_fg) < score_protect_window(p->name)) continue;
        if (recently_killed(p->name)) { skipped_cd++; continue; }
        /* Bounce suppression */
        if (p->restart_count >= BOUNCE_SUPPRESS_RC &&
            p->swap_kb < BOUNCE_SUPPRESS_SWAP_KB)   continue;
        if (p->bounce_count > BOUNCE_WINDOW_KILLS &&
            p->swap_kb < 200*1024)                  continue;
        if (kill(p->pid, SIGKILL) == 0) {
            freed_swap += p->swap_kb;
            kills++;
            record_kill(p);
            logk("ZRAM%d%% pid=%-6d swap=%4ldMB avg=%4ldMB rc=%-2d [%s]",
                 zram_used_pct, p->pid,
                 p->swap_kb/1024, p->avg_swap_kb/1024,
                 p->restart_count, p->name);
            usleep(40000);
        }
    }

    if (skipped_cd > 0 && kills == 0) {
        if (now - g_log_zramcd_t >= LOG_RATELIMIT_S) {
            g_log_zramcd_t = now;
            logi("ZRAMcd: %d candidate(s) on cooldown, nothing killed at %d%%",
                 skipped_cd, zram_used_pct);
        }
    }

    /* Emergency pass */
    if (kills == 0 && skipped_cd > 0 && zram_used_pct >= ZRAM_STUCK_PCT) {
        logi("ZRAMemerg: all %d blocked at %d%% — relaxing cd to %ds",
             skipped_cd, zram_used_pct, ZRAM_EMERG_CD_S);
        int emerg_kills = 0;
        for (int i = 0; i < cnt && emerg_kills < ZRAM_EMERG_KILL_MAX; i++) {
            ProcInfo *p = &tbl[i];
            if (p->prio < PRIO_CACHED)           continue;
            if (p->oom_adj <= ADJ_VISIBLE_MAX)    continue;
            if (p->swap_kb < 256)                 continue;
            if (p->restart_count >= BOUNCE_SUPPRESS_RC &&
                p->swap_kb < BOUNCE_SUPPRESS_SWAP_KB)   continue;
            if (p->bounce_count > BOUNCE_WINDOW_KILLS &&
                p->swap_kb < 200*1024)            continue;
            bool blocked = false;
            for (int j = 0; j < KILL_HIST_SIZE; j++) {
                if (g_kill_hist[j].name[0] &&
                    strcmp(g_kill_hist[j].name, p->name) == 0 &&
                    (now - g_kill_hist[j].killed_at) < ZRAM_EMERG_CD_S) {
                    blocked = true; break;
                }
            }
            if (blocked) continue;
            if (kill(p->pid, SIGKILL) == 0) {
                freed_swap += p->swap_kb;
                emerg_kills++;
                record_kill(p);
                logk("ZRAMemerg pid=%-6d swap=%4ldMB rc=%-2d [%s]",
                     p->pid, p->swap_kb/1024, p->restart_count, p->name);
                usleep(40000);
            }
        }
        if (emerg_kills == 0)
            logi("ZRAMemerg: no eligible target — deep clean will engage if stuck");
    }

    if (freed_swap > 0) {
        usleep(200000);
        int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
        if (fd >= 0) { write(fd, "1\n", 2); close(fd); }
    }
}

/* ================================================================
 *  PROACTIVE LAUNCH TRIM (unchanged)
 * ================================================================ */
static void proactive_trim(ProcInfo *tbl, int cnt, MemInfo *mi, ZramInfo *z,
                            const char *new_fg_name) {
    bool zram_hot = z->active && z->used_pct >= ZRAM_TRIM_PCT;
    if (mi->avail_pct >= LAUNCH_TRIM_AVAIL_PCT && !zram_hot)
        return;

    AppScore *as = score_lookup(new_fg_name);
    bool heavy = as && as->avg_swap_kb >= LAUNCH_TRIM_HEAVY_SWAP_KB;
    int target_pct = heavy ? LAUNCH_TRIM_HEAVY_TARGET_PCT : LAUNCH_TRIM_TARGET_PCT;
    if (z->active && z->used_pct >= ZRAM_STUCK_PCT) target_pct += 5;

    /* Warm relaunch (app was foreground very recently, e.g. quick recents
     * bounce): its pages are still hot, killing other apps to make room
     * isn't needed and just costs CPU/relaunch-thrash elsewhere. Still
     * let zram_pressure_kill below run if ZRAM itself is genuinely hot. */
    bool warm_relaunch = as && as->last_bg > 0 &&
                         (time(NULL) - as->last_bg) < LAUNCH_TRIM_WARM_SKIP_S;
    if (warm_relaunch && !zram_hot) {
        logi("Launch: %s warm relaunch (%lds ago) — skipping RAM trim",
             new_fg_name, (long)(time(NULL) - as->last_bg));
        return;
    }

    logi("Launch: %s foreground%s — proactive trim (avail=%d%% zram=%d%% target=%d%%)",
         new_fg_name, heavy ? " [heavy]" : "", mi->avail_pct,
         z->active ? z->used_pct : -1, target_pct);

    if (mi->avail_pct < target_pct && !warm_relaunch) {
        long want = (long)g_total_ram_kb * target_pct / 100 - mi->avail_kb;
        if (want > 0) {
            long freed = do_kill(tbl, cnt, want, PRIO_BACKGROUND, false, mi->avail_pct);
            if (freed > 0) {
                logi("Launch: proactive RAM trim freed %ldMB ahead of %s",
                     freed/1024, new_fg_name);
                g_last_kill_t = time(NULL);
            }
        }
    }

    if (zram_hot)
        zram_pressure_kill(tbl, cnt, z->used_pct);
}

/* ================================================================
 *  ADAPTIVE CLEAN (merged with deep-clean in 1.18 — 5 tiers, always runs)
 *
 *  Tier selection:
 *    IDLE  (-2) — phone idle ≥IDLE_DETECT_S, ZRAM normal: gentle cold/stale sweep
 *    MAINT (-1) — ZRAM normal; maintenance sweep (STALE, or COLD+high-swap if RAM low)
 *    LOW   ( 0) — ZRAM mildly above WARN_PCT or RAM slightly low
 *    MED   ( 1) — ZRAM above TRIM_PCT, or rising fast
 *    HIGH  ( 2) — ZRAM above STUCK_PCT
 *    DEEP  ( 3) — ZRAM stuck ≥ ZRAM_STUCK_S + deep-clean CD elapsed;
 *                 performs kills + drop_caches + compact_memory + zram/compact
 *
 *  Rank==EXEMPT processes (HAL, system, launchers) are never killed.
 *  Trend (rise over ADAPTIVE_TREND_WINDOW_S) escalates tier.
 *  Recents-dismiss removed: unreliable on this device.
 * ================================================================ */

/* Comparator: rank desc, swap_kb desc, adaptive_kill_count asc, score asc */
static int cmp_adaptive_clean(const void *a, const void *b) {
    const ProcInfo *pa = a, *pb = b;
    if (pa->rank != pb->rank)
        return pb->rank - pa->rank;
    if (pa->swap_kb != pb->swap_kb)
        return (int)(pb->swap_kb - pa->swap_kb);
    AppScore *sa = score_lookup(pa->name);
    AppScore *sb = score_lookup(pb->name);
    int ak_a = sa ? sa->adaptive_kill_count : 0;
    int ak_b = sb ? sb->adaptive_kill_count : 0;
    if (ak_a != ak_b) return ak_a - ak_b;
    if (pa->score != pb->score) return pa->score - pb->score;
    if (pa->last_fg != pb->last_fg)
        return (pa->last_fg > pb->last_fg) ? 1 : -1;
    return 0;
}

static void adaptive_clean(ProcInfo *tbl, int cnt, MemInfo *mi, ZramInfo *z) {
    if (!z->active) return;

    int zpct    = z->used_pct;
    bool ram_low = mi->avail_pct < FREE_LOW_PCT;
    time_t now  = time(NULL);

    /* Trend detection */
    bool rising = false;
    if (g_adaptive_trend_t == 0) {
        g_adaptive_trend_t       = now;
        g_adaptive_prev_zram_pct = zpct;
    } else if ((now - g_adaptive_trend_t) >= ADAPTIVE_TREND_WINDOW_S) {
        int rise = zpct - g_adaptive_prev_zram_pct;
        rising   = (rise >= ADAPTIVE_RISE_THRESH_PCT);
        g_adaptive_prev_zram_pct = zpct;
        g_adaptive_trend_t       = now;
    }

    /* ── Tier selection (IDLE=-2, MAINT=-1, LOW=0, MED=1, HIGH=2, DEEP=3) ── */
    bool do_compact = false;
    int  tier;

    bool deep_eligible =
        (zpct >= ZRAM_STUCK_PCT) &&
        (g_zram_stuck_since > 0) &&
        ((now - g_zram_stuck_since)  >= ZRAM_STUCK_S) &&
        ((now - g_last_deepclean)    >= ZRAM_DEEPCLEAN_CD_S) &&
        !(g_deepclean_pause_until > 0 && now < g_deepclean_pause_until);

    /* Phone idle: no fg-app change for IDLE_DETECT_S and ZRAM not elevated */
    bool phone_idle = (g_last_fg_change_t > 0) &&
                      ((now - g_last_fg_change_t) >= IDLE_DETECT_S) &&
                      (zpct < (100 - IDLE_MIN_ZRAM_FREE_PCT)) &&
                      !ram_low;

    /* Screen off ≥30 min: intense idle clean — device is fully idle, so
     * sweep harder and run compaction even if ZRAM isn't under pressure. */
    bool idle_deep_eligible = (g_screen_off_since > 0) &&
                              ((now - g_screen_off_since) >= IDLE_DEEP_S);

    if (deep_eligible) {
        tier = 3; do_compact = true;
    } else if (zpct >= ZRAM_STUCK_PCT || (zpct >= ZRAM_TRIM_PCT && rising)) {
        tier = 2;
    } else if (zpct >= ZRAM_TRIM_PCT || rising || ram_low) {
        tier = 1;
    } else if (zpct >= ZRAM_WARN_PCT) {
        tier = 0;
    } else if (idle_deep_eligible) {
        tier = -3; do_compact = true; /* IDLE-DEEP: intense + compaction */
    } else if (phone_idle) {
        tier = -2; /* IDLE */
    } else {
        tier = -1; /* MAINTENANCE */
    }

    /* ── PSI escalation (reference engine hook) ──
     * Boost tier if kernel PSI says real memory stall beyond ZRAM%. */
    {
        double psi = psi_mem_avg10();
        if (psi >= PSI_MEM_URGENT_PCT   && tier < 2) tier = (tier < 1 ? 2 : tier + 1);
        else if (psi >= PSI_MEM_ESCALATE_PCT && tier < 3) tier++;
    }

    ai_learn_train_maybe(tier); /* NEW 1.23: no-op unless IDLE-DEEP + due */

    int max_kills, target_pct, intvl;
    switch (tier) {
    case 3:
        max_kills  = DEEPCLEAN_KILL_MAX;
        target_pct = DEEPCLEAN_KILL_TARGET_PCT;
        intvl      = ADAPTIVE_INTVL_HIGH_S;
        break;
    case 2:
        max_kills  = ADAPTIVE_KILLS_HIGH;
        target_pct = ADAPTIVE_TARGET_HIGH_PCT;
        intvl      = ADAPTIVE_INTVL_HIGH_S;
        break;
    case 1:
        max_kills  = ADAPTIVE_KILLS_MED;
        target_pct = ADAPTIVE_TARGET_MED_PCT;
        intvl      = ADAPTIVE_INTVL_MED_S;
        break;
    case 0:
        max_kills  = ADAPTIVE_KILLS_LOW;
        target_pct = ADAPTIVE_TARGET_LOW_PCT;
        intvl      = ADAPTIVE_INTVL_LOW_S;
        break;
    case -2: /* IDLE */
        max_kills  = IDLE_KILLS_MAX;
        target_pct = 0;
        intvl      = IDLE_INTVL_S;
        break;
    case -3: /* IDLE-DEEP: screen off 30+ min */
        max_kills  = IDLE_DEEP_KILLS_MAX;
        target_pct = IDLE_DEEP_TARGET_PCT;
        intvl      = IDLE_DEEP_INTVL_S;
        break;
    default: /* MAINTENANCE */
        max_kills  = ADAPTIVE_KILLS_MAINT;
        target_pct = 0;
        intvl      = ADAPTIVE_INTVL_MAINT_S;
        break;
    }

    /* Per-tier interval gate (7 independent timers: IDLE-DEEP/IDLE/MAINT/LOW/MED/HIGH/DEEP) */
    static time_t s_last_t[7] = {0, 0, 0, 0, 0, 0, 0};
    int tidx = tier + 3; /* 0=IDLE-DEEP,1=IDLE,2=MAINT,3=LOW,4=MED,5=HIGH,6=DEEP */
    if ((now - s_last_t[tidx]) < intvl) return;
    s_last_t[tidx] = now;

    long target_swap = target_pct > 0 ? z->disksize_kb * target_pct / 100 : 0;
    long freed_swap  = 0;
    int  kills       = 0;
    const char *tier_label = tier == 3  ? "T3(DEEP)" :
                             tier == 2  ? "T2" :
                             tier == 1  ? "T1" :
                             tier == 0  ? "T0" :
                             tier == -2 ? "IDLE" :
                             tier == -3 ? "IDLE-DEEP" : "MAINT";

    qsort(tbl, cnt, sizeof(ProcInfo), cmp_adaptive_clean);

    int hot_spared = 0; /* count of RANK_HOT apps spared from T3 critical override */
    for (int i = 0; i < cnt && kills < max_kills; i++) {
        if (target_pct > 0 && freed_swap >= target_swap) break;
        ProcInfo *p = &tbl[i];

        /* Never kill exempt (HAL/system/launcher) processes */
        if (p->rank == RANK_EXEMPT) continue;

        if (tier >= 2) {
            /* HIGH/DEEP may touch SEMI_PROTECTED if holding swap:
             * T3(DEEP): ≥40MB swap; T2(HIGH): ≥50MB swap */
            if (p->prio < PRIO_SEMI_PROTECTED) continue;
            long sp_thresh = (tier == 3) ? 40*1024 : 50*1024;
            if (p->prio == PRIO_SEMI_PROTECTED && p->swap_kb < sp_thresh) continue;
        } else {
            if (p->prio < PRIO_BACKGROUND) continue;
        }

        if (p->oom_adj <= ADJ_VISIBLE_MAX)                   continue;
        if (p->swap_kb < 256 && p->rss_kb < 64*1024)        continue;
        if (recently_killed(p->name))                        continue;

        /* T3(DEEP) override: when RAM is critically low, retained/spared
         * (AI_Swap-pinned) apps are no longer exempt — a memory choke
         * outranks retention. Foreground apps are still never touched
         * (RANK_EXEMPT/FOREGROUND guards). A small quota of RANK_HOT apps
         * is spared too, so light multitasking survives a critical sweep
         * instead of every retained app being wiped at once. */
        bool ram_critical = mi->avail_pct < FREE_CRIT_PCT;
        bool spare_this_hot = (p->rank == RANK_HOT) && (hot_spared < T3_SPARE_HOT_N);
        bool retain_override = (tier == 3) && ram_critical &&
                               p->rank != RANK_FOREGROUND && !spare_this_hot;
        if (is_actively_retained(p->name) && !retain_override) {
            if (spare_this_hot && tier == 3 && ram_critical) {
                hot_spared++;
                logi("Adaptive[T3]: RAM critical — sparing HOT [%s] for "
                     "multitasking (%d/%d)", p->name, hot_spared, T3_SPARE_HOT_N);
            }
            continue;
        }
        if (retain_override && is_actively_retained(p->name))
            logw("Adaptive[T3]: RAM critical (%d%%) — overriding retention "
                 "for [%s]", mi->avail_pct, p->name);

        /* IDLE / IDLE-DEEP tiers: age-gated kills — IDLE-DEEP uses relaxed
         * thresholds since the device has already been idle 30+ min. */
        if (tier == -2 || tier == -3) {
            int hot_age   = (tier == -3) ? IDLE_DEEP_HOT_AGE_S   : IDLE_KILL_HOT_AGE_S;
            int lower_age = (tier == -3) ? IDLE_DEEP_LOWER_AGE_S : IDLE_KILL_LOWER_AGE_S;
            if (p->rank < RANK_HOT) continue;  /* fg/exempt: never */
            if (p->rank == RANK_HOT) {
                /* HOT apps: wait before idle-killing */
                if (p->last_fg == 0 ||
                    (now - p->last_fg) < hot_age) continue;
            } else {
                /* WARM/COLD/STALE: minimum wait since last fg */
                if (p->last_fg > 0 &&
                    (now - p->last_fg) < lower_age) continue;
            }
            /* cooldown still applies inside idle tiers */
        } else if (tier == -1) {
            /* MAINT: STALE apps always eligible;
             * COLD apps with significant swap also eligible when RAM mildly low */
            bool maint_eligible = (p->rank >= RANK_STALE) ||
                                  (p->rank == RANK_COLD &&
                                   ram_low && p->swap_kb >= 80*1024);
            if (!maint_eligible) continue;
            /* No score protect window for MAINT stale apps */
        } else {
            if (p->last_fg > 0 &&
                (now - p->last_fg) < score_protect_window(p->name)) continue;
        }

        if (p->restart_count >= BOUNCE_SUPPRESS_RC &&
            p->swap_kb < BOUNCE_SUPPRESS_SWAP_KB)            continue;
        if (p->bounce_count > BOUNCE_WINDOW_KILLS &&
            p->swap_kb < 200*1024)                           continue;
        /* Don't kill FOREGROUND or HOT apps below HIGH tier — except
         * IDLE-DEEP, which already age-gated HOT above and the device
         * has been screen-off 30+ min, so it's safe here. */
        if (tier < 2 && tier != -3 &&
            (p->rank == RANK_FOREGROUND || p->rank == RANK_HOT)) continue;
        if (tier == -3 && p->rank == RANK_FOREGROUND) continue;

        if (kill(p->pid, SIGKILL) == 0) {
            freed_swap += p->swap_kb + p->rss_kb / 4;
            kills++;
            record_kill(p);
            AppScore *as = score_lookup(p->name);
            if (as) { as->adaptive_kill_count++; g_scores_dirty = true; }
            logk("Adaptive[%s%s] rank%d pid=%-6d swap=%4ldMB rss=%4ldMB [%s]",
                 tier_label, rising ? " rising" : "",
                 p->rank, p->pid, p->swap_kb/1024, p->rss_kb/1024, p->name);
            usleep(30000);
        }
    }

    if (kills > 0 || tier >= 2) {
        if (kills > 0)
            logi("Adaptive[%s]: killed %d proc(s), freed ~%ldMB "
                 "(avail=%d%% ZRAM=%d%% rising=%s)",
                 tier_label, kills, freed_swap/1024,
                 mi->avail_pct, zpct, rising ? "yes" : "no");
        if (!do_compact) {
            int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
            if (fd >= 0) { write(fd, tier <= 0 ? "1\n" : "3\n", 2); close(fd); }
        }
    }

    /* ── T3(DEEP): compaction stages after kills ──
     * Smarter 1.21: re-check actual pressure between stages instead of
     * blindly running the full drop_caches→compact_memory→zram/compact
     * sequence every time. Each stage only runs if the previous one
     * didn't already relieve enough pressure — saves CPU/IO and the
     * long ZRAM_COMPACT_WAIT_MS stall when it's not needed. */
    if (do_compact) {
        if (kills > 0) usleep(200000);

        MemInfo mi_chk; read_meminfo(&mi_chk);
        ZramInfo z_chk; zram_read_stats(&z_chk);
        if (mi_chk.avail_pct >= FREE_LOW_PCT && z_chk.used_pct < ZRAM_STUCK_PCT) {
            logi("Adaptive[T3]: kills alone relieved pressure "
                 "(avail=%d%% ZRAM=%d%%) — skipping compaction stages",
                 mi_chk.avail_pct, z_chk.used_pct);
            do_compact = false; /* downgrade — nothing left to compact */
        } else {
            int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
            if (fd >= 0) { write(fd, "3\n", 2); close(fd);
                logi("Adaptive[T3]: drop_caches=3 done"); }
            usleep(300000);

            fd = open("/proc/sys/vm/compact_memory", O_WRONLY);
            if (fd >= 0) { write(fd, "1\n", 2); close(fd);
                logi("Adaptive[T3]: compact_memory done"); }
            usleep(500000);

            zram_read_stats(&z_chk);
            if (z_chk.used_pct < ZRAM_TRIM_PCT) {
                logi("Adaptive[T3]: drop_caches+compact already sufficient "
                     "(ZRAM=%d%%) — skipping slow zram/compact stage",
                     z_chk.used_pct);
            } else if (g_zram_sys[0]) {
                char cpath[256];
                snprintf(cpath, sizeof(cpath), "%s/compact", g_zram_sys);
                fd = open(cpath, O_WRONLY);
                if (fd >= 0) {
                    write(fd, "1\n", 2); close(fd);
                    logi("Adaptive[T3]: zram/compact — waiting %dms",
                         ZRAM_COMPACT_WAIT_MS);
                    usleep((useconds_t)ZRAM_COMPACT_WAIT_MS * 1000);
                } else {
                    logi("Adaptive[T3]: zram/compact unavailable");
                }
            }
        }

        ZramInfo za; zram_read_stats(&za);
        int spct = (z->used_kb > 0)
                   ? (int)((z->used_kb - za.used_kb) * 100 / z->used_kb) : 0;
        logi("Adaptive[T3]: done — %d%%→%d%% (%ldMB→%ldMB, recovered ~%d%%, killed=%d)",
             zpct, za.used_pct, z->used_kb/1024, za.used_kb/1024, spct, kills);

        g_last_deepclean   = now;
        g_zram_stuck_since = 0;

        /* Futility tracking */
        if (kills == 0 && spct <= DEEPCLEAN_FUTILE_PCT) {
            g_deepclean_fail_cnt++;
            if (g_deepclean_fail_cnt >= DEEPCLEAN_FUTILE_STRIKES) {
                logw("Adaptive[T3]: %d consecutive futile cleans — "
                     "pausing %d min",
                     g_deepclean_fail_cnt, DEEPCLEAN_FUTILE_PAUSE_S / 60);
                g_deepclean_pause_until = now + DEEPCLEAN_FUTILE_PAUSE_S;
                g_deepclean_fail_cnt    = 0;
            }
        } else {
            g_deepclean_fail_cnt = 0;
        }
    }
}

/* ================================================================
 *  PRE‑CYCLE KILL (unchanged)
 * ================================================================ */
static bool pre_cycle_kill(ProcInfo *tbl, int cnt) {
    MemInfo mi; read_meminfo(&mi);
    ZramInfo z; zram_read_stats(&z);
    long need_kb = z.orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
    long gap_kb  = need_kb - mi.avail_kb;

    if (gap_kb <= 0) return true;

    if (g_total_ram_kb > 0 &&
        z.orig_data_kb > (long)g_total_ram_kb * ZRAM_CYCLE_IMPOSSIBLE_PCT / 100) {
        if (!g_cycle_impossible) {
            g_cycle_impossible = true;
            logi("PreCycle: ADVISORY — orig_data (%ldMB) > %d%% of RAM (%ldMB); "
                 "full ZRAM cycle is structurally impossible on this device. "
                 "Deep clean (zram/compact) will be used instead.",
                 z.orig_data_kb/1024, ZRAM_CYCLE_IMPOSSIBLE_PCT,
                 g_total_ram_kb/1024);
        }
        return false;
    }

    {
        time_t now2 = time(NULL);
        if (now2 - g_log_precycle_t >= LOG_RATELIMIT_S) {
            g_log_precycle_t = now2;
            logi("PreCycle: need %ldMB more free (orig=%ldMB margin=%ldMB avail=%ldMB)",
                 gap_kb/1024, z.orig_data_kb/1024,
                 ZRAM_CYCLE_MARGIN_KB/1024, mi.avail_kb/1024);
        }
    }

    if (gap_kb > (long)g_total_ram_kb / 2) {
        logi("PreCycle: gap too large (%ldMB) — skip cycle", gap_kb/1024);
        return false;
    }

    qsort(tbl, cnt, sizeof(ProcInfo), cmp_killable_ram);
    long freed = 0;
    time_t now = time(NULL);
    for (int i = 0; i < cnt && freed < gap_kb; i++) {
        ProcInfo *p = &tbl[i];
        if (p->prio < PRIO_BACKGROUND)        continue;
        if (p->oom_adj <= ADJ_VISIBLE_MAX)     continue;
        if (p->rss_kb < 10*1024)               continue;
        if (p->last_fg > 0 &&
            (now - p->last_fg) < 30)           continue;
        if (kill(p->pid, SIGKILL) == 0) {
            freed += p->rss_kb;
            record_kill(p);
            logk("PreCycle pid=%-6d rss=%4ldMB [%s]",
                 p->pid, p->rss_kb/1024, p->name);
            usleep(50000);
        }
    }

    if (freed > 0) {
        usleep(500000);
        read_meminfo(&mi);
        zram_read_stats(&z);
        need_kb = z.orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
        logi("PreCycle: freed %ldMB, avail now %ldMB (need %ldMB)",
             freed/1024, mi.avail_kb/1024, need_kb/1024);
        return mi.avail_kb >= need_kb;
    }
    return false;
}

/* ================================================================
 *  MAIN DAEMON LOOP
 * ================================================================ */
static void signal_handler(int sig) {
    logi("Signal %d – shutting down", sig);
    g_running = false;
}

static void run_daemon(void) {
    logi("══════════════════════════════════════");
    logi("  LMK Engine %s by %s", LMK_VERSION, LMK_AUTHOR);
    logi("  ZRAM target %d%% of RAM", ZRAM_SIZE_PCT);
    logi("  ZRAM stages: warn>%d%% trim>%d%% stuck>%d%% crit>%d%%",
         ZRAM_WARN_PCT, ZRAM_TRIM_PCT, ZRAM_STUCK_PCT, ZRAM_CRIT_PCT);
    logi("  Adaptive clean: IDLE/MAINT/T0/T1/T2/T3(DEEP) always-running, 6 tiers");
    logi("  Adaptive intervals: %d/%d/%d/%d/%d/%ds (IDLE/MAINT/LOW/MED/HIGH/DEEP)",
         IDLE_INTVL_S, ADAPTIVE_INTVL_MAINT_S, ADAPTIVE_INTVL_LOW_S,
         ADAPTIVE_INTVL_MED_S, ADAPTIVE_INTVL_HIGH_S, ADAPTIVE_INTVL_HIGH_S);
    logi("  Adaptive kills: %d/%d/%d/%d/%d/%d (IDLE/MAINT/LOW/MED/HIGH/DEEP)",
         IDLE_KILLS_MAX, ADAPTIVE_KILLS_MAINT, ADAPTIVE_KILLS_LOW,
         ADAPTIVE_KILLS_MED, ADAPTIVE_KILLS_HIGH, DEEPCLEAN_KILL_MAX);
    logi("  T3(DEEP): fires when ZRAM stuck >%d%% for >%ds, cd=%ds; "
         "kills+drop_caches+compact_memory+zram/compact",
         ZRAM_STUCK_PCT, ZRAM_STUCK_S, ZRAM_DEEPCLEAN_CD_S);
    logi("  T3 futility: pause %dm after %d×≤%d%% recovery",
         DEEPCLEAN_FUTILE_PAUSE_S/60, DEEPCLEAN_FUTILE_STRIKES, DEEPCLEAN_FUTILE_PCT);
    logi("  ZRAM emerg kill: cd relaxed to %ds when all blocked at >%d%%",
         ZRAM_EMERG_CD_S, ZRAM_STUCK_PCT);
    logi("  Idle mode: COLD+STALE sweep after %ds no fg-change, every %ds, max %d kills",
         IDLE_DETECT_S, IDLE_INTVL_S, IDLE_KILLS_MAX);
    logi("  Rank cache: %s saved every %ds (avg_swap warm-up after restart)",
         RANK_CACHE_FILE, RANK_CACHE_SAVE_S);
    logi("  App ranking: 0=exempt 1=fg 2=hot(<%ds) 3=warm(<%dmin) "
         "4=cold(<%dmin) 5=stale",
         RANK_HOT_S, RANK_WARM_S/60, RANK_COLD_S/60);
    logi("  Proactive launch trim: target %d%% (heavy app %d%%) on fg app switch",
         LAUNCH_TRIM_TARGET_PCT, LAUNCH_TRIM_HEAVY_TARGET_PCT);
    logi("  RAM kill thresholds: low<%d%% crit<%d%% stop>%d%%",
         FREE_LOW_PCT, FREE_CRIT_PCT, FREE_HIGH_PCT);
    logi("  Protect window: %d–%ds (score+RAM-aware)",
         FG_PROTECT_BASE, FG_PROTECT_MAX);
    logi("  Kill cooldown: %d–%ds (restart_count scaled)",
         RESTART_CD_BASE_S, RESTART_CD_MAX_S);
    logi("  Retention: top-%d..%d apps pinned oom_adj=%d..%d graduated by rank, "
         "count scaled smoothly by headroom+ZRAM (was flat top-%d/top-%d step at %d%%)",
         RETAIN_TOP_N, RETAIN_MAX_N, RETAIN_OOM_ADJ_TOP, RETAIN_OOM_ADJ,
         RETAIN_TOP_N, RETAIN_TOP_N + RETAIN_BONUS_N, RETAIN_HEADROOM_PCT);
    logi("  Bounce suppression: rc>=%d and swap<%dMB, plus time-window (%d kills in %ds)",
         BOUNCE_SUPPRESS_RC, BOUNCE_SUPPRESS_SWAP_KB/1024,
         BOUNCE_WINDOW_KILLS, BOUNCE_WINDOW_S);
    logi("  Retention protect: pinned apps exempt from opportunistic kills for %ds after pin",
         RETAIN_PROTECT_GRACE_S);
    logi("  Log rate-limit: noisy-state messages throttled to 1/%ds",
         LOG_RATELIMIT_S);
    logi("══════════════════════════════════════");

    setpriority(PRIO_PROCESS, 0, -15);
    prctl(PR_SET_NAME, "lmk_engine", 0, 0, 0);

    g_start_time = time(NULL);
    {
        FILE *sf = fopen(START_TIME_FILE, "w");
        if (sf) { fprintf(sf, "%ld\n", (long)g_start_time); fclose(sf); }
    }

    score_load();
    rank_cache_load();
    psi_check_available();
    lmkd_minfree_cleanup();

    if (zram_find(g_zram_dev, sizeof(g_zram_dev), g_zram_sys, sizeof(g_zram_sys))) {
        zram_setup();
    } else {
        logw("ZRAM: no device found – running without ZRAM management");
    }

    /* ── Boot-settle wait ───────────────────────────────────────────
     * Poll sys.boot_completed for up to BOOT_WAIT_S seconds before
     * entering the main loop.  This prevents acting on an incomplete
     * process table or a missing appwidgets.xml at early boot.
     * ─────────────────────────────────────────────────────────────── */
#define BOOT_WAIT_S  90
    {
        logi("Boot: waiting for sys.boot_completed (max %ds)", BOOT_WAIT_S);
        time_t deadline = time(NULL) + BOOT_WAIT_S;
        bool settled = false;
        while (!settled && g_running && time(NULL) < deadline) {
            FILE *gp = popen("getprop sys.boot_completed 2>/dev/null", "r");
            if (gp) {
                char val[8] = {0};
                fgets(val, sizeof(val), gp);
                pclose(gp);
                if (val[0] == '1') { settled = true; }
            }
            if (!settled) sleep(2);
        }
        if (settled)
            logi("Boot: settled – entering main loop");
        else
            logw("Boot: timeout after %ds – continuing anyway", BOOT_WAIT_S);
        /* Allow AppWidgetService extra time to write appwidgets.xml */
        g_widget_settle_until = time(NULL) + WIDGET_SETTLE_S;
        logi("Boot: widget-settle grace %ds (kills suppressed until providers loaded)", WIDGET_SETTLE_S);
    }

    ProcInfo *tbl = malloc(2048 * sizeof(ProcInfo));
    if (!tbl) { loge("malloc failed"); return; }

    MemInfo mi;
    int  tick = 0;
    time_t last_zram_cycle  = 0;
    time_t last_drop_caches = 0;
    useconds_t loop_sleep_us = 500000; /* adaptive: 500ms busy, up to 1.5s calm */

    while (g_running) {
        usleep(loop_sleep_us);
        if (read_meminfo(&mi) < 0) continue;
        load_active_widget_pkgs();

        int cnt = enumerate_procs(tbl, 2048);
        if (cnt < 0) continue;

        track_foreground(tbl, cnt);
        score_maybe_save();
        ai_learn_log_resolved(); /* NEW 1.23: append resolved kill outcomes */
        rank_cache_maybe_save(tbl, cnt);

        /* ── Active retention (must run before kill paths so
         *    is_actively_retained() guard is current) ── */
        oom_pin_retained(tbl, cnt, mi.avail_pct);

        /* ── Periodic status log ── */
        ZramInfo z; zram_read_stats(&z);
        g_zram_used_pct = z.used_pct;   /* NEW 1.22: feed AI_Swap retention */
        if (++tick >= 20) {
            tick = 0;
            logi("RAM total=%ldMB avail=%ldMB(%d%%)  "
                 "ZRAM %ldMB/%ldMB(%d%%)  orig=%ldMB  procs=%d  "
                 "state=%s",
                 mi.total_kb/1024, mi.avail_kb/1024, mi.avail_pct,
                 z.used_kb/1024, z.disksize_kb/1024, z.used_pct,
                 z.orig_data_kb/1024, cnt,
                 mi.avail_pct < FREE_CRIT_PCT ? "CRITICAL" :
                 (mi.avail_pct < FREE_LOW_PCT ? "LOW" : "NORMAL"));
        }

        /* ── Dynamic swappiness (reference engine hook) ── */
        if (z.active) update_swappiness(z.used_pct);

        /* ── ZRAM stage management ── */
        time_t now = time(NULL);

        /* ── Screen-off / Doze strengthens idle detection ── */
        if (g_last_fg_change_t == 0) g_last_fg_change_t = now;
        bool scr_off = is_screen_off();
        if (scr_off) {
            if (g_screen_off_since == 0) g_screen_off_since = now;
        } else if (g_screen_off_since != 0) {
            logi("Idle: screen on — exiting idle-deep window (was off %ldm)",
                 (long)(now - g_screen_off_since) / 60);
            g_screen_off_since = 0;
        }
        /* Screen-off or Doze fast-tracks idle detection */
        if ((scr_off || is_doze_active()) &&
            (now - g_last_fg_change_t) < IDLE_DETECT_S)
            g_last_fg_change_t = now - IDLE_DETECT_S; /* fast-track idle */
        {
            time_t now = time(NULL);
            if (now < g_widget_settle_until) {
                /* still in widget-settle grace — skip kills */
            } else {
            char fg_name[256];
            if (find_true_foreground(tbl, cnt, fg_name, sizeof(fg_name)) &&
                strcmp(fg_name, g_true_fg_name) != 0) {
                /* Debounce: require the same candidate across consecutive
                 * ticks before accepting it as a genuine fg change. Guards
                 * against scan-order flicker when multiple procs share
                 * ADJ_FOREGROUND simultaneously. */
                if (strcmp(fg_name, g_pending_fg_name) == 0) {
                    g_pending_fg_streak++;
                } else {
                    strncpy(g_pending_fg_name, fg_name, sizeof(g_pending_fg_name) - 1);
                    g_pending_fg_name[sizeof(g_pending_fg_name) - 1] = '\0';
                    g_pending_fg_streak = 1;
                }
                if (g_pending_fg_streak >= FG_DEBOUNCE_TICKS) {
                    if (!is_launcher_like(fg_name))
                        proactive_trim(tbl, cnt, &mi, &z, fg_name);
                    strncpy(g_true_fg_name, fg_name, sizeof(g_true_fg_name) - 1);
                    g_true_fg_name[sizeof(g_true_fg_name) - 1] = '\0';
                    g_last_fg_change_t = now; /* reset idle timer on fg switch */
                    g_pending_fg_streak = 0;
                    g_pending_fg_name[0] = '\0';
                }
            } else if (strcmp(fg_name, g_true_fg_name) == 0) {
                /* Candidate matches confirmed fg again — clear stale pending state */
                g_pending_fg_streak = 0;
                g_pending_fg_name[0] = '\0';
            }
            }
        }

        /* ── Adaptive clean (self-gated, tiered) ── */
        if (time(NULL) >= g_widget_settle_until)
        adaptive_clean(tbl, cnt, &mi, &z);

        /* ── ZRAM stage management (uses `now` declared above) ── */
        if (z.active) {
            if (g_zram_needs_resize)
                zram_try_deferred_resize(&mi);

            if (z.used_pct >= ZRAM_WARN_PCT && z.used_pct < ZRAM_TRIM_PCT) {
                if (now - last_drop_caches > 60) {
                    last_drop_caches = now;
                    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
                    if (fd >= 0) { write(fd, "1\n", 2); close(fd); }
                }
            }

            if (z.used_pct >= ZRAM_TRIM_PCT) {
                if (time(NULL) >= g_widget_settle_until)
                zram_pressure_kill(tbl, cnt, z.used_pct);
            }

            /* Stuck ZRAM tracking — Adaptive[T3] fires automatically */
            {
                if (z.used_pct >= ZRAM_STUCK_PCT) {
                    if (g_zram_stuck_since == 0) {
                        g_zram_stuck_since = now;
                        logi("ZRAMstuck: ZRAM at %d%% — Adaptive[T3] will engage "
                             "after %ds",
                             z.used_pct, ZRAM_STUCK_S);
                    } else if (g_deepclean_pause_until > 0 &&
                               now < g_deepclean_pause_until) {
                        if (now - g_log_stuck_t >= LOG_RATELIMIT_S) {
                            g_log_stuck_t = now;
                            logi("ZRAMstuck: T3 paused after futile runs (%ldm remain)",
                                 (long)(g_deepclean_pause_until - now) / 60);
                        }
                    } else {
                        if (now - g_log_stuck_t >= LOG_RATELIMIT_S) {
                            g_log_stuck_t = now;
                            logi("ZRAMstuck: %d%% for %lds — T3 eligible=%s cd_remain=%lds",
                                 z.used_pct, (long)(now - g_zram_stuck_since),
                                 (now - g_zram_stuck_since) >= ZRAM_STUCK_S ? "yes" : "no",
                                 (long)(ZRAM_DEEPCLEAN_CD_S - (now - g_last_deepclean)));
                        }
                    }
                } else {
                    if (g_zram_stuck_since != 0) {
                        logi("ZRAMstuck: cleared (now %d%%)", z.used_pct);
                        g_zram_stuck_since = 0;
                        g_log_stuck_t      = 0;
                    }
                }
            }

            /* Critical >90%: attempt cycle */
            if (z.used_pct >= ZRAM_CRIT_PCT && !g_cycle_impossible) {
                if ((now - last_zram_cycle) > 300) {
                    long need_kb = z.orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
                    bool can_cycle = (mi.avail_kb >= need_kb);
                    if (!can_cycle) can_cycle = pre_cycle_kill(tbl, cnt);
                    if (can_cycle) {
                        last_zram_cycle = now;
                        zram_cycle();
                    } else {
                        last_zram_cycle = now;
                        if (now - g_log_crit_t >= LOG_RATELIMIT_S) {
                            g_log_crit_t = now;
                            logi("ZRAMcrit: cannot cycle (orig=%ldMB avail=%ldMB)",
                                 z.orig_data_kb/1024, mi.avail_kb/1024);
                        }
                    }
                }
            }
        }

        /* ── RAM pressure kill ── */
        int avail = mi.avail_pct;
        if (avail >= FREE_HIGH_PCT) continue;

        int cd = (avail < FREE_CRIT_PCT) ? 1 : 5;
        if ((now - g_last_kill_t) < cd) continue;

        long want, freed = 0;
        if (avail < FREE_CRIT_PCT) {
            want = (long)g_total_ram_kb * FREE_LOW_PCT / 100 - mi.avail_kb;
            if (want < 64*1024) want = 64*1024;
            freed = do_kill(tbl, cnt, want, PRIO_SEMI_PROTECTED, true, avail);
            logi("CRITICAL: freed %ldMB", freed/1024);
        } else if (avail < FREE_LOW_PCT) {
            want = (long)g_total_ram_kb * (FREE_LOW_PCT + 4) / 100 - mi.avail_kb;
            if (want < 32*1024) want = 32*1024;
            freed = do_kill(tbl, cnt, want, PRIO_BACKGROUND, false, avail);
            if (freed > 0) logi("LOW: freed %ldMB", freed/1024);
        }

        g_last_kill_t = now;
        if (freed == 0 && avail < FREE_CRIT_PCT) {
            logi("Nothing killable – backing off 5s");
            g_last_kill_t += 5;
        }

        /* ── Adaptive tick: back off the loop period when calm so the
         * full /proc scan + per-pid reads in enumerate_procs() don't
         * burn CPU 2x/sec while the device is idle and healthy. Snap
         * back to fast polling the instant pressure rises. ── */
        bool calm = (mi.avail_pct >= FREE_LOW_PCT + 5) &&
                    (z.used_pct < ZRAM_WARN_PCT) &&
                    (now - g_last_fg_change_t) > 5;
        loop_sleep_us = calm ? 1500000 : 500000;
    }

    free(tbl);
    score_save();
    logi("LMK Engine stopped.");
}

/* ================================================================
 *  DAEMON PID MANAGEMENT
 * ================================================================ */
#define PID_FILE "/data/local/tmp/lmk_engine.pid"

static void write_pid(void) {
    FILE *f = fopen(PID_FILE,"w");
    if (f) { fprintf(f,"%d\n",getpid()); fclose(f); }
}
static bool daemon_is_running(pid_t *out) {
    FILE *f = fopen(PID_FILE,"r"); if (!f) return false;
    pid_t pid = 0; fscanf(f,"%d",&pid); fclose(f);
    if (pid > 0 && kill(pid,0) == 0) { if (out) *out = pid; return true; }
    return false;
}
static void kill_daemon(void) {
    pid_t pid = 0;
    if (!daemon_is_running(&pid)) { puts("LMK Engine: not running"); return; }
    if (kill(pid, SIGTERM) == 0) {
        printf("LMK Engine stopped (pid %d)\n", pid);
        unlink(PID_FILE);
    } else {
        fprintf(stderr,"kill %d: %s\n", pid, strerror(errno));
    }
}

/* ================================================================
 *  STATUS DISPLAY
 * ================================================================ */
static void print_status(void) {
    MemInfo mi;
    if (read_meminfo(&mi) != 0) { printf("Failed to read memory info\n"); return; }

    char dev[128] = {0}, sys[128] = {0};
    bool zram_found = zram_find(dev, sizeof(dev), sys, sizeof(sys));
    ZramInfo z;
    if (zram_found) {
        strcpy(g_zram_dev, dev); strcpy(g_zram_sys, sys);
        zram_read_stats(&z);
    } else {
        memset(&z, 0, sizeof(z));
    }

    pid_t daemon_pid = 0;
    bool  daemon_running = daemon_is_running(&daemon_pid);

    printf("LMK Engine %s by %s\n", LMK_VERSION, LMK_AUTHOR);
    printf("Status:\n");
    printf("  Daemon     : %s\n", daemon_running ? "RUNNING" : "STOPPED");
    if (daemon_running) {
        printf("  PID        : %d\n", daemon_pid);
        FILE *sf = fopen(START_TIME_FILE, "r");
        if (sf) {
            long st = 0; fscanf(sf, "%ld", &st); fclose(sf);
            if (st > 0) {
                long up = (long)time(NULL) - st;
                long ud = up / 86400; up %= 86400;
                long uh = up / 3600;  up %= 3600;
                long um = up / 60;    up %= 60;
                if (ud > 0)
                    printf("  Uptime     : %ldd %ldh %ldm %lds\n", ud, uh, um, up);
                else if (uh > 0)
                    printf("  Uptime     : %ldh %ldm %lds\n", uh, um, up);
                else
                    printf("  Uptime     : %ldm %lds\n", um, up);
            }
        }
    }
    {
        FILE *uf = fopen("/proc/uptime", "r");
        if (uf) {
            double sys_up = 0; fscanf(uf, "%lf", &sys_up); fclose(uf);
            long su = (long)sys_up;
            long sd = su / 86400; su %= 86400;
            long sh = su / 3600;  su %= 3600;
            long sm = su / 60;    su %= 60;
            if (sd > 0)
                printf("  Sys uptime : %ldd %ldh %ldm %lds\n", sd, sh, sm, su);
            else if (sh > 0)
                printf("  Sys uptime : %ldh %ldm %lds\n", sh, sm, su);
            else
                printf("  Sys uptime : %ldm %lds\n", sm, su);
        }
    }
    printf("  RAM total  : %ld MB\n", mi.total_kb / 1024);
    printf("  RAM avail  : %ld MB (%d%%)\n", mi.avail_kb / 1024, mi.avail_pct);
    printf("  RAM free   : %ld MB (%d%%)\n", mi.free_kb  / 1024, mi.free_pct);
    printf("  RAM state  : %s\n",
           mi.avail_pct < FREE_CRIT_PCT ? "CRITICAL" :
           (mi.avail_pct < FREE_LOW_PCT  ? "LOW" : "NORMAL"));

    if (zram_found && z.active) {
        printf("  ZRAM       : active on %s\n", z.dev);
        printf("  ZRAM size  : %ld MB (%d%% of RAM)\n",
               z.disksize_kb/1024, ZRAM_SIZE_PCT);
        printf("  ZRAM used  : %ld MB (%d%%)\n", z.used_kb/1024, z.used_pct);
        if (z.orig_data_kb > 0)
            printf("  ZRAM orig  : %ld MB (uncompressed, %.1fx ratio)\n",
                   z.orig_data_kb/1024,
                   z.used_kb > 0 ? (double)z.orig_data_kb/z.used_kb : 0.0);
    } else if (zram_found) {
        printf("  ZRAM       : device %s exists but inactive\n", dev);
    } else {
        printf("  ZRAM       : not detected\n");
    }

    FILE *f = fopen("/proc/swaps","r");
    if (f) {
        char line[256]; bool found = false;
        fgets(line, sizeof(line), f);
        while (fgets(line, sizeof(line), f)) {
            char path[200], type[32]; long sz, used;
            if (sscanf(line, "%199s %31s %ld %ld", path, type, &sz, &used) == 4 &&
                strcmp(type, "file") == 0) {
                printf("  File swap  : %s\n", path);
                printf("  Swap size  : %ld MB\n", sz/1024);
                printf("  Swap used  : %ld MB (%ld%%)\n", used/1024,
                       sz > 0 ? used*100/sz : 0);
                found = true; break;
            }
        }
        if (!found) printf("  File swap  : none\n");
        fclose(f);
    }
}

/* ================================================================
 *  USAGE
 * ================================================================ */
static void usage(const char *prog) {
    printf(
"LMK Engine %s by %s\n"
"Usage:\n"
"  %s --start         start daemon\n"
"  %s --stop          stop daemon\n"
"  %s --status        show memory/ZRAM/swap status\n"
"  %s --log           tail live log\n"
"  %s --swap <MB>     create & enable file swap\n"
"  %s --delswap       remove file swap\n",
        LMK_VERSION, LMK_AUTHOR,
        prog, prog, prog, prog, prog, prog);
}

/* ================================================================
 *  MAIN
 * ================================================================ */
int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 0; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "--features")) {
        printf("LMK Engine %s by %s — feature list\n\n", LMK_VERSION, LMK_AUTHOR);
        printf("1) AI_Swap — app retention & learning\n"
               "   - Per-app scoring (lmk_scores.dat, v5 format, dirty-gated\n"
               "     incremental writes + daily stale-entry compaction)\n"
               "   - fg_count / session_count / avg_swap_kb learning\n"
               "   - rank blends recency with session_count frequency, so\n"
               "     frequent-but-not-recent apps don't rank as low as one-offs\n"
               "   - True foreground vs background-launch differentiation,\n"
               "     with debounced foreground-change detection (2-tick confirm)\n"
               "   - rank-based kill ordering (lmk_rank.cache, persists across restarts)\n"
               "   - OOM pinning / active retention, smoothly scaled by RAM\n"
               "     headroom + ZRAM pressure (was a flat step threshold)\n"
               "   - bounce-loop suppression (restart_count + window tracking)\n"
               "   - generalized runaway-growth detection for any non-fg,\n"
               "     non-HAL, non-pinned process (was scene-daemon-shaped)\n"
               "   - self-contained C-only learning: kill outcomes logged to\n"
               "     lmk_learn.log, logistic-regression weights retrained every\n"
               "     12h during IDLE-DEEP via plain gradient descent\n\n");
        printf("2) Adaptive_Clean — memory & ZRAM reclaim\n"
               "   - 7-tier escalation: IDLE-DEEP / IDLE / MAINT / LOW / MED / HIGH / DEEP\n"
               "   - PSI (/proc/pressure/memory) pressure escalation\n"
               "   - Trend detection (rising ZRAM%% over time window)\n"
               "   - T3 deep clean: kill -> drop_caches -> compact_memory -> zram/compact,\n"
               "     each stage skipped adaptively once pressure is already relieved\n"
               "   - Idle-deep intense sweep after 30 min continuous screen-off\n"
               "   - Futility tracking with auto-pause on repeated no-op deep cleans\n\n");
        printf("3) Other features\n"
               "   - PSI-driven dynamic swappiness\n"
               "   - Screen-off / Doze detection feeding idle timers\n"
               "   - Widget provider protection (multi-user XML scan + cmd fallback,\n"
               "     boot-settle grace window, parse-failure resilience)\n"
               "   - Launcher / SystemUI / IME (keyboard) hard kill guards\n"
               "   - HAL / native-daemon / bg-service-component detection\n"
               "   - Proactive launch trim, with warm-relaunch skip\n"
               "   - Adaptive main-loop polling (CPU backoff when calm)\n"
               "   - ZRAM 3-attempt setup retry, cycle-safety guard, deferred resize\n");
        return 0;
    }
    if (!strcmp(cmd, "--log")) {
        FILE *lf = fopen(LOG_FILE, "r");
        if (!lf) { fprintf(stderr,"No log at %s\n", LOG_FILE); return 1; }
        fseek(lf, 0, SEEK_END); long fsz = ftell(lf);
        long tail = fsz > 8192 ? fsz - 8192 : 0;
        fseek(lf, tail, SEEK_SET);
        if (tail > 0) { char d[512]; fgets(d, sizeof(d), lf); }
        char line[512];
        while (fgets(line, sizeof(line), lf)) fputs(line, stdout);
        fflush(stdout);
        printf("--- following log (Ctrl-C to stop) ---\n");
        while (1) {
            while (fgets(line, sizeof(line), lf)) { fputs(line, stdout); fflush(stdout); }
            usleep(500000);
        }
        fclose(lf); return 0;
    }
    if (!strcmp(cmd, "--swap")) {
        if (argc < 3) { fprintf(stderr,"Usage: %s --swap <SIZE_MB>\n",argv[0]); return 1; }
        swap_create(atoi(argv[2])); return 0;
    }
    if (!strcmp(cmd, "--delswap")) { swap_delete(); return 0; }
    if (!strcmp(cmd, "--stop"))    { kill_daemon(); return 0; }
    if (!strcmp(cmd, "--status"))  { print_status(); return 0; }

    if (!strcmp(cmd, "--start")) {
        pid_t running;
        if (daemon_is_running(&running)) {
            printf("LMK Engine already running (pid %d)\n", running);
            return 0;
        }
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) { printf("LMK Engine started (pid %d)\n", pid); return 0; }
        setsid();
        g_log = fopen(LOG_FILE, "a");
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        write_pid();
        signal(SIGTERM, signal_handler);
        signal(SIGINT,  signal_handler);
        signal(SIGHUP,  signal_handler);
        run_daemon();
        unlink(PID_FILE);
        unlink(START_TIME_FILE);
        if (g_log) fclose(g_log);
        return 0;
    }

    fprintf(stderr,"Unknown command: %s\n\n", cmd);
    usage(argv[0]); return 1;
}