/*
 * test_hisfile.c -- regression tests for the file-format layer.
 *
 * Every case here is a bug that actually happened, not a hypothetical. The
 * parsing layer is the part with no visible symptoms when it is wrong: a
 * mis-split header does not crash, it silently reports zeros, and a domain
 * that loses a race just stays blank. Those are the failures worth pinning
 * down in a test, because the screen looks plausible either way.
 *
 * Build and run:  make test
 */
#define _POSIX_C_SOURCE 200809L
#include "hisfile.h"
#include "wave.h"
#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails, checks;

#define CHECK(cond, ...) do {                                               \
    checks++;                                                               \
    if (!(cond)) {                                                          \
        fails++;                                                            \
        printf("  FAIL %s:%d  ", __FILE__, __LINE__);                       \
        printf(__VA_ARGS__);                                                \
        printf("\n");                                                       \
    }                                                                       \
} while (0)

static const char *TMP = "/tmp/his_monitor_test";

static void write_file(const char *name, const char *text)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", TMP, name);
    f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static FILE *open_tmp(const char *name, const char *mode)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", TMP, name);
    return fopen(path, mode);
}

/* ------------------------------------------------------------------------ */

/* The header names carry argument lists -- "P(x,t)" -- and the fields are
 * comma separated. Splitting on commas without removing the parenthesised
 * parts turns 7 columns into 12 and puts every index off by the number of
 * parentheses seen so far. On screen that shows up as every value being 0. */
static void test_header_with_parens(void)
{
    Dom d;
    char line[] = "# t, P(x,t), Pe(x,t), U(x,t), Q(x,t), A(x,t), # point\n";
    memset(&d, 0, sizeof d);
    parse_header(&d, line);
    CHECK(d.hdr,        "header should have parsed");
    CHECK(d.ncol == 7,  "ncol %d, want 7", d.ncol);
    CHECK(d.cP  == 1,   "P at %d, want 1", d.cP);
    CHECK(d.cU  == 3,   "U at %d, want 3", d.cU);
    CHECK(d.cA  == 5,   "A at %d, want 5", d.cA);
    CHECK(d.cPt == 6,   "point at %d, want 6", d.cPt);
}

/* The column set depends on the run's options, so the indices must come from
 * the file. This is the same tree with Pe and Q switched off. */
static void test_header_alternate_columns(void)
{
    Dom d;
    char line[] = "# t, P(x,t), U(x,t), A(x,t), # point\n";
    memset(&d, 0, sizeof d);
    parse_header(&d, line);
    CHECK(d.hdr,       "header should have parsed");
    CHECK(d.cP == 1 && d.cU == 2 && d.cA == 3,
          "P/U/A at %d/%d/%d, want 1/2/3", d.cP, d.cU, d.cA);
}

/* A header wider than the value array must be refused, not indexed past. */
static void test_header_too_wide(void)
{
    Dom  d;
    char line[1024];
    int  i;
    strcpy(line, "# t");
    for (i = 0; i < MAXCOL + 4; i++) strcat(line, ", junk(x,t)");
    strcat(line, ", P(x,t), U(x,t), A(x,t)\n");
    memset(&d, 0, sizeof d);
    parse_header(&d, line);
    CHECK(!d.hdr, "a header with %d columns must be refused, got hdr=%d",
          d.ncol, d.hdr);
}

/* A line that does not start with "# t" is a different comment. */
static void test_header_ignores_other_comments(void)
{
    Dom d;
    char line[] = "# History points: 2\n";
    memset(&d, 0, sizeof d);
    parse_header(&d, line);
    CHECK(!d.hdr, "non-header comment must not set hdr");
}

/* ------------------------------------------------------------------------ */

static void setup_dom(Dom *d, const char *file)
{
    memset(d, 0, sizeof *d);
    d->id = 1;
    d->watch = 0;
    d->curmin = 1e9;
    d->fp = open_tmp(file, "r");
}

/* The solver appends while this reads, so the last line is routinely half
 * written. Consuming it would produce a truncated row that parses as perfectly
 * good numbers. The position must be restored and the row picked up whole on
 * the next poll. */
static void test_partial_last_line(void)
{
    Dom d;
    int n1, n2;
    write_file("part.his",
        "# t, P(x,t), U(x,t), A(x,t), # point\n"
        "0.001 10000 0.10 0.0003 1\n"
        "0.002 10500 0.11 0.0003 1\n"
        "0.003 11000 0.12");                     /* no newline: still being written */
    setup_dom(&d, "part.his");
    CHECK(d.fp != NULL, "could not open part.his");
    n1 = his_read_new(&d, NULL);
    CHECK(n1 == 2, "read %d complete rows, want 2", n1);
    CHECK(fabs(d.t - 0.002) < 1e-9, "t %.6f, want 0.002", d.t);

    /* now the solver finishes that line and adds another */
    {
        FILE *a = open_tmp("part.his", "a");
        fputs(" 0.0003 1\n0.004 11500 0.13 0.0003 1\n", a);
        fclose(a);
    }
    n2 = his_read_new(&d, NULL);
    CHECK(n2 == 2, "read %d rows after append, want 2", n2);
    CHECK(fabs(d.t - 0.004) < 1e-9, "t %.6f, want 0.004", d.t);
    /* The row that was half written must arrive exactly once, and whole. */
    CHECK(d.nsamp == 4, "nsamp %ld, want 4", d.nsamp);
    fclose(d.fp);
}

/* clearerr() has to run after fgets hits EOF, or the stream latches it and
 * every later poll returns nothing even though the file has grown. */
static void test_eof_then_growth(void)
{
    Dom d;
    write_file("grow.his",
        "# t, P(x,t), U(x,t), A(x,t), # point\n"
        "0.001 10000 0.10 0.0003 1\n");
    setup_dom(&d, "grow.his");
    CHECK(his_read_new(&d, NULL) == 1, "first poll should read 1 row");
    CHECK(his_read_new(&d, NULL) == 0, "second poll has nothing to read");
    {
        FILE *a = open_tmp("grow.his", "a");
        fputs("0.002 10500 0.11 0.0003 1\n", a);
        fclose(a);
    }
    CHECK(his_read_new(&d, NULL) == 1,
          "after growth the stream must not still be at EOF");
    fclose(d.fp);
}

/* A diverging solver keeps writing rows, but writes nan. Every comparison
 * against nan is false, so letting one into the state freezes the foot
 * detector and pins dSBP at exactly 0.00 -- which the verdict would read as
 * CONVERGED. They must be counted and dropped. */
static void test_nonfinite_rows_are_rejected(void)
{
    Dom d;
    write_file("nan.his",
        "# t, P(x,t), U(x,t), A(x,t), # point\n"
        "0.001 10000 0.10 0.0003 1\n"
        "0.002 nan 0.11 0.0003 1\n"
        "0.003 inf 0.12 0.0003 1\n"
        "0.004 10500 nan 0.0003 1\n"
        "0.005 11000 0.13 0.0003 1\n");
    setup_dom(&d, "nan.his");
    his_read_new(&d, NULL);
    CHECK(d.nsamp == 2,     "nsamp %ld, want 2 good rows", d.nsamp);
    CHECK(d.nonfinite == 3, "nonfinite %ld, want 3", d.nonfinite);
    CHECK(isfinite(d.P),    "P must never be non-finite, got %f", d.P);
    fclose(d.fp);
}

/* Several history points share one file, one row each. Only the watched point
 * belongs to this cell -- and the point column is not always last, which is
 * why the header is parsed rather than assumed. */
static void test_history_point_filter(void)
{
    Dom d;
    write_file("pts.his",
        "# History points: 3\n"
        "# t, P(x,t), U(x,t), A(x,t), # point\n"
        "0.001 10000 0.10 0.0003 1\n"
        "0.001 11000 0.11 0.0003 2\n"
        "0.001 12000 0.12 0.0003 3\n");
    setup_dom(&d, "pts.his");
    d.watch = 2;                                  /* zero-based: point 3 */
    his_read_new(&d, NULL);
    CHECK(d.nsamp == 1, "nsamp %ld, want 1 (only the watched point)", d.nsamp);
    CHECK(fabs(d.P - 12000.0 * PA2MMHG) < 1e-6,
          "P %.3f mmHg, want the point-3 row", d.P);
    fclose(d.fp);
}

/* The .in says how many history points were asked for; the .his says how many
 * the solver could actually place. Trusting the .in leaves the tool watching a
 * point number that is never written, and the domain reads as permanently
 * blank. */
static void test_his_header_overrides_point_count(void)
{
    Dom d;
    write_file("fewer.his",
        "# History points: 2\n"
        "# t, P(x,t), U(x,t), A(x,t), # point\n"
        "0.001 10000 0.10 0.0003 1\n"
        "0.001 11000 0.11 0.0003 2\n");
    setup_dom(&d, "fewer.his");
    d.npts  = 4;                                  /* what the .in claimed */
    d.watch = 3;                                  /* the distal one, per the .in */
    his_read_new(&d, NULL);
    CHECK(d.npts == 2,  "npts %d, want 2 from the .his header", d.npts);
    CHECK(d.watch == 1, "watch %d, want it clamped to the last real point",
          d.watch);
    CHECK(d.nsamp == 1, "nsamp %ld, want 1", d.nsamp);
    fclose(d.fp);
}

/* A line longer than the buffer also comes back without a newline. Treating
 * that as "still being written" rewinds onto the same bytes forever and the
 * domain freezes with no error anywhere. */
static void test_overlong_line_does_not_wedge(void)
{
    Dom  d;
    FILE *f;
    int  i, n;
    f = open_tmp("long.his", "w");
    fputs("# t, P(x,t), U(x,t), A(x,t), # point\n", f);
    for (i = 0; i < 9000; i++) fputc('9', f);     /* > the 8192 read buffer */
    fputs("\n0.002 10500 0.11 0.0003 1\n", f);
    fclose(f);
    setup_dom(&d, "long.his");
    n = his_read_new(&d, NULL);
    CHECK(n == 1, "read %d rows, want 1 (the row after the over-long line)", n);
    CHECK(fabs(d.t - 0.002) < 1e-9, "t %.6f, want 0.002", d.t);
    fclose(d.fp);
}

/* ------------------------------------------------------------------------ */

static void test_abbreviate(void)
{
    char out[20];
    abbreviate("Right Superior Middle Cerebral Artery (M2)", out, sizeof out);
    CHECK(strlen(out) < sizeof out, "must fit the cell, got '%s'", out);
    CHECK(strstr(out, "R.") != NULL, "'Right ' should shorten, got '%s'", out);

    abbreviate("Left Common Carotid Artery", out, sizeof out);
    CHECK(strstr(out, "L.") && strstr(out, "Car"),
          "expected L. and Car in '%s'", out);

    /* Must not write past the buffer even when nothing shortens. */
    abbreviate("Xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", out, sizeof out);
    CHECK(strlen(out) == sizeof out - 1, "want exactly n-1 chars, got %zu",
          strlen(out));
}

/* ------------------------------------------------------------------------ */

/* Foot detection leaves its state on a time lockout rather than on the
 * pressure returning to the diastolic value it just recorded. The first cycle
 * records the t=0 start-up pressure as its diastole and the solution never
 * goes back there, so a level-based exit latches on cycle 1 and every later
 * cycle is missed. That only shows up against real solver output: a synthetic
 * wave that starts at its own diastole never triggers it, which is why this
 * test deliberately starts high and decays. */
static void test_foot_detection_across_cycles(void)
{
    Dom d;
    int i;
    const double period = 0.8, dt = 0.001;
    memset(&d, 0, sizeof d);
    d.curmin = 1e9;
    for (i = 0; i < 6000; i++) {                  /* 6 s = 7.5 cycles */
        double t   = i * dt;
        double ph  = fmod(t, period) / period;
        double dia = 75.0 + 25.0 * exp(-t / 1.5); /* start-up decay */
        d.t = t;
        d.P = dia + 30.0 * exp(-3.0 * ph) * sin(3.14159 * (ph < 0.5 ? ph * 2 : 0));
        wave_sample(&d);
    }
    CHECK(d.cycle >= 6, "found %d cycles in 6 s at 0.8 s each, want >= 6",
          d.cycle);
    CHECK(d.prevfoot > 0 && d.foot > d.prevfoot,
          "consecutive feet must advance, got prev %.3f foot %.3f",
          d.prevfoot, d.foot);
    CHECK(fabs((d.foot - d.prevfoot) - period) < 0.15,
          "foot spacing %.3f s, want about %.2f", d.foot - d.prevfoot, period);
}

/* nan must never enter the wave state: every comparison against it is false,
 * so a single one would pin curmin forever and no foot would be found again. */
static void test_wave_sample_ignores_nan(void)
{
    Dom d;
    memset(&d, 0, sizeof d);
    d.curmin = 1e9;
    d.t = 0.1; d.P = 80.0;  wave_sample(&d);
    d.t = 0.2; d.P = NAN;   wave_sample(&d);
    CHECK(isfinite(d.curmin), "curmin poisoned by nan: %f", d.curmin);
    d.t = 0.3; d.P = 120.0; wave_sample(&d);
    CHECK(d.cycle == 1, "a foot should still be found after a nan, cycle %d",
          d.cycle);
}

/* ------------------------------------------------------------------------ */

/* The two feet must come from the same heartbeat. The wave reaches the carotid
 * about 17 ms after the root and the femoral about 134 ms after, so once per
 * cycle the carotid has the new foot while the femoral still holds the
 * previous one. Subtracting those gives a negative transit time; the value
 * must come from the matching pair instead of collapsing to 0. Reading a
 * finished file to EOF never exposes this -- only a live solver does. */
static void test_cf_pwv_pairs_feet_by_beat(void)
{
    Dom  dom[46];
    double a, b;
    memset(dom, 0, sizeof dom);
    /* lengths only matter through the path sums; give every domain 0.1 m */
    { int i; for (i = 0; i < 46; i++) { dom[i].id = i + 1; dom[i].len = 0.1; } }

    /* aligned: both on cycle 5 */
    dom[14].cycle = 5; dom[14].foot = 4.000; dom[14].prevfoot = 3.200;
    dom[45].cycle = 5; dom[45].foot = 4.100; dom[45].prevfoot = 3.300;
    a = wave_cf_pwv(dom, 46);
    CHECK(a > 0, "aligned feet should give a value, got %.3f", a);

    /* carotid has moved on to cycle 6, femoral has not yet */
    dom[14].cycle = 6; dom[14].prevfoot = 4.000; dom[14].foot = 4.800;
    b = wave_cf_pwv(dom, 46);
    CHECK(b > 0, "straddling feet must not collapse to 0, got %.3f", b);
    CHECK(fabs(a - b) < 1e-6,
          "same beat, so same answer: %.4f vs %.4f", a, b);
}

/* The arrival time shown in each cell has the same trap as cf-PWV: by the time
 * the wave reaches the ankle the root has already recorded the next beat's
 * foot, and the naive difference reads about minus one period. This was
 * visible on a live capture as "-656 ms" at the posterior tibial. */
static void test_arrival_pairs_to_the_same_beat(void)
{
    Dom root, ankle;
    double ms;
    memset(&root,  0, sizeof root);
    memset(&ankle, 0, sizeof ankle);

    /* aligned: both on this beat */
    root.cycle  = 5; root.prevfoot  = 3.200; root.foot  = 4.000;
    ankle.cycle = 5; ankle.prevfoot = 3.364; ankle.foot = 4.164;
    ms = wave_arrival_ms(&ankle, &root);
    CHECK(fabs(ms - 164.0) < 1.0, "arrival %.1f ms, want about 164", ms);

    /* the root has moved on to the next beat, the ankle has not yet */
    root.cycle = 6; root.prevfoot = 4.000; root.foot = 4.820;
    ms = wave_arrival_ms(&ankle, &root);
    CHECK(ms > 0, "must not go negative across a beat boundary, got %.1f", ms);
    CHECK(fabs(ms - 164.0) < 1.0,
          "arrival %.1f ms, want the same 164 as before", ms);

    /* the root itself is the reference and must read 0 */
    ms = wave_arrival_ms(&root, &root);
    CHECK(fabs(ms) < 1e-9, "the root's own arrival must be 0, got %.3f", ms);
}

/* The default mode opens an existing .his and follows what is appended from
 * then on. The header is at the TOP of that file, and without it every data
 * row is thrown away -- so seeking straight to the end makes the tool read the
 * entire run and accept nothing, while still counting bytes.
 *
 * This is the case `make test` could not see: every test above opens at
 * position 0, and every screenshot was captured with --from-start. It is the
 * tool's default path, and it was completely broken. */
static void test_follow_mode_learns_the_header(void)
{
    Dom d;
    write_file("follow.his",
        "# 1D nonlinear hp code outfile\n"
        "# History points: 2\n"
        "# t, P(x,t), U(x,t), A(x,t), # point\n"
        "0.001 10000 0.10 0.0003 1\n"
        "0.002 10500 0.11 0.0003 1\n");

    memset(&d, 0, sizeof d);
    d.id = 1; d.watch = 0; d.curmin = 1e9;
    d.fp = open_tmp("follow.his", "r");
    CHECK(d.fp != NULL, "could not open follow.his");

    his_prime(&d);                       /* what follow mode does on open */
    CHECK(d.hdr, "follow mode must learn the column layout before seeking");
    CHECK(d.npts == 2, "npts %d, want 2 from the .his header", d.npts);
    CHECK(his_read_new(&d, NULL) == 0,
          "priming seeks to the end, so the existing rows are not replayed");

    {                                    /* now the solver appends */
        FILE *a = open_tmp("follow.his", "a");
        fputs("0.003 11000 0.12 0.0003 1\n", a);
        fclose(a);
    }
    CHECK(his_read_new(&d, NULL) == 1, "the appended row must be accepted");
    CHECK(d.nsamp == 1, "nsamp %ld, want 1", d.nsamp);
    CHECK(fabs(d.t - 0.003) < 1e-9, "t %.6f, want 0.003", d.t);
    fclose(d.fp);
}

/* A nan in the time column is worse than one in the pressure column: t becomes
 * a foot time, and every guard downstream is a comparison, all of which are
 * false against nan. The nan therefore passes each test and comes out as a nan
 * transit time. Column 0 has to be checked with the rest. */
static void test_nonfinite_time_is_rejected(void)
{
    Dom d;
    write_file("nant.his",
        "# t, P(x,t), U(x,t), A(x,t), # point\n"
        "0.001 10000 0.10 0.0003 1\n"
        "nan 10500 0.11 0.0003 1\n"
        "0.003 11000 0.12 0.0003 1\n");
    setup_dom(&d, "nant.his");
    his_read_new(&d, NULL);
    CHECK(d.nsamp == 2,     "nsamp %ld, want 2", d.nsamp);
    CHECK(d.nonfinite == 1, "nonfinite %ld, want 1", d.nonfinite);
    CHECK(isfinite(d.t),    "t must never be non-finite, got %f", d.t);
    fclose(d.fp);
}

/* Neither pairing function may return a nan, whatever is in the feet. Every
 * range test in them is a comparison, so the negated form admits nan; they are
 * written as positive tests for this reason. */
static void test_pairing_never_returns_nan(void)
{
    Dom root, d, dom[46];
    double ms, pwv;
    int i;

    memset(&root, 0, sizeof root);
    memset(&d, 0, sizeof d);
    root.cycle = 3; root.foot = NAN;      root.prevfoot = 3.200;
    d.cycle    = 3; d.foot    = 4.164;    d.prevfoot    = 3.364;
    ms = wave_arrival_ms(&d, &root);
    CHECK(isfinite(ms), "arrival must not be nan, got %f", ms);
    CHECK(fabs(ms - 164.0) < 1.0,
          "the good pairing should still be found: %.1f ms", ms);

    memset(dom, 0, sizeof dom);
    for (i = 0; i < 46; i++) { dom[i].id = i + 1; dom[i].len = 0.1; }
    dom[14].cycle = 5; dom[14].foot = NAN;   dom[14].prevfoot = 4.000;
    dom[45].cycle = 5; dom[45].foot = 4.100; dom[45].prevfoot = 3.300;
    pwv = wave_cf_pwv(dom, 46);
    CHECK(isfinite(pwv), "cf-PWV must not be nan, got %f", pwv);
    CHECK(pwv > 0, "the clean pairing should still give a value, got %f", pwv);
}

/* ------------------------------------------------------------------------ */
/* wave_verdict -- the function that kills runs through --abort-on-fail.
 * It is a pure function of a Ctx and a clock, so it can be driven by hand. */

static Dom  vdom[116];
static Ctx  vctx;

/* every domain streaming, `n` samples each, last seen `age` seconds ago */
static void verdict_setup(double now, double age)
{
    int i;
    memset(vdom, 0, sizeof vdom);
    memset(&vctx, 0, sizeof vctx);
    vctx.dom = vdom; vctx.ndom = 116;
    for (i = 0; i < 116; i++) {
        vdom[i].id = i + 1;
        vdom[i].len = 0.1;
        vdom[i].nsamp = 100;
        vdom[i].last_rx = now - age;
        vdom[i].cycle = 6;
        vdom[i].sbp = 100; vdom[i].dbp = 76; vdom[i].psbp = 99.8;
    }
    vctx.wall = 300.0;                       /* long past any startup grace */
}

/* The 116 files do not flush in step: a domain writing 3 rows per history
 * step fills its stdio buffer several times slower than one writing 14. If
 * every domain is judged against --stall individually, the slowest flusher
 * crosses the line first and a healthy run is called STALLED -- and because
 * that branch returns, every check below it is skipped too. */
static void test_verdict_tolerates_uneven_flushing(void)
{
    double now = 1000.0;
    verdict_setup(now, 1.0);
    vdom[7].last_rx  = now - 25.0;           /* a slow flusher, --stall is 20 */
    vdom[42].last_rx = now - 30.0;
    wave_verdict(&vctx, 0, 10.0, 20.0, 2.0, now);
    CHECK(vctx.verdict != VERDICT_STALLED,
          "uneven flush timing must not read as STALLED: got '%s'", vctx.note);
}

/* A domain whose .his never appears, while the other 115 stream, is the case
 * the per-domain check exists for. Before it, root->cycle stayed 0 and the
 * verdict sat on FILLING for the whole run with every later check skipped. */
static void test_verdict_sees_one_dead_domain(void)
{
    double now = 1000.0;
    verdict_setup(now, 1.0);
    vdom[0].nsamp = 0; vdom[0].last_rx = 0; vdom[0].cycle = 0;
    wave_verdict(&vctx, 0, 10.0, 20.0, 2.0, now);
    CHECK(vctx.verdict == VERDICT_STALLED,
          "a domain that never produced anything must be reported: got '%s'",
          vctx.note);
    CHECK(strstr(vctx.note, "aortic root") != NULL,
          "and it should say which one: '%s'", vctx.note);
}

/* Nothing anywhere has arrived. That is the solver not having started, which
 * the global check owns -- the per-domain branch must not claim it. */
static void test_verdict_startup_is_not_a_dead_domain(void)
{
    double now = 1000.0;
    int i;
    verdict_setup(now, 1.0);
    for (i = 0; i < 116; i++) {
        vdom[i].nsamp = 0; vdom[i].last_rx = 0; vdom[i].cycle = 0;
    }
    wave_verdict(&vctx, 0, 10.0, 20.0, 2.0, now);
    CHECK(vctx.silent == 0 || strstr(vctx.note, "domains silent") == NULL,
          "nothing running yet is not 'some domains silent': '%s'", vctx.note);
}

/* The cf-PWV hold has to be bounded in heartbeats, not wall seconds: the
 * window it bridges is a fraction of a cardiac cycle, and this solver runs
 * ~60x slower than real time, so a wall-clock bound covers almost none of it
 * on this machine and all of it on a fast one. */
static void test_verdict_pwv_hold_is_counted_in_beats(void)
{
    double now = 1000.0;
    verdict_setup(now, 1.0);
    /* a real pairing: carotid (15) and femoral (46), 117 ms apart */
    vdom[14].cycle = 6; vdom[14].foot = 5.000; vdom[14].prevfoot = 4.180;
    vdom[45].cycle = 6; vdom[45].foot = 5.117; vdom[45].prevfoot = 4.297;
    wave_verdict(&vctx, 0, 10.0, 20.0, 2.0, now);
    CHECK(vctx.pwv > 0, "a clean pairing should produce a value, got %.2f",
          vctx.pwv);

    /* Now the pairing goes away but the beat has not advanced, and a long
     * time passes in wall-clock terms. The value must still be held. */
    {
        double held = vctx.pwv;
        vdom[45].foot = 0; vdom[45].prevfoot = 0;
        wave_verdict(&vctx, 0, 10.0, 20.0, 2.0, now + 120.0);
        CHECK(fabs(vctx.pwv - held) < 1e-9,
              "same beat, 120 wall seconds later: held %.2f, got %.2f",
              held, vctx.pwv);
    }

    /* Three beats on with no new pairing, it must be dropped rather than
     * judged against the target. */
    {
        int i;
        for (i = 0; i < 116; i++) vdom[i].cycle = 10;
        wave_verdict(&vctx, 0, 10.0, 20.0, 2.0, now + 121.0);
        CHECK(vctx.pwv == 0, "a stale value must expire, got %.2f", vctx.pwv);
    }
}

/* ------------------------------------------------------------------------ */

/* A run that has reached its final time is silent because it is finished.
 * It used to turn STALLED twenty seconds after its last step -- and under
 * --abort-on-fail exit 2, reporting every successful run as a failure. Found
 * while building the GUI, which painted a finished run red. */
static void test_verdict_finished_run_is_not_stalled(void)
{
    double now = 1000.0;
    int i;
    verdict_setup(now, 100.0);                /* nothing new for 100 s */
    vctx.tfinal = 6.5; vctx.dt_step = 1e-5; vctx.hisstep = 100;
    for (i = 0; i < 116; i++) vdom[i].t = 6.5;
    wave_verdict(&vctx, 0, 10.0, 20.0, 2.0, now);
    CHECK(vctx.done, "every domain at the final time should read as done");
    CHECK(vctx.verdict == VERDICT_CONVERGED,
          "a finished run is not STALLED: got '%s'", vctx.note);
    CHECK(strstr(vctx.note, "run complete") != NULL,
          "and it should say it is complete: '%s'", vctx.note);
}

/* Finished means every domain. One that stopped at t = 3 s is still a fault
 * after all the others have reached the end: the excuse is per domain, not
 * "the fastest file got there". */
static void test_verdict_early_stop_still_caught_when_others_finish(void)
{
    double now = 1000.0;
    int i;
    verdict_setup(now, 100.0);
    vctx.tfinal = 6.5; vctx.dt_step = 1e-5; vctx.hisstep = 100;
    for (i = 0; i < 116; i++) vdom[i].t = 6.5;
    vdom[60].t = 3.0;                         /* died halfway */
    wave_verdict(&vctx, 0, 10.0, 20.0, 2.0, now);
    CHECK(!vctx.done, "one domain short of the final time: not done");
    CHECK(vctx.verdict == VERDICT_STALLED,
          "a domain that stopped early must still be caught: got '%s'",
          vctx.note);
}

/* dSBP must not dip on every upstroke. A new foot resets the running systolic
 * to near-diastolic pressure; read naively, sbp - psbp swung to -18.4 mmHg on
 * 14% of the last beat of the reference run -- a beat whose settled change is
 * 0.49 -- and the verdict flickered back to CONVERGING every heartbeat. While
 * a beat is still rising the previous settled change is reported, so dSBP
 * never goes below the most negative change that really happened. */
static void test_dsbp_holds_through_the_upstroke(void)
{
    static Dom d;
    int i;
    double lo_shown = 0, lo_real = 0;
    const double period = 0.8, dt = 0.001;
    memset(&d, 0, sizeof d);
    d.curmin = 1e9;
    for (i = 0; i < 6000; i++) {
        double t = i * dt, ph = fmod(t, period) / period, ds;
        d.t = t;
        d.P = 75.0 + 25.0 * exp(-t / 1.5)
            + 30.0 * exp(-3.0 * ph) * sin(3.14159 * (ph < 0.5 ? ph * 2 : 0));
        wave_sample(&d);
        ds = wave_dsbp(&d);
        if (d.cycle >= 4 && isfinite(ds) && ds < lo_shown) lo_shown = ds;
    }
    for (i = 2; i < d.nsbp; i++)       /* the changes it may show from beat 4 */
        if (d.sbp_log[i] - d.sbp_log[i - 1] < lo_real)
            lo_real = d.sbp_log[i] - d.sbp_log[i - 1];
    CHECK(lo_shown >= lo_real - 1e-9,
          "dSBP showed %.2f mid-beat, below any real beat-to-beat change (%.2f)",
          lo_shown, lo_real);
}

/* ------------------------------------------------------------ --stream */

/* One sample per 5 ms of simulated time whatever the row rate, and the ring
 * keeps only the newest HIST_N. A display asking for "what arrived since last
 * time" after a from-start sweep must get the last two seconds -- not the
 * first two, and not an overrun. */
static void test_stream_history_decimates_and_keeps_newest(void)
{
    static Dom d;                    /* static: the ring is a few KB */
    int i, bad = 0, last, first;
    memset(&d, 0, sizeof d);
    for (i = 0; i < 3000; i++) {     /* 3 s of rows at 1 kHz */
        d.t = i * 0.001;
        d.P = 80.0 + i * 0.01;
        stream_sample(&d);
    }
    CHECK(d.hist_total == 600, "stored %ld samples from 3000 rows, want 600",
          d.hist_total);
    last  = (d.hist_head - 1 + HIST_N) % HIST_N;   /* newest */
    first = d.hist_head;                           /* ring full: oldest here */
    CHECK(fabs(d.hist_t[last] - 2.995) < 1e-4,
          "newest kept t = %.4f, want 2.995", d.hist_t[last]);
    CHECK(fabs(d.hist_t[first] - 1.000) < 1e-4,
          "oldest kept t = %.4f, want 1.000 (the newest 2 s)", d.hist_t[first]);
    for (i = 1; i < HIST_N; i++) {
        int a = (first + i - 1) % HIST_N, b = (first + i) % HIST_N;
        if (fabs((d.hist_t[b] - d.hist_t[a]) - 0.005) > 1e-4) bad++;
    }
    CHECK(bad == 0, "%d gaps in the kept history are not 5 ms", bad);
}

/* A rerun into the same files starts again at t = 0. That must restart the
 * history rather than freeze it until the new run catches up with the old. */
static void test_stream_history_restarts_when_time_goes_back(void)
{
    static Dom d;
    memset(&d, 0, sizeof d);
    d.t = 4.000; d.P = 100.0; stream_sample(&d);
    d.t = 0.500; d.P =  80.0; stream_sample(&d);
    CHECK(d.hist_total == 2, "time going backwards was dropped: %ld stored",
          d.hist_total);
}

/* The terminal's '>' marker and the stream's front flag are one test, and it
 * is relative to the whole tree this frame, not to the vessel's own upstroke.
 * The README once described it the other way; this pins what it really is. */
static void test_wave_front_is_relative_to_the_tree(void)
{
    static Dom dom[3];
    memset(dom, 0, sizeof dom);
    dom[0].nsamp = 1; dom[0].dPdt = 100;
    dom[1].nsamp = 1; dom[1].dPdt = 50;
    dom[2].nsamp = 0; dom[2].dPdt = 5000;   /* no samples: must not count */
    CHECK(wave_dpmax(dom, 3) == 100, "dpmax %.0f, want 100: a domain with no "
          "samples must not set it", wave_dpmax(dom, 3));
    CHECK(wave_front(&dom[1], 100),  "rising at 50 of 100 is on the front");
    CHECK(!wave_front(&dom[1], 1000), "rising at 50 of 1000 is not");
    CHECK(!wave_front(&dom[2], 100),  "a domain with no samples never is");
}

/* Every completed beat's systolic is kept, in order, so a display can draw
 * convergence -- including one that attached late or read a finished run,
 * where the live dSBP has long since moved on. With a startup decay in the
 * diastole, each beat's systolic is lower than the last. */
static void test_sbp_log_records_each_completed_beat(void)
{
    static Dom d;
    int i, down = 1;
    const double period = 0.8, dt = 0.001;
    memset(&d, 0, sizeof d);
    d.curmin = 1e9;
    for (i = 0; i < 6000; i++) {
        double t = i * dt, ph = fmod(t, period) / period;
        d.t = t;
        d.P = 75.0 + 25.0 * exp(-t / 1.5)
            + 30.0 * exp(-3.0 * ph) * sin(3.14159 * (ph < 0.5 ? ph * 2 : 0));
        wave_sample(&d);
    }
    CHECK(d.nsbp == d.cycle - 1, "%d beats logged for %d feet, want one per "
          "closed beat", d.nsbp, d.cycle);
    for (i = 1; i < d.nsbp; i++)
        if (!(d.sbp_log[i] < d.sbp_log[i - 1])) down = 0;
    CHECK(down, "systolic should fall beat by beat with the startup decay");
}

int main(void)
{
    if (system("mkdir -p /tmp/his_monitor_test") != 0) {
        fprintf(stderr, "cannot create %s\n", TMP);
        return 1;
    }
    printf("test_hisfile\n");

    test_header_with_parens();
    test_header_alternate_columns();
    test_header_too_wide();
    test_header_ignores_other_comments();
    test_partial_last_line();
    test_eof_then_growth();
    test_nonfinite_rows_are_rejected();
    test_history_point_filter();
    test_his_header_overrides_point_count();
    test_overlong_line_does_not_wedge();
    test_abbreviate();
    test_foot_detection_across_cycles();
    test_wave_sample_ignores_nan();
    test_cf_pwv_pairs_feet_by_beat();
    test_arrival_pairs_to_the_same_beat();
    test_follow_mode_learns_the_header();
    test_nonfinite_time_is_rejected();
    test_pairing_never_returns_nan();
    test_verdict_tolerates_uneven_flushing();
    test_verdict_sees_one_dead_domain();
    test_verdict_startup_is_not_a_dead_domain();
    test_verdict_pwv_hold_is_counted_in_beats();
    test_verdict_finished_run_is_not_stalled();
    test_verdict_early_stop_still_caught_when_others_finish();
    test_dsbp_holds_through_the_upstroke();
    test_stream_history_decimates_and_keeps_newest();
    test_stream_history_restarts_when_time_goes_back();
    test_wave_front_is_relative_to_the_tree();
    test_sbp_log_records_each_completed_beat();

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
