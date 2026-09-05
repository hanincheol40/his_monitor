/*
 * wave.c — what the numbers mean.
 *
 * Three questions, answered from the .his stream alone:
 *   where is the foot of each cardiac cycle,
 *   what carotid-femoral pulse wave velocity does that imply,
 *   and is the solution still moving from cycle to cycle.
 */
#include "wave.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The carotid-femoral path, as domain numbers. Taken from the PWDB
 * pre-processing (calculate_pwdb_input_parameters.m) so that the number this
 * prints is comparable with the PWV_cf the database reports. */
static const int PATH_CAR[] = {1, 2, 15};
static const int PATH_FEM[] = {1, 2, 14, 18, 27, 28, 35, 37, 39, 41, 42, 44, 46};
#define NCAR ((int)(sizeof PATH_CAR / sizeof PATH_CAR[0]))
#define NFEM ((int)(sizeof PATH_FEM / sizeof PATH_FEM[0]))

/*
 * Foot detection.
 *
 * Hold a running minimum. Once the pressure has climbed a quarter of the last
 * pulse pressure above it, the minimum being held was the foot of this cycle.
 *
 * Leaving that state is done on a time lockout, not on the pressure coming
 * back down to the diastolic value just recorded. The first cycle records the
 * t = 0 startup pressure as its diastole, and the solution never returns
 * there, so a level-based exit latches on cycle 1 and every later cycle is
 * missed. That bug was visible only against real solver output: with a
 * synthetic waveform that starts at its own diastole, it never fires.
 */
void wave_sample(Dom *d)
{
    double per, thr;

    if (!isfinite(d->P)) return;          /* never let nan into the state */

    per = (d->prevfoot > 0 && d->foot > d->prevfoot)
        ? d->foot - d->prevfoot : 0.80;
    /* One mis-detected foot during start-up can make `per` absurd, and the
     * lockout is a fraction of it: too long and whole cycles are skipped, too
     * short and it chatters. Hold it to a physiological range. */
    if (per < 0.30) per = 0.30;
    if (per > 2.00) per = 2.00;
    thr = (d->amp > 2 ? d->amp : 20.0) * 0.25;

    if (d->st == 1) {                     /* past the foot: track extremes */
        if (d->P > d->sbp) d->sbp = d->P;
        if (d->P < d->dbp) d->dbp = d->P;
        if (d->t - d->foot > 0.45 * per) {          /* lockout over */
            d->amp = d->sbp - d->dbp;
            d->st = 0;
            /* Seeding curmin with a nan would latch it: every later
             * comparison against nan is false, so the minimum would never
             * update again and no foot would ever be found -- not even after
             * the data came back clean. */
            d->curmin  = isfinite(d->P) ? d->P : HUGE_VAL;
            d->curmint = d->t;
        }
        return;
    }
    if (d->P < d->curmin) { d->curmin = d->P; d->curmint = d->t; }
    if (d->P > d->curmin + thr) {
        d->prevfoot = d->foot; d->foot = d->curmint;
        d->psbp = d->sbp;
        d->sbp  = d->P;        d->dbp  = d->curmin;
        d->cycle++;
        d->st = 1;
    }
}

/*
 * Milliseconds from the aortic root's foot to this domain's, paired to the
 * same heartbeat.
 *
 * Same trap as wave_cf_pwv, and it was found the same way -- by looking at a
 * capture of a live run. Plain `d->foot - root->foot` reads -656 ms at the
 * ankle, because by the time the wave gets there the root has already
 * recorded the next beat's foot. -656 ms and a 820 ms period is really
 * 164 ms; the number was not wrong by a little, it was a whole beat out.
 */
double wave_arrival_ms(const Dom *d, const Dom *root)
{
    /* The root's own foot is 0, so allow a hair below zero for rounding; no
     * vessel in this tree is more than 400 ms from the root. */
    const double TMIN = -0.001, TMAX = 0.400;
    double best = 0;
    int found = 0, i, j;

    if (!d->cycle || !root->cycle) return 0;
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            double f = i ? d->prevfoot    : d->foot;
            double r = j ? root->prevfoot : root->foot;
            double dt;
            /* Positive tests, not negated ones: `dt < TMIN` is false for a
             * nan, so the negated form lets a nan through and then keeps it,
             * because `dt < best` is false for every later candidate too.
             *
             * `found` is a separate flag rather than a negative sentinel in
             * `best`, because TMIN is itself slightly negative -- the root's
             * own arrival is 0 and rounding can put it just below -- so a
             * legal value and "nothing found" would otherwise be the same
             * thing. */
            if (!(f > 0) || !(r > 0)) continue;
            dt = f - r;
            if (!(dt >= TMIN && dt <= TMAX)) continue;
            if (!found || dt < best) { best = dt; found = 1; }
        }
    }
    return found ? best * 1000.0 : 0;
}

static double path_len(const Dom *dom, int ndom, const int *path, int n)
{
    double L = 0;
    int i;
    for (i = 0; i < n; i++) if (path[i] <= ndom) L += dom[path[i]-1].len;
    return L;
}

/*
 * The extra distance down to the femoral divided by the extra time the foot
 * takes to get there — the model's own definition of cf-PWV.
 *
 * The two feet have to come from the same heartbeat. The wave reaches the
 * carotid about 17 ms after the root and the femoral about 134 ms after, so
 * for roughly a sixth of every cycle the carotid has already recorded the new
 * foot while the femoral is still holding the previous one. Subtracting those
 * gives a negative transit time.
 *
 * This is invisible when the .his files are read to EOF, because then every
 * domain sits at the same final sample. It only appears against a solver that
 * is still running -- measured here at 30 of 161 frames, 18.6%, which matches
 * the arrival gap. The old code returned 0 in that window, and a 0 made
 * wave_verdict skip the PWV check altogether: --abort-on-fail could sail past
 * an off-target run for a fifth of its frames.
 */
double wave_cf_pwv(const Dom *dom, int ndom)
{
    /* Carotid-to-femoral transit is tens of milliseconds. Anything outside
     * this window is two feet from different beats, not a transit time. */
    const double TMIN = 0.010, TMAX = 0.400;
    const Dom *car, *fem;
    double dt = 0;
    int i, j;

    if (ndom < 46) return 0;
    car = &dom[15-1]; fem = &dom[46-1];
    if (car->cycle < 2 || fem->cycle < 2) return 0;

    /* Each domain keeps this beat's foot and the previous one, so there are
     * four candidate pairings; the right one is the pairing whose difference
     * is a plausible transit time.
     *
     * Pairing by cycle number instead looks obvious and is wrong: the counters
     * drift apart over a run because the detector does not fire the same
     * number of times on every waveform. Requiring car->cycle == fem->cycle
     * made this return 0 on every frame of an eight-cycle run, where the
     * carotid and femoral counters had separated by more than one. */
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            double c0 = i ? car->prevfoot : car->foot;
            double f0 = j ? fem->prevfoot : fem->foot;
            double d  = f0 - c0;
            /* Positive tests so a nan is excluded rather than admitted: every
             * `<` and `>` against a nan is false, so the negated form accepts
             * it and then never replaces it. */
            if (!(c0 > 0) || !(f0 > 0)) continue;
            if (!(d >= TMIN && d <= TMAX)) continue;
            if (dt == 0 || d < dt) dt = d;      /* the closest matching beat */
        }
    }
    if (dt == 0) return 0;

    return (path_len(dom, ndom, PATH_FEM, NFEM)
          - path_len(dom, ndom, PATH_CAR, NCAR)) / dt;
}

/*
 * The verdict. This is the whole point of the tool: a run that is going to be
 * thrown away should say so in the first few cycles, not after the cohort has
 * finished and the post-processing has run.
 */
void wave_verdict(Ctx *c, double target_pwv, double tol_pct, double stall_s,
                  double umax_lim, double now)
{
    const Dom *root = &c->dom[0];
    double dsbp = (root->psbp > 0) ? root->sbp - root->psbp : 0.0;
    double pwv  = wave_cf_pwv(c->dom, c->ndom);
    double newest = 0;
    int i, nf = 0, alive = 0, never = 0;

    /* Hold the last good value rather than showing 0 whenever the two feet
     * momentarily belong to different beats. cf-PWV is a property of the
     * vessel tree; it does not vanish for 130 ms and then come back.
     *
     * But hold it for a bounded time, not forever. Without the expiry, a value
     * computed at cycle 2 -- while the pressure field is still filling, which
     * the comment further down says is meaningless -- survives to cycle 5,
     * where `settled` turns true and it is judged against the target. A run
     * gets killed, or passed, on a number that has not been recomputed since
     * the transient.
     *
     * The bound is counted in heartbeats, not wall seconds. What it has to
     * bridge is a fraction of one cardiac cycle -- simulated time -- and this
     * tool exists precisely because the solver runs far slower than real time:
     * 6.5 s of simulation takes about 6.5 minutes here, a ratio near 60. A
     * three-second wall-clock hold would cover that straddle completely on a
     * machine fast enough to run in real time, and almost none of it here,
     * which is exactly the wrong way round. root->cycle is already in hand and
     * is in the right units. */
    if (pwv > 0) { c->pwv = pwv; c->pwv_cycle = root->cycle; }
    else if (c->pwv > 0 && root->cycle - c->pwv_cycle <= 2) pwv = c->pwv;
    else { c->pwv = 0; pwv = 0; }
    c->umax = 0; c->umax_dom = 0; c->ioerr_dom = 0; c->silent = 0;

    for (i = 0; i < c->ndom; i++) {
        const Dom *d = &c->dom[i];
        if (d->last_rx > newest) newest = d->last_rx;
        if (d->nonfinite > 0) nf++;
        if (d->ioerr > 0) c->ioerr_dom++;
        if (d->umax > c->umax) { c->umax = d->umax; c->umax_dom = d->id; }
        /* Count domains that are individually quiet.
         *
         * `newest` alone is the maximum over all 116, so one live domain hides
         * 115 dead ones and the stall check never fires. A domain that never
         * appears at all -- no .his, or a file whose header never parsed --
         * is exactly the case this tool should catch, and it was the case it
         * could not see. */
        /* "Alive" is generous on purpose: the 116 files do not flush in step.
         * Each domain writes one row per history point, and the point count
         * varies from 3 to 14, so a domain filling its stdio buffer with 3
         * rows per step flushes several times less often than one writing 14.
         * Judging every domain against the same --stall as if the appends were
         * synchronous calls a healthy run stalled the moment the slowest
         * flusher crosses the line.
         *
         * So two separate tests, both deliberately hard to trip:
         *   never produced anything, while others have  -> a real fault
         *                                                  (missing file, or
         *                                                   a header that
         *                                                   never parsed)
         *   was streaming and stopped for 3x --stall    -> generous enough to
         *                                                  clear any flush
         *                                                  jitter */
        if (d->nsamp > 0 && now - d->last_rx <= stall_s) alive++;
        else if (c->wall > stall_s &&
                 (d->nsamp == 0 || now - d->last_rx > 3.0 * stall_s)) {
            c->silent++;
            if (d->nsamp == 0) never++;      /* which of the two tests it was */
        }
    }

    if (newest == 0) {
        /* Nothing has arrived at all. Without this the verdict sits on
         * FILLING forever when the base name is wrong or the solver already
         * exited -- exactly the case the watchdog exists to catch. */
        if (c->wall > stall_s) {
            c->verdict = VERDICT_STALLED;
            snprintf(c->note, sizeof c->note,
                     "no samples after %.0f s - wrong base name, or the solver "
                     "never started (try --from-start)", c->wall);
            return;
        }
    } else if (now - newest > stall_s) {
        c->verdict = VERDICT_STALLED;
        snprintf(c->note, sizeof c->note,
                 "no new samples for %.0f s - solver stopped or wedged",
                 now - newest);
        return;
    }

    /* A diverged run keeps producing rows, so it is neither stalled nor
     * converging -- it is writing nan. Say so before anything else looks at
     * the numbers. */
    if (nf) {
        c->verdict = VERDICT_OFFTARGET;
        snprintf(c->note, sizeof c->note,
                 "%d domain(s) writing non-finite values - the solver diverged",
                 nf);
        return;
    }
    /* Some domains are quiet while others stream.
     *
     * `alive > 0` is the discriminator, and it is the whole point: if nothing
     * anywhere has arrived, the solver simply has not started and the global
     * check above owns that case. It is only a fault when the rest of the tree
     * is plainly running and these particular domains are not.
     *
     * Reported before FILLING, because otherwise: if the quiet one happens to
     * be domain 1, root->cycle stays 0, the verdict sits on "filling: cycle 0
     * of ~8" for the whole run, and every check below -- PWV, peak velocity,
     * convergence -- is skipped by the return under it. One missing file used
     * to blind the entire tool. */
    if (alive > 0 && c->silent > 0) {
        /* Name the threshold that actually applied. The two tests above use
         * different bars -- a domain that never wrote is judged at --stall,
         * one that was streaming and stopped at three times that -- and a
         * message quoting --stall for both sends the reader looking at the
         * wrong number when the run is diagnosed later from a log. */
        const char *root_note =
            c->dom[0].nsamp == 0 ? " (including the aortic root)" : "";
        c->verdict = VERDICT_STALLED;
        if (never == c->silent)
            snprintf(c->note, sizeof c->note,
                     "%d of %d domains have written nothing in %.0f s%s",
                     c->silent, c->ndom, stall_s, root_note);
        else if (never == 0)
            snprintf(c->note, sizeof c->note,
                     "%d of %d domains stopped writing over %.0f s ago%s",
                     c->silent, c->ndom, 3.0 * stall_s, root_note);
        else
            snprintf(c->note, sizeof c->note,
                     "%d of %d domains silent - %d never wrote, %d stopped "
                     "over %.0f s ago%s",
                     c->silent, c->ndom, never, c->silent - never,
                     3.0 * stall_s, root_note);
        return;
    }

    if (root->cycle < 2) {
        c->verdict = VERDICT_FILLING;
        snprintf(c->note, sizeof c->note, "filling: cycle %d of ~8", root->cycle);
        return;
    }

    /* Peak velocity fires before cf-PWV does, and it names the domain, so it
     * is checked first. The failure it catches is specific: if the radii are
     * scaled for a subject but the terminal resistances are left at the
     * reference values, the same flow is pushed through a smaller area and
     * every velocity rises together. The pressure waveform still looks like a
     * pressure waveform for several more cycles, which is why a run like that
     * is normally only caught in post-processing.
     *
     * The default limit is set from measurement, not from a textbook -- see
     * README, "peak velocity limit". It is deliberately well above the healthy
     * peak so that a real subject with a stiff aorta is not failed. */
    if (umax_lim > 0 && c->umax > umax_lim) {
        c->verdict = VERDICT_OFFTARGET;
        snprintf(c->note, sizeof c->note,
                 "peak |U| %.2f m/s in domain %d (limit %.2f) - radii scaled "
                 "without the terminal resistances?", c->umax, c->umax_dom,
                 umax_lim);
        return;
    }

    /* The pressure field is still growing for the first several cycles, so
     * cf-PWV measured there is meaningless and would fail a perfectly good
     * run. Judge it only once the solution has settled, or once enough cycles
     * have gone by that it is not going to. */
    {
        int settled = (fabs(dsbp) < 3.0) || (root->cycle >= 5);
        if (settled && target_pwv > 0 && pwv > 0) {
            double err = 100.0 * (pwv - target_pwv) / target_pwv;
            if (fabs(err) > tol_pct) {
                c->verdict = VERDICT_OFFTARGET;
                snprintf(c->note, sizeof c->note,
                         "cf-PWV %.2f vs target %.2f m/s (%+.1f%%) - check k3 / radii",
                         pwv, target_pwv, err);
                return;
            }
        }
    }

    if (fabs(dsbp) < 0.5) {
        c->verdict = VERDICT_CONVERGED;
        snprintf(c->note, sizeof c->note,
                 "cycle-to-cycle SBP change %.2f mmHg - periodic%s", dsbp,
                 (target_pwv > 0 && pwv > 0) ? ", cf-PWV within tolerance" : "");
    } else {
        c->verdict = VERDICT_CONVERGING;
        snprintf(c->note, sizeof c->note,
                 "cycle %d, cycle-to-cycle SBP change %+.2f mmHg",
                 root->cycle, dsbp);
    }
}
