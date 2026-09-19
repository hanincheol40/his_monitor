/*
 * render.c -- the terminal display.
 *
 * ANSI truecolour, cursor addressing, no full-screen clear between frames so
 * nothing flickers. Works over ssh with no X11, which is the only place this
 * ever runs.
 */
/* ioctl()/TIOCGWINSZ are POSIX/BSD, not ISO C -- same reason as hisfile.c. */
#define _POSIX_C_SOURCE 200809L
#include "render.h"
#include "wave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define CELLW 31

static const char *RNAME[4] = {"AORTA / VISCERAL", "HEAD / NECK",
                               "ARM / HAND", "PELVIS / LEG"};

/* Classic rainbow (jet) -- the map 1-D solver post-processors conventionally
 * use, so this picture matches the figures people already know. */
void jet(double u, int *r, int *g, int *b)
{
    double x;
    /* Written as !(u >= 0) so that a nan lands on 0 too. `u < 0` is false for
     * nan, and (int)(255*nan) is INT_MIN, which would emit an escape sequence
     * with negative colour components. */
    if (!(u >= 0)) u = 0;
    if (u > 1) u = 1;
    x = 1.5 - fabs(4*u - 3); *r = (int)(255 * (x < 0 ? 0 : x > 1 ? 1 : x));
    x = 1.5 - fabs(4*u - 2); *g = (int)(255 * (x < 0 ? 0 : x > 1 ? 1 : x));
    x = 1.5 - fabs(4*u - 1); *b = (int)(255 * (x < 0 ? 0 : x > 1 ? 1 : x));
}

/* Window size, in order of authority: the real terminal, then COLUMNS/LINES,
 * then a default.
 *
 * The environment fallback is not decoration. Piped into a file or a capture
 * script there is no terminal to ask, ioctl fails, and the tool would draw for
 * a window it has invented -- which is how the first screenshots ended up
 * silently missing a third of the field. Honouring COLUMNS/LINES is also what
 * makes the layout testable without a tty. */
void term_size(int *cols, int *rows)
{
    struct winsize ws;
    const char *ec, *er;
    *cols = 100; *rows = 40;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20 &&
        ws.ws_row > 10) {                  /* some pty wrappers report row 0 */
        *cols = ws.ws_col; *rows = ws.ws_row;
        return;
    }
    if ((ec = getenv("COLUMNS")) && atoi(ec) > 20) *cols = atoi(ec);
    if ((er = getenv("LINES"))   && atoi(er) > 10) *rows = atoi(er);
}

static void bar(double frac, int w)
{
    int i, f = (int)(frac * w + 0.5);
    for (i = 0; i < w; i++) putchar(i < f ? '#' : '.');
}

#define HEADROWS 6                      /* title..colour bar, plus one blank */

/* How tall the frame would be with these settings.
 *
 * This matters because the frame is redrawn by homing the cursor rather than
 * by clearing the screen. If it is taller than the window, the terminal
 * scrolls, home is no longer the top of the frame, and every later frame lands
 * one line lower -- on screen it looks like the tool is corrupting itself. So
 * the height is computed first and the layout is compacted until it fits.
 */
static int frame_rows(const Ctx *c, int per, int gap)
{
    int reg, i, n, r = HEADROWS;
    for (reg = 0; reg < 4; reg++) {
        for (n = 0, i = 0; i < c->ndom; i++) if (c->dom[i].region == reg) n++;
        if (!n) continue;
        r += 1 + (n + per - 1) / per + gap;
    }
    return r + 1;                       /* the "Ctrl-C to quit" line */
}

static void put_cell(const Dom *d, const Dom *root, double lo, double hi,
                     double dpmax)
{
    char lab[160];
    const char *nm = d->shrt[0] ? d->shrt : (d->name[0] ? d->name : "");
    double u = (hi > lo) ? (d->P - lo) / (hi - lo) : 0.5;
    int r, g, b, dark, k;
    int ms = (int)wave_arrival_ms(d, root);
    int front = wave_front(d, dpmax);     /* same rule as --stream: wave.c */

    jet(u, &r, &g, &b);
    dark = (0.299*r + 0.587*g + 0.114*b) > 150;
    if (!d->nsamp) { r = g = b = 55; dark = 0; }

    snprintf(lab, sizeof lab, "%03d %-15.15s%4.0f %c%3d", d->id, nm, d->P,
             front ? '>' : ' ', ms);
    printf("\x1b[48;2;%d;%d;%dm\x1b[38;2;%d;%d;%dm", r, g, b,
           dark ? 12 : 246, dark ? 22 : 250, dark ? 30 : 252);
    for (k = 0; k < CELLW - 1 && lab[k]; k++) putchar(lab[k]);
    for (; k < CELLW - 1; k++) putchar(' ');
    printf("\x1b[0m ");
}

void render(const Ctx *c, double target_pwv)
{
    static const char *VCOL[] = { "\x1b[37m", "\x1b[37m", "\x1b[33m",
                                  "\x1b[1;32m", "\x1b[1;31m", "\x1b[1;31m" };
    const Dom *root = &c->dom[0];
    double lo = 1e9, hi = -1e9, dpmax, tnow, pwv, eta;
    double dsbp = wave_dsbp(root);
    int cols, rows, i, per, reg, gap = 1, used, budget, hidden = 0;

    term_size(&cols, &rows);
    per = cols / CELLW;
    if (per < 1) per = 1;

    /* Drop the blank line between regions first -- it costs four lines and no
     * information. Only if that is still not enough do we start hiding cells,
     * and then we say how many. */
    if (frame_rows(c, per, gap) > rows) gap = 0;
    budget = rows - 2;                  /* the quit line, and room for a notice */
    used   = HEADROWS;

    for (i = 0; i < c->ndom; i++) {
        const Dom *d = &c->dom[i];
        if (!d->nsamp || !isfinite(d->P)) continue;   /* one inf would make
                                                         every cell's u nan */
        if (d->P < lo)     lo = d->P;
        if (d->P > hi)     hi = d->P;
    }
    if (hi <= lo) { lo = 60; hi = 140; }
    lo -= 2; hi += 2;
    dpmax = wave_dpmax(c->dom, c->ndom);
    tnow  = wave_tnow(c->dom, c->ndom);
    pwv   = c->pwv;                      /* main.c fills this via wave.c */
    eta   = wave_eta(c, tnow);           /* see wave.c for why not tnow/wall */

    printf("\x1b[H");
    printf("\x1b[1m  Nektar1D live field\x1b[0m   sim %7.3f / %.3f s  [",
           tnow, c->tfinal);
    bar(c->tfinal > 0 ? tnow / c->tfinal : 0, 30);
    printf("]  wall %4.0fs  eta %4.0fs\x1b[K\n", c->wall, eta);

    printf("  %s%-11s\x1b[0m %-58.58s\x1b[K\n",
           VCOL[c->verdict], wave_verdict_name(c->verdict), c->note);

    {
        char ds[16];                    /* "--" until two beats have settled */
        if (isfinite(dsbp)) snprintf(ds, sizeof ds, "%+6.2f", dsbp);
        else                snprintf(ds, sizeof ds, "%6s", "--");
        printf("  cycle %-3d  aortic root \x1b[1m%5.1f / %-5.1f\x1b[0m mmHg   "
               "PP %4.1f   dSBP %s   cf-PWV %5.2f m/s",
               root->cycle, root->sbp, root->dbp, root->sbp - root->dbp, ds, pwv);
    }
    if (target_pwv > 0) printf("   target %.2f", target_pwv);
    printf("\x1b[K\n");

    /* mean is reassuring, max is the one that decides whether a period is
     * missed, so both are shown. */
    printf("  %d domains  %d reader threads  %.1f MB read  peak |U| %.2f m/s"
           "  tick now/mean/max %.1f/%.1f/%.1f ms  jitter %.1f ms  "
           "%ld/%ld late",
           c->ndom, c->nthreads, c->mb_read, c->umax, c->tick_ms,
           c->ticks ? c->tick_sum_ms / (double)c->ticks : 0.0, c->tick_max_ms,
           c->jitter_ms, c->misses, c->ticks);
    /* A read error is the monitor's problem, not the run's, so it is shown
     * here with the other health numbers and deliberately does not change the
     * verdict -- --abort-on-fail must never kill a good run over a hiccup on
     * the file server. */
    if (c->ioerr_dom)
        printf("  \x1b[1;33m%d domain(s) had read errors\x1b[0m", c->ioerr_dom);
    printf("\x1b[K\n");

    printf("  %.0f ", lo);
    for (i = 0; i < 34; i++) {
        int r, g, b; jet(i / 33.0, &r, &g, &b);
        printf("\x1b[48;2;%d;%d;%dm \x1b[0m", r, g, b);
    }
    printf(" %.0f mmHg     cell = id  name  P[mmHg]  > wave front  arrival[ms]"
           "\x1b[K\n\x1b[K\n", hi);

    for (reg = 0; reg < 4; reg++) {
        int ntot = 0, drawn = 0;
        for (i = 0; i < c->ndom; i++) if (c->dom[i].region == reg) ntot++;
        if (!ntot) continue;
        /* heading + at least one row of cells + the closing newline */
        if (used + 2 + gap > budget) { hidden += ntot; continue; }
        printf("  \x1b[38;2;150;170;185m%s\x1b[0m\x1b[K\n", RNAME[reg]);
        used++;
        for (i = 0; i < c->ndom; i++) {
            if (c->dom[i].region != reg) continue;
            if (drawn && drawn % per == 0) {
                printf("\x1b[K\n");
                used++;
                if (used + 1 + gap > budget) break;   /* no room for one more */
            }
            put_cell(&c->dom[i], root, lo, hi, dpmax);
            drawn++;
        }
        hidden += ntot - drawn;
        printf("\x1b[K\n");
        used++;
        if (gap) { printf("\x1b[K\n"); used++; }
    }
    if (hidden)
        printf("  \x1b[1;33m%d cell(s) not shown - window is %d lines, "
               "the field needs %d\x1b[0m\x1b[K\n",
               hidden, rows, frame_rows(c, per, 1));
    printf("  Ctrl-C to quit\x1b[K\n\x1b[J");
    fflush(stdout);
}
