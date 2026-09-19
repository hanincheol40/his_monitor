/*
 * monitor.h -- shared types for his_monitor.
 *
 * Layering:
 *   hisfile.[ch]  the file formats  (.in geometry / .his time series)
 *   wave.[ch]     the signal analysis (foot detection, PWV, convergence)
 *   render.[ch]   the terminal display
 *   stream.[ch]   the same display as JSON lines, for a GUI (--stream)
 *   main.c        threading, periodic scheduling, watchdog
 */
#ifndef MONITOR_H
#define MONITOR_H

#include <stdio.h>
#include <pthread.h>

#define MAXDOM    256
#define MAXPTS     16
#define MAXCOL     24
#define PA2MMHG (1.0/133.322)     /* .his pressure is in Pa */
#define HIST_N    400             /* --stream waveform history: 2 s at 200 Hz */
#define SBP_LOG    64             /* systolic pressure of each completed beat  */

/* One arterial segment: its geometry, its open .his file, and its state. */
typedef struct {
    int    id;                    /* domain number, 1-based == segment number */
    char   name[72];              /* artery name, when a name table is given  */
    char   shrt[20];              /* abbreviated to fit a cell                */
    int    region;                /* 0 aorta, 1 head, 2 arm, 3 leg            */
    double len;                   /* domain length [m], from the .in mesh     */

    FILE  *fp;
    long   bytes;                 /* consumed so far, for the throughput stat */
    int    hdr;                   /* header parsed yet                        */
    int    ncol;                  /* numeric columns per row                  */
    int    cP, cU, cA, cPt;       /* column indices                           */

    int    npts;                  /* history points declared in the .in       */
    int    watch;                 /* which point we display, default distal   */

    /* latest sample */
    double t, P, U, A;            /* P in mmHg, U in m/s, A in m^2            */
    double prevP, prevT, dPdt;
    double umax;                  /* peak |U| seen: see the note in wave.c    */
    long   nsamp;
    long   nonfinite;             /* rows carrying nan/inf: the solver blew up */
    long   ioerr;                 /* read errors, as opposed to end-of-file   */
    long   resume;                /* offset to reopen at after a read error   */
    int    holes;                 /* consecutive zero-length reads: see hisfile.c */
    double last_rx;               /* monotonic clock at the last new sample   */

    /* per-cycle wave analysis (see wave.c) */
    int    st;                    /* 0 hunting for the foot, 1 past it        */
    double curmin, curmint;
    double foot, prevfoot;
    double sbp, dbp;
    double psbp;                  /* previous cycle's systolic, for dSBP      */
    double amp;
    int    cycle;

    /* Systolic pressure of every completed beat, in order. dSBP on screen is
     * only the latest difference; a display that wants to draw convergence
     * needs the whole series, and in --from-start mode the first tick
     * swallows every beat at once, so it cannot be rebuilt from ticks. */
    double sbp_log[SBP_LOG];
    int    nsbp;

    /* Decimated pressure history for --stream, so another process can draw
     * the waveform. Written only by the reader thread that holds this domain
     * during a tick and read only by the main thread after the tick barrier;
     * the barrier's mutex orders the two, so it needs no lock of its own. */
    float  hist_t[HIST_N], hist_p[HIST_N];
    int    hist_head;             /* next slot to write                       */
    long   hist_total;            /* samples ever stored                      */
    long   hist_sent;             /* hist_total when the last line went out   */
    double hist_last;             /* t of the last stored sample              */
} Dom;

/* Everything the render layer needs that is not per-domain. */
typedef struct {
    Dom   *dom;
    int    ndom;
    double dt_step, hisstep, nsteps, tfinal;

    /* scheduling health, filled in by main.c */
    double wall;                  /* seconds since this monitor started       */
    /* Simulated time and wall time elapsed since the first sample arrived.
     * The eta is computed from these two, so that attaching to a run that is
     * already under way does not report the whole backlog as progress made in
     * the second the monitor has been alive. */
    double prog_dsim, prog_dwall;
    double tick_ms;               /* work time of the last tick               */
    double tick_max_ms;           /* worst ever - the number that matters     */
    double tick_sum_ms;           /* running total, for the mean              */
    double jitter_ms;             /* worst |actual - scheduled| so far        */
    long   ticks, misses;         /* periods run / periods that overran       */
    int    nthreads;
    double mb_read;

    /* verdict, filled in by wave.c through main.c */
    int    verdict;               /* see VERDICT_* below                      */
    double pwv;                   /* cf-PWV [m/s], 0 until two cycles are seen */
    double umax;                  /* worst peak |U| over all domains [m/s]    */
    int    umax_dom;              /* which domain that was                    */
    int    ioerr_dom;             /* domains that hit a read error            */
    int    silent;                /* domains individually quiet: see wave.c   */
    int    pwv_cycle;             /* the beat pwv was last computed on        */
    int    done;                  /* every domain has reached tfinal          */
    char   note[128];
} Ctx;

enum { VERDICT_WAIT = 0, VERDICT_FILLING, VERDICT_CONVERGING,
       VERDICT_CONVERGED, VERDICT_OFFTARGET, VERDICT_STALLED };

#endif /* MONITOR_H */
