

#define _GNU_SOURCE
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
#include <sched.h>
#include <poll.h>
#include <math.h>

/* ================================================================
 *  VERSION
 * ================================================================ */
#define LMK_VERSION   "1.26"
#define LMK_AUTHOR    "P34"

/* ================================================================
 *  MEMORY PRESSURE THRESHOLDS
 * ================================================================ */
#define FREE_HIGH_PCT    25   /* stop killing above this % avail   */
#define FREE_LOW_PCT     15   /* start killing below this %        */
#define FREE_CRIT_PCT     8   /* aggressive kill tier              */
#define PIN_ALLOW_FLOOR_PCT 4

/* ================================================================
 *  ZRAM SETTINGS
 * ================================================================ */
#define ZRAM_SIZE_PCT    50

#define ZRAM_WARN_PCT    90   /* early-warning: proactive page drop    */
#define ZRAM_TRIM_PCT    93   /* pressure kill tier                    */
#define ZRAM_CRIT_PCT    98   /* try full cycle                        */

/* Minimum rate at which zram_pressure_kill() is called (seconds). */
#define ZRAM_KILL_INTVL_S    5

/* Minimum avail buffer (kB) above orig_data_size to attempt a cycle. */
#define ZRAM_CYCLE_MARGIN_KB  (200 * 1024)   /* 200 MB safety buffer   */

/* Maximum ZRAM pressure kills per invocation of zram_pressure_kill(). */
#define ZRAM_KILL_BATCH       6

/* ================================================================
 *  ZRAM STUCK / DEEP CLEAN
 * ================================================================ */
#define ZRAM_STUCK_PCT            96

#define ZRAM_STUCK_S              30
#define ZRAM_DEEPCLEAN_CD_S      180   /* min seconds between deep cleans */
#define ZRAM_COMPACT_WAIT_MS    1500   /* ms to wait after zram compact   */

#define ZRAM_CYCLE_RETRIES           8
#define ZRAM_CYCLE_RETRY_BACKOFF_US  150000

#define DEEPCLEAN_KILL_MAX         24   /* max procs killed per deep clean */
#define DEEPCLEAN_KILL_TARGET_PCT  25   /* stop once this % of ZRAM freed  */

/* Emergency kill: when ZRAM ≥ STUCK and all blocked by cooldown, a
 * second pass runs with this reduced cooldown floor (seconds). */
#define ZRAM_EMERG_CD_S           10
#define ZRAM_EMERG_KILL_MAX        2

#define BOUNCE_SUPPRESS_RC        20        /* rc ≥ this → suppress ZRAM kill */
#define BOUNCE_SUPPRESS_SWAP_KB   (150*1024) /* override: >150 MB swap = kill  */
#define BOUNCE_WINDOW_S           60        /* time window for bounce count    */
#define BOUNCE_WINDOW_KILLS       3         /* >3 kills in window → suppress   */
#define BOUNCE_DECAY_S            (24*60*60) /* 24h since last bounce → reset rc */

#define LOWFREQ_MIN_OBS_S         (24*60*60) /* don't judge until tracked 24h+  */
#define LOWFREQ_FG_RATE_THRESH    0.5        /* fg-credited ticks per hour tracked;
                                               * below this → background candidate */

/* ================================================================
 *  DEEP-CLEAN FUTILITY DETECTION
 * ================================================================ */
#define DEEPCLEAN_FUTILE_PCT       2    /* ≤2% recovered = futile            */
#define DEEPCLEAN_FUTILE_STRIKES   1
#define DEEPCLEAN_FUTILE_PAUSE_S  300

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
#define RANK_CACHE_SAVE_S  10

/* ================================================================
 *  IDLE MODE  (in adaptive_clean)
 *  When the phone has had no foreground-app change for IDLE_DETECT_S
 *  seconds (screen likely off / phone sitting idle), run a gentle
 *  COLD+STALE sweep up to IDLE_KILLS_MAX kills every IDLE_INTVL_S.
 *  Only fires when RAM is not already under pressure (pressure tiers
 *  take precedence).
 * ================================================================ */
#define IDLE_DETECT_S     300   /* seconds of no fg change = idle      */
#define IDLE_INTVL_S      120
#define IDLE_KILLS_MAX    2
#define IDLE_MIN_ZRAM_FREE_PCT 10
/* Age-based kill gates within IDLE tier */
#define IDLE_KILL_HOT_AGE_S    (14*60)
#define IDLE_PROTECT_PCT        50

/* Intense idle clean: device screen-off ≥30 min — best opportunity to
 * sweep harder and run compaction since nothing is user-visible. */
#define IDLE_DEEP_S            1800  /* 30 min continuous screen-off       */
#define IDLE_DEEP_KILLS_MAX    8     /* generous kill budget                */
#define IDLE_DEEP_INTVL_S      300   /* run at most every 5 min             */
#define IDLE_DEEP_TARGET_PCT   70
#define IDLE_DEEP_HOT_AGE_S    60    /* relaxed age gate — already idle 30m */
#define IDLE_DEEP_PROTECT_PCT   20

#define LOG_RATELIMIT_S           30   /* noisy-state logs: once per 30 s    */

#define RANK_EXEMPT   0
#define RANK_TIER1    1   /* was FOREGROUND+HOT */
#define RANK_TIER2    2   /* was WARM+COLD */
#define RANK_TIER3    3   /* was STALE */
#define RANK_TIER4    4   /* was BACKGROUND */

#define RANK_FREQ_MED_SESSIONS  8
#define RANK_FREQ_HIGH_SESSIONS 20
#define RANK_HOT_S       60          /*  <60 s → TIER1                 */
#define RANK_WARM_S      (10*60)     /*  <10 min → old WARM boundary, still used
                                       *  standalone by the TIER2 MAINT-clause
                                       *  split (see adaptive_clean) */
#define RANK_COLD_S      (60*60)     /*  <1 hr → TIER2; else TIER3     */

#define RANK1_HOLD_S     3

#define TIER1_MAX_SLOTS   4      /* tier1 tier capacity */

#define TIER1_PIN_MAX     4      /* of TIER1_MAX_SLOTS, how many get a real pin */

#define TIER1_PIN_HOLD_S  RETAIN_FG_AGE_S

#define TIER1_DWELL_S     300    /* min time in tier1 before demotion to tier2 */
#define TIER2_MAX_SLOTS   6
#define TIER2_PIN_MAX     3
#define TIER2_DWELL_S     420    /* min time in tier2 before demotion to tier3
                                  * (widened 240->420 same request as
                                  * TIER1_DWELL_S 180->300 above) */
#define BG_IDLE_DEBOUNCE_S 30
#define UNTRUSTED_PROMO_DEBOUNCE_S 30
#define CAPACITY_PROMO_DEBOUNCE_S 30

#define BLIP_HEADLESS_MIN_S   600   /* 10 cumulative min of non-genuine ADJ_FOREGROUND */

#define FLAP_WINDOW_MIN_S    (30*60)  /* don't judge a rate until the window
                                        * has been open at least this long —
                                        * avoids false alarms on a burst of
                                        * legitimate early re-opens */
#define FLAP_RATE_WARN_THRESH  15.0   /* flaps/hour that triggers a warn */
#define FLAP_WINDOW_RESET_S  (4*60*60) /* rolling window reset period, also
                                         * re-arms the one-shot warn */

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
#define FG_PROTECT_BASE   (30*60)
#define FG_PROTECT_MAX    (6*60*60)
/* Reduced windows under memory pressure */
#define FG_PROTECT_LOW_PCT  40  /* LOW state: % of computed window   */
#define FG_PROTECT_CRIT_S   20  /* CRITICAL state: flat floor (s)    */
#define MIN_BG_KILL_AGE_S    8

/* ================================================================
 *  APP RETENTION (oom pinning)
 * ================================================================ */

#define RETAIN_FG_AGE_S  900    /* only pin if used within 15 min    */
#define PIN_SETTLE_S     10
#define LEARN_RESET_GEN     7
#define LEARN_LOG_FILE     "/data/local/lmk_learn.log" /* legacy path, kept
                                  * only so score_load()'s gen-mismatch
                                  * archive step has something to rename —
                                  * the daemon no longer writes to this file
                                  * (AI_Swap learning model removed). */
#define RETAIN_MAX_N       6
#define RETAIN_PROTECT_GRACE_S 8
/* T3 critical-RAM override sparing — keep a couple of tier1-but-not-
 * true-fg apps (is_tier1_background(), was "rank-2 HOT") retained even
 * when overriding AI_Swap retention, so the user still has light
 * multitasking (e.g. switching back to the last app or two) instead of
 * every background app getting wiped in one critical sweep. */
#define T3_SPARE_HOT_N      2

/* ================================================================
 *  PROACTIVE LAUNCH TRIM
 * ================================================================ */
#define LAUNCH_TRIM_AVAIL_PCT     FREE_HIGH_PCT  /* only trim if avail below this   */
#define LAUNCH_TRIM_HEAVY_SWAP_KB (150 * 1024)   /* learned avg_swap_kb → "heavy"   */
#define LAUNCH_TRIM_TARGET_PCT    (FREE_HIGH_PCT - 3)
#define LAUNCH_TRIM_HEAVY_TARGET_PCT (FREE_HIGH_PCT + 7)
#define LAUNCH_TRIM_WARM_SKIP_S   90  /* skip RAM trim if relaunched within this   */

/* ================================================================
 *  SESSION TRACKING
 * ================================================================ */
#define SESSION_GAP_S     60   /* seconds of absence = new session  */

#define SESSION_DURATION_CAP_S     600
#define SESSION_DURATION_BONUS_MAX 150
#define MIN_GENUINE_SESSION_S      20

/* ================================================================
 *  RESTART RESILIENCE
 * ================================================================ */

#define RESTART_WINDOW_S    240
#define RESTART_CD_BASE_S   30
#define RESTART_CD_SCALE_S  20
#define RESTART_CD_MAX_S   180
#define BOUNCE_KILL_CD_MAX_S (30*60)

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

static const char * const SERVICE_EXEMPT[] = {
    "sandboxed_process",               /* webview/chrome sandboxed renderers */
    ":rcs",                            /* carrier RCS messaging service      */
    "googlequicksearchbox:interactor", /* GMS quicksearch interactor         */
    "com.truecaller",                  /* caller-ID/call-screening service;  */

                                        /* respawn cycles, rc 75->85, each    */
                                        /* kill freed 0-30MB RSS. OS re-wakes */
                                        /* it for call events regardless —   */
                                        /* pure battery/relaunch cost, no    */
                                        /* memory benefit.                   */
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

static char          g_true_fg_name[256] = {0};
static char          g_pending_fg_name[256] = {0};
static int           g_pending_fg_streak = 0;
#define FG_DEBOUNCE_TICKS 2 /* candidate must persist this many ticks before accepted */

#define FG_MIN_HOLD_S 3
#define FG_HOLD_WOBBLE_TOLERANCE_S 2

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
    bool   scheduled;
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

/* Adaptive clean trend tracking */
static int    g_adaptive_prev_zram_pct = 0;
static time_t g_adaptive_trend_t       = 0;

/* Idle mode tracking — updated when true-foreground app changes */
static time_t g_last_fg_change_t = 0;
static time_t g_screen_off_since = 0;  /* 0 = screen currently on */

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

#define RESET_GATE_S       (12*3600)  /* min time since boot/last reset  */
static time_t g_last_full_reset_t = 0;   /* 0 = not done yet this boot   */
static bool   g_reset_in_progress = false; /* tells check_bounced() this
                                             * kill was scheduled, not an
                                             * organic instability signal */

#define START_TIME_FILE  "/data/local/tmp/lmk_engine.start"

static time_t g_start_time           = 0;  /* daemon start epoch           */

/* Log rate-limit timestamps (prevent 4+ lines/s when ZRAM saturated) */
static time_t g_log_stuck_t          = 0;  /* last ZRAMstuck cd-active log */
static time_t g_log_crit_t           = 0;  /* last ZRAMcrit log            */
static time_t g_log_zramcd_t         = 0;  /* last ZRAMcd cooldown log     */

/* Deep-clean futility */
static int    g_deepclean_fail_cnt   = 0;  /* consecutive ≤2% cleans       */
static time_t g_deepclean_pause_until = 0; /* pause end timestamp          */

#define SCORE_FILE          "/data/local/lmk_scores.dat"

#define SCORE_JOURNAL_FILE          "/data/local/lmk_scores.journal"
#define SCORE_JOURNAL_CONSOLIDATE_N 400  /* consolidate after this many
                                           * journal-appended entries    */

#define CLASSIFY_IDLE_S        240  /* 4 min screen-off before classify starts */
#define SYSCHECK_QUEUE_MAX     128
#define SYSCHECK_DRAIN_PER_TICK  2   /* max dumpsys calls per idle tick   */
#define SYSCHECK_MAX_ATTEMPTS    4   /* give up + use fallback after this many
                                       * failed classify attempts, so a
                                       * genuinely un-lookupable name (a
                                       * process alias with no matching
                                       * installed package at any dot-level)
                                       * can't loop forever */
#define CATEGORY_RECONFIRM_S  (3L*24*3600)
#define SCORE_MAX_APPS      260
#define SCORE_MAX           1000   /* final score_compute() clamp only    */

#define FG_COUNT_MAX        100000
#define FG_COUNT_SCALE      100
#define SCORE_SAVE_INTVL    15
#define SCORE_AVG_ALPHA_PCT  25   /* EMA weight for new sample (25%)  */

#define GROWTH_WINDOW_S        600    /* sample window: 10 min             */
#define GROWTH_MIN_BASE_KB     51200  /* ignore <50MB procs (noise)        */
#define GROWTH_STRIKE_PCT      12     /* % growth in a window = 1 strike   */
#define GROWTH_STRIKES_FLAG    6      /* consecutive strikes (~1h) → flag  */
#define GROWTH_REALERT_S       3600   /* re-log a still-growing flag hourly*/

typedef struct {
    char   name[256];
    int    fg_count;       /* cumulative foreground ticks               */

    time_t raw_last_fg_do_not_read_directly;        /* last foreground timestamp                 */
    int    restart_count;  /* times seen alive <RESTART_WINDOW_S post-kill */
    long   avg_swap_kb;    /* EMA of swap usage in kB                  */

    long   avg_session_duration_s;

    time_t session_start_t;
    int    session_count;  /* distinct foreground sessions (v3)         */
    int    category;       /* 0=native daemon, 1=system app, 2=user app */
    int    dc_kill_count;  /* times killed by deep clean (v5)           */
    int    adaptive_kill_count; /* times killed by adaptive clean (v5)  */
    time_t retain_pin_t;   /* last tick this app was actively pinned.
                             * In-memory only — not persisted. */

    time_t last_restart_t;

    int    last_logged_rank;

    int    flap_count;
    time_t flap_window_start;
    bool   flap_warned;
    time_t last_bg;
    bool   was_true_fg;
    bool   dirty;

    time_t last_genuine_fg;

    time_t last_true_fg_seen;

    time_t bg_idle_since;

    time_t rank_fg_hold_start;

    time_t untrusted_promo_since;

    time_t capacity_demote_t;
    int    capacity_demote_from;

    int    static_class_cache;

    long   rss_watch_kb;      /* RSS sampled at start of current window   */
    time_t rss_watch_t;       /* when the current window started         */
    int    rss_growth_strikes;/* consecutive windows with sustained growth*/
    bool   flagged_runaway;   /* confirmed runaway — bias toward killing  */
    time_t runaway_alert_t;   /* last time we logged the alert            */

    bool   category_confirmed;

    time_t category_confirmed_at;

    time_t last_reclaim_t;

    bool   cg_high_set;   /* true if memory.high is currently < max on this app */
    time_t cg_high_set_t; /* when set, for a future safety-net max-duration release */
    bool   cg_low_set;    /* true if memory.low is currently > 0 on this app */

    time_t fg_hold_start;

    time_t fg_hold_last_genuine_t;

    time_t first_seen;

    time_t adj0_blip_total_s;
    time_t adj0_last_tick;

    int    rank_tier;        /* current dwell-confirmed tier; 0 = not yet seeded */
    time_t tier_entered_t;   /* when rank_tier was last set; 0 = not yet seeded */

    long   rank_computed_seq;

    int    rank_computed_result;

    time_t last_seen_t;
} AppScore;

static AppScore g_scores[SCORE_MAX_APPS];
static int      g_score_count     = 0;

static long g_rank_seq = 0;
static time_t   g_score_last_save = 0;
static bool     g_scores_dirty    = false;
static time_t   g_score_last_compact = 0;
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
    time_t   raw_last_fg_do_not_read_directly;
    int      score;
    int      restart_count;
    long     avg_swap_kb;
    int      bounce_count;
    time_t   bounce_first;      /* first kill in the window               */
    int      rank;
    time_t   last_genuine_fg;

    bool     true_fg;
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
    long compr_data_kb;
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

/* Forward declaration — full definition is in SCORE CATEGORY section below */
static int classify_score_cat(const char *name);

/* Forward declaration — full definition is in APP USAGE SCORING section below */
static AppScore *score_lookup(const char *name);

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
    /* Native binaries/daemons (cat 0) stay hard-exempt. SYSAPP (cat 1)
     * is deliberately NOT exempted here — a confirmed system-shipped
     * package (GMS, iwlan, pixel.digitalkey.*, ...) is routed to
     * RANK_BACKGROUND instead (see enumerate_procs()), so it stays
     * visible to check_runaway_growth() and MAINT/idle reclaim rather
     * than disappearing behind full exemption the way a blanket cat<2
     * check would have it. Prefer the dumpsys-confirmed category when
     * available, since classify_score_cat()'s curated prefix list alone
     * never recognizes these packages. */
    AppScore *as = score_lookup(name);
    int cat = (as && as->category_confirmed) ? as->category : classify_score_cat(name);
    return (cat == 0);
}

/* Forward declaration — full definition lives later in the file (multi-
 * signal importance score, 0..SCORE_MAX). Needed here because app_rank()
 * now consults it for score_dwell_mult_pct() below. */
static int score_compute(const AppScore *s);

static int score_dwell_mult_pct(int score) {
    if (score >= 800) return 200;   /* heavy daily-driver: double dwell   */
    if (score >= 500) return 160;
    if (score >= 200) return 130;
    return 100;                     /* baseline/junk: unchanged behavior */
}

static int app_rank(AppScore *as, bool true_fg, time_t recency,
                     bool recency_trusted, int session_count) {
    time_t now = time(NULL);
    int candidate;

    if (recency == 0 && !true_fg) candidate = RANK_TIER3;
    else {
        time_t age = true_fg ? 0 : now - recency;

        if (true_fg || (recency_trusted && age < RANK_HOT_S)) candidate = RANK_TIER1;
        else if (recency_trusted && age < RANK_COLD_S) candidate = RANK_TIER2;
        else                                candidate = RANK_TIER3;

        bool has_genuine_sessions = as && as->avg_session_duration_s >= MIN_GENUINE_SESSION_S;
        if (has_genuine_sessions && session_count >= RANK_FREQ_HIGH_SESSIONS && candidate > RANK_TIER1)
            candidate = RANK_TIER2;
        else if (has_genuine_sessions && session_count >= RANK_FREQ_MED_SESSIONS && candidate > RANK_TIER2)
            candidate = RANK_TIER2;
    }

    if (!as) return candidate;

    if (as->tier_entered_t == 0) {              /* first time seeding this app */
        as->rank_tier = candidate;
        as->tier_entered_t = now;
        return candidate;
    }
    if (candidate < as->rank_tier) {             /* promotion/reopen */

        bool untrusted_tier2_promo = (as->rank_tier == RANK_TIER3) &&
                                      (candidate == RANK_TIER2) &&
                                      !true_fg && !recency_trusted;
        if (untrusted_tier2_promo) {
            if (as->untrusted_promo_since == 0)
                as->untrusted_promo_since = now;
            if ((now - as->untrusted_promo_since) < UNTRUSTED_PROMO_DEBOUNCE_S)
                return as->rank_tier;            /* not held long enough yet */
        } else {
            as->untrusted_promo_since = 0;        /* any other promotion path resets it */
        }

        if (!true_fg && as->capacity_demote_from == candidate &&
            as->capacity_demote_t != 0 &&
            (now - as->capacity_demote_t) < CAPACITY_PROMO_DEBOUNCE_S)
            return as->rank_tier;                /* debounce not satisfied yet */
        as->capacity_demote_t = 0;
        as->capacity_demote_from = 0;
        as->rank_tier = candidate;
        as->tier_entered_t = now;
        as->untrusted_promo_since = 0;   /* debounce satisfied or n/a — clear for next cycle */
        return candidate;
    }
    as->untrusted_promo_since = 0;                /* not promoting this tick */
    if (candidate > as->rank_tier) {             /* demotion: dwell-gated */
        int base_dwell = (as->rank_tier == RANK_TIER1) ? TIER1_DWELL_S :
                          (as->rank_tier == RANK_TIER2) ? TIER2_DWELL_S : 0;

        int dwell = (base_dwell * score_dwell_mult_pct(score_compute(as))) / 100;
        if ((now - as->tier_entered_t) < dwell)
            return as->rank_tier;                /* hold current tier a while longer */
        as->rank_tier = candidate;
        as->tier_entered_t = now;
        return candidate;
    }

    if (true_fg) as->tier_entered_t = now;
    return as->rank_tier;                        /* unchanged */
}

static inline bool is_never_kill_now(const ProcInfo *p) {
    return p->rank == RANK_EXEMPT || p->true_fg;
}

static inline bool is_tier1_background(const ProcInfo *p) {
    return p->rank == RANK_TIER1 && !p->true_fg;
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

static long zram_orig_data_kb(long *compr_kb_out) {
    if (!g_zram_sys[0]) return -1;
    char path[256]; snprintf(path, sizeof(path), "%s/mm_stat", g_zram_sys);
    FILE *f = fopen(path,"r"); if (!f) return -1;
    unsigned long long orig = 0, compr = 0;
    int n = fscanf(f, "%llu %llu", &orig, &compr);
    fclose(f);
    if (compr_kb_out) *compr_kb_out = (n >= 2) ? (long)(compr / 1024) : -1;
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
    long orig = zram_orig_data_kb(&z->compr_data_kb);
    z->orig_data_kb = (orig > 0) ? orig : (z->used_kb * 4 / 10);
    /* compr_data_kb fallback: if mm_stat's second field wasn't readable
     * (older kernel format, parse failure), leave it at 0 rather than
     * guessing — callers must check >0 before trusting it, same pattern
     * as orig_data_kb's fallback but without inventing a fake ratio. */
    if (z->compr_data_kb < 0) z->compr_data_kb = 0;
    if (z->disksize_kb > 0)
        z->used_pct = (int)((z->used_kb * 100) / z->disksize_kb);
    else if (mi.swap_total_kb > 0)
        z->used_pct = (int)((z->used_kb * 100) / mi.swap_total_kb);
    else
        z->used_pct = 0;
}

static pid_t g_kswapd_pid          = -1;
static unsigned long g_last_self_jf    = 0;
static unsigned long g_last_kswapd_jf  = 0;
static time_t        g_last_cpu_smpl_t = 0;

static unsigned long read_proc_cpu_jiffies(pid_t pid) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[512];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    /* comm field can contain spaces/parens, so parse from the last ')' */
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    unsigned long utime = 0, stime = 0;
    int n = sscanf(p + 2,
        "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
        &utime, &stime);
    if (n != 2) return 0;
    return utime + stime;
}

static unsigned long long read_proc_starttime(pid_t pid) {
    char path[64]; snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[512];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    unsigned long long starttime = 0;
    int n = sscanf(p + 2,
        "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
        &starttime);
    if (n != 1) return 0;
    return starttime;
}

static pid_t find_kswapd_pid(void) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *de;
    pid_t found = -1;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        /* buffer sized generously — de->d_name here is always a numeric
         * PID (already filtered by isdigit(de->d_name[0]) above), but a
         * bare %s can't be proven bounded by gcc statically, hence the
         * -Wformat-truncation false positive at the old 64-byte size. */
        char path[300]; snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char comm[64] = {0};
        if (!fgets(comm, sizeof(comm), f)) { fclose(f); continue; }
        fclose(f);
        if (!strncmp(comm, "kswapd", 6)) { found = (pid_t)atoi(de->d_name); break; }
    }
    closedir(d);
    return found;
}

static bool      g_kswapd_pinned     = false;
static cpu_set_t g_little_mask;
static int       g_little_core_count = 0;

#define KSWAPD_EVENT_UNPIN_S   8   /* stay unpinned this long after any kill/squeeze */
static time_t g_kswapd_event_unpin_until = 0;

#define KSWAPD_PIN_DWELL_S     15  /* min time before re-pinning to little cluster */
static time_t g_kswapd_last_transition_t = 0;

/* Called on any kill (record_kill(), any source) or successful squeeze/
 * reclaim — the two event classes confirmed correlated with kswapd
 * spikes. Cheap: just extends a deadline, no syscalls here. */
static void kswapd_note_disruptive_event(void) {
    g_kswapd_event_unpin_until = time(NULL) + KSWAPD_EVENT_UNPIN_S;
}

/* Groups all present cores by distinct cpuinfo_max_freq value. The
 * lowest-frequency group becomes `mask`. Returns false (mask left
 * empty) on a single-cluster/symmetric device — caller must skip
 * pinning entirely in that case rather than isolating an arbitrary
 * subset of cores for no real benefit. Tri-cluster SoCs (big/mid/
 * little) are handled the same way: only the lowest-freq group is
 * ever selected, the mid cluster is simply never matched — no
 * separate three-way branch needed. */
static bool detect_little_cluster(cpu_set_t *mask, int *count_out) {
    CPU_ZERO(mask);
    *count_out = 0;

    long freqs[64]; int core_ids[64]; int n = 0;
    for (int i = 0; i < 64; i++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        FILE *f = fopen(path, "r");
        if (!f) {
            if (i == 0) continue;   /* be tolerant of a missing cpu0 file */
            break;                  /* core IDs are contiguous on Android — first gap = end */
        }
        long freq = 0;
        int got = fscanf(f, "%ld", &freq);
        fclose(f);
        if (got != 1 || freq <= 0) continue;
        freqs[n] = freq; core_ids[n] = i; n++;
    }
    if (n < 2) return false;   /* nothing usable to group */

    long min_freq = freqs[0], max_freq = freqs[0];
    for (int i = 1; i < n; i++) {
        if (freqs[i] < min_freq) min_freq = freqs[i];
        if (freqs[i] > max_freq) max_freq = freqs[i];
    }
    if (min_freq == max_freq) return false;   /* symmetric — no little cluster */

    for (int i = 0; i < n; i++) {
        if (freqs[i] == min_freq) {
            CPU_SET(core_ids[i], mask);
            (*count_out)++;
        }
    }
    return *count_out > 0;
}

/* Pins kswapd onto the detected little cluster. Never retry-loops on
 * failure (e.g. a PF_NO_SETAFFINITY kernel thread, though kswapd
 * normally doesn't carry that flag) — falls back to kswapd's default
 * affinity, which is the safe no-op state, not a broken one. */
static void pin_kswapd_to_little_cores(void) {
    if (g_kswapd_pid < 0) g_kswapd_pid = find_kswapd_pid();
    if (g_kswapd_pid <= 0) {
        logw("KswapdPin: kswapd PID not found — skipping");
        return;
    }
    if (!detect_little_cluster(&g_little_mask, &g_little_core_count)) {
        logi("KswapdPin: single-cluster/symmetric CPU topology — skipping "
             "(no little cluster to isolate kswapd onto)");
        return;
    }
    if (sched_setaffinity(g_kswapd_pid, sizeof(cpu_set_t), &g_little_mask) != 0) {
        logw("KswapdPin: sched_setaffinity failed: %s — kswapd left at "
             "default affinity", strerror(errno));
        return;
    }
    g_kswapd_pinned = true;
    logi("KswapdPin: kswapd (pid=%d) pinned to %d little core(s)",
         g_kswapd_pid, g_little_core_count);
}

/* Unpins kswapd back to all cores. Called only at ZRAM_CRIT_PCT — at
 * that pressure level we want reclaim as fast as possible, and
 * artificially slowing it down by restricting it to the little
 * cluster could make a near-OOM situation worse, not better. */
static void unpin_kswapd(void) {
    if (!g_kswapd_pinned || g_kswapd_pid <= 0) return;
    cpu_set_t all_mask;
    CPU_ZERO(&all_mask);
    long ncpus = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpus < 1) ncpus = 8;
    for (int i = 0; i < ncpus && i < CPU_SETSIZE; i++) CPU_SET(i, &all_mask);
    if (sched_setaffinity(g_kswapd_pid, sizeof(cpu_set_t), &all_mask) == 0) {
        g_kswapd_pinned = false;
        logi("KswapdPin: ZRAM_CRIT_PCT reached — unpinned kswapd to all cores");
    } else {
        logw("KswapdPin: unpin failed: %s", strerror(errno));
    }
}

/* Re-pins back to the little cluster once pressure drops back under
 * STUCK after a CRIT-triggered unpin. No-op if already pinned or if
 * detection never found a little cluster to begin with. */
static void repin_kswapd_if_needed(void) {
    if (g_kswapd_pinned || g_little_core_count == 0) return;
    if (g_kswapd_pid <= 0) return;
    if (sched_setaffinity(g_kswapd_pid, sizeof(cpu_set_t), &g_little_mask) == 0) {
        g_kswapd_pinned = true;
        logi("KswapdPin: pressure eased below STUCK — re-pinned to little cores");
    } else {
        logw("KswapdPin: re-pin failed: %s", strerror(errno));
    }
}

/* Returns self/kswapd CPU usage as % of one core since the previous
 * call. First call always returns 0/0 (no prior sample to diff against). */
static void cpu_track_sample(double *self_pct_out, double *kswapd_pct_out) {
    *self_pct_out = 0.0; *kswapd_pct_out = 0.0;
    if (g_kswapd_pid < 0) g_kswapd_pid = find_kswapd_pid();

    unsigned long self_jf   = read_proc_cpu_jiffies(getpid());
    unsigned long kswapd_jf = (g_kswapd_pid > 0) ? read_proc_cpu_jiffies(g_kswapd_pid) : 0;
    time_t now = time(NULL);
    long clk_tck = sysconf(_SC_CLK_TCK);

    if (g_last_cpu_smpl_t > 0 && now > g_last_cpu_smpl_t && clk_tck > 0) {
        double elapsed = (double)(now - g_last_cpu_smpl_t);
        *self_pct_out = 100.0 * (double)(self_jf - g_last_self_jf) / clk_tck / elapsed;
        if (g_kswapd_pid > 0)
            *kswapd_pct_out = 100.0 * (double)(kswapd_jf - g_last_kswapd_jf) / clk_tck / elapsed;
    }
    g_last_self_jf    = self_jf;
    g_last_kswapd_jf  = kswapd_jf;
    g_last_cpu_smpl_t = now;
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
        bool swapoff_ok = false;
        for (int t = 0; t < 3 && !swapoff_ok; t++) {
            if (swapoff(g_zram_dev) == 0) { swapoff_ok = true; break; }
            if (t < 2) usleep(200000); /* transient EBUSY at boot — brief backoff */
        }
        if (!swapoff_ok) {

            loge("ZRAM: swapoff failed after 3 attempts: %s — adopting as-is; "
                 "will retry resize once ZRAM drops to <=50%% used",
                 strerror(errno));
            g_zram_active       = true;
            g_zram_needs_resize = true;
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

static void zram_cycle(bool bypass_gate) {
    if (!g_zram_dev[0] || !g_zram_active) return;
    MemInfo mi; read_meminfo(&mi);
    ZramInfo z; zram_read_stats(&z);

    long need_kb = z.orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
    if (mi.avail_kb < need_kb && !bypass_gate) {
        logi("ZRAMcycle: need %ldMB free (orig=%ldMB + margin), have %ldMB — skip",
             need_kb/1024, z.orig_data_kb/1024, mi.avail_kb/1024);
        return;
    }
    if (mi.avail_kb < need_kb && bypass_gate) {
        logw("ZRAMcycle: avail_kb gate would have skipped (need %ldMB, have "
             "%ldMB) — proceeding anyway, --force-zram-cycle --yes-i-know",
             need_kb/1024, mi.avail_kb/1024);
    }

    logi("ZRAMcycle: flushing (used=%ldMB orig=%ldMB avail=%ldMB)…",
         z.used_kb/1024, z.orig_data_kb/1024, mi.avail_kb/1024);

    int zc_attempt, zc_rc = -1;
    for (zc_attempt = 1; zc_attempt <= ZRAM_CYCLE_RETRIES; zc_attempt++) {
        zc_rc = swapoff(g_zram_dev);
        if (zc_rc == 0) break;
        if (errno != EINTR) break;
        usleep(ZRAM_CYCLE_RETRY_BACKOFF_US);
    }
    if (zc_rc < 0) {
        loge("ZRAMcycle: swapoff failed after %d attempt(s): %s — abandoning "
             "this cycle, will retry next interval", zc_attempt, strerror(errno));
        return;
    }
    if (zc_attempt > 1) {
        logi("ZRAMcycle: swapoff succeeded on attempt %d/%d",
             zc_attempt, ZRAM_CYCLE_RETRIES);
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

#define ZRAM_RESIZE_STUCK_WARN_S 1800  /* 30min */
static time_t g_zram_resize_deferred_since = 0;
static time_t g_zram_resize_defer_log_t = 0;
static bool   g_zram_resize_stuck_warned = false;
static void zram_try_deferred_resize(MemInfo *mi) {
    if (!g_zram_active || !g_zram_dev[0] || !g_zram_sys[0]) return;

    ZramInfo z; zram_read_stats(&z);
    long need_kb = z.orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
    if (z.used_pct > 50 || mi->avail_kb < need_kb) {
        time_t now = time(NULL);
        if (g_zram_resize_deferred_since == 0)
            g_zram_resize_deferred_since = now;
        long stuck_s = now - g_zram_resize_deferred_since;
        if (now - g_zram_resize_defer_log_t >= LOG_RATELIMIT_S) {
            g_zram_resize_defer_log_t = now;
            logi("ZRAMresize: deferred %lds (used=%d%% need<=50%%, "
                 "avail=%ldMB need=%ldMB)",
                 stuck_s, z.used_pct, mi->avail_kb/1024, need_kb/1024);
        }
        if (stuck_s >= ZRAM_RESIZE_STUCK_WARN_S && !g_zram_resize_stuck_warned) {
            g_zram_resize_stuck_warned = true; /* once per stuck episode */
            loge("ZRAMresize: stuck deferred for %lds — resize target not "
                 "yet applied", stuck_s);
        }
        return;
    }
    g_zram_resize_deferred_since = 0; /* conditions cleared - reset tracker */
    g_zram_resize_stuck_warned = false;

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
 *  PSI EVENT-DRIVEN MONITOR
 * ================================================================ */

static int g_psi_fd = -1;

static void psi_monitor_init(void) {
    if (!g_psi_available) return;
    int fd = open("/proc/pressure/memory", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        logw("PSI: monitor open failed (%s) — falling back to tick-only PSI reads",
             strerror(errno));
        return;
    }
    const char trig[] = "full 100000 1000000";
    if (write(fd, trig, sizeof(trig)) < 0) {
        logw("PSI: monitor trigger register failed (%s) — falling back to "
             "tick-only PSI reads", strerror(errno));
        close(fd);
        return;
    }

    const char trig_some[] = "some 70000 1000000";
    if (write(fd, trig_some, sizeof(trig_some)) < 0) {
        logw("PSI: 'some' trigger register failed (%s) — 'full' trigger "
             "still armed", strerror(errno));
    }
    g_psi_fd = fd;
    logi("PSI: event monitor armed (full-stall >100ms/1s + partial-stall "
         ">70ms/1s window, speculative threshold)");
}

/* Sleeps up to sleep_us, but wakes immediately if the PSI trigger fires.
 * Falls back to a plain usleep() if the monitor isn't armed, so this is a
 * strict addition — behavior is unchanged when g_psi_fd < 0. */
static bool wait_for_next_tick(useconds_t sleep_us) {
    if (g_psi_fd < 0) { usleep(sleep_us); return false; }
    struct pollfd pfd = { .fd = g_psi_fd, .events = POLLPRI };
    int timeout_ms = (int)(sleep_us / 1000);
    if (timeout_ms < 1) timeout_ms = 1;
    int r = poll(&pfd, 1, timeout_ms);
    if (r > 0 && (pfd.revents & POLLPRI)) {
        static time_t last_log = 0;
        time_t now = time(NULL);
        if (now - last_log >= LOG_RATELIMIT_S) {
            logi("PSI: pressure trigger fired — reacting early");
            last_log = now;
        }
        return true;
    }
    return false;
}

/* ================================================================
 *  DYNAMIC SWAPPINESS — from reference engine concept
 *  Higher ZRAM usage → lower swappiness to preserve headroom.
 * ================================================================ */
static void update_swappiness(int zram_pct) {

#define SWAP_HYST 3
    static int tier = 0; /* 0=100 1=80 2=60 3=40 4=20 */

    static const int enter[]   = {  0, 60, 82, 86, 94 }; /* rising boundary into tier i */
    static const int target[]  = {100, 80, 60, 40, 20 };

    int t = tier;
    while (t < 4 && zram_pct >= enter[t + 1] + SWAP_HYST) t++;
    while (t > 0 && zram_pct <  enter[t]     - SWAP_HYST) t--;
    if (t == tier) return;
    tier = t;

    FILE *f = fopen("/proc/sys/vm/swappiness", "w");
    if (f) { fprintf(f, "%d\n", target[tier]); fclose(f); }
    logi("Swappiness: ZRAM=%d%% → %d (tier=%d)", zram_pct, target[tier], tier);
}

/* ================================================================
 *  SLEEP / DOZE DETECTION — from reference engine
 * ================================================================ */

#define SCREEN_POLL_INTVL_S 5
static bool is_screen_off(void) {
    static time_t last_check = 0;
    static bool   cached = false;
    time_t now = time(NULL);
    if (now - last_check < SCREEN_POLL_INTVL_S) return cached;
    last_check = now;
    FILE *p = popen("dumpsys power 2>/dev/null | grep -o 'mWakefulness=[A-Za-z]*'", "r");
    if (!p) return cached;
    char buf[64] = {0};
    fgets(buf, sizeof(buf), p); pclose(p);

    cached = strstr(buf, "Asleep") != NULL || strstr(buf, "Dozing") != NULL;
    return cached;
}

static bool is_doze_active(void) {
    static time_t last_check = 0;
    static bool   cached = false;
    time_t now = time(NULL);
    if (now - last_check < SCREEN_POLL_INTVL_S) return cached;
    last_check = now;
    FILE *p = popen("dumpsys deviceidle get deep 2>/dev/null", "r");
    if (!p) return cached;
    char buf[32] = {0};
    fgets(buf, sizeof(buf), p); pclose(p);
    int n = (int)strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
    cached = strcmp(buf, "IDLE") == 0;
    return cached;
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
    char tmp_pkgs[64][128] = {{0}};

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

    AppScore *s = score_lookup(name);
    if (s) {
            int rc = s->restart_count;
            int cd = RESTART_CD_BASE_S + rc * RESTART_CD_SCALE_S;

            int cap = (rc >= BOUNCE_SUPPRESS_RC) ? BOUNCE_KILL_CD_MAX_S
                                                  : RESTART_CD_MAX_S;
            return cd > cap ? cap : cd;
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

/* Forward decls: defined later */
static AppScore *score_lookup(const char *name);
static int score_compute(const AppScore *s);

static void record_kill(ProcInfo *p) {
    time_t now = time(NULL);
    const char *name = p->name;

    kswapd_note_disruptive_event();

    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        if (strcmp(g_kill_hist[i].name, name) == 0) {
            g_kill_hist[i].killed_at = now;
            g_kill_hist[i].bounced   = false;
            g_kill_hist[i].scheduled = g_reset_in_progress;
            return;
        }
    }
    strncpy(g_kill_hist[g_kill_hist_idx].name, name, 255);
    g_kill_hist[g_kill_hist_idx].name[255]   = '\0';
    g_kill_hist[g_kill_hist_idx].killed_at   = now;
    g_kill_hist[g_kill_hist_idx].bounced     = false;
    g_kill_hist[g_kill_hist_idx].scheduled   = g_reset_in_progress;
    g_kill_hist_idx = (g_kill_hist_idx + 1) % KILL_HIST_SIZE;
}

static void check_bounced(const char *name) {
    time_t now = time(NULL);
    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        if (g_kill_hist[i].name[0] &&
            strcmp(g_kill_hist[i].name, name) == 0 &&
            !g_kill_hist[i].bounced &&
            (now - g_kill_hist[i].killed_at) < RESTART_WINDOW_S) {
            g_kill_hist[i].bounced = true;
            if (g_kill_hist[i].scheduled) {

                continue;
            }
            for (int j = 0; j < g_score_count; j++) {
                if (strcmp(g_scores[j].name, name) == 0) {
                    g_scores[j].restart_count++;
                    g_scores[j].last_restart_t = now;
                    g_scores[j].dirty = true;
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

    if (strchr(name, '/')) return 0;
    static const char * const NATIVE_NAMES[] = {
        "zygote","zygote64","webview_zygote","system_server",
        "magiskd","logcat","top","su","lmk_engine",
        "<pre-initialized>",
        NULL
    };
    for (int i = 0; NATIVE_NAMES[i]; i++)
        if (!strcmp(name, NATIVE_NAMES[i])) return 0;
    if (strncmp(name, "media.", 6) == 0) return 0;

    {
        char base[256]; strncpy(base, name, 255); base[255] = '\0';
        char *colon = strchr(base, ':'); if (colon) *colon = '\0';
        if (!strchr(base, '.')) return 0;
    }

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

#define SCORE_HASH_SIZE 512  /* power of two, > 2x SCORE_MAX_APPS */
static int  g_score_hash[SCORE_HASH_SIZE];
static bool g_score_hash_dirty = true;

static unsigned score_hash_str(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)(*s++); h *= 16777619u; }
    return h;
}

static void score_hash_rebuild(void) {
    for (int i = 0; i < SCORE_HASH_SIZE; i++) g_score_hash[i] = -1;
    for (int i = 0; i < g_score_count; i++) {
        unsigned h = score_hash_str(g_scores[i].name) & (SCORE_HASH_SIZE - 1);
        while (g_score_hash[h] != -1) h = (h + 1) & (SCORE_HASH_SIZE - 1);
        g_score_hash[h] = i;
    }
    g_score_hash_dirty = false;
}

static AppScore *score_lookup(const char *name) {
    if (g_score_hash_dirty) score_hash_rebuild();
    unsigned h = score_hash_str(name) & (SCORE_HASH_SIZE - 1);
    for (int probes = 0; probes < SCORE_HASH_SIZE; probes++) {
        int idx = g_score_hash[h];
        if (idx == -1) return NULL;
        if (strcmp(g_scores[idx].name, name) == 0) return &g_scores[idx];
        h = (h + 1) & (SCORE_HASH_SIZE - 1);
    }
    return NULL;
}

static int score_compute(const AppScore *s) {
    if (!s || s->raw_last_fg_do_not_read_directly == 0) return 0;
    time_t now = time(NULL);
    long idle_h = (now - s->raw_last_fg_do_not_read_directly) / 3600;
    long idle_d = idle_h / 24;

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

    int duration_bonus = 0;
    if (s->avg_session_duration_s > 0) {
        long capped = s->avg_session_duration_s > SESSION_DURATION_CAP_S
                      ? SESSION_DURATION_CAP_S : s->avg_session_duration_s;
        duration_bonus = (int)(capped * SESSION_DURATION_BONUS_MAX / SESSION_DURATION_CAP_S);
    }

    int sc = ticks + freq_bonus + recency + duration_bonus;

    return sc > SCORE_MAX ? SCORE_MAX : (sc < 0 ? 0 : sc);
}

static int score_get(const char *name) {
    AppScore *s = score_lookup(name);
    return s ? score_compute(s) : 0;
}

static int score_protect_window_ram(const char *name, int avail_pct) {
    int sc  = score_get(name);

    int win = FG_PROTECT_BASE + (int)((long)sc * FG_PROTECT_MAX / SCORE_MAX);
    if (win > FG_PROTECT_MAX) win = FG_PROTECT_MAX;

    if (avail_pct < FREE_CRIT_PCT)  return FG_PROTECT_CRIT_S;
    if (avail_pct < FREE_LOW_PCT)   return win * FG_PROTECT_LOW_PCT / 100;

    if (g_zram_used_pct > 0 && g_zram_used_pct < (ZRAM_WARN_PCT - 15))
        win = win * 130 / 100;
    else if (g_zram_used_pct < ZRAM_WARN_PCT)
        win = win * 100 / 100;         /* no discount below WARN_PCT */
    else if (g_zram_used_pct < ZRAM_TRIM_PCT)
        win = win * 80 / 100;
    else if (g_zram_used_pct < ZRAM_STUCK_PCT)
        win = win * 50 / 100;
    else
        win = win * 20 / 100;          /* was ~8% compounded; now a clean 20% */
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

typedef struct {
    char name[256];
    int  attempts;
} SysCheckEntry;
static SysCheckEntry g_syscheck_queue[SYSCHECK_QUEUE_MAX];
static int  g_syscheck_queue_n = 0;

/* Queue `name` (its `:`-suffix stripped, since process aliases like
 * ":persistent" share the base package's install record) for a later
 * dumpsys confirmation, unless already queued or the queue is full. Dot
 * suffixes are NOT stripped — e.g. com.google.android.gms.ui/.unstable are
 * genuinely separate installed packages and need their own lookup. */
static void classify_queue_maybe(const char *name) {
    char base[256];
    strncpy(base, name, 255); base[255] = '\0';
    char *colon = strchr(base, ':');
    if (colon) *colon = '\0';
    if (!base[0] || strchr(base, '/')) return; /* native binaries, not packages */
    if (!strchr(base, '.')) return; /* dot-less = bare command name, already
                                       * classified native by classify_score_cat() */

    for (int i = 0; i < g_syscheck_queue_n; i++)
        if (!strcmp(g_syscheck_queue[i].name, base)) return; /* already queued */
    if (g_syscheck_queue_n >= SYSCHECK_QUEUE_MAX) {
        logw("Classify: queue full (%d), dropping [%s]", SYSCHECK_QUEUE_MAX, base);
        return;
    }

    strncpy(g_syscheck_queue[g_syscheck_queue_n].name, base, 255);
    g_syscheck_queue[g_syscheck_queue_n].name[255] = '\0';
    g_syscheck_queue[g_syscheck_queue_n].attempts = 0;
    g_syscheck_queue_n++;
}

/* Internal: requeue with a preserved attempt count (used on retry, unlike
 * classify_queue_maybe() which is for brand-new names only). */
static void classify_queue_requeue(const char *name, int attempts) {
    for (int i = 0; i < g_syscheck_queue_n; i++)
        if (!strcmp(g_syscheck_queue[i].name, name)) return; /* already queued */
    if (g_syscheck_queue_n >= SYSCHECK_QUEUE_MAX) {
        logw("Classify: queue full (%d), dropping [%s] on retry", SYSCHECK_QUEUE_MAX, name);
        return;
    }
    strncpy(g_syscheck_queue[g_syscheck_queue_n].name, name, 255);
    g_syscheck_queue[g_syscheck_queue_n].name[255] = '\0';
    g_syscheck_queue[g_syscheck_queue_n].attempts = attempts;
    g_syscheck_queue_n++;
}

/* Runs `dumpsys package <pkg>` once and reports whether a parseable
 * flags=[ ... ] line was found and whether it carried SYSTEM. */
static bool classify_try(const char *pkg, bool *is_system) {
    char cmd[300];
    snprintf(cmd, sizeof(cmd),
             "/system/bin/dumpsys package %s 2>/dev/null", pkg);
    FILE *fp = popen(cmd, "r");
    if (!fp) return false;

    bool parsed = false;
    *is_system = false;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *flags = strstr(line, "flags=[");
        if (!flags) continue;
        parsed = true;
        char *p = flags + 7;
        while (*p && *p != ']') {
            while (*p == ' ') p++;
            char tok[64]; int ti = 0;
            while (*p && *p != ' ' && *p != ']' && ti < 63) tok[ti++] = *p++;
            tok[ti] = '\0';
            if (!strcmp(tok, "SYSTEM")) { *is_system = true; break; }
        }
        break; /* only the flags=[ line matters */
    }
    pclose(fp);
    return parsed;
}

/* Apply a resolved category to every tracked entry whose colon-stripped
 * name matches pkg exactly (fixes alias-only packages like gearhead,
 * which only ever exist as ":car"/":provider" — never a bare entry). */
static int classify_apply(const char *pkg, int category) {
    int applied = 0;
    for (int i = 0; i < g_score_count; i++) {
        AppScore *s = &g_scores[i];
        char ename[256];
        strncpy(ename, s->name, 255); ename[255] = '\0';
        char *ecolon = strchr(ename, ':');
        if (ecolon) *ecolon = '\0';
        if (strcmp(ename, pkg) != 0) continue;

        s->category = category;
        s->category_confirmed = true;
        s->category_confirmed_at = time(NULL);
        s->dirty = true;
        g_scores_dirty = true;
        applied++;
    }
    return applied;
}

/* Drain up to SYSCHECK_DRAIN_PER_TICK queued names per call, gated to
 * screen-off ≥ CLASSIFY_IDLE_S. For each, run `dumpsys package <pkg>`.
 * If that exact name doesn't resolve (common for internal process
 * aliases like "com.google.android.gms.ui"/".learning"/".unstable" or
 * "com.zhiliaoapp.musically.go", which are process names inside a
 * package, not separately installed packages), walk up the dot-segments
 * (gms.ui → gms) and try each shorter candidate — the first one that
 * resolves supplies the answer, which is then applied to the ORIGINAL
 * name's entries. If nothing resolves at any level, retry a few times
 * (SYSCHECK_MAX_ATTEMPTS) in case it's a transient dumpsys hiccup, then
 * give up and fall back to classify_score_cat()'s guess — permanently
 * marking it confirmed so it can never loop forever. */
static void classify_dumpsys_drain(void) {
    /* Decoupled from the IDLE-DEEP tier (tier==-3, 30 min) — a dumpsys
     * lookup is cheap and one-off, so it only needs the screen to have
     * been off CLASSIFY_IDLE_S (7 min), not the full conservative bar
     * that kill-tier escalation uses. */
    if (g_screen_off_since == 0) return;
    if ((time(NULL) - g_screen_off_since) < CLASSIFY_IDLE_S) return;
    int drained = 0;
    while (g_syscheck_queue_n > 0 && drained < SYSCHECK_DRAIN_PER_TICK) {
        char pkg[256]; int attempts = g_syscheck_queue[0].attempts;
        strncpy(pkg, g_syscheck_queue[0].name, 255); pkg[255] = '\0';
        /* pop front */
        for (int i = 1; i < g_syscheck_queue_n; i++)
            g_syscheck_queue[i - 1] = g_syscheck_queue[i];
        g_syscheck_queue_n--;
        drained++;

        char candidate[256];
        strncpy(candidate, pkg, 255); candidate[255] = '\0';
        bool parsed = false, is_system = false;
        for (;;) {
            parsed = classify_try(candidate, &is_system);
            if (parsed) break;
            char *dot = strrchr(candidate, '.');
            if (!dot) break; /* nothing shorter left to try */
            *dot = '\0';
        }

        if (parsed) {
            int applied = classify_apply(pkg, is_system ? 1 : 2);
            if (applied > 0) {
                logi("Classify: [%s] confirmed category=%d (SYSTEM=%s)%s, applied to %d entr%s",
                     pkg, is_system ? 1 : 2, is_system ? "yes" : "no",
                     strcmp(candidate, pkg) ? " via parent lookup" : "",
                     applied, applied == 1 ? "y" : "ies");
            } else {
                logw("Classify: [%s] confirmed category=%d (SYSTEM=%s) but no matching entries found",
                     pkg, is_system ? 1 : 2, is_system ? "yes" : "no");
            }
            continue;
        }

        attempts++;
        if (attempts >= SYSCHECK_MAX_ATTEMPTS) {
            int fallback = classify_score_cat(pkg);
            int applied = classify_apply(pkg, fallback);
            logw("Classify: giving up on [%s] after %d attempts (no dumpsys match at any level), "
                 "using fallback category=%d, applied to %d entr%s",
                 pkg, attempts, fallback, applied, applied == 1 ? "y" : "ies");
        } else {
            logw("Classify: no parseable dumpsys output for [%s] (attempt %d/%d), requeueing",
                 pkg, attempts, SYSCHECK_MAX_ATTEMPTS);
            classify_queue_requeue(pkg, attempts);
        }
    }
}

static void category_reconfirm_sweep(void) {
    static time_t last_sweep_t = 0;
    time_t now = time(NULL);

    if (last_sweep_t == 0) { last_sweep_t = now; return; }
    if ((now - last_sweep_t) < CATEGORY_RECONFIRM_S) return;
    last_sweep_t = now;

    int requeued = 0;
    for (int i = 0; i < g_score_count; i++) {
        AppScore *s = &g_scores[i];
        if (!s->category_confirmed) continue;
        /* confirmed_at==0 means unknown/legacy (pre-v9 file, or never
         * actually timestamped) — treat as immediately eligible, same
         * "unknown = not yet trustworthy" convention used elsewhere for
         * first_seen==0. Everything else only re-checks past the window. */
        if (s->category_confirmed_at != 0 &&
            (now - s->category_confirmed_at) < CATEGORY_RECONFIRM_S) continue;
        s->category_confirmed = false;
        s->dirty = true;
        g_scores_dirty = true;
        classify_queue_maybe(s->name);
        requeued++;
    }
    if (requeued > 0)
        logi("CategoryReconfirm: re-queued %d stale/unverified entr%s for a fresh dumpsys check",
             requeued, requeued == 1 ? "y" : "ies");
}

/* Shared with score_touch()'s new-entry branch below: reuse the
 * lowest-scoring existing entry once SCORE_MAX_APPS is full, instead of
 * silently refusing to track anything new. The enumerate_procs()
 * background-service stub creation and rank_cache_load()'s bootstrap
 * stub creation both use this now — without it, a full table meant a
 * newly-discovered persistent service could be locked out indefinitely
 * by older, less relevant entries that happened to fill the table first. */
#define EVICT_CONFIRMED_BONUS 80

static AppScore *score_alloc_slot(void) {
    if (g_score_count < SCORE_MAX_APPS)
        return &g_scores[g_score_count++];

    int min_idx = 0;
    int min_key = score_compute(&g_scores[0]) +
                  (g_scores[0].category_confirmed ? EVICT_CONFIRMED_BONUS : 0);
    for (int i = 1; i < g_score_count; i++) {
        int key = score_compute(&g_scores[i]) +
                  (g_scores[i].category_confirmed ? EVICT_CONFIRMED_BONUS : 0);
        if (key < min_key) { min_key = key; min_idx = i; }
    }

    logw("ScoreEvict: [%s] evicted for a new entry (score=%d confirmed=%d "
         "fg_count=%d table=%d/%d)",
         g_scores[min_idx].name, min_key, g_scores[min_idx].category_confirmed ? 1 : 0,
         g_scores[min_idx].fg_count, g_score_count, SCORE_MAX_APPS);
    return &g_scores[min_idx];
}

/* Lookup an AppScore entry, creating one if it doesn't exist yet. Shared
 * allocation/classification logic used by score_touch() and by the
 * genuine-fg hold-tracking in track_foreground(), which needs an entry to
 * exist to store fg_hold_start even before a session is credited. */
static AppScore *score_lookup_or_create(const char *name) {
    AppScore *s = score_lookup(name);
    if (s) return s;

    s = score_alloc_slot();
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, 255); s->name[255] = '\0';
    s->static_class_cache = -1;
    s->first_seen = time(NULL);
    s->dirty = true;
    g_scores_dirty = true;
    g_score_hash_dirty = true;

    int guess = classify_score_cat(name);
    s->category = guess;
    if (guess != 2) {
        s->category_confirmed = true;
        s->category_confirmed_at = time(NULL);
    } else {
        classify_queue_maybe(name);
    }
    return s;
}

static void score_touch(const char *name, time_t now, long swap_kb) {
    if (!name[0]) return;
    AppScore *s = score_lookup_or_create(name);

    bool new_session = (s->raw_last_fg_do_not_read_directly == 0 ||
                        (now - s->raw_last_fg_do_not_read_directly) >= SESSION_GAP_S);
    if (new_session && s->session_count < 99999) {
        s->session_count++;
        s->dirty = true;
        g_scores_dirty = true;   /* session boundary is worth persisting */
    }

    if (new_session) {
        if (s->session_start_t != 0) {
            long dur = (long)(s->raw_last_fg_do_not_read_directly - s->session_start_t);
            if (dur > 0) {
                long old_avg = s->avg_session_duration_s;
                s->avg_session_duration_s = (s->avg_session_duration_s * (100 - SCORE_AVG_ALPHA_PCT) +
                                              dur * SCORE_AVG_ALPHA_PCT) / 100;
                if (labs(s->avg_session_duration_s - old_avg) >= 5) {
                    s->dirty = true;
                    g_scores_dirty = true;
                }
            }
        }
        s->session_start_t = now;
    }

    s->raw_last_fg_do_not_read_directly = now;
    s->last_genuine_fg = now;
    if (s->fg_count < FG_COUNT_MAX) s->fg_count++;

    if (swap_kb >= 0) {
        long old_avg = s->avg_swap_kb;
        s->avg_swap_kb = (s->avg_swap_kb * (100 - SCORE_AVG_ALPHA_PCT) +
                          swap_kb * SCORE_AVG_ALPHA_PCT) / 100;
        /* Only a meaningful swap move is worth persisting; EMA noise
         * every tick isn't. */
        if (labs(s->avg_swap_kb - old_avg) >= 1024) {
            s->dirty = true;
            g_scores_dirty = true;
        }
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
    s->raw_last_fg_do_not_read_directly = now;   /* in-memory recency refresh only, see score_touch() */
    if (swap_kb >= 0) {
        long old_avg = s->avg_swap_kb;
        s->avg_swap_kb = (s->avg_swap_kb * (100 - SCORE_AVG_ALPHA_PCT) +
                          swap_kb * SCORE_AVG_ALPHA_PCT) / 100;
        if (labs(s->avg_swap_kb - old_avg) >= 1024) {
            s->dirty = true;
            g_scores_dirty = true;
        }
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
                 "(%d consecutive growth windows) — prioritizing for kill",
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

static int g_score_journal_pending = 0;

/* Full rewrite of SCORE_FILE from current in-memory state, then clears
 * the journal (its contents are now folded into the base file, so an
 * old, stale copy of them replayed on top would be redundant — or
 * wrong, for entries pruned since their last journal append). Used for:
 * periodic consolidation (journal grew past the threshold), structural
 * changes (compaction/pruning), and first-ever save. */
static void score_save_full(void) {

    char tmp_path[sizeof(SCORE_FILE) + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", SCORE_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) { loge("Scores: cannot write"); return; }

    fprintf(f, "# lmk usage scores v9 gen=%d\n", LEARN_RESET_GEN);

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
            /* v8: name fg_count raw_last_fg_do_not_read_directly restart_count avg_swap_kb session_count
             *         category confirmed dc_kill_count adaptive_kill_count
             *         first_seen avg_session_duration_s
             * "confirmed" (v6) persists category_confirmed so a
             * dumpsys-verified category doesn't have to be re-derived (and
             * re-queued through classify_dumpsys_drain()) from scratch on
             * every boot — previously this flag was never saved, so every
             * restart re-ran the full classify pipeline for every
             * non-curated-prefix package that needed dumpsys to resolve.
             * "first_seen" (v7) persists when this entry was first
             * created, so it survives restarts instead of resetting to
             * "now" every boot — see AppScore.first_seen.
             * "avg_session_duration_s" (v8) persists the EMA of
             * continuous-foreground session length — see score_touch().
             * "category_confirmed_at" (v9, new) persists when the category
             * was last actually confirmed, so category_reconfirm_sweep()
             * can force a fresh dumpsys check after CATEGORY_RECONFIRM_S
             * instead of trusting a classification permanently. */
            fprintf(f, "%s %d %ld %d %ld %d %d %d %d %d %ld %ld %ld\n",
                    s->name, s->fg_count, (long)s->raw_last_fg_do_not_read_directly,
                    s->restart_count, s->avg_swap_kb,
                    s->session_count, c, s->category_confirmed ? 1 : 0,
                    s->dc_kill_count, s->adaptive_kill_count,
                    (long)s->first_seen, s->avg_session_duration_s,
                    (long)s->category_confirmed_at);
            s->dirty = false;
            saved++;
        }
    }
    fclose(f);
    if (rename(tmp_path, SCORE_FILE) != 0) {
        loge("Scores: rename %s -> %s failed (%s)", tmp_path, SCORE_FILE, strerror(errno));
        return;
    }

    /* Base file now reflects every entry, so any journal contents are
     * either already folded in or stale (pruned) — clear it. */
    FILE *jf = fopen(SCORE_JOURNAL_FILE, "w");
    if (jf) fclose(jf);
    g_score_journal_pending = 0;

    g_scores_dirty = false;

    logi("Scores: full save %d entries (v8)", saved);
}

/* Append only entries changed since the last flush to a small journal
 * file (v5 line format, same as the base file) instead of rewriting the
 * whole table. Far cheaper on a low-RAM/flash device when only 1-2
 * entries changed out of ~200. Consolidates into a full save once the
 * journal has accumulated SCORE_JOURNAL_CONSOLIDATE_N entries, so the
 * journal (and score_load()'s replay work) never grows unbounded. */
static void score_save_incremental(void) {
    int to_write = 0;
    for (int i = 0; i < g_score_count; i++)
        if (g_scores[i].dirty) to_write++;

    if (to_write == 0) { g_scores_dirty = false; return; }

    FILE *f = fopen(SCORE_JOURNAL_FILE, "a");
    if (!f) { loge("Scores: cannot write journal"); return; }

    for (int i = 0; i < g_score_count; i++) {
        AppScore *s = &g_scores[i];
        if (!s->dirty) continue;
        int c = s->category;
        if (c < 0 || c > 2) c = classify_score_cat(s->name);
        fprintf(f, "%s %d %ld %d %ld %d %d %d %d %d %ld %ld %ld\n",
                s->name, s->fg_count, (long)s->raw_last_fg_do_not_read_directly,
                s->restart_count, s->avg_swap_kb,
                s->session_count, c, s->category_confirmed ? 1 : 0,
                s->dc_kill_count, s->adaptive_kill_count,
                (long)s->first_seen, s->avg_session_duration_s,
                (long)s->category_confirmed_at);
        s->dirty = false;
    }
    fclose(f);
    g_score_journal_pending += to_write;
    g_scores_dirty = false;
    logi("Scores: journal +%d entr%s (%d pending consolidation)",
         to_write, to_write == 1 ? "y" : "ies", g_score_journal_pending);

    if (g_score_journal_pending >= SCORE_JOURNAL_CONSOLIDATE_N) {
        logi("Scores: journal threshold reached — consolidating");
        score_save_full();
    }
}

static void score_save(void) {
    if (!g_scores_dirty) return;
    score_save_incremental();
}

static void score_compact(void) {
    time_t now = time(NULL);
    if (now - g_score_last_compact < SCORE_COMPACT_INTVL_S) return;
    g_score_last_compact = now;

    int kept = 0, pruned = 0;
    for (int i = 0; i < g_score_count; i++) {
        AppScore *s = &g_scores[i];
        if (s->raw_last_fg_do_not_read_directly != 0 && (now - s->raw_last_fg_do_not_read_directly) > SCORE_STALE_PRUNE_S) {
            pruned++;
            continue;
        }
        if (kept != i) g_scores[kept] = *s;
        kept++;
    }
    if (pruned > 0) {
        g_score_count  = kept;
        g_score_hash_dirty = true;
        logi("Scores: compacted, pruned %d stale entr%s (%d remain)",
             pruned, pruned == 1 ? "y" : "ies", kept);

        score_save_full();
    }
}

static void score_load(bool read_only) {
    FILE *f = fopen(SCORE_FILE,"r");
    if (!f) { if (!read_only) logi("Scores: no data file – starting fresh"); return; }
    char line[512]; int loaded = 0;

    bool gen_checked = false, need_reset = false;
    while (fgets(line, sizeof(line), f) && g_score_count < SCORE_MAX_APPS) {
        if (line[0] == '#') {
            if (!gen_checked) {
                int file_gen = 0;

                sscanf(line, "# lmk usage scores v%*d gen=%d", &file_gen);
                gen_checked = true;
                need_reset = (file_gen != LEARN_RESET_GEN);
                if (need_reset && !read_only) {
                    char archived[300];
                    snprintf(archived, sizeof(archived), "%s.gen%d",
                             LEARN_LOG_FILE, file_gen);
                    if (rename(LEARN_LOG_FILE, archived) == 0)
                        logw("LearnReset: archived %s -> %s (gen %d -> %d)",
                             LEARN_LOG_FILE, archived, file_gen, LEARN_RESET_GEN);
                    if (unlink(RANK_CACHE_FILE) == 0)
                        logw("LearnReset: cleared %s", RANK_CACHE_FILE);
                    logw("LearnReset: category_confirmed will be cleared for "
                         "re-verification; usage counters (fg_count/"
                         "restart_count/session_count/avg_swap_kb) preserved");
                }
            }
            continue;
        }
        if (line[0] == '\n') continue;
        char pkg[256]; int cnt; long ts; int rc; long avg_sw; int ses; int cat;
        int confirmed_field = 0, dckills = 0, adpkills = 0;
        long first_seen_ts = 0;
        long avg_session_dur = 0;
        long confirmed_at_ts = 0;
        int fields = sscanf(line, "%255s %d %ld %d %ld %d %d %d %d %d %ld %ld %ld",
                            pkg, &cnt, &ts, &rc, &avg_sw, &ses, &cat,
                            &confirmed_field, &dckills, &adpkills,
                            &first_seen_ts, &avg_session_dur, &confirmed_at_ts);
        if (fields < 3 || cnt <= 0) continue;
        if (fields < 5) { rc = 0; avg_sw = 0; }
        if (fields < 6) { ses = 0; }
        if (fields < 7) { cat = classify_score_cat(pkg); }
        /* v6 adds a "confirmed" field before dc_kill_count/adaptive_kill_count.
         * Older (v5 and earlier) files don't have it, so a 9-field legacy
         * line reads its dckills/adpkills into confirmed_field/dckills above
         * — shift those back into place explicitly per field count rather
         * than risk misreading columns. */
        bool has_confirmed_field = (fields >= 10) && !need_reset;
        if (fields == 9) {
            adpkills = dckills;
            dckills  = confirmed_field;
            confirmed_field = 0;
        } else if (fields == 8) {
            dckills = confirmed_field;
            confirmed_field = 0;
        }
        /* v7 adds first_seen as an 11th field, v8 adds
         * avg_session_duration_s as a 12th. Any line with fewer fields
         * (older format, or a short/legacy line) simply leaves the newer
         * output arg(s) at their 0 default — sscanf() doesn't touch an
         * output arg it never reached — which is exactly the "unknown/
         * legacy" sentinel both fields are designed around, so no extra
         * branch is needed here the way has_confirmed_field needed one. */
        if (cat < 0 || cat > 2) cat = classify_score_cat(pkg);
        AppScore *s = &g_scores[g_score_count++];
        strncpy(s->name, pkg, 255); s->name[255] = '\0';
        s->fg_count          = cnt < FG_COUNT_MAX ? cnt : FG_COUNT_MAX;
        s->raw_last_fg_do_not_read_directly           = (time_t)ts;
        s->restart_count     = rc;
        s->avg_swap_kb       = avg_sw;
        s->session_count     = ses;
        s->category          = cat;
        s->dc_kill_count     = dckills;
        s->adaptive_kill_count = adpkills;
        s->first_seen        = (time_t)first_seen_ts;
        s->avg_session_duration_s = avg_session_dur;
        s->category_confirmed_at = (fields >= 13) ? (time_t)confirmed_at_ts : 0;
        s->session_start_t   = 0; /* in-memory only, re-seeds on next fg touch */
        s->static_class_cache = -1;
        if (has_confirmed_field && confirmed_field == 1) {
            /* v6: category was already dumpsys-confirmed in a prior run —
             * trust it as-is, no re-derivation or dumpsys re-queue needed. */
            s->category_confirmed = true;
        } else {
            /* Legacy file (no confirmed field), a genuinely-unconfirmed v6
             * entry, or a LEARN_RESET_GEN mismatch forcing re-verification
             * — fall back to the existing heuristic-first behavior: covers
             * native paths and dot-less command names immediately, only
             * queues a dumpsys check when genuinely ambiguous (falls
             * through to the USER default). Usage counters above are
             * untouched either way. */
            int fresh_guess = classify_score_cat(pkg);
            if (fresh_guess != 2) {
                s->category = fresh_guess;
                s->category_confirmed = true;

                s->category_confirmed_at = time(NULL);
            } else {
                classify_queue_maybe(pkg);
            }
        }
        loaded++;
    }
    fclose(f);
    g_score_hash_dirty = true;
    logi("Scores: loaded %d entries", loaded);

    FILE *jf = fopen(SCORE_JOURNAL_FILE, "r");
    if (!jf) return;
    int replayed = 0;
    while (fgets(line, sizeof(line), jf) && g_score_count < SCORE_MAX_APPS) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char pkg[256]; int cnt; long ts; int rc; long avg_sw; int ses; int cat;
        int confirmed_field = 0, dckills = 0, adpkills = 0;
        long first_seen_ts = 0;
        long avg_session_dur = 0;
        long confirmed_at_ts = 0;
        int fields = sscanf(line, "%255s %d %ld %d %ld %d %d %d %d %d %ld %ld %ld",
                            pkg, &cnt, &ts, &rc, &avg_sw, &ses, &cat,
                            &confirmed_field, &dckills, &adpkills,
                            &first_seen_ts, &avg_session_dur, &confirmed_at_ts);
        if (fields < 3 || cnt <= 0) continue;
        if (fields < 5) { rc = 0; avg_sw = 0; }
        if (fields < 6) { ses = 0; }
        if (fields < 7) { cat = classify_score_cat(pkg); }
        bool has_confirmed_field = (fields >= 10) && !need_reset;
        if (fields == 9) {
            adpkills = dckills;
            dckills  = confirmed_field;
            confirmed_field = 0;
        } else if (fields == 8) {
            dckills = confirmed_field;
            confirmed_field = 0;
        }
        if (cat < 0 || cat > 2) cat = classify_score_cat(pkg);

        AppScore *s = score_lookup(pkg);
        bool is_new = !s;
        if (!s) s = &g_scores[g_score_count++];
        strncpy(s->name, pkg, 255); s->name[255] = '\0';
        s->fg_count            = cnt < FG_COUNT_MAX ? cnt : FG_COUNT_MAX;
        s->raw_last_fg_do_not_read_directly             = (time_t)ts;
        s->restart_count       = rc;
        s->avg_swap_kb         = avg_sw;
        s->session_count       = ses;
        s->category            = cat;
        s->dc_kill_count       = dckills;
        s->adaptive_kill_count = adpkills;

        if (fields >= 11) s->first_seen = (time_t)first_seen_ts;
        /* v8: same overwrite-only-if-present convention as first_seen above
         * — a pre-v8 journal line replayed against a base entry that
         * already has a real avg_session_duration_s shouldn't get
         * clobbered back to the legacy 0 sentinel. */
        if (fields >= 12) s->avg_session_duration_s = avg_session_dur;
        /* v9: same overwrite-only-if-present convention as first_seen/
         * avg_session_duration_s above. */
        if (fields >= 13) s->category_confirmed_at = (time_t)confirmed_at_ts;
        if (is_new) { s->session_start_t = 0; s->static_class_cache = -1; g_score_hash_dirty = true; }
        if (has_confirmed_field && confirmed_field == 1) {
            s->category_confirmed = true;
        } else if (!s->category_confirmed) {
            int fresh_guess = classify_score_cat(pkg);
            if (fresh_guess != 2) {
                s->category = fresh_guess;
                s->category_confirmed = true;

                s->category_confirmed_at = time(NULL);
            } else {
                classify_queue_maybe(pkg);
            }
        }
        replayed++;
    }
    fclose(jf);
    if (replayed > 0) logi("Scores: replayed %d journal entr%s",
                           replayed, replayed == 1 ? "y" : "ies");
}

static void score_maybe_save(void) {
    time_t now = time(NULL);
    if (now - g_score_last_save >= SCORE_SAVE_INTVL) {
        score_compact();
        score_save(); g_score_last_save = now;
    }
}

/* ================================================================
 *  RANK CACHE  — lightweight per-app swap + rank snapshot
 *  Format: name avg_swap_kb rank
 *  Purpose: warm up avg_swap_kb for background (non-fg) apps so
 *  cmp_killable_zram sorts correctly immediately after restart.
 * ================================================================ */

typedef struct { char name[256]; long swap; int rank; } RankCacheEntry;
static int rank_cache_cmp(const void *a, const void *b) {
    return ((const RankCacheEntry *)a)->rank - ((const RankCacheEntry *)b)->rank;
}

static void rank_cache_save(ProcInfo *tbl, int cnt) {

    char tmp_path[sizeof(RANK_CACHE_FILE) + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", RANK_CACHE_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return;
    time_t now = time(NULL);
    fprintf(f, "# lmk rank cache ts=%ld\n", (long)now);

    static RankCacheEntry entries[SCORE_MAX_APPS];
    int n = 0;
    for (int i = 0; i < cnt && n < SCORE_MAX_APPS; i++) {
        ProcInfo *p = &tbl[i];
        if (!p->name[0]) continue;

        AppScore *as = score_lookup(p->name);
        int cat = (as && as->category_confirmed)
                      ? as->category : classify_score_cat(p->name);

        if (cat == 0) continue; /* native binaries only */
        if (is_rank_exempt(p->name)) continue;
        /* Use live swap_kb blended with stored avg */
        long swap = p->swap_kb;
        if (as && as->avg_swap_kb > 0)
            swap = (as->avg_swap_kb * 80 + p->swap_kb * 20) / 100;
        strncpy(entries[n].name, p->name, sizeof(entries[n].name) - 1);
        entries[n].name[sizeof(entries[n].name) - 1] = '\0';
        entries[n].swap = swap;
        entries[n].rank = p->rank;
        n++;
    }
    qsort(entries, n, sizeof(RankCacheEntry), rank_cache_cmp);
    for (int i = 0; i < n; i++)
        fprintf(f, "%s %ld %d\n", entries[i].name, entries[i].swap, entries[i].rank);

    fclose(f);
    if (rename(tmp_path, RANK_CACHE_FILE) != 0)
        loge("RankCache: rename %s -> %s failed (%s)", tmp_path, RANK_CACHE_FILE, strerror(errno));
}

static void rank_cache_load(void) {
    FILE *f = fopen(RANK_CACHE_FILE, "r");
    if (!f) return;
    char line[300]; int loaded = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char pkg[256]; long avg_sw; int rank;
        if (sscanf(line, "%255s %ld %d", pkg, &avg_sw, &rank) < 2) continue;

        (void)rank;
        /* Find or create a minimal score entry to hold avg_swap_kb */
        AppScore *s = score_lookup(pkg);
        if (!s) {
            s = score_alloc_slot();
            memset(s, 0, sizeof(*s));
            strncpy(s->name, pkg, 255); s->name[255] = '\0';
            s->category = classify_score_cat(pkg);

            s->first_seen = time(NULL);
            if (s->category != 2) {
                s->category_confirmed = true;
                s->category_confirmed_at = time(NULL);
            } else if (!s->category_confirmed) {
                classify_queue_maybe(pkg);
            }
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

    AppScore *cs = score_lookup(p->name);
    int cached = cs ? cs->static_class_cache : -1;
    if (cached == -1) {
        cached = (name_matches(p->name, NEVER_KILL) ||
                  name_matches(p->name, SERVICE_EXEMPT) ||
                  is_launcher_like(p->name) ||
                  is_ime_like(p->name) ||
                  is_active_widget_provider(p->name)) ? 1 : 0;
        if (cs) cs->static_class_cache = cached;
    }
    if (cached == 1)                          return PRIO_NEVER;
    if (p->oom_adj <= 0)                      return PRIO_NEVER;
    if (p->oom_adj <= ADJ_VISIBLE_MAX)        return PRIO_SEMI_PROTECTED;
    if (is_media_player(p->name))             return PRIO_SEMI_PROTECTED;
    if (p->oom_adj <= ADJ_SERVICE_MAX)        return PRIO_BACKGROUND;
    if (p->oom_adj <= ADJ_CACHED_MAX)         return PRIO_CACHED;
    return PRIO_JUNK;
}

/* Forward decls: both defined later in the file (is_fg_trackable/
 * is_genuine_fg live near the retention logic they were originally added
 * for), but needed here so enumerate_procs() can compute a clean
 * true-focus/recency signal instead of trusting raw oom_adj/raw_last_fg_do_not_read_directly. */
static bool is_fg_trackable(const char *n);
static bool is_genuine_fg(const char *n, long oom_adj);
static AppScore *score_lookup(const char *name);

static bool is_structurally_headless(const AppScore *s, time_t now) {
    return s && s->fg_count == 0
        && s->adj0_blip_total_s >= BLIP_HEADLESS_MIN_S
        && s->first_seen > 0
        && (now - s->first_seen) >= RANK_COLD_S;
}

static time_t effective_fg_recency(const ProcInfo *p) {
    if (p->last_genuine_fg) return p->last_genuine_fg;
    if (!is_fg_trackable(p->name)) return 0;
    AppScore *as = score_lookup(p->name);
    if (is_structurally_headless(as, time(NULL))) return 0;
    return p->raw_last_fg_do_not_read_directly;
}

/* ================================================================
 *  PROCESS ENUMERATION
 * ================================================================ */

static AppScore *lookup_or_create_score_stub(ProcInfo *p) {
    AppScore *as     = score_lookup(p->name);
    if (!as && !is_rank_exempt(p->name)) {
        as = score_alloc_slot();
        memset(as, 0, sizeof(*as));
        strncpy(as->name, p->name, 255); as->name[255] = '\0';
        as->category = classify_score_cat(p->name);
        as->static_class_cache = -1;

        as->first_seen = time(NULL);
        as->dirty = true;
        g_scores_dirty = true;
        g_score_hash_dirty = true;

        if (as->category != 2) {
            as->category_confirmed = true;
            as->category_confirmed_at = time(NULL);
        }
        else classify_queue_maybe(p->name);
    }
    return as;
}

static const char *rank_name(int r) {
    switch (r) {
        case RANK_EXEMPT: return "EXEMPT";
        case RANK_TIER1:  return "TIER1";
        case RANK_TIER2:  return "TIER2";
        case RANK_TIER3:  return "TIER3";
        case RANK_TIER4:  return "BACKGROUND";
        default:          return "?";
    }
}

/* Rank assignment for a single process: RANK_EXEMPT short-circuit,
 * true_fg detection, app_rank() tiering, then the RANK_BACKGROUND
 * override (structural/bounce-proven/low-freq), followed by
 * transition logging + FlapWatch. Pure extract from enumerate_procs()
 * — logic unchanged, only reachable via the per-process loop. */
static void compute_proc_rank(ProcInfo *p, AppScore *as) {

        if (is_rank_exempt(p->name))
            p->rank = RANK_EXEMPT;
        else {

            p->true_fg = is_genuine_fg(p->name, p->oom_adj);

            if (p->true_fg && as) as->last_true_fg_seen = time(NULL);

            if (as && as->rank_computed_seq == g_rank_seq) {
                p->rank = as->rank_computed_result;
                return;
            }
            if (as) as->rank_computed_seq = g_rank_seq;

            bool true_fg_for_rank = p->true_fg;
            if (as) {
                time_t rank_now = time(NULL);
                if (p->true_fg) {
                    if (as->rank_fg_hold_start == 0)
                        as->rank_fg_hold_start = rank_now;
                    true_fg_for_rank =
                        (rank_now - as->rank_fg_hold_start) >= RANK1_HOLD_S;
                } else {
                    as->rank_fg_hold_start = 0;
                }
            }
            time_t rank_recency = effective_fg_recency(p);
            p->rank = app_rank(as, true_fg_for_rank,
                               rank_recency,
                               p->last_genuine_fg != 0,
                               as ? as->session_count : 0);

            bool bg_structural = false, bg_bounce = false, bg_decayed = false;
            bool bg_lowfreq = false;
            if (!p->true_fg) {
                int cat = (as && as->category_confirmed)
                              ? as->category : classify_score_cat(p->name);

                time_t recency = effective_fg_recency(p);

                time_t last_seen = (as && as->last_true_fg_seen > recency)
                                    ? as->last_true_fg_seen : recency;
                bool idle_enough = (last_seen == 0) ||
                                    ((time(NULL) - last_seen) >= RANK_COLD_S);

                bg_structural = (cat == 1) && idle_enough;

                if (!idle_enough) {
                    if (as) as->bg_idle_since = 0;
                } else if (as && as->bg_idle_since == 0) {
                    as->bg_idle_since = time(NULL);
                }
                bool idle_confirmed = idle_enough && as && as->bg_idle_since &&
                    (time(NULL) - as->bg_idle_since) >= BG_IDLE_DEBOUNCE_S;
                if (idle_confirmed) {

                    if (as && as->restart_count >= BOUNCE_SUPPRESS_RC &&
                        as->last_restart_t &&
                        (time(NULL) - as->last_restart_t) >= BOUNCE_DECAY_S) {
                        as->restart_count = 0;
                        as->dirty = true;
                        bg_decayed = true;
                    }
                    bg_bounce = as && as->restart_count >= BOUNCE_SUPPRESS_RC;

                    if (!bg_structural && !bg_bounce &&
                        as && as->first_seen) {
                        time_t tracked_s = time(NULL) - as->first_seen;
                        if (tracked_s >= LOWFREQ_MIN_OBS_S) {
                            double fg_rate = (double)as->fg_count /
                                              ((double)tracked_s / 3600.0);
                            bg_lowfreq = (fg_rate < LOWFREQ_FG_RATE_THRESH);
                        }
                    }
                }
                if (bg_structural || bg_bounce || bg_lowfreq)
                    p->rank = RANK_TIER4;
            }

            if (as && p->rank != as->last_logged_rank) {
                if (p->rank == RANK_TIER4)
                    logi("Rank: [%s] -> BACKGROUND (%s)", p->name,
                         bg_structural ? "structural" :
                         bg_bounce ? "bounce-proven" : "low-freq");
                else if (as->last_logged_rank == RANK_TIER4) {
                    if (bg_decayed)
                        logi("Rank: [%s] -> normal (bounce evidence expired)",
                             p->name);
                    else
                        logi("Rank: [%s] -> normal (rank=%d, genuinely reopened)",
                             p->name, p->rank);
                }
                else {

                    logi("Rank: [%s] %s -> %s (recency_age=%lds rc=%d true_fg=%d)",
                         p->name, rank_name(as->last_logged_rank), rank_name(p->rank),
                         rank_recency ? (long)(time(NULL) - rank_recency) : -1L,
                         as->restart_count, p->true_fg ? 1 : 0);
                }
                as->last_logged_rank = p->rank;
                as->dirty = true;

                time_t fnow = time(NULL);
                if (as->flap_window_start == 0)
                    as->flap_window_start = fnow;
                as->flap_count++;
                time_t win_elapsed = fnow - as->flap_window_start;
                if (win_elapsed >= FLAP_WINDOW_MIN_S) {
                    double rate = (double)as->flap_count /
                                  ((double)win_elapsed / 3600.0);
                    if (rate >= FLAP_RATE_WARN_THRESH && !as->flap_warned) {
                        logw("FlapWatch: [%s] flapping BACKGROUND<->normal "
                             "at %.1f/hr (%d in %ldm) — candidate for "
                             "is_fg_trackable() exclusion review", p->name,
                             rate, as->flap_count, (long)(win_elapsed / 60));
                        as->flap_warned = true;
                    }
                }
                if (win_elapsed >= FLAP_WINDOW_RESET_S) {
                    as->flap_count = 0;
                    as->flap_window_start = fnow;
                    as->flap_warned = false;
                }
            }

            if (as) as->rank_computed_result = p->rank;
        }
}
static void check_proc_runaway(ProcInfo *p) {

        if (p->prio != PRIO_NEVER && p->rank != RANK_EXEMPT) {
            check_runaway_growth(p->name, time(NULL), p->rss_kb);
            AppScore *gs = score_lookup(p->name);
            if (gs && gs->flagged_runaway && p->oom_adj != ADJ_FOREGROUND)
                p->prio = PRIO_JUNK;
        }
}
static void track_proc_bounce_window(ProcInfo *p, time_t now) {
    p->bounce_count  = 0;
    p->bounce_first  = 0;
    for (int i = 0; i < KILL_HIST_SIZE; i++) {
        if (g_kill_hist[i].name[0] &&
            strcmp(g_kill_hist[i].name, p->name) == 0 &&
            (now - g_kill_hist[i].killed_at) < BOUNCE_WINDOW_S) {
            if (p->bounce_count == 0) p->bounce_first = g_kill_hist[i].killed_at;
            p->bounce_count++;
        }
    }
}
static void enforce_tier_caps(ProcInfo *tbl, int cnt) {

    for (int tier = RANK_TIER1; tier <= RANK_TIER2; tier++) {
        int cap = (tier == RANK_TIER1) ? TIER1_MAX_SLOTS : TIER2_MAX_SLOTS;
        int demote_to = tier + 1;

        char uniq_name[64][256];
        int  uniq_score[64];
        bool uniq_demoted[64] = {0};
        int  uniq_n = 0;
        for (int i = 0; i < cnt; i++) {
            if (tbl[i].rank != tier || tbl[i].true_fg) continue;
            int found = -1;
            for (int u = 0; u < uniq_n; u++)
                if (strcmp(uniq_name[u], tbl[i].name) == 0) { found = u; break; }
            if (found >= 0) {
                if (tbl[i].score < uniq_score[found]) uniq_score[found] = tbl[i].score;
            } else if (uniq_n < 64) {
                strncpy(uniq_name[uniq_n], tbl[i].name, sizeof(uniq_name[0]) - 1);
                uniq_name[uniq_n][sizeof(uniq_name[0]) - 1] = '\0';
                uniq_score[uniq_n] = tbl[i].score;
                uniq_n++;
            }
        }
        int excess = uniq_n - cap;
        while (excess > 0) {
            int weakest = -1;
            for (int u = 0; u < uniq_n; u++) {
                if (uniq_demoted[u]) continue;
                if (weakest == -1 || uniq_score[u] < uniq_score[weakest] ||
                    (uniq_score[u] == uniq_score[weakest] &&
                     strcmp(uniq_name[u], uniq_name[weakest]) > 0))
                    weakest = u;
            }
            if (weakest == -1) break;
            logi("TierCap: [%s] demoted TIER%d -> TIER%d (capacity %d/%d, score=%d)",
                 uniq_name[weakest], tier, demote_to, uniq_n, cap, uniq_score[weakest]);
            /* Apply to every live process of this package this tick, not
             * just one PID — closes the secondary/transient inconsistency
             * where sibling processes kept a stale rank for the rest of
             * the tick until next tick's dedup guard resynced. */
            for (int i = 0; i < cnt; i++)
                if (tbl[i].rank == tier && !tbl[i].true_fg &&
                    strcmp(tbl[i].name, uniq_name[weakest]) == 0)
                    tbl[i].rank = demote_to;
            AppScore *ws = score_lookup(uniq_name[weakest]);
            if (ws) {
                ws->rank_tier = demote_to; ws->tier_entered_t = time(NULL);

                ws->capacity_demote_t = time(NULL);
                ws->capacity_demote_from = tier;
            }
            uniq_demoted[weakest] = true;
            excess--;
        }
    }
}
static int enumerate_procs(ProcInfo *tbl, int maxn) {
    g_rank_seq++;
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
        p->raw_last_fg_do_not_read_directly       = get_last_fg(p->name);
        check_bounced(p->name);

        AppScore *as = lookup_or_create_score_stub(p);

        /* Fall back to persisted raw_last_fg_do_not_read_directly if fg_history has no entry
         * (covers daemon restart — fixes recents-dismiss and retention) */
        if (p->raw_last_fg_do_not_read_directly == 0 && as) p->raw_last_fg_do_not_read_directly = as->raw_last_fg_do_not_read_directly;
        p->score         = as ? score_compute(as) : 0;
        p->restart_count = as ? as->restart_count : 0;
        p->avg_swap_kb   = as ? as->avg_swap_kb : 0;
        p->last_genuine_fg   = as ? as->last_genuine_fg : 0;

        compute_proc_rank(p, as);
        check_proc_runaway(p);

        time_t now = time(NULL);
        track_proc_bounce_window(p, now);
        cnt++;
    }
    closedir(dir);

    enforce_tier_caps(tbl, cnt);

    return cnt;
}

/* ================================================================
 *  FOREGROUND TRACKING + BOUNCE DETECTION
 * ================================================================ */

/* Name-pattern exclusion shared by is_genuine_fg() and the retention
 * candidacy recency fallback below — apps matching this can NEVER earn
 * last_genuine_fg, which matters for whether it's safe to fall back to
 * raw_last_fg_do_not_read_directly for them (see oom_pin_retained()). */
static bool is_fg_trackable(const char *n) {
    if (n[0] == '/' || strstr(n, ".persistent") ||
        strstr(n, ":persistent") || strstr(n, ".providers.") ||
        strncmp(n, "android.process.", 16) == 0)
        return false;

    if (strstr(n, "permissioncontroller") ||
        strstr(n, "cellbroadcastreceiver") ||
        strstr(n, "settings.intelligence"))
        return false;

    if (strstr(n, "apps.turbo") || strstr(n, "adservices.api"))
        return false;

    if (strstr(n, "chromecast.app"))
        return false;
    return true;
}

static bool is_genuine_fg(const char *n, long oom_adj) {

    return oom_adj == ADJ_FOREGROUND && is_fg_trackable(n) &&
           g_true_fg_name[0] != '\0' && !strcmp(n, g_true_fg_name);
}

static bool cgroup_path_for_pid(pid_t pid, char *out, size_t out_sz);
static bool cgroup_write(pid_t pid, const char *file, const char *val);

static void track_foreground(ProcInfo *tbl, int cnt) {
    time_t now = time(NULL);
    for (int i = 0; i < cnt; i++) {
        const char *n = tbl[i].name;
        bool genuine_fg = is_genuine_fg(n, tbl[i].oom_adj);

        AppScore *s = score_lookup(n);
        if (s) {
            if (s->was_true_fg && !genuine_fg) s->last_bg = now;

            if (genuine_fg) {
                s->fg_hold_last_genuine_t = now;
            } else if (s->fg_hold_start != 0 &&
                       (s->fg_hold_last_genuine_t == 0 ||
                        (now - s->fg_hold_last_genuine_t) > FG_HOLD_WOBBLE_TOLERANCE_S)) {

                logi("HoldReset: [%s] fg_hold reset after held_s=%ld (wobble_tol=%ds)",
                     n, (long)(now - s->fg_hold_start), FG_HOLD_WOBBLE_TOLERANCE_S);
                s->fg_hold_start = 0;
            }
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

            AppScore *bs = score_lookup_or_create(n);
            time_t d = (bs->adj0_last_tick && now > bs->adj0_last_tick)
                       ? now - bs->adj0_last_tick : 0;
            if (d > 2) d = 2;
            bs->adj0_blip_total_s += d;
            bs->adj0_last_tick = now;
            continue;
        }

        AppScore *rs = score_lookup(n);
        if (rs && rs->cg_high_set) {
            cgroup_write(tbl[i].pid, "memory.high", "max");
            rs->cg_high_set = false;
            logi("SqueezeRelease: [%s] memory.high cleared on genuine fg", n);
        }

        AppScore *hs = score_lookup_or_create(n);
        if (hs->fg_hold_start == 0) hs->fg_hold_start = now;
        if ((now - hs->fg_hold_start) < FG_MIN_HOLD_S) {
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

static bool confirm_true_foreground(const char *pkg) {
    FILE *fp = popen("/system/bin/dumpsys window windows 2>/dev/null", "r");
    if (!fp) return true;
    char line[512];
    bool found_focus_line = false, matched = false;
    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, "mCurrentFocus=");
        if (!p) continue;
        found_focus_line = true;
        char *slash = strchr(p, '/');
        if (slash) {
            char *pkg_end = slash, *pkg_start = slash;
            while (pkg_start > line && pkg_start[-1] != ' ' && pkg_start[-1] != '{')
                pkg_start--;
            size_t len = (size_t)(pkg_end - pkg_start);
            if (len > 0 && len < 200 &&
                strncmp(pkg_start, pkg, len) == 0 && pkg[len] == '\0')
                matched = true;
        }
        break; /* first mCurrentFocus= line is authoritative */
    }
    pclose(fp);
    return found_focus_line ? matched : true; /* fail open on a missing line */
}

static bool find_true_foreground(ProcInfo *tbl, int cnt, char *out, size_t outsz) {
    for (int i = 0; i < cnt; i++) {
        if (tbl[i].oom_adj != ADJ_FOREGROUND) continue;
        const char *n = tbl[i].name;
        /* Skip absolute-path native binaries (e.g. Termux bash) */
        if (n[0] == '/') continue;
        /* Skip persistent background services (GMS persistent, etc.) */
        if (strstr(n, ".persistent")) continue;

        strncpy(out, n, outsz - 1);
        out[outsz - 1] = '\0';
        return true;
    }
    return false;
}

/* ================================================================
 *  OOM PINNING — ACTIVE APP RETENTION
 * ================================================================ */

#define PIN_STICKY_PCT     20   /* sticky bonus = 20% of SCORE_MAX */
#define PIN_STICKY_BONUS   ((SCORE_MAX * PIN_STICKY_PCT) / 100)
static char    g_prev_pinned[RETAIN_MAX_N][64];
static int     g_prev_pinned_n = 0;
static time_t  g_kill_roi_log_t = 0;

static bool was_prev_pinned(const char *name) {
    for (int i = 0; i < g_prev_pinned_n; i++)
        if (strcmp(g_prev_pinned[i], name) == 0) return true;
    return false;
}

static void oom_pin_retained(ProcInfo *tbl, int cnt, int avail_pct) {

    bool allow_new_pins = avail_pct >= PIN_ALLOW_FLOOR_PCT;

    time_t now = time(NULL);

    bool   done[2048];
    int    max_done = cnt < 2048 ? cnt : 2048;
    memset(done, 0, (size_t)max_done * sizeof(bool));

    int  retained = 0;
    int  cg_low_ok = 0;
    char retained_names[512] = "";
    char local_pinned_name[RETAIN_MAX_N][64];
    int  local_pinned_count = 0;

    /* Shared "commit this candidate as pinned" step — identical for both
     * phases below, kept inline per phase (not factored into a separate
     * function) since each phase's *selection* loop differs but the small
     * apply-side effects don't warrant a third (ProcInfo*, out-params...)
     * signature. */
#define PIN_APPLY(p) do { \
        AppScore *as = score_lookup((p)->name); \
        if (as) as->retain_pin_t = now; /* grants the 8s opportunistic-kill grace */ \
         \
        char lowbuf[32]; \
        snprintf(lowbuf, sizeof(lowbuf), "%ld", (long)(p)->rss_kb * 1024); \
        if (cgroup_write((p)->pid, "memory.low", lowbuf)) { \
            if (as) as->cg_low_set = true; \
            cg_low_ok++; \
        } \
        retained++; \
        size_t used = strlen(retained_names); \
        if (used < sizeof(retained_names) - 1) \
            snprintf(retained_names + used, sizeof(retained_names) - used, \
                     "%s%s", used ? "," : "", (p)->name); \
        if (local_pinned_count < RETAIN_MAX_N) { \
            strncpy(local_pinned_name[local_pinned_count], (p)->name, \
                    sizeof(local_pinned_name[0]) - 1); \
            local_pinned_name[local_pinned_count][sizeof(local_pinned_name[0]) - 1] = 0; \
            local_pinned_count++; \
        } \
    } while (0)

    /* ---- Phase A + B: only run new-pin selection when RAM isn't
     * critical (see allow_new_pins comment above) — release below always
     * runs regardless. */
    if (allow_new_pins) {
    /* ---- Phase A: reserve TIER1_PIN_MAX guaranteed slots, tier1-only.
     * No other rank competes for these — a real, recently-used tier1 app
     * can never be shut out of its floor by a higher-scoring tier2 app. */
    for (int k = 0; k < TIER1_PIN_MAX; k++) {
        int best = -1;
        for (int i = 0; i < cnt; i++) {
            if (done[i]) continue;
            ProcInfo *p = &tbl[i];
            if (p->rank != RANK_TIER1) continue;
            if (p->prio <= PRIO_SEMI_PROTECTED) continue;
            if (p->score <= 0) continue;
            time_t recency = effective_fg_recency(p);
            if (recency == 0) continue;

            /* Skip apps that only *just* left foreground — avoids pinning
             * something mid-transition for a single flickering tick. Still
             * fully eligible next tick. */
            if ((now - recency) < PIN_SETTLE_S) continue;
            int eff = p->score + (was_prev_pinned(p->name) ? PIN_STICKY_BONUS : 0);
            int best_eff = best >= 0
                ? tbl[best].score + (was_prev_pinned(tbl[best].name) ? PIN_STICKY_BONUS : 0)
                : -1;
            if (best == -1 || eff > best_eff ||
                (eff == best_eff && strcmp(p->name, tbl[best].name) < 0))
                best = i;
        }
        if (best < 0) break;
        done[best] = true;
        PIN_APPLY(&tbl[best]);
    }

    for (int k = 0; k < TIER2_PIN_MAX; k++) {
        int best = -1;
        for (int i = 0; i < cnt; i++) {
            if (done[i]) continue;
            ProcInfo *p = &tbl[i];
            if (p->rank != RANK_TIER1 && p->rank != RANK_TIER2) continue;
            if (p->prio <= PRIO_SEMI_PROTECTED) continue;
            if (p->score <= 0) continue;

            if (p->rank == RANK_TIER2 && !p->last_genuine_fg) continue;
            time_t recency = effective_fg_recency(p);
            if (recency == 0) continue;

            if (p->rank == RANK_TIER2 && (now - recency) > RETAIN_FG_AGE_S) continue;
            if ((now - recency) < PIN_SETTLE_S) continue;
            int eff = p->score + (was_prev_pinned(p->name) ? PIN_STICKY_BONUS : 0);
            int best_eff = best >= 0
                ? tbl[best].score + (was_prev_pinned(tbl[best].name) ? PIN_STICKY_BONUS : 0)
                : -1;
            if (best == -1 || eff > best_eff ||
                (eff == best_eff && strcmp(p->name, tbl[best].name) < 0))
                best = i;
        }
        if (best < 0) break;
        done[best] = true;
        PIN_APPLY(&tbl[best]);
    }
    } /* allow_new_pins */
#undef PIN_APPLY

    for (int i = 0; i < g_prev_pinned_n; i++) {
        bool still_pinned = false;
        for (int k = 0; k < local_pinned_count; k++)
            if (strcmp(g_prev_pinned[i], local_pinned_name[k]) == 0) { still_pinned = true; break; }
        if (still_pinned) continue;
        AppScore *das = score_lookup(g_prev_pinned[i]);
        if (!das || !das->cg_low_set) continue;
        bool found = false;
        for (int j = 0; j < cnt; j++) {
            if (strcmp(tbl[j].name, g_prev_pinned[i]) == 0) {
                cgroup_write(tbl[j].pid, "memory.low", "0");
                das->cg_low_set = false;
                found = true;
                break;
            }
        }

        if (!found) das->cg_low_set = false;
    }

    /* Remember this cycle's retained set (names only now — no adj to track
     * since we no longer write one) for next tick's stickiness bonus. */
    g_prev_pinned_n = local_pinned_count;
    for (int i = 0; i < local_pinned_count; i++) {
        strncpy(g_prev_pinned[i], local_pinned_name[i], sizeof(g_prev_pinned[0]) - 1);
        g_prev_pinned[i][sizeof(g_prev_pinned[0]) - 1] = 0;
    }

    if (retained > 0 && now - g_retain_log_t >= LOG_RATELIMIT_S) {
        g_retain_log_t = now;
        logi("Retain: protected %d app(s) [%s] (8s opportunistic-kill grace, avail=%d%%, cg_low=%d/%d)",
             retained, retained_names, avail_pct, cg_low_ok, retained);
    }
}

/* ================================================================
 *  KILL ORDERING
 * ================================================================ */
/* RAM kill ordering: rank desc (stale first), then prio, score, swap, age, rss */

static inline int kill_rank_key(int rank) {
    return rank == RANK_TIER4 ? RANK_TIER3 : rank;
}

static int cmp_killable_ram(const void *a, const void *b) {
    const ProcInfo *pa = a, *pb = b;
    /* Primary: higher rank (less important) killed first */
    int ra = kill_rank_key(pa->rank), rb = kill_rank_key(pb->rank);
    if (ra != rb)
        return rb - ra;
    if (pa->prio != pb->prio)
        return (int)pa->prio - (int)pb->prio;
    if (pa->score != pb->score)
        return pa->score - pb->score;
    if (pa->avg_swap_kb != pb->avg_swap_kb)
        return (pa->avg_swap_kb > pb->avg_swap_kb) ? -1 : 1;
    if (pa->restart_count != pb->restart_count)
        return pb->restart_count - pa->restart_count;
    /* FIXED (was raw_last_fg_do_not_read_directly, the one remaining call
     * site not migrated when effective_fg_recency() became the shared
     * choke point): background-noise refreshes of the raw field could
     * make a never-genuinely-used app tie-break as "more recent" than a
     * genuinely-used one. Narrow-scope (only affects ordering among
     * already-equal-rank candidates) but closes the last gap. */
    time_t rec_a = effective_fg_recency(pa), rec_b = effective_fg_recency(pb);
    if (rec_a != rec_b)
        return (rec_a > rec_b) ? 1 : -1;
    return (int)(pb->rss_kb - pa->rss_kb);
}

/* New comparator for ZRAM kills: rank desc, blended-swap desc, score, rc, raw_last_fg_do_not_read_directly.
 * Uses avg_swap_kb (EMA) blended 70/30 with live swap_kb for stable ordering. */
static int cmp_killable_zram(const void *a, const void *b) {
    const ProcInfo *pa = a, *pb = b;
    /* Primary: higher rank (stale/cold) first */
    int ra = kill_rank_key(pa->rank), rb = kill_rank_key(pb->rank);
    if (ra != rb)
        return rb - ra;
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
    /* Quinary: FIXED (was raw_last_fg_do_not_read_directly) — same fix as
     * cmp_killable_ram, use the trusted choke point instead. */
    time_t rec_a = effective_fg_recency(pa), rec_b = effective_fg_recency(pb);
    if (rec_a != rec_b)
        return (rec_a > rec_b) ? 1 : -1;
    /* Fallback: larger RSS */
    return (int)(pb->rss_kb - pa->rss_kb);
}

/* ================================================================
 *  RAM PRESSURE KILL
 * ================================================================ */

#define SQUEEZE_COOLDOWN_S         60

#define VM_TUNE_AVAIL_MID_PCT      15   /* below this: mid-pressure tier */
#define VM_TUNE_AVAIL_LOW_PCT      10   /* below this: low-pressure tier (the 25.4%-kswapd band) */

#define VM_TUNE_EXTRA_FREE_KB_NORMAL   0        /* default — no extra headroom requested */
#define VM_TUNE_EXTRA_FREE_KB_MID      100000   /* ~100MB */
#define VM_TUNE_EXTRA_FREE_KB_LOW      200000   /* ~200MB */

#define VM_TUNE_WATERMARK_SCALE_NORMAL  10   /* kernel default */
#define VM_TUNE_WATERMARK_SCALE_MID     100
#define VM_TUNE_WATERMARK_SCALE_LOW     200

/* Dwell requirement before acting on a tier change — avoids write-
 * storming the /proc/sys/vm sysctls if avail_pct oscillates right at a
 * boundary for several consecutive ticks. Same philosophy as SQUEEZE_COOLDOWN_S,
 * adapted to a "must stay in new tier this long before switching"
 * shape rather than a flat repeat-cooldown, since tier flapping (not
 * repetition) is the failure mode being guarded against here. */
#define VM_TUNE_DWELL_S   15

static int  g_vm_tune_tier    = -1;   /* -1 = uninitialized, forces first-tick apply */
static time_t g_vm_tune_pending_since = 0;
static int  g_vm_tune_pending_tier    = -1;

static void vm_tune_write_int(const char *path, long val) {
    FILE *f = fopen(path, "w");
    if (!f) { logw("VmTune: cannot open %s: %s", path, strerror(errno)); return; }
    fprintf(f, "%ld\n", val);
    fclose(f);
}

/* Drops only the clean page cache (echo 1), never dentries/inodes
 * (echo 2) or both (echo 3) — this is deliberately the narrowest,
 * cheapest option: reclaimed file cache costs nothing but a future
 * re-read, unlike dentry/inode drops which can make subsequent
 * filesystem lookups measurably slower right after. Safe to call
 * fairly often. */
static void drop_clean_caches(const char *reason) {
    static time_t last_drop = 0;
    time_t now = time(NULL);
    if (now - last_drop < 20) return;   /* hard floor — never spam even if called back-to-back */
    last_drop = now;
    vm_tune_write_int("/proc/sys/vm/drop_caches", 1);
    logi("DropCaches: clean page cache dropped (%s)", reason);
}

/* Applies extra_free_kbytes/watermark_scale_factor for the current
 * avail_pct tier, gated by VM_TUNE_DWELL_S so a boundary-hugging
 * avail_pct doesn't cause rewrite-thrash. Also triggers a clean-cache
 * drop when entering the low tier, and drives kswapd
 * pin/unpin based on the ZRAM tier (unrelated to avail_pct, but this
 * is the natural single per-tick call site for both). */
static void apply_avail_tier_tunables(int avail_pct, int zram_used_pct) {
    int tier;   /* 0=normal 1=mid 2=low */
    if (avail_pct < VM_TUNE_AVAIL_LOW_PCT)       tier = 2;
    else if (avail_pct < VM_TUNE_AVAIL_MID_PCT)  tier = 1;
    else                                          tier = 0;

    time_t now = time(NULL);
    if (g_vm_tune_tier < 0) {
        /* first tick — apply immediately, nothing to dwell against yet */
        g_vm_tune_tier = tier;
        g_vm_tune_pending_tier = tier;
        g_vm_tune_pending_since = now;
    } else if (tier != g_vm_tune_tier) {
        if (tier != g_vm_tune_pending_tier) {
            /* tier just changed to something new — start the dwell clock */
            g_vm_tune_pending_tier = tier;
            g_vm_tune_pending_since = now;
        } else if (now - g_vm_tune_pending_since >= VM_TUNE_DWELL_S) {
            /* held the new tier long enough — commit it */
            g_vm_tune_tier = tier;
        }
        /* else: still dwelling, keep current tier's sysctls as-is */
    } else {
        /* back to the already-active tier — cancel any pending switch */
        g_vm_tune_pending_tier = tier;
        g_vm_tune_pending_since = now;
    }

    static int last_applied_tier = -1;
    if (g_vm_tune_tier != last_applied_tier) {
        long extra_free, wmark;
        switch (g_vm_tune_tier) {
            case 2:  extra_free = VM_TUNE_EXTRA_FREE_KB_LOW; wmark = VM_TUNE_WATERMARK_SCALE_LOW; break;
            case 1:  extra_free = VM_TUNE_EXTRA_FREE_KB_MID; wmark = VM_TUNE_WATERMARK_SCALE_MID; break;
            default: extra_free = VM_TUNE_EXTRA_FREE_KB_NORMAL; wmark = VM_TUNE_WATERMARK_SCALE_NORMAL; break;
        }
        vm_tune_write_int("/proc/sys/vm/extra_free_kbytes", extra_free);
        vm_tune_write_int("/proc/sys/vm/watermark_scale_factor", wmark);
        logi("VmTune: avail=%d%% -> tier=%d  extra_free_kbytes=%ldKB  watermark_scale_factor=%ld",
             avail_pct, g_vm_tune_tier, extra_free, wmark);
        last_applied_tier = g_vm_tune_tier;
        if (g_vm_tune_tier == 2) drop_clean_caches("entered low-avail tier");
    }

    time_t now_kp = time(NULL);
    bool want_unpinned = (zram_used_pct >= ZRAM_CRIT_PCT) ||
                          (now_kp < g_kswapd_event_unpin_until);
    if (want_unpinned) {
        if (g_kswapd_pinned) {
            unpin_kswapd();
            g_kswapd_last_transition_t = now_kp;
        }
    } else if (zram_used_pct < ZRAM_STUCK_PCT) {
        if (!g_kswapd_pinned &&
            (now_kp - g_kswapd_last_transition_t) >= KSWAPD_PIN_DWELL_S) {
            repin_kswapd_if_needed();
            g_kswapd_last_transition_t = now_kp;
        }
    }
}

static int graduated_squeeze_target_pct(void) {
    int z = g_zram_used_pct;

    if (z < 50)              return 65;  /* plenty of ZRAM headroom — full squeeze */
    if (z < 70)              return 75;  /* moderate */
    if (z < ZRAM_WARN_PCT)   return 90;  /* gentle — taper off as headroom shrinks */
    return 0;                             /* at/above WARN_PCT — no more room to push into */
}
#define RECLAIM_MIN_RSS_KB    20480  /* only worth trying on apps holding >=20MB */
#define RECLAIM_MIN_FREED_KB   5120  /* must free >=5MB to count as a real reprieve */
#define RECLAIM_COOLDOWN_S      600  /* don't retry the same app within 10min */
#define KILL_ROI_MIN_RSS_KB   15360  /* below this, RAM gain from killing is marginal */
#define KILL_ROI_RC_THRESHOLD     5  /* restart_count >= this = expensive/thrashy to relaunch */

#define CGROUP_PATH_CACHE_SIZE 32
typedef struct { pid_t pid; char path[192]; bool valid; unsigned long long starttime; } CgroupPathCacheEntry;
static CgroupPathCacheEntry g_cgroup_path_cache[CGROUP_PATH_CACHE_SIZE];

static bool cgroup_path_for_pid_fresh(pid_t pid, char *out, size_t out_sz) {
    char procpath[64], line[320];
    snprintf(procpath, sizeof(procpath), "/proc/%d/cgroup", pid);
    FILE *f = fopen(procpath, "r");
    if (!f) return false;
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "0::", 3) == 0) {
            char *p = line + 3;
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            snprintf(out, out_sz, "/sys/fs/cgroup%s", p);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static void cgroup_path_cache_invalidate(pid_t pid) {
    for (int i = 0; i < CGROUP_PATH_CACHE_SIZE; i++)
        if (g_cgroup_path_cache[i].valid && g_cgroup_path_cache[i].pid == pid)
            g_cgroup_path_cache[i].valid = false;
}

static bool cgroup_path_for_pid(pid_t pid, char *out, size_t out_sz) {

    for (int i = 0; i < CGROUP_PATH_CACHE_SIZE; i++) {
        if (g_cgroup_path_cache[i].valid && g_cgroup_path_cache[i].pid == pid) {
            unsigned long long cur_start = read_proc_starttime(pid);
            if (cur_start != 0 && cur_start == g_cgroup_path_cache[i].starttime) {
                snprintf(out, out_sz, "%s", g_cgroup_path_cache[i].path);
                return true;
            }
            /* starttime mismatch (or process gone) — PID was recycled
             * since this entry was cached, or it's no longer readable.
             * Fall through and re-resolve fresh rather than trust it. */
            g_cgroup_path_cache[i].valid = false;
            break;
        }
    }
    if (!cgroup_path_for_pid_fresh(pid, out, out_sz)) return false;
    static int cache_next = 0;
    snprintf(g_cgroup_path_cache[cache_next].path,
             sizeof(g_cgroup_path_cache[cache_next].path), "%s", out);
    g_cgroup_path_cache[cache_next].pid = pid;
    g_cgroup_path_cache[cache_next].starttime = read_proc_starttime(pid);
    g_cgroup_path_cache[cache_next].valid = true;
    cache_next = (cache_next + 1) % CGROUP_PATH_CACHE_SIZE;
    return true;
}

static bool cgroup_write(pid_t pid, const char *file, const char *val) {
    char base[192], path[224];
    if (!cgroup_path_for_pid(pid, base, sizeof(base))) return false;
    snprintf(path, sizeof(path), "%s/%s", base, file);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        /* Cached path may be stale — invalidate and retry once with a
         * fresh resolve before giving up. */
        cgroup_path_cache_invalidate(pid);
        if (!cgroup_path_for_pid_fresh(pid, base, sizeof(base))) return false;
        snprintf(path, sizeof(path), "%s/%s", base, file);
        fd = open(path, O_WRONLY);
        if (fd < 0) return false;
    }
    ssize_t w = write(fd, val, strlen(val));
    close(fd);
    return w == (ssize_t)strlen(val);
}

#define LOGCAT_DRAIN_MAX_LINES 40
static FILE *g_logcat_fp = NULL;
static bool  g_logcat_failed = false;

static FILE *g_logcat_events_fp = NULL;
static bool  g_logcat_events_failed = false;

static void logcat_watch_init(void) {
    if (!g_logcat_fp && !g_logcat_failed) {
        g_logcat_fp = popen("logcat -b system -v brief "
                             "ActivityManager:I lowmemorykiller:V *:S 2>/dev/null", "r");
        if (!g_logcat_fp) {
            logw("ExternalKill: logcat popen failed — attribution disabled "
                 "for this run (%s)", strerror(errno));
            g_logcat_failed = true;
        } else {
            int fd = fileno(g_logcat_fp);
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            logi("ExternalKill: logcat attribution armed (system)");
        }
    }
    if (!g_logcat_events_fp && !g_logcat_events_failed) {
        g_logcat_events_fp = popen("logcat -b events -v brief "
                                    "am_kill:I am_proc_died:I am_low_memory:I *:S 2>/dev/null", "r");
        if (!g_logcat_events_fp) {
            logw("ExternalKill: logcat events-buffer popen failed — "
                 "events-buffer attribution disabled for this run (%s)",
                 strerror(errno));
            g_logcat_events_failed = true;
        } else {
            int fd = fileno(g_logcat_events_fp);
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            logi("ExternalKill: logcat attribution armed (events, "
                 "UNVERIFIED tag names — confirm with raw capture)");
        }
    }
}

static bool pkg_name_matches_line(const char *line, const char *name) {
    size_t nlen = strlen(name);
    if (nlen == 0) return false;
    const char *p = line;
    while ((p = strstr(p, name)) != NULL) {
        char before = (p == line) ? '\0' : *(p - 1);
        char after  = p[nlen];
        bool before_ok = !(isalnum((unsigned char)before) || before == '_' || before == '.');
        bool after_ok  = !(isalnum((unsigned char)after)  || after  == '_' || after  == '.');
        if (before_ok && after_ok) return true;
        p += 1; /* false match — keep scanning the rest of the line */
    }
    return false;
}

/* Shared by both logcat_kill_watch() (system buffer) and
 * logcat_kill_watch_events() (events buffer) — drains one FILE*, matches
 * kill-looking lines against live tracked app names, logs + records. */
static void logcat_drain_and_attribute(FILE **fp, bool *failed_flag,
                                        ProcInfo *tbl, int cnt,
                                        const char *source_label) {
    if (!*fp) return;
    char line[512];
    int drained = 0;
    while (drained < LOGCAT_DRAIN_MAX_LINES) {
        if (!fgets(line, sizeof(line), *fp)) {
            if (feof(*fp)) {
                pclose(*fp);
                *fp = NULL;
                *failed_flag = true;
                logw("ExternalKill: logcat %s stream ended — attribution "
                     "disabled for this run", source_label);
            } else {
                clearerr(*fp); /* benign EAGAIN — no data right now */
            }
            break;
        }
        drained++;
        if (!strstr(line, "Kill") && !strstr(line, "kill") &&
            !strstr(line, "died")) continue;
        for (int i = 0; i < cnt; i++) {
            if (tbl[i].rank == RANK_EXEMPT) continue;
            if (pkg_name_matches_line(line, tbl[i].name)) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = 0;
                logi("ExternalKill: [%s] %s: %s", tbl[i].name, source_label, line);
                record_kill(&tbl[i]);
                break;
            }
        }
    }
}

/* Called once per tick with the current live-process table. Drains
 * whatever logcat has buffered (bounded per tick so a burst can't stall
 * the main loop — any remainder is picked up next tick), and for any
 * line that looks kill-related, checks it against every currently-live
 * tracked (non-EXEMPT) app name. Pure observation — no kill/protect
 * decision anywhere in this function. */
static void logcat_kill_watch(ProcInfo *tbl, int cnt) {
    logcat_watch_init();

    logcat_drain_and_attribute(&g_logcat_fp, &g_logcat_failed, tbl, cnt, "system");
    logcat_drain_and_attribute(&g_logcat_events_fp, &g_logcat_events_failed, tbl, cnt, "events");
}

#define UNATTRIB_SEEN_WINDOW_S       6
#define UNATTRIB_RECENT_KILL_S      20

static void unattrib_check_vanished(ProcInfo *tbl, int cnt, time_t now) {
    for (int s = 0; s < g_score_count; s++) {
        AppScore *as = &g_scores[s];
        if (as->last_seen_t == 0) continue;
        time_t since_seen = now - as->last_seen_t;
        if (since_seen <= 0 || since_seen > UNATTRIB_SEEN_WINDOW_S) continue;

        bool still_present = false;
        for (int i = 0; i < cnt; i++) {
            if (strcmp(tbl[i].name, as->name) == 0) { still_present = true; break; }
        }
        if (still_present) continue; /* normal case — mark_seen below refreshes it */

        if (as->category == 0) continue;

        bool already_attributed = false;
        for (int k = 0; k < KILL_HIST_SIZE; k++) {
            if (g_kill_hist[k].name[0] &&
                strcmp(g_kill_hist[k].name, as->name) == 0 &&
                (now - g_kill_hist[k].killed_at) < UNATTRIB_RECENT_KILL_S) {
                already_attributed = true;
                break;
            }
        }
        if (already_attributed) continue;

        logw("Unattributed: [%s] vanished (last seen %llds ago, tier=%d "
             "score=%d) — no kill/ExternalKill record for it",
             as->name, (long long)since_seen, as->rank_tier, score_compute(as));
        ProcInfo ghost = {0};
        strncpy(ghost.name, as->name, sizeof(ghost.name) - 1);
        record_kill(&ghost);
    }
}

static void unattrib_mark_seen(ProcInfo *tbl, int cnt, time_t now) {
    for (int i = 0; i < cnt; i++) {
        AppScore *as = score_lookup(tbl[i].name);
        if (as) as->last_seen_t = now;
    }
}

static long try_reclaim(ProcInfo *p, AppScore *as) {
    if (g_zram_used_pct >= ZRAM_WARN_PCT) return 0;
    if (p->rss_kb < RECLAIM_MIN_RSS_KB) return 0;
    time_t now = time(NULL);
    if (as && as->last_reclaim_t > 0 &&
        (now - as->last_reclaim_t) < RECLAIM_COOLDOWN_S)
        return 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/reclaim", p->pid);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return 0; /* not supported on this kernel, or process gone */
    ssize_t w = write(fd, "all", 3);
    close(fd);
    if (as) as->last_reclaim_t = now; /* count the attempt either way */
    if (w != 3) return 0;
    long rss_before = p->rss_kb;
    long rss_after = 0, swap_after = 0;
    proc_mem_stats(p->pid, &rss_after, &swap_after);
    if (rss_after <= 0) return 0; /* process gone/unreadable post-reclaim */
    long freed = rss_before - rss_after;
    if (freed < RECLAIM_MIN_FREED_KB) return 0;
    p->rss_kb = rss_after; /* keep cached value consistent for downstream use */
    logi("Reclaim: [%s] freed=%ldMB (rss %ldMB->%ldMB)",
         p->name, freed/1024, rss_before/1024, rss_after/1024);
    kswapd_note_disruptive_event();
    return freed;
}

static bool try_squeeze_then_reclaim(ProcInfo *p, long *freed_out) {
    if (freed_out) *freed_out = 0;
    /* Skip PRIO_JUNK — already the lowest-value candidates, not worth the
     * write+reread syscall pair. */
    if (p->prio == PRIO_JUNK) return false;

    AppScore *cas = score_lookup(p->name);
    /* Try the cgroup v2 memory.high squeeze first — confirmed on-device to
     * trigger real synchronous reclaim inside the write() syscall itself
     * (78MB->46MB observed live). memory.high forces anon-page reclaim,
     * and ZRAM is the ONLY swap backend on this device, so a squeeze that
     * succeeds pushes pages INTO ZRAM — but per graduated_squeeze_target_pct()
     * above, we now WANT that even at real pressure, just less of it, since
     * squeeze is the tool that relieves ZRAM pressure in the first place. */
    int squeeze_pct = graduated_squeeze_target_pct();
    long high_target_kb = squeeze_pct > 0 ? (p->rss_kb * squeeze_pct) / 100 : 0;

    bool squeeze_cooling_down = cas && cas->cg_high_set &&
        (time(NULL) - cas->cg_high_set_t) < SQUEEZE_COOLDOWN_S;
    if (high_target_kb > 0 && !squeeze_cooling_down) {
        char valbuf[32];
        snprintf(valbuf, sizeof(valbuf), "%ld", high_target_kb * 1024);
        if (cgroup_write(p->pid, "memory.high", valbuf)) {
            if (cas) { cas->cg_high_set = true; cas->cg_high_set_t = time(NULL); }
            long rss_after = 0, swap_after = 0;
            proc_mem_stats(p->pid, &rss_after, &swap_after);
            long freed = (rss_after > 0) ? (p->rss_kb - rss_after) : 0;
            if (freed >= RECLAIM_MIN_FREED_KB) {
                if (freed_out) *freed_out = freed;
                p->rss_kb = rss_after;
                logi("CgSqueeze: [%s] freed=%ldMB via memory.high (target=%d%%)",
                     p->name, freed/1024, squeeze_pct);
                kswapd_note_disruptive_event();
                return true;
            }
        }
    }
    long reclaimed = try_reclaim(p, cas);
    if (reclaimed > 0) {
        if (freed_out) *freed_out = reclaimed;
        return true;
    }
    return false;
}

static time_t g_last_pin_squeeze_t = 0;

#define SQUEEZE_PIN_INTERVAL_S     60
#define SQUEEZE_PIN_SKIP_ACTIVE_S  30

static void pin_squeeze_pass(ProcInfo *tbl, int cnt) {
    time_t now = time(NULL);
    if (now - g_last_pin_squeeze_t < SQUEEZE_PIN_INTERVAL_S) return;
    g_last_pin_squeeze_t = now;
    /* Cheap early-out: graduated curve returns 0 below 50% anyway, no
     * point doing the table walk/lookups at all when there's no chance
     * of a squeeze happening. */
    if (g_zram_used_pct < 50) return;

    int squeezed = 0;
    for (int i = 0; i < g_prev_pinned_n; i++) {
        ProcInfo *p = NULL;
        for (int j = 0; j < cnt; j++) {
            if (strcmp(tbl[j].name, g_prev_pinned[i]) == 0) { p = &tbl[j]; break; }
        }
        if (!p) continue;
        AppScore *as = score_lookup(p->name);
        if (!as || !as->cg_low_set) continue; /* only actually-pinned right now */
        time_t recency = effective_fg_recency(p);
        if (recency != 0 && (now - recency) < SQUEEZE_PIN_SKIP_ACTIVE_S) continue;
        /* Cooldown is now enforced inside try_squeeze_then_reclaim() itself
         * (SQUEEZE_COOLDOWN_S, shared by every caller) — no separate check
         * needed here anymore. */
        long freed = 0;
        if (try_squeeze_then_reclaim(p, &freed)) {
            squeezed++;
            logi("PinSqueeze: [%s] freed=%ldMB (periodic pass)", p->name, freed/1024);
        }
    }
    if (squeezed > 0)
        logi("PinSqueeze: %d/%d pinned app(s) squeezed this pass",
             squeezed, g_prev_pinned_n);
}

#define SQUEEZE_KILL_GRACE_S   45

static bool should_spare_kill(ProcInfo *p, bool is_critical, long *freed_out) {
    if (freed_out) *freed_out = 0;
    if (is_critical) return false;

    time_t now_sk = time(NULL);
    AppScore *sk_as = score_lookup(p->name);
    if (sk_as &&
        ((sk_as->cg_high_set     && (now_sk - sk_as->cg_high_set_t)   < SQUEEZE_KILL_GRACE_S) ||
         (sk_as->last_reclaim_t > 0 && (now_sk - sk_as->last_reclaim_t) < SQUEEZE_KILL_GRACE_S))) {
        return true;
    }

    /* Squeeze before killing — a real reprieve here keeps the app warm
     * instead of forcing a cold relaunch. */
    if (try_squeeze_then_reclaim(p, freed_out)) return true;

    /* Kill ROI gate — don't spend a costly relaunch (high restart_count =
     * bounces back a lot, expensive/thrashy) on a candidate holding barely
     * any RAM. */
    if (p->restart_count >= KILL_ROI_RC_THRESHOLD &&
        p->rss_kb < KILL_ROI_MIN_RSS_KB) {
        time_t now = time(NULL);
        if (now - g_kill_roi_log_t >= LOG_RATELIMIT_S) {
            g_kill_roi_log_t = now;
            logi("KillROI: sparing [%s] rss=%ldMB rc=%d (below ROI floor)",
                 p->name, p->rss_kb/1024, p->restart_count);
        }
        return true;
    }
    return false;
}

static long do_kill(ProcInfo *tbl, int cnt, long target_kb,
                    Priority min_prio, bool is_critical, int avail_pct) {
    qsort(tbl, cnt, sizeof(ProcInfo), cmp_killable_ram);
    long freed = 0;
    time_t now = time(NULL);
    for (int i = 0; i < cnt && freed < target_kb; i++) {
        ProcInfo *p = &tbl[i];
        if (p->prio < min_prio)                  continue;
        if (p->oom_adj <= ADJ_VISIBLE_MAX)        continue;
        if (!is_critical && p->prio != PRIO_JUNK && is_actively_retained(p->name)) {
            logi("Retained: sparing %s (pinned by retention)", p->name);
            continue;
        }

        time_t rec_dk = effective_fg_recency(p);
        if (!is_critical && rec_dk > 0 &&
            (now - rec_dk) < score_protect_window_ram(p->name, avail_pct)) {
            logi("Sparing %s (score=%d win=%ds age=%lds)",
                 p->name, p->score,
                 score_protect_window_ram(p->name, avail_pct),
                 now - rec_dk);
            continue;
        }
        if (!is_critical && recently_killed(p->name)) {
            logi("Cooldown: skipping %s", p->name); continue;
        }
        /* In CRITICAL, allow PRIO_SEMI_PROTECTED if they hold substantial RAM */
        if (is_critical && p->prio == PRIO_SEMI_PROTECTED && p->rss_kb < 100*1024)
            continue;
        long reclaimed = 0;
        if (should_spare_kill(p, is_critical, &reclaimed)) {
            freed += reclaimed;
            continue;
        }
        if (kill(p->pid, SIGKILL) == 0) {
            freed += p->rss_kb;
            record_kill(p);
            logk("RAM pid=%-6d prio=%d sc=%-4d rc=%-2d fg_age=%-4lds rss=%4ldMB sw=%4ldMB [%s]",
                 p->pid, p->prio, p->score, p->restart_count,
                 p->raw_last_fg_do_not_read_directly ? (now - p->raw_last_fg_do_not_read_directly) : -1,
                 p->rss_kb/1024, p->avg_swap_kb/1024, p->name);
            usleep(30000);
        }
    }
    return freed;
}

static void full_reset_sweep(ProcInfo *tbl, int cnt) {
    time_t now = time(NULL);
    if (g_screen_off_since == 0 ||
        (now - g_screen_off_since) < IDLE_DEEP_S)
        return;

    time_t base = g_last_full_reset_t ? g_last_full_reset_t : g_start_time;
    if ((now - base) < RESET_GATE_S)
        return;

    g_reset_in_progress = true;
    int  killed   = 0;
    long freed_kb = 0;
    for (int i = 0; i < cnt; i++) {
        ProcInfo *p = &tbl[i];
        if (p->prio == PRIO_NEVER)          continue;
        if (p->rank == RANK_EXEMPT)         continue;
        if (p->oom_adj <= ADJ_VISIBLE_MAX)  continue; /* extra safety net */
        if (kill(p->pid, SIGKILL) == 0) {
            freed_kb += p->rss_kb;
            killed++;
            record_kill(p);
            logk("RESET pid=%-6d prio=%d sc=%-4d rc=%-2d rss=%4ldMB sw=%4ldMB [%s]",
                 p->pid, p->prio, p->score, p->restart_count,
                 p->rss_kb/1024, p->avg_swap_kb/1024, p->name);
            usleep(30000);
        }
    }
    g_reset_in_progress = false;
    g_last_full_reset_t = now;
    logi("Reset: full flush complete — killed=%d freed=%ldMB, next eligible in %dh",
         killed, freed_kb/1024, RESET_GATE_S/3600);
}

/* ================================================================
 *  ZRAM PRESSURE KILL (improved with new comparator)
 * ================================================================ */
static void zram_pressure_kill(ProcInfo *tbl, int cnt, int zram_used_pct) {
    if (zram_used_pct < ZRAM_TRIM_PCT) {

        if (g_zram_kill_start_t != 0) { g_zram_kill_start_t = 0; g_zram_pct_at_start = 0; }
        return;
    }

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

        bool zram_critical_retain = zram_used_pct >= ZRAM_CRIT_PCT;
        if (is_actively_retained(p->name) && p->prio != PRIO_JUNK &&
            !zram_critical_retain) continue;
        if (zram_critical_retain && is_actively_retained(p->name) &&
            p->prio != PRIO_JUNK) {
            logw("ZRAMkill: ZRAM critical (%d%%) — overriding retention for [%s]",
                 zram_used_pct, p->name);
        }

        {
            int win = score_protect_window(p->name);

            time_t rec_zk = effective_fg_recency(p);
            if (rec_zk > 0 && (now - rec_zk) < win) continue;
        }
        if (recently_killed(p->name)) { skipped_cd++; continue; }

        if (p->restart_count >= BOUNCE_SUPPRESS_RC &&
            p->swap_kb < BOUNCE_SUPPRESS_SWAP_KB &&
            p->prio != PRIO_JUNK)                   continue;
        if (p->bounce_count > BOUNCE_WINDOW_KILLS &&
            p->swap_kb < 200*1024)                  continue;

        if (p->restart_count >= KILL_ROI_RC_THRESHOLD &&
            p->rss_kb < KILL_ROI_MIN_RSS_KB) {
            if (now - g_kill_roi_log_t >= LOG_RATELIMIT_S) {
                g_kill_roi_log_t = now;
                logi("KillROI: sparing [%s] rss=%ldMB rc=%d (below ROI floor)",
                     p->name, p->rss_kb/1024, p->restart_count);
            }
            continue;
        }
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
                p->swap_kb < BOUNCE_SUPPRESS_SWAP_KB &&
                p->prio != PRIO_JUNK)               continue;
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

            AppScore *emerg_as = score_lookup(p->name);
            if (emerg_as &&
                ((emerg_as->cg_high_set && (now - emerg_as->cg_high_set_t) < SQUEEZE_KILL_GRACE_S) ||
                 (emerg_as->last_reclaim_t > 0 && (now - emerg_as->last_reclaim_t) < SQUEEZE_KILL_GRACE_S))) {
                continue;
            }
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

static bool has_trim_candidate(ProcInfo *tbl, int cnt) {
    for (int i = 0; i < cnt; i++) {
        if (tbl[i].prio >= PRIO_BACKGROUND && tbl[i].oom_adj > ADJ_VISIBLE_MAX)
            return true;
    }
    return false;
}

#define PROACTIVE_SQUEEZE_MAX_CANDIDATES 256
static long proactive_squeeze_pass(ProcInfo *tbl, int cnt, long want) {
    int  cand_idx[PROACTIVE_SQUEEZE_MAX_CANDIDATES];
    bool done[PROACTIVE_SQUEEZE_MAX_CANDIDATES] = {0};
    int  n = 0;
    for (int i = 0; i < cnt && n < PROACTIVE_SQUEEZE_MAX_CANDIDATES; i++) {
        if (tbl[i].prio >= PRIO_BACKGROUND && tbl[i].oom_adj > ADJ_VISIBLE_MAX)
            cand_idx[n++] = i;
    }
    long squeezed = 0;
    while (squeezed < want) {
        int biggest = -1;
        for (int k = 0; k < n; k++) {
            if (done[k]) continue;
            int i = cand_idx[k];
            if (biggest == -1 || tbl[i].rss_kb > tbl[cand_idx[biggest]].rss_kb)
                biggest = k;
        }
        if (biggest == -1) break;
        done[biggest] = true;
        int i = cand_idx[biggest];
        long freed = 0;
        if (try_squeeze_then_reclaim(&tbl[i], &freed) && freed > 0)
            squeezed += freed;
    }
    return squeezed;
}

static void proactive_squeeze(ProcInfo *tbl, int cnt, MemInfo *mi, ZramInfo *z,
                            const char *new_fg_name) {
    bool zram_hot = z->active && z->used_pct >= ZRAM_TRIM_PCT;
    if (mi->avail_pct >= LAUNCH_TRIM_AVAIL_PCT && !zram_hot)
        return;

    AppScore *as = score_lookup(new_fg_name);
    bool heavy = as && as->avg_swap_kb >= LAUNCH_TRIM_HEAVY_SWAP_KB;
    int target_pct = heavy ? LAUNCH_TRIM_HEAVY_TARGET_PCT : LAUNCH_TRIM_TARGET_PCT;
    if (z->active && z->used_pct >= ZRAM_STUCK_PCT) target_pct += 5;

    /* Warm relaunch (app was foreground very recently, e.g. quick recents
     * bounce): its pages are still hot, freeing room elsewhere isn't
     * needed and just costs CPU/relaunch-thrash. Still let
     * zram_pressure_kill below run if ZRAM itself is genuinely hot. */
    bool warm_relaunch = as && as->last_bg > 0 &&
                         (time(NULL) - as->last_bg) < LAUNCH_TRIM_WARM_SKIP_S;
    if (warm_relaunch && !zram_hot) {
        logi("Launch: %s warm relaunch (%lds ago) — skipping RAM squeeze",
             new_fg_name, (long)(time(NULL) - as->last_bg));
        return;
    }

    bool need_free = mi->avail_pct < target_pct && !warm_relaunch;
    if (need_free && !has_trim_candidate(tbl, cnt)) {
        /* Nothing structurally eligible — skip the log line and the
         * squeeze/qsort scans entirely rather than burn cycles at launch
         * time for a guaranteed 0. */
        need_free = false;
    } else if (need_free) {
        logi("Launch: %s foreground%s — proactive squeeze (avail=%d%% zram=%d%% target=%d%%)",
             new_fg_name, heavy ? " [heavy]" : "", mi->avail_pct,
             z->active ? z->used_pct : -1, target_pct);
    }

    if (need_free) {
        long want = (long)g_total_ram_kb * target_pct / 100 - mi->avail_kb;
        if (want > 0) {
            long squeezed = proactive_squeeze_pass(tbl, cnt, want);
            if (squeezed > 0) {
                logi("Launch: proactive squeeze freed %ldMB ahead of %s (no kills)",
                     squeezed/1024, new_fg_name);
                mi->avail_kb += squeezed;
                mi->avail_pct = (int)(mi->avail_kb * 100 / g_total_ram_kb);
            }
            long remaining = want - squeezed;
            if (remaining > 0) {
                /* Squeeze alone didn't cover it — fall back to killing
                 * for the shortfall only, same as the old proactive_trim
                 * behavior for the whole amount. */
                long freed = do_kill(tbl, cnt, remaining, PRIO_BACKGROUND, false, mi->avail_pct);
                if (freed > 0) {
                    logi("Launch: proactive RAM trim freed %ldMB ahead of %s (squeeze alone fell short)",
                         freed/1024, new_fg_name);
                    g_last_kill_t = time(NULL);
                }
            }
        }
    }

    if (zram_hot)
        zram_pressure_kill(tbl, cnt, z->used_pct);
}

/* Comparator: rank desc, swap_kb desc, adaptive_kill_count asc, score asc */
static int cmp_adaptive_clean(const void *a, const void *b) {
    const ProcInfo *pa = a, *pb = b;
    int ra = kill_rank_key(pa->rank), rb = kill_rank_key(pb->rank);
    if (ra != rb)
        return rb - ra;
    if (pa->swap_kb != pb->swap_kb)
        return (int)(pb->swap_kb - pa->swap_kb);
    AppScore *sa = score_lookup(pa->name);
    AppScore *sb = score_lookup(pb->name);
    int ak_a = sa ? sa->adaptive_kill_count : 0;
    int ak_b = sb ? sb->adaptive_kill_count : 0;
    if (ak_a != ak_b) return ak_a - ak_b;
    if (pa->score != pb->score) return pa->score - pb->score;
    if (pa->raw_last_fg_do_not_read_directly != pb->raw_last_fg_do_not_read_directly)
        return (pa->raw_last_fg_do_not_read_directly > pb->raw_last_fg_do_not_read_directly) ? 1 : -1;
    return 0;
}

static int adaptive_select_tier(MemInfo *mi, ZramInfo *z, bool *do_compact, bool *rising_out) {
    int zpct    = z->used_pct;
    bool ram_low = mi->avail_pct < FREE_LOW_PCT;

    bool ram_critical = mi->avail_pct < FREE_CRIT_PCT;
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
    *rising_out = rising;

    /* ── Tier selection (IDLE=-2, MAINT=-1, LOW=0, MED=1, HIGH=2, DEEP=3) ── */
    *do_compact = false;
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
        tier = 3; *do_compact = true;
    } else if (ram_critical) {
        tier = 2;
        if (!(zpct >= ZRAM_STUCK_PCT || (zpct >= ZRAM_TRIM_PCT && rising)))
            logi("AdaptiveTier: avail=%d%% < FREE_CRIT_PCT(%d%%) forced tier=2 "
                 "independent of ZRAM(%d%%)", mi->avail_pct, FREE_CRIT_PCT, zpct);
    } else if (zpct >= ZRAM_STUCK_PCT || (zpct >= ZRAM_TRIM_PCT && rising)) {
        tier = 2;
    } else if (zpct >= ZRAM_TRIM_PCT || rising || ram_low) {
        tier = 1;
    } else if (zpct >= ZRAM_WARN_PCT) {
        tier = 0;
    } else if (idle_deep_eligible) {
        tier = -3; *do_compact = true; /* IDLE-DEEP: intense + compaction */
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

    return tier;
}

static bool adaptive_kill_eligible(int tier, ProcInfo *p, MemInfo *mi, int zpct,
                                    bool ram_low, time_t now, int *hot_spared,
                                    int *dbg_win, long *dbg_age) {
    (void)ram_low;
    *dbg_win = -1;
    *dbg_age = -1;

    /* Never kill exempt (HAL/system/launcher) processes */
    if (p->rank == RANK_EXEMPT) return false;

    if (tier >= 2) {
        /* HIGH/DEEP may touch SEMI_PROTECTED if holding swap:
         * T3(DEEP): ≥40MB swap; T2(HIGH): ≥50MB swap */
        if (p->prio < PRIO_SEMI_PROTECTED) return false;
        long sp_thresh = (tier == 3) ? 40*1024 : 50*1024;
        if (p->prio == PRIO_SEMI_PROTECTED && p->swap_kb < sp_thresh) return false;
    } else {
        if (p->prio < PRIO_BACKGROUND) return false;
    }

    if (p->oom_adj <= ADJ_VISIBLE_MAX)                   return false;
    if (p->swap_kb < 256 && p->rss_kb < 64*1024)        return false;
    if (recently_killed(p->name))                        return false;
    if (p->raw_last_fg_do_not_read_directly > 0 &&
        (now - p->raw_last_fg_do_not_read_directly) < MIN_BG_KILL_AGE_S)           return false;

    /* T3(DEEP) override: when RAM is critically low, retained/spared
     * (AI_Swap-pinned) apps are no longer exempt — a memory choke
     * outranks retention. Foreground apps are still never touched
     * (RANK_EXEMPT/FOREGROUND guards). A small quota of RANK_HOT apps
     * is spared too, so light multitasking survives a critical sweep
     * instead of every retained app being wiped at once. */
    bool ram_critical = mi->avail_pct < FREE_CRIT_PCT;

    bool zram_critical = zpct >= ZRAM_CRIT_PCT;

    bool spare_this_hot = is_tier1_background(p) && (*hot_spared < T3_SPARE_HOT_N);

    bool retain_override = false;
    if (p->prio == PRIO_JUNK && !p->true_fg) {
        retain_override = true;
    } else if (tier == 3 && (ram_critical || zram_critical) &&
               !p->true_fg && !spare_this_hot) {
        if (ram_critical) {
            /* Real OOM proximity, not pool-fullness — hard override, no
             * gate, same as before. */
            retain_override = true;
        } else if (is_actively_retained(p->name)) {

            long freed = 0;
            if (try_squeeze_then_reclaim(p, &freed) && freed >= RECLAIM_MIN_FREED_KB) {
                logi("Adaptive[T3]: ZRAM critical — squeezed [%s] instead "
                     "of overriding (freed=%ldMB)", p->name, freed/1024);
            } else {
                retain_override = true;
            }
        } else {
            retain_override = true;
        }
    }
    if (is_actively_retained(p->name) && !retain_override) {
        if (spare_this_hot && tier == 3 && (ram_critical || zram_critical)) {
            (*hot_spared)++;
            logi("Adaptive[T3]: RAM critical — sparing HOT [%s] for "
                 "multitasking (%d/%d)", p->name, *hot_spared, T3_SPARE_HOT_N);
        }
        return false;
    }
    if (retain_override && is_actively_retained(p->name)) {
        if (p->prio == PRIO_JUNK)
            logw("Adaptive[T3]: confirmed runaway growth — overriding "
                 "retention for [%s]", p->name);
        else
            logw("Adaptive[T3]: %s (%d%%) — overriding retention for [%s]",
                 ram_critical ? "RAM critical" : "ZRAM critical",
                 ram_critical ? mi->avail_pct : zpct, p->name);
    }

    /* IDLE / IDLE-DEEP tiers: previously used flat age thresholds
     * (IDLE_KILL_LOWER_AGE_S=6min, IDLE_DEEP_LOWER_AGE_S=20s) that
     * completely bypassed score_protect_window() — meaning the 6h
     * FG_PROTECT window only ever applied to the T0-T3 RAM/ZRAM
     * pressure tiers below. Since IDLE-DEEP fires any time the screen
     * has been off 30+ min (very common — pocket, overnight), it was
     * the tier actually doing most of the killing in practice,
     * sweeping anything not touched in the last 20s regardless of how
     * well-used or well-scored it is. Now scaled against the same
     * score-based window, just discounted per tier so idle sweeps
     * still reclaim opportunistically sooner than a full RAM-pressure
     * wait — IDLE-DEEP more aggressively than plain IDLE, matching
     * the previous relaxed-vs-strict relationship. */
    if (tier == -2 || tier == -3) {
        int hot_age = (tier == -3) ? IDLE_DEEP_HOT_AGE_S : IDLE_KILL_HOT_AGE_S;

        time_t rec = effective_fg_recency(p);

        if (is_never_kill_now(p)) return false;  /* fg/exempt: never */
        if (is_tier1_background(p)) {
            /* tier1-background apps: wait before idle-killing */
            if (rec == 0 || (now - rec) < hot_age) return false;
            *dbg_age = now - rec; *dbg_win = hot_age;
        } else {
            /* tier2/tier3: scaled score_protect_window instead of
             * a flat age, so a well-used app is protected in line with
             * its actual score even during an idle sweep. */
            int pct = (tier == -3) ? IDLE_DEEP_PROTECT_PCT : IDLE_PROTECT_PCT;
            int win = score_protect_window(p->name) * pct / 100;
            if (rec > 0 && (now - rec) < win) return false;
            *dbg_age = rec > 0 ? now - rec : -1; *dbg_win = win;
        }
        /* cooldown still applies inside idle tiers */
    } else if (tier == -1) {
        /* MAINT: tier3 apps always eligible; tier2 apps with
         * significant swap also eligible when RAM mildly low — but
         * ONLY the old-COLD-equivalent half of tier2 (>=RANK_WARM_S
         * idle), not the old-WARM half, per explicit design decision:
         * the frequency floor in app_rank() parks heavily-used apps at
         * tier2 specifically to protect them, and MAINT shouldn't
         * undercut that just because WARM+COLD share a rank number now.
         * Reuses the existing RANK_WARM_S constant rather than a new
         * one. */
        time_t maint_rec = effective_fg_recency(p);
        bool maint_eligible = (p->rank == RANK_TIER3) ||
                              (p->rank == RANK_TIER2 &&
                               (maint_rec == 0 || (now - maint_rec) >= RANK_WARM_S) &&
                               ram_low && p->swap_kb >= 80*1024);
        if (!maint_eligible) return false;
        /* No score protect window for MAINT stale apps */
    } else {

        int win = score_protect_window(p->name);

        time_t rec_t = effective_fg_recency(p);
        if (rec_t > 0 && (now - rec_t) < win) return false;
    }

    if (p->restart_count >= BOUNCE_SUPPRESS_RC &&
        p->swap_kb < BOUNCE_SUPPRESS_SWAP_KB &&
        p->prio != PRIO_JUNK)                             return false;
    if (p->bounce_count > BOUNCE_WINDOW_KILLS &&
        p->swap_kb < 200*1024)                           return false;

    if (tier < 2 && tier != -3 && p->rank == RANK_TIER1) return false;
    if (tier == -3 && p->true_fg) return false;

    if (p->restart_count >= KILL_ROI_RC_THRESHOLD &&
        p->rss_kb < KILL_ROI_MIN_RSS_KB) {
        if (now - g_kill_roi_log_t >= LOG_RATELIMIT_S) {
            g_kill_roi_log_t = now;
            logi("KillROI: sparing [%s] rss=%ldMB rc=%d (below ROI floor)",
                 p->name, p->rss_kb/1024, p->restart_count);
        }
        return false;
    }

    return true;
}

static void adaptive_run_compaction(ZramInfo *z, int zpct, int kills, time_t now) {
    if (kills > 0) usleep(200000);

    MemInfo mi_chk; read_meminfo(&mi_chk);
    ZramInfo z_chk; zram_read_stats(&z_chk);
    if (mi_chk.avail_pct >= FREE_LOW_PCT && z_chk.used_pct < ZRAM_STUCK_PCT) {
        logi("Adaptive[T3]: kills alone relieved pressure "
             "(avail=%d%% ZRAM=%d%%) — skipping compaction stages",
             mi_chk.avail_pct, z_chk.used_pct);
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

static void adaptive_clean(ProcInfo *tbl, int cnt, MemInfo *mi, ZramInfo *z) {
    if (!z->active) return;

    int zpct    = z->used_pct;
    bool ram_low = mi->avail_pct < FREE_LOW_PCT;
    time_t now  = time(NULL);

    bool do_compact = false, rising = false;
    int tier = adaptive_select_tier(mi, z, &do_compact, &rising);

    classify_dumpsys_drain();
    category_reconfirm_sweep();

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
        int   dbg_win;
        long  dbg_age;

        if (!adaptive_kill_eligible(tier, p, mi, zpct, ram_low, now,
                                     &hot_spared, &dbg_win, &dbg_age))
            continue;

        long adaptive_reclaimed = 0;
        if (should_spare_kill(p, ram_low, &adaptive_reclaimed)) {
            freed_swap += adaptive_reclaimed;
            continue;
        }

        if (kill(p->pid, SIGKILL) == 0) {
            freed_swap += p->swap_kb + p->rss_kb / 4;
            kills++;
            record_kill(p);
            AppScore *as = score_lookup(p->name);
            if (as) { as->adaptive_kill_count++; as->dirty = true; g_scores_dirty = true; }

            logk("Adaptive[%s%s] rank%d pid=%-6d swap=%4ldMB rss=%4ldMB "
                 "win=%d age=%ld [%s]",
                 tier_label, rising ? " rising" : "",
                 p->rank, p->pid, p->swap_kb/1024, p->rss_kb/1024,
                 dbg_win, dbg_age, p->name);
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

    if (do_compact) adaptive_run_compaction(z, zpct, kills, now);
}

/* ================================================================
 *  MAIN DAEMON LOOP
 * ================================================================ */
static void signal_handler(int sig) {
    logi("Signal %d – shutting down", sig);
    g_running = false;
}

static void log_startup_banner(void) {
    logi("══════════════════════════════════════");
    logi("  LMK Engine %s by %s", LMK_VERSION, LMK_AUTHOR);
    logi("  ZRAM target %d%% of RAM", ZRAM_SIZE_PCT);
    logi("  ZRAM stages: warn>%d%% trim>%d%% stuck>%d%% crit>%d%%",
         ZRAM_WARN_PCT, ZRAM_TRIM_PCT, ZRAM_STUCK_PCT, ZRAM_CRIT_PCT);

    logi("  Adaptive clean: IDLE-DEEP/IDLE/MAINT/T0/T1/T2/T3(DEEP) always-running, 7 tiers");
    logi("  Adaptive intervals: %d/%d/%d/%d/%d/%d/%ds (IDLE-DEEP/IDLE/MAINT/LOW/MED/HIGH/DEEP)",
         IDLE_DEEP_INTVL_S, IDLE_INTVL_S, ADAPTIVE_INTVL_MAINT_S, ADAPTIVE_INTVL_LOW_S,
         ADAPTIVE_INTVL_MED_S, ADAPTIVE_INTVL_HIGH_S, ADAPTIVE_INTVL_HIGH_S);
    logi("  Adaptive kills: %d/%d/%d/%d/%d/%d/%d (IDLE-DEEP/IDLE/MAINT/LOW/MED/HIGH/DEEP)",
         IDLE_DEEP_KILLS_MAX, IDLE_KILLS_MAX, ADAPTIVE_KILLS_MAINT, ADAPTIVE_KILLS_LOW,
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

    logi("  Proactive launch squeeze: target %d%% (heavy app %d%%) on fg app "
         "switch — cgroup memory.high squeeze first, do_kill() fallback "
         "only for any shortfall",
         LAUNCH_TRIM_TARGET_PCT, LAUNCH_TRIM_HEAVY_TARGET_PCT);
    logi("  RAM kill thresholds: low<%d%% crit<%d%% stop>%d%%",
         FREE_LOW_PCT, FREE_CRIT_PCT, FREE_HIGH_PCT);
    logi("  Protect window: %d–%ds (score+RAM-aware)",
         FG_PROTECT_BASE, FG_PROTECT_MAX);
    logi("  Kill cooldown: %d–%ds (restart_count scaled)",
         RESTART_CD_BASE_S, RESTART_CD_MAX_S);

    logi("  Retention: Phase A up to %d TIER1-only pin slot(s) (of %d TIER1 cap), "
         "Phase B up to %d shared slot(s) (of %d TIER2 cap) — real cgroup "
         "memory.low floors, held >=%lds after leaving foreground, "
         "candidacy requires use within %lds (%ds settle delay)",
         TIER1_PIN_MAX, TIER1_MAX_SLOTS, TIER2_PIN_MAX, TIER2_MAX_SLOTS,
         (long)TIER1_PIN_HOLD_S, (long)RETAIN_FG_AGE_S, PIN_SETTLE_S);
    logi("  Bounce suppression: rc>=%d and swap<%dMB, plus time-window (%d kills in %ds)",
         BOUNCE_SUPPRESS_RC, BOUNCE_SUPPRESS_SWAP_KB/1024,
         BOUNCE_WINDOW_KILLS, BOUNCE_WINDOW_S);
    logi("  Retention protect: pinned apps exempt from opportunistic kills for %ds after pin",
         RETAIN_PROTECT_GRACE_S);
    logi("  Log rate-limit: noisy-state messages throttled to 1/%ds",
         LOG_RATELIMIT_S);
    logi("══════════════════════════════════════");
}

/* Pure sequential init: priority/name, start-time file, persisted state
 * loads (scores/rank-cache), PSI setup, ZRAM device discovery+setup.
 * No control-flow branches that affect the caller — safe extract. */
static void daemon_init(void) {
    setpriority(PRIO_PROCESS, 0, -15);
    prctl(PR_SET_NAME, "lmk_engine", 0, 0, 0);

    g_start_time = time(NULL);
    {
        FILE *sf = fopen(START_TIME_FILE, "w");
        if (sf) { fprintf(sf, "%ld\n", (long)g_start_time); fclose(sf); }
    }

    score_load(false);
    rank_cache_load();
    psi_check_available();
    psi_monitor_init();
    lmkd_minfree_cleanup();

    if (zram_find(g_zram_dev, sizeof(g_zram_dev), g_zram_sys, sizeof(g_zram_sys))) {
        zram_setup();
    } else {
        logw("ZRAM: no device found – running without ZRAM management");
    }

    pin_kswapd_to_little_cores();
}

/* Boot-settle wait ───────────────────────────────────────────
 * Poll sys.boot_completed for up to BOOT_WAIT_S seconds before
 * entering the main loop.  This prevents acting on an incomplete
 * process table or a missing appwidgets.xml at early boot.
 * ─────────────────────────────────────────────────────────────── */
#define BOOT_WAIT_S  90

#define FRESH_BOOT_UPTIME_MAX_S  300
static double read_uptime_s(void) {
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) return -1.0;
    double up = -1.0;
    if (fscanf(f, "%lf", &up) != 1) up = -1.0;
    fclose(f);
    return up;
}
static void wait_for_boot_settle(void) {

    double up = read_uptime_s();
    if (up >= 0.0 && up > FRESH_BOOT_UPTIME_MAX_S) {
        logi("Boot: system uptime %.0fs — manual restart, not a fresh boot; "
             "skipping widget-settle grace", up);
        g_widget_settle_until = 0;
        return;
    }

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

/* Screen-off / Doze strengthens idle detection — fast-tracks the
 * COLD+STALE idle sweep timer when the screen is off or Doze is
 * active, since there's no genuine fg-change to wait for. */
static void update_idle_detection_state(time_t now) {
    if (g_last_fg_change_t == 0) g_last_fg_change_t = now;
    bool scr_off = is_screen_off();
    if (scr_off) {
        if (g_screen_off_since == 0) g_screen_off_since = now;
    } else if (g_screen_off_since != 0) {
        logi("Idle: screen on — exiting idle-deep window (was off %ldm)",
             (long)(now - g_screen_off_since) / 60);
        g_screen_off_since = 0;

        drop_clean_caches("screen-on/unlock");
    }
    /* Screen-off or Doze fast-tracks idle detection */
    if ((scr_off || is_doze_active()) &&
        (now - g_last_fg_change_t) < IDLE_DETECT_S)
        g_last_fg_change_t = now - IDLE_DETECT_S; /* fast-track idle */
}

/* True-foreground detection + debounce + proactive trim. Skipped
 * entirely during the widget-settle grace window. */
static void handle_true_foreground_detection(ProcInfo *tbl, int cnt,
                                              MemInfo *mi, ZramInfo *z) {
    time_t now = time(NULL);
    if (now < g_widget_settle_until) {
        /* still in widget-settle grace — skip kills */
        return;
    }
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

            char base_pkg[128];
            strncpy(base_pkg, fg_name, sizeof(base_pkg) - 1);
            base_pkg[sizeof(base_pkg) - 1] = '\0';
            char *colon = strchr(base_pkg, ':');
            if (colon) *colon = '\0';
            bool confirmed = is_launcher_like(fg_name) ||
                              confirm_true_foreground(base_pkg);
            if (!confirmed) {
                logi("Launch: %s reached fg (adj=0) but isn't the "
                     "focused activity — skipping trim/credit "
                     "(likely overlay/foreground-service)", fg_name);
            } else {
                if (!is_launcher_like(fg_name))
                    proactive_squeeze(tbl, cnt, mi, z, fg_name);
                strncpy(g_true_fg_name, fg_name, sizeof(g_true_fg_name) - 1);
                g_true_fg_name[sizeof(g_true_fg_name) - 1] = '\0';
                g_last_fg_change_t = now; /* reset idle timer on fg switch */
            }
            g_pending_fg_streak = 0;
            g_pending_fg_name[0] = '\0';
        }
    } else if (strcmp(fg_name, g_true_fg_name) == 0) {
        /* Candidate matches confirmed fg again — clear stale pending state */
        g_pending_fg_streak = 0;
        g_pending_fg_name[0] = '\0';
    }
}

/* ZRAM stage management: drop_caches nudge, pressure-kill escalation,
 * stuck-ZRAM tracking/logging, and critical-band cycle attempt.
 * last_zram_cycle/last_drop_caches are caller-owned tick-to-tick state. */
static void handle_zram_stage_management(ProcInfo *tbl, int cnt,
                                          MemInfo *mi, ZramInfo *z, time_t now,
                                          time_t *last_zram_cycle,
                                          time_t *last_drop_caches) {
    if (!z->active) return;

    if (g_zram_needs_resize)
        zram_try_deferred_resize(mi);

    if (z->used_pct >= ZRAM_WARN_PCT && z->used_pct < ZRAM_TRIM_PCT) {
        if (now - *last_drop_caches > 60) {
            *last_drop_caches = now;
            int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
            if (fd >= 0) { write(fd, "1\n", 2); close(fd); }
        }
    }

    if (z->used_pct >= ZRAM_TRIM_PCT) {
        if (time(NULL) >= g_widget_settle_until)
        zram_pressure_kill(tbl, cnt, z->used_pct);
    }

    /* Stuck ZRAM tracking — Adaptive[T3] fires automatically */
    {
        if (z->used_pct >= ZRAM_STUCK_PCT) {
            if (g_zram_stuck_since == 0) {
                g_zram_stuck_since = now;
                logi("ZRAMstuck: ZRAM at %d%% — Adaptive[T3] will engage "
                     "after %ds",
                     z->used_pct, ZRAM_STUCK_S);
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
                    long cd_remain = (g_last_deepclean == 0) ? 0 :
                        (long)(ZRAM_DEEPCLEAN_CD_S - (now - g_last_deepclean));
                    if (cd_remain < 0) cd_remain = 0;
                    logi("ZRAMstuck: %d%% for %lds — T3 eligible=%s cd_remain=%lds",
                         z->used_pct, (long)(now - g_zram_stuck_since),
                         (now - g_zram_stuck_since) >= ZRAM_STUCK_S ? "yes" : "no",
                         cd_remain);
                }
            }
        } else {
            if (g_zram_stuck_since != 0) {
                logi("ZRAMstuck: cleared (now %d%%)", z->used_pct);
                g_zram_stuck_since = 0;
                g_log_stuck_t      = 0;
            }
        }
    }

    if (z->used_pct >= ZRAM_CRIT_PCT) {
        if ((now - *last_zram_cycle) > 300) {
            long need_kb = z->orig_data_kb + ZRAM_CYCLE_MARGIN_KB;
            bool can_cycle = (mi->avail_kb >= need_kb);
            if (can_cycle) {
                *last_zram_cycle = now;
                zram_cycle(false);
            } else {
                *last_zram_cycle = now;
                if (now - g_log_crit_t >= LOG_RATELIMIT_S) {
                    g_log_crit_t = now;
                    logi("ZRAMcrit: cannot cycle passively (orig=%ldMB avail=%ldMB) "
                         "— forced pre-clean disabled, see comment above",
                         z->orig_data_kb/1024, mi->avail_kb/1024);
                }
            }
        }
    }
}

/* RAM-pressure kill pass + adaptive loop-period tuning. Mirrors the
 * original inline logic's early-exit behavior exactly: when avail is
 * comfortably high or the kill cooldown hasn't elapsed, this returns
 * without touching *loop_sleep_us, leaving the previous tick's value
 * in place (same as the original bare `continue` skipping the tail
 * of the loop body). */
static void ram_pressure_and_adapt(ProcInfo *tbl, int cnt,
                                    MemInfo *mi, ZramInfo *z, time_t now,
                                    useconds_t *loop_sleep_us) {
    int avail = mi->avail_pct;
    if (avail >= FREE_HIGH_PCT) return;

    int cd = (avail < FREE_CRIT_PCT) ? 1 : 5;
    if ((now - g_last_kill_t) < cd) return;

    long want, freed = 0;
    if (avail < FREE_CRIT_PCT) {
        want = (long)g_total_ram_kb * FREE_LOW_PCT / 100 - mi->avail_kb;
        if (want < 64*1024) want = 64*1024;
        freed = do_kill(tbl, cnt, want, PRIO_SEMI_PROTECTED, true, avail);
        logi("CRITICAL: freed %ldMB", freed/1024);
    } else if (avail < FREE_LOW_PCT) {
        want = (long)g_total_ram_kb * (FREE_LOW_PCT + 4) / 100 - mi->avail_kb;
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
    bool calm = (mi->avail_pct >= FREE_LOW_PCT + 5) &&
                (z->used_pct < ZRAM_WARN_PCT) &&
                (now - g_last_fg_change_t) > 5;
    *loop_sleep_us = calm ? 1500000 : 500000;
}

static void fix_lmkd_psi_stall(void) {
    FILE *fp = popen("getprop ro.lmk.psi_partial_stall_ms 2>/dev/null", "r");
    char val[32] = {0};
    if (fp) { if (!fgets(val, sizeof(val), fp)) val[0] = 0; pclose(fp); }
    int len = (int)strlen(val);
    while (len > 0 && (val[len-1] == '\n' || val[len-1] == '\r')) val[--len] = 0;

    if (strcmp(val, "200") == 0) {
        logi("PSItune: ro.lmk.psi_partial_stall_ms already 200ms, no action");
        return;
    }
    logi("PSItune: ro.lmk.psi_partial_stall_ms=%s (expected 200ms low-ram "
         "profile) — correcting and restarting lmkd to apply this boot",
         val[0] ? val : "(unset/default 70ms high-end profile)");
    if (system("resetprop ro.lmk.psi_partial_stall_ms 200 2>/dev/null") != 0)
        logw("PSItune: resetprop call failed — is Magisk's resetprop on PATH?");
    if (system("stop lmkd 2>/dev/null; start lmkd 2>/dev/null") != 0)
        logw("PSItune: lmkd restart command failed — value set but not "
             "applied until next full reboot");
}

static void run_daemon(void) {
    log_startup_banner();
    daemon_init();
    fix_lmkd_psi_stall();
    wait_for_boot_settle();

    ProcInfo *tbl = malloc(2048 * sizeof(ProcInfo));
    if (!tbl) { loge("malloc failed"); return; }

    MemInfo mi;
    int  tick = 0;
    time_t last_zram_cycle  = 0;
    time_t last_drop_caches = 0;
    useconds_t loop_sleep_us = 500000; /* adaptive: 500ms busy, up to 1.5s calm */

    double prof_enum_ms = 0, prof_pin_ms = 0, prof_adapt_ms = 0, prof_zram_ms = 0;

    while (g_running) {
        wait_for_next_tick(loop_sleep_us);
        if (read_meminfo(&mi) < 0) continue;
        load_active_widget_pkgs();

        struct timespec _t0, _t1;
        clock_gettime(CLOCK_MONOTONIC, &_t0);
        int cnt = enumerate_procs(tbl, 2048);
        clock_gettime(CLOCK_MONOTONIC, &_t1);
        prof_enum_ms += (_t1.tv_sec - _t0.tv_sec) * 1000.0 +
                        (_t1.tv_nsec - _t0.tv_nsec) / 1e6;
        if (cnt < 0) continue;

        track_foreground(tbl, cnt);
        logcat_kill_watch(tbl, cnt); /* NEW: external-kill attribution, diagnostic only */
        score_maybe_save();
        rank_cache_maybe_save(tbl, cnt);

        /* ── Active retention (must run before kill paths so
         *    is_actively_retained() guard is current) ── */
        clock_gettime(CLOCK_MONOTONIC, &_t0);
        oom_pin_retained(tbl, cnt, mi.avail_pct);
        clock_gettime(CLOCK_MONOTONIC, &_t1);
        prof_pin_ms += (_t1.tv_sec - _t0.tv_sec) * 1000.0 +
                       (_t1.tv_nsec - _t0.tv_nsec) / 1e6;

        /* ── Periodic status log ── */
        ZramInfo z; zram_read_stats(&z);
        g_zram_used_pct = z.used_pct;

        apply_avail_tier_tunables(mi.avail_pct, g_zram_used_pct);

        pin_squeeze_pass(tbl, cnt);
        if (++tick >= 20) {
            tick = 0;
            double self_cpu_pct = 0.0, kswapd_cpu_pct = 0.0;
            cpu_track_sample(&self_cpu_pct, &kswapd_cpu_pct);
            logi("RAM total=%ldMB avail=%ldMB(%d%%)  "
                 "ZRAM %ldMB/%ldMB(%d%%)  orig=%ldMB  procs=%d  "
                 "cpu self=%.1f%% kswapd=%.1f%%  "
                 "state=%s",
                 mi.total_kb/1024, mi.avail_kb/1024, mi.avail_pct,
                 z.used_kb/1024, z.disksize_kb/1024, z.used_pct,
                 z.orig_data_kb/1024, cnt, self_cpu_pct, kswapd_cpu_pct,
                 mi.avail_pct < FREE_CRIT_PCT ? "CRITICAL" :
                 (mi.avail_pct < FREE_LOW_PCT ? "LOW" : "NORMAL"));
            logi("Profile: enum=%.1fms pin=%.1fms adapt=%.1fms zram=%.1fms "
                 "(avg ms/tick over last 20)",
                 prof_enum_ms/20, prof_pin_ms/20, prof_adapt_ms/20, prof_zram_ms/20);
            prof_enum_ms = prof_pin_ms = prof_adapt_ms = prof_zram_ms = 0;
        }

        /* ── Dynamic swappiness (reference engine hook) ── */
        if (z.active) update_swappiness(z.used_pct);

        time_t now = time(NULL);

        unattrib_check_vanished(tbl, cnt, now);
        unattrib_mark_seen(tbl, cnt, now);

        update_idle_detection_state(now);
        handle_true_foreground_detection(tbl, cnt, &mi, &z);

        /* ── Full reset sweep (self-gated: 12h + 30min idle) ── */
        if (time(NULL) >= g_widget_settle_until)
        full_reset_sweep(tbl, cnt);

        /* ── Adaptive clean (self-gated, tiered) ── */
        if (time(NULL) >= g_widget_settle_until) {
            clock_gettime(CLOCK_MONOTONIC, &_t0);
            adaptive_clean(tbl, cnt, &mi, &z);
            clock_gettime(CLOCK_MONOTONIC, &_t1);
            prof_adapt_ms += (_t1.tv_sec - _t0.tv_sec) * 1000.0 +
                             (_t1.tv_nsec - _t0.tv_nsec) / 1e6;
        }

        /* ── ZRAM stage management (uses `now` declared above) ── */
        clock_gettime(CLOCK_MONOTONIC, &_t0);
        handle_zram_stage_management(tbl, cnt, &mi, &z, now,
                                      &last_zram_cycle, &last_drop_caches);
        clock_gettime(CLOCK_MONOTONIC, &_t1);
        prof_zram_ms += (_t1.tv_sec - _t0.tv_sec) * 1000.0 +
                        (_t1.tv_nsec - _t0.tv_nsec) / 1e6;

        /* ── RAM pressure kill + adaptive loop period ── */
        ram_pressure_and_adapt(tbl, cnt, &mi, &z, now, &loop_sleep_us);
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

        strncpy(g_zram_dev, dev, sizeof(g_zram_dev) - 1);
        g_zram_dev[sizeof(g_zram_dev) - 1] = '\0';
        strncpy(g_zram_sys, sys, sizeof(g_zram_sys) - 1);
        g_zram_sys[sizeof(g_zram_sys) - 1] = '\0';
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
        if (z.orig_data_kb > 0) {

            if (z.compr_data_kb > 0)
                printf("  ZRAM orig  : %ld MB (uncompressed, %.2fx ratio)\n",
                       z.orig_data_kb/1024,
                       (double)z.orig_data_kb/z.compr_data_kb);
            else
                printf("  ZRAM orig  : %ld MB (uncompressed, ratio unavailable "
                       "— mm_stat field 2 unreadable)\n", z.orig_data_kb/1024);
        }
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

static void explain_pkg(const char *pkg) {
    score_load(true);
    AppScore *s = score_lookup(pkg);

    printf("=== %s ===\n", pkg);
    if (!s) {
        printf("  No entry in %s — never scored (not yet seen foregrounded,\n"
               "  or evicted from the %d-slot table without a confirmed-\n"
               "  category protection bonus).\n", SCORE_FILE, SCORE_MAX_APPS);
    } else {
        time_t now = time(NULL);
        int score = score_compute(s);
        printf("  Score        : %d / %d\n", score, SCORE_MAX);
        printf("  Category     : %s%s\n",
               s->category == 0 ? "NATIVE" : s->category == 1 ? "SYSAPP" : "USER",
               s->category_confirmed ? " (confirmed)" : " (unconfirmed guess)");
        printf("  fg_count     : %d\n", s->fg_count);
        printf("  session_count: %d\n", s->session_count);
        printf("  restart_count: %d%s\n", s->restart_count,
               s->restart_count >= BOUNCE_SUPPRESS_RC ? "  [confirmed bouncer]" : "");
        printf("  avg_swap_kb  : %ld KB\n", s->avg_swap_kb);
        printf("  avg_session  : %ld s%s\n", s->avg_session_duration_s,
               s->avg_session_duration_s >= MIN_GENUINE_SESSION_S ? "" : " (below genuine-session floor)");
        if (s->raw_last_fg_do_not_read_directly > 0) {
            long ago = now - s->raw_last_fg_do_not_read_directly;
            printf("  last_fg      : %lds ago (%ldh %ldm)\n", ago, ago/3600, (ago%3600)/60);
        } else {
            printf("  last_fg      : never\n");
        }
        printf("  dc_kill_count: %d   adaptive_kill_count: %d\n",
               s->dc_kill_count, s->adaptive_kill_count);
    }

    bool never_kill  = name_matches(pkg, NEVER_KILL);
    bool svc_exempt  = name_matches(pkg, SERVICE_EXEMPT);
    bool launcher    = is_launcher_like(pkg);
    bool ime         = is_ime_like(pkg);
    printf("  Static class : %s\n",
           never_kill ? "NEVER_KILL" : svc_exempt ? "SERVICE_EXEMPT" :
           launcher   ? "launcher"   : ime         ? "IME" : "(none — normal candidate)");

    /* Rank cache: last snapshot the live daemon wrote (rank/swap as of its
     * last save interval, not necessarily current this instant). */
    FILE *rf = fopen(RANK_CACHE_FILE, "r");
    bool found_rank = false;
    if (rf) {
        char line[300], name[256]; long sw; int rk;
        while (fgets(line, sizeof(line), rf)) {
            if (sscanf(line, "%255s %ld %d", name, &sw, &rk) == 3 &&
                strcmp(name, pkg) == 0) {

                static const char * const RANK_NAMES[] = {
                    "EXEMPT","TIER1","TIER2","TIER3","TIER4"
                };
                printf("  Cached rank  : %s (swap=%ldKB, as of last rank-cache save)\n",
                       (rk >= 0 && rk <= 4) ? RANK_NAMES[rk] : "?", sw);
                found_rank = true;
                break;
            }
        }
        fclose(rf);
    }
    if (!found_rank) printf("  Cached rank  : not in rank cache\n");

    /* Last few kill/bounce/protect log lines mentioning this pkg. */
    FILE *lf = fopen(LOG_FILE, "r");
    if (lf) {
        char line[512];
        char hist[5][512]; int hn = 0;
        while (fgets(line, sizeof(line), lf)) {
            if (strstr(line, pkg) &&
                (strstr(line, "pid=") || strstr(line, "Resilient") ||
                 strstr(line, "Sparing") || strstr(line, "Retained"))) {
                strncpy(hist[hn % 5], line, sizeof(hist[0]) - 1);
                hist[hn % 5][sizeof(hist[0]) - 1] = 0;
                hn++;
            }
        }
        fclose(lf);
        if (hn > 0) {
            printf("  Recent log events (last %d):\n", hn < 5 ? hn : 5);
            int start = hn < 5 ? 0 : hn - 5;
            for (int i = start; i < hn; i++) {
                char *l = hist[i % 5];
                size_t len = strlen(l);
                if (len && l[len-1] == '\n') l[len-1] = 0;
                printf("    %s\n", l);
            }
        } else {
            printf("  Recent log events: none found in current log\n");
        }
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
"  %s --force-zram-cycle [--yes-i-know]  test-trigger a ZRAM cycle\n"
"                     standalone, no live daemon needed (see notes in\n"
"                     source above --force-zram-cycle in main())\n"
"  %s --explain <pkg> show what the daemon knows about one app\n"
"  %s --log           tail live log\n"
"  %s --swap <MB>     create & enable file swap\n"
"  %s --delswap       remove file swap\n",
        LMK_VERSION, LMK_AUTHOR,
        prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ================================================================
 *  MAIN
 * ================================================================ */
int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 0; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "--features")) {
        printf("LMK Engine %s by %s — feature list\n\n", LMK_VERSION, LMK_AUTHOR);
        printf("1) Score Retention — per-app usage scoring & pinning\n"
               "   - Per-app scoring (lmk_scores.dat, v9 format, dirty-gated\n"
               "     incremental writes + daily stale-entry compaction)\n"
               "   - fg_count / session_count / avg_swap_kb tracking\n"
               "   - rank blends recency with session_count frequency, so\n"
               "     frequent-but-not-recent apps don't rank as low as one-offs\n"
               "   - True foreground vs background-launch differentiation,\n"
               "     with debounced foreground-change detection (2-tick confirm)\n"
               "   - rank-based kill ordering (lmk_rank.cache, persists across restarts)\n"
               "   - OOM pinning / active retention: phased memory.low floors\n"
               "     (TIER1-only slots, then shared TIER1/TIER2 slots), tier\n"
               "     capacity enforced per-package (was a flat oom_adj step\n"
               "     threshold, then dynamic headroom-scaled oom_adj — both removed)\n"
               "   - bounce-loop suppression (restart_count + window tracking)\n"
               "   - generalized runaway-growth detection for any non-fg,\n"
               "     non-HAL, non-pinned process (was scene-daemon-shaped)\n\n");
        printf("2) Adaptive_Clean — memory & ZRAM reclaim\n"
               "   - 7-tier escalation: IDLE-DEEP / IDLE / MAINT / LOW / MED / HIGH / DEEP\n"
               "   - PSI (/proc/pressure/memory) pressure escalation\n"
               "   - PSI event-driven fast-wake (poll() on full-stall trigger,\n"
               "     same mechanism real lmkd uses — reacts to jank-causing\n"
               "     stalls within the current tick's sleep window instead of\n"
               "     waiting out the full interval)\n"
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
               "   - Proactive launch squeeze (cgroup memory.high first, kill\n"
               "     fallback only for any shortfall), with warm-relaunch skip\n"
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

    if (!strcmp(cmd, "--force-zram-cycle")) {
        MemInfo mi;
        if (read_meminfo(&mi) != 0) { fprintf(stderr, "Failed to read meminfo\n"); return 1; }
        g_total_ram_kb = mi.total_kb;
        char dev[128] = {0}, sys[128] = {0};
        if (!zram_find(dev, sizeof(dev), sys, sizeof(sys))) {
            fprintf(stderr, "No active ZRAM device found\n"); return 1;
        }
        strncpy(g_zram_dev, dev, sizeof(g_zram_dev) - 1);
        g_zram_dev[sizeof(g_zram_dev) - 1] = '\0';
        strncpy(g_zram_sys, sys, sizeof(g_zram_sys) - 1);
        g_zram_sys[sizeof(g_zram_sys) - 1] = '\0';
        g_zram_active = true;
        bool bypass_gate = (argc >= 3 && !strcmp(argv[2], "--yes-i-know"));
        printf(bypass_gate
               ? "Forcing ZRAM cycle (bypassing avail_kb safety gate)...\n"
               : "Attempting ZRAM cycle (normal gate applies — pass "
                 "--yes-i-know to bypass it for testing)...\n");
        zram_cycle(bypass_gate);
        printf("Done — check log for ZRAMcycle: lines.\n");
        return 0;
    }
    if (!strcmp(cmd, "--explain")) {
        if (argc < 3) { fprintf(stderr,"Usage: %s --explain <package.name>\n",argv[0]); return 1; }
        explain_pkg(argv[2]); return 0;
    }

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