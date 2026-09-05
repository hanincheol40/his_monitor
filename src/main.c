/*
 * main.c — threading, periodic scheduling, watchdog.
 *
 * The tool has to do two jobs at once and they have different natures.
 *
 *   Reading is I/O bound and bursty. 116 files are appended to independently;
 *   in a from-start pass they are ~1 MB each, and the time to walk them all
 *   varies with what the solver has just written. Done serially in the display
 *   loop, the screen stalls whenever the disk does.
 *
 *   Drawing is a periodic task. It should happen at a fixed rate whatever the
 *   readers are doing, and a period that slips should be visible rather than
 *   silently absorbed.
 *
 * So: a pool of reader threads pulls domains off a work queue, the main thread
 * waits on a tick barrier and then renders, and the period is held with
 * clock_nanosleep(TIMER_ABSTIME) against an absolute deadline rather than a
 * relative sleep, so the work time of one tick does not push the next one
 * late. Jitter and overruns are measured and shown, because a monitor that
 * quietly falls behind is worse than no monitor.
 */
#define _POSIX_C_SOURCE 200809L
#include "monitor.h"
#include "hisfile.h"
#include "wave.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

static Dom  g_dom[MAXDOM];
static Ctx  g_ctx;
static char g_base[256];

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void show_cursor(void) { fputs("\x1b[?25h", stdout); fflush(stdout); }

static double mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------- reader thread pool */

static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_go  = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  q_done = PTHREAD_COND_INITIALIZER;
static int   q_next, q_active, q_nthreads, q_quit;
static long  q_gen;                        /* tick generation, avoids lost wakeups */

static void on_sample(Dom *d) { wave_sample(d); }

/* Open lazily: the solver may not have created every .his yet when we start. */
static void ensure_open(Dom *d, int from_start)
{
    char path[512];
    if (d->fp) return;
    snprintf(path, sizeof path, "%s_%d.his", g_base, d->id);
    d->fp = fopen(path, "r");
    if (!d->fp) return;
    /* Reopening after a read error: resume where the error happened, so the
     * byte count stays right and the waveform does not jump. The header was
     * already parsed before the error, so there is nothing to prime. */
    if (d->resume > 0) { fseek(d->fp, d->resume, SEEK_SET); return; }
    /* Follow mode still has to read the header before skipping to the end --
     * it appears once, at the top. See his_prime(). */
    if (!from_start) his_prime(d);
}

static int g_from_start;

static void *reader(void *arg)
{
    long seen = 0;
    (void)arg;
    for (;;) {
        int idx;
        pthread_mutex_lock(&q_mtx);
        while (!q_quit && q_gen == seen) pthread_cond_wait(&q_go, &q_mtx);
        if (q_quit) { pthread_mutex_unlock(&q_mtx); break; }
        seen = q_gen;
        pthread_mutex_unlock(&q_mtx);

        for (;;) {                                  /* drain the work queue */
            pthread_mutex_lock(&q_mtx);
            idx = (q_next < g_ctx.ndom) ? q_next++ : -1;
            pthread_mutex_unlock(&q_mtx);
            if (idx < 0) break;
            ensure_open(&g_dom[idx], g_from_start);
            if (his_read_new(&g_dom[idx], on_sample) > 0)
                g_dom[idx].last_rx = mono();
        }

        pthread_mutex_lock(&q_mtx);
        if (--q_active == 0) pthread_cond_signal(&q_done);
        pthread_mutex_unlock(&q_mtx);
    }
    return NULL;
}

/* One pass over every domain, done by the pool. Returns when all are read. */
static void tick_read(void)
{
    pthread_mutex_lock(&q_mtx);
    q_next = 0;
    q_active = q_nthreads;
    q_gen++;
    pthread_cond_broadcast(&q_go);
    while (q_active > 0) pthread_cond_wait(&q_done, &q_mtx);
    pthread_mutex_unlock(&q_mtx);
}

/* --------------------------------------------------------------------- main */

static void usage(const char *p)
{
    fprintf(stderr,
"his_monitor - live field monitor for a running Nektar1D simulation\n"
"\n"
"usage: %s <base> [options]\n"
"  <base>               simulation base name, e.g. sim_1. Reads sim_1.in and\n"
"                       sim_1_1.his .. sim_1_<Ndomains>.his in the current dir\n"
"\n"
"  --names <file>       116_artery_model.txt, to label the cells\n"
"  --from-start         read the .his files from the beginning instead of\n"
"                       following only what is appended from now on\n"
"  --proximal           watch the proximal history point, not the distal one\n"
"  --period <ms>        display period, default 250, floor 20\n"
"  --threads <n>        reader threads, default min(8, cores), clamped to 1..64\n"
"  --pwv-target <m/s>   expected carotid-femoral PWV for this subject\n"
"  --tol <pct>          how far cf-PWV may drift before it is a failure (10)\n"
"  --umax <m/s>         peak |U| above which the run is called off target\n"
"                       (default 2.0; 0 disables the check)\n"
"  --stall <s>          seconds without new samples before calling it stalled (20)\n"
"  --abort-on-fail      exit 2 once the verdict has been OFF TARGET or STALLED\n"
"                       for three display periods in a row, so a launch script\n"
"                       can kill the run and move on. Only ever aborts a run\n"
"                       that has been seen to advance: a solver still doing\n"
"                       mesh setup is STALLED on screen but is not killed\n"
"  --bench              time a full from-start read: one warm-up pass, then\n"
"                       1 thread, N threads, and 1 thread again to show drift.\n"
"                       Prints the four times and exits\n", p);
}

/* Read every domain once, serially or with the pool, and time it. */
static double bench_pass(int threads)
{
    char path[512];
    double t0;
    int i;

    for (i = 0; i < g_ctx.ndom; i++) {
        if (g_dom[i].fp) fclose(g_dom[i].fp);
        snprintf(path, sizeof path, "%s_%d.his", g_base, i + 1);
        g_dom[i].fp = fopen(path, "r");
        g_dom[i].hdr = 0; g_dom[i].nsamp = 0; g_dom[i].bytes = 0;
        g_dom[i].st = 0; g_dom[i].cycle = 0; g_dom[i].amp = 0;
        g_dom[i].curmin = 1e9; g_dom[i].foot = g_dom[i].prevfoot = 0;
        g_dom[i].sbp = g_dom[i].dbp = g_dom[i].psbp = 0;
        g_dom[i].umax = 0; g_dom[i].nonfinite = 0;
        g_dom[i].ioerr = 0; g_dom[i].resume = 0;
    }
    t0 = mono();
    if (threads <= 1) {
        for (i = 0; i < g_ctx.ndom; i++)
            if (his_read_new(&g_dom[i], on_sample) > 0) g_dom[i].last_rx = mono();
    } else {
        tick_read();
    }
    return mono() - t0;
}

int main(int argc, char **argv)
{
    pthread_t th[64];
    char inpath[512];
    const char *names = NULL;
    int i, proximal = 0, period_ms = 250, abort_on_fail = 0, bench = 0;
    int nth = (int)sysconf(_SC_NPROCESSORS_ONLN);
    /* umax_lim is measured, not assumed. Across all 116 domains and every
     * history point of the validated 25-year-old reference run, the largest
     * |U| is 1.047 m/s (domain 61) and nothing exceeds 1.5. The failure this
     * is here to catch scales the radii without scaling the terminal
     * resistances, which pushes the same flow through 42-51% of the area and
     * so roughly doubles every velocity. 2.0 m/s sits above the healthy peak
     * with 1.9x of room and below where that failure lands. */
    double target = 0, tol = 10.0, stall = 20.0, umax_lim = 2.0;
    double t_start, next, worst_jitter = 0, tick_max = 0;
    double t_anchor_sim = -1, t_anchor_wall = 0;
    long bad_streak = 0;

    if (argc < 2) { usage(argv[0]); return 1; }
    snprintf(g_base, sizeof g_base, "%s", argv[1]);
    if (nth > 8) nth = 8;
    if (nth < 1) nth = 1;

    for (i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--names")       && i+1 < argc) names     = argv[++i];
        else if (!strcmp(argv[i], "--from-start"))                g_from_start = 1;
        else if (!strcmp(argv[i], "--proximal"))                  proximal  = 1;
        else if (!strcmp(argv[i], "--period")      && i+1 < argc) period_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads")     && i+1 < argc) nth       = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pwv-target")  && i+1 < argc) target    = atof(argv[++i]);
        else if (!strcmp(argv[i], "--tol")         && i+1 < argc) tol       = atof(argv[++i]);
        else if (!strcmp(argv[i], "--umax")        && i+1 < argc) umax_lim  = atof(argv[++i]);
        else if (!strcmp(argv[i], "--stall")       && i+1 < argc) stall     = atof(argv[++i]);
        else if (!strcmp(argv[i], "--abort-on-fail"))             abort_on_fail = 1;
        else if (!strcmp(argv[i], "--bench"))                     bench     = 1;
        else { usage(argv[0]); return 1; }
    }
    if (nth < 1)  nth = 1;
    if (nth > 64) nth = 64;
    if (period_ms < 20) period_ms = 20;

    g_ctx.dom = g_dom;
    snprintf(inpath, sizeof inpath, "%s.in", g_base);
    g_ctx.ndom = in_mesh(inpath, g_dom, MAXDOM);
    if (!g_ctx.ndom) {
        fprintf(stderr, "his_monitor: cannot read domains from %s\n", inpath);
        return 1;
    }
    g_ctx.dt_step = 1e-5; g_ctx.hisstep = 100; g_ctx.nsteps = 0;
    in_param(inpath, "DT",      &g_ctx.dt_step);
    in_param(inpath, "HISSTEP", &g_ctx.hisstep);
    in_param(inpath, "NSTEPS",  &g_ctx.nsteps);
    g_ctx.tfinal = g_ctx.dt_step * g_ctx.nsteps;
    if (names) load_names(names, g_dom, g_ctx.ndom);
    if (proximal) for (i = 0; i < g_ctx.ndom; i++) g_dom[i].watch = 0;
    for (i = 0; i < g_ctx.ndom; i++) g_dom[i].curmin = 1e9;

    /* tick_read() waits for exactly q_nthreads workers to report in. If fewer
     * threads than requested actually started, that count is never reached and
     * the main thread blocks in pthread_cond_wait forever -- where SIGINT does
     * not reach it, so the only way out is SIGKILL. Count what really started.
     * RLIMIT_NPROC on a shared cluster makes this a real case, not a hypothetical. */
    {
        int made = 0;
        for (i = 0; i < nth; i++) {
            if (pthread_create(&th[made], NULL, reader, NULL) != 0) break;
            made++;
        }
        if (made == 0) {
            fprintf(stderr, "his_monitor: cannot create reader threads\n");
            return 1;
        }
        if (made < nth)
            fprintf(stderr, "his_monitor: only %d of %d reader threads started\n",
                    made, nth);
        nth = made;
    }
    q_nthreads = nth;
    g_ctx.nthreads = nth;

    if (bench) {
        double warm, a1, b, a2, mb = 0;
        int saved = g_from_start;
        g_from_start = 1;
        /* One discarded pass first, so both timed passes see the same page
         * cache. Without it the single-threaded run pays to warm the cache and
         * then hands a hot one to the threaded run, which flatters the speedup.
         * The second single-threaded pass at the end says whether anything
         * drifted between the two measurements. */
        warm = bench_pass(nth);
        a1   = bench_pass(1);
        b    = bench_pass(nth);
        a2   = bench_pass(1);
        g_from_start = saved;
        for (i = 0; i < g_ctx.ndom; i++) mb += g_dom[i].bytes;
        mb /= 1024.0 * 1024.0;
        char lab[32];
        printf("full from-start read of %d domains, %.1f MB\n", g_ctx.ndom, mb);
        snprintf(lab, sizeof lab, "warm-up (%d thr)", nth);
        printf("  %-15s : %6.3f s   (discarded)\n", lab, warm);
        printf("  %-15s : %6.3f s   %6.1f MB/s\n", "1 thread",
               a1, mb / (a1 > 0 ? a1 : 1));
        snprintf(lab, sizeof lab, "%d threads", nth);
        printf("  %-15s : %6.3f s   %6.1f MB/s   speedup x%.2f\n", lab,
               b, mb / (b > 0 ? b : 1), a1 / (b > 0 ? b : 1));
        printf("  %-15s : %6.3f s   (drift %+.1f%%)\n", "1 thread again",
               a2, 100.0 * (a2 - a1) / (a1 > 0 ? a1 : 1));
        pthread_mutex_lock(&q_mtx); q_quit = 1; q_gen++;
        pthread_cond_broadcast(&q_go); pthread_mutex_unlock(&q_mtx);
        for (i = 0; i < nth; i++) pthread_join(th[i], NULL);
        for (i = 0; i < g_ctx.ndom; i++)
            if (g_dom[i].fp) { fclose(g_dom[i].fp); g_dom[i].fp = NULL; }
        return 0;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    /* Every normal exit shows the cursor again, but a path that does not --
     * an assertion, a fatal signal we do not handle -- would leave the user's
     * shell with no cursor until they ran `reset`. One line to make that
     * impossible. */
    atexit(show_cursor);
    printf("\x1b[?25l\x1b[2J");                    /* hide cursor, clear once */

    t_start = mono();
    next = t_start;
    while (!g_stop) {
        struct timespec ts;
        double t0, t1, late, mb = 0;

        t0 = mono();
        tick_read();
        for (i = 0; i < g_ctx.ndom; i++) mb += g_dom[i].bytes;
        g_ctx.mb_read = mb / (1024.0 * 1024.0);
        g_ctx.wall = t0 - t_start;

        /* Anchor the progress rate on the first sample seen rather than on
         * program start, so the eta is right when attaching to a run that is
         * already going. Until a second sample arrives there is no rate and
         * render() shows none. */
        {
            double tnow = 0;
            for (i = 0; i < g_ctx.ndom; i++)
                if (g_dom[i].nsamp && g_dom[i].t > tnow) tnow = g_dom[i].t;
            if (tnow > 0) {
                /* mono() here, not t0: t0 was taken before tick_read(),
                 * and the first tick in --from-start mode spends tens of
                 * seconds sweeping 147 MB. Anchoring on t0 charges that
                 * sweep to the elapsed wall time and understates the rate
                 * for the rest of the run. */
                if (t_anchor_sim < 0) { t_anchor_sim = tnow; t_anchor_wall = mono(); }
                g_ctx.prog_dsim  = tnow - t_anchor_sim;
                g_ctx.prog_dwall = t0 - t_anchor_wall;
            }
        }

        wave_verdict(&g_ctx, target, tol, stall, umax_lim, t0);
        render(&g_ctx, target);
        t1 = mono();

        g_ctx.tick_ms = (t1 - t0) * 1000.0;
        if (g_ctx.tick_ms > tick_max) tick_max = g_ctx.tick_ms;
        g_ctx.tick_max_ms  = tick_max;
        g_ctx.tick_sum_ms += g_ctx.tick_ms;
        g_ctx.ticks++;

        /* Never abort on a run that has not produced anything yet. The verdict
         * says STALLED after --stall seconds with no samples, which is right
         * for the display but wrong as grounds to kill a process: a solver
         * started alongside the monitor spends its first seconds on mesh setup
         * and stdio buffering, and would be killed for being slow to start.
         * Aborting requires having seen the run work at least once. */
        if (abort_on_fail && g_ctx.prog_dsim > 0 &&
            (g_ctx.verdict == VERDICT_OFFTARGET || g_ctx.verdict == VERDICT_STALLED)) {
            if (++bad_streak >= 3) {              /* three periods in a row */
                printf("\x1b[?25h\n");
                fprintf(stderr, "his_monitor: %s\n", g_ctx.note);
                pthread_mutex_lock(&q_mtx); q_quit = 1; q_gen++;
                pthread_cond_broadcast(&q_go); pthread_mutex_unlock(&q_mtx);
                for (i = 0; i < nth; i++) pthread_join(th[i], NULL);
                return 2;
            }
        } else bad_streak = 0;

        /* Absolute-deadline sleep: the next period starts when it was always
         * going to start, not one period after this one happened to finish. */
        next += period_ms / 1000.0;
        late = mono() - next;
        if (late > 0) {                            /* the tick overran */
            g_ctx.misses++;
            if (late * 1000.0 > worst_jitter) worst_jitter = late * 1000.0;
            g_ctx.jitter_ms = worst_jitter;
            /* Re-phase, but still sleep. Returning straight to the top means
             * that a tick which consistently overruns -- a small --period
             * during the first from-start sweep, say -- spins at 100% and
             * competes with the solver it is supposed to be watching. */
            next = mono() + period_ms / 4000.0;
        }
        ts.tv_sec  = (time_t)next;
        ts.tv_nsec = (long)((next - (double)ts.tv_sec) * 1e9);
        /* A signal cuts the sleep short and returns EINTR. Because the deadline
         * is absolute, resuming is just calling again with the same ts -- no
         * remaining-time arithmetic, and no drift. Without the retry, one
         * SIGWINCH from resizing the window would silently shorten a period. */
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR
               && !g_stop)
            ;
        {
            double err = fabs(mono() - next) * 1000.0;
            if (err > worst_jitter) worst_jitter = err;
            g_ctx.jitter_ms = worst_jitter;
        }
    }

    printf("\x1b[?25h\n");
    pthread_mutex_lock(&q_mtx); q_quit = 1; q_gen++;
    pthread_cond_broadcast(&q_go); pthread_mutex_unlock(&q_mtx);
    for (i = 0; i < nth; i++) pthread_join(th[i], NULL);
    for (i = 0; i < g_ctx.ndom; i++) if (g_dom[i].fp) fclose(g_dom[i].fp);
    return g_ctx.verdict == VERDICT_OFFTARGET || g_ctx.verdict == VERDICT_STALLED ? 2 : 0;
}
