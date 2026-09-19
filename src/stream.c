/*
 * stream.c -- the display as data, for a front end in another process.
 *
 * --stream replaces the terminal frame with one JSON object per display
 * period, on stdout. Text on stdout rather than a socket, on purpose: the
 * monitor runs wherever the solver runs, usually a server reached over ssh,
 * and `ssh host his_monitor ... --stream` then carries the stream to the
 * display for free -- encrypted, authenticated, through any firewall that lets
 * ssh in. A socket server would need a port, a firewall rule and its own
 * authentication, all to do less.
 *
 * Nothing here decides anything. The verdict, the arrival times and the wave
 * front come from wave.c exactly as the terminal shows them, so a GUI built on
 * this cannot disagree with the terminal about whether a run is healthy.
 */
#define _POSIX_C_SOURCE 200809L
#include "stream.h"
#include "wave.h"

#include <stdio.h>
#include <math.h>

/*
 * Keep one sample per 1/STREAM_HZ of simulated time. Time-based rather than
 * every-Nth-row, so the output rate does not depend on HISSTEP or on how many
 * rows the history-point filter throws away.
 *
 * Time going backwards restarts the history instead of being ignored: a rerun
 * into the same files starts again at t = 0, and "wait until t passes the last
 * stored sample" would leave the waveform frozen until the new run caught up
 * with the old one.
 */
void stream_sample(Dom *d)
{
    double dt = d->t - d->hist_last;
    if (!isfinite(d->t) || !isfinite(d->P)) return;
    if (d->hist_total > 0 && dt >= 0 && dt < 1.0 / STREAM_HZ - 1e-9) return;
    d->hist_t[d->hist_head] = (float)d->t;
    d->hist_p[d->hist_head] = (float)d->P;
    d->hist_head = (d->hist_head + 1) % HIST_N;
    d->hist_total++;
    d->hist_last = d->t;
}

/* JSON has no nan and no inf. A non-finite value goes out as null; printing
 * it with %f would put "nan" on the wire and break every parser downstream. */
static void jnum(double v, const char *fmt)
{
    if (isfinite(v)) printf(fmt, v); else fputs("null", stdout);
}

static void jstr(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')  { putchar('\\'); putchar(ch); }
        else if (ch < 0x20)           printf("\\u%04x", ch);
        else                          putchar(ch);
    }
    putchar('"');
}

void stream_init(const Ctx *c, const char *base, int period_ms)
{
    int i;
    printf("{\"type\":\"init\",\"version\":1,\"base\":");
    jstr(base);
    printf(",\"ndom\":%d,\"tfinal\":", c->ndom);
    jnum(c->tfinal, "%.6g");
    printf(",\"period_ms\":%d,\"hist_hz\":%d,\"region\":[", period_ms, STREAM_HZ);
    for (i = 0; i < c->ndom; i++) printf("%s%d", i ? "," : "", c->dom[i].region);
    printf("],\"name\":[");
    for (i = 0; i < c->ndom; i++) { if (i) putchar(','); jstr(c->dom[i].name); }
    printf("],\"short\":[");
    for (i = 0; i < c->ndom; i++) { if (i) putchar(','); jstr(c->dom[i].shrt); }
    printf("]}\n");
    fflush(stdout);
}

int stream_tick(Ctx *c)
{
    const Dom *root = &c->dom[0];
    double tnow  = wave_tnow(c->dom, c->ndom);
    double dpmax = wave_dpmax(c->dom, c->ndom);
    double dsbp  = wave_dsbp(root);
    int i;

    printf("{\"type\":\"tick\",\"t\":");  jnum(tnow, "%.4f");
    printf(",\"wall\":");                 jnum(c->wall, "%.2f");
    printf(",\"eta\":");                  jnum(wave_eta(c, tnow), "%.1f");
    printf(",\"verdict\":\"%s\",\"note\":", wave_verdict_name(c->verdict));
    jstr(c->note);
    /* Pressures go out with more decimals than any display shows. The display
     * rounds them again, and rounding an already rounded value is how one
     * screen showed +0.48 and 0.49 for the same dSBP: 0.485 sent, then halved
     * down in binary, while the verdict note had rounded the full value up. */
    printf(",\"cycle\":%d,\"sbp\":", root->cycle); jnum(root->sbp, "%.4f");
    printf(",\"dbp\":");                  jnum(root->dbp, "%.4f");
    printf(",\"dsbp\":");                 jnum(dsbp, "%.6f");
    printf(",\"pwv\":");                  jnum(c->pwv, "%.3f");
    printf(",\"umax\":");                 jnum(c->umax, "%.3f");
    printf(",\"umax_dom\":%d,\"threads\":%d,\"mb\":", c->umax_dom, c->nthreads);
    jnum(c->mb_read, "%.1f");
    printf(",\"tick_ms\":");              jnum(c->tick_ms, "%.2f");
    printf(",\"tick_mean_ms\":");
    jnum(c->ticks ? c->tick_sum_ms / (double)c->ticks : 0.0, "%.2f");
    printf(",\"tick_max_ms\":");          jnum(c->tick_max_ms, "%.2f");
    printf(",\"jitter_ms\":");            jnum(c->jitter_ms, "%.2f");
    printf(",\"late\":%ld,\"ticks\":%ld,\"ioerr_dom\":%d",
           c->misses, c->ticks, c->ioerr_dom);

    /* Systolic of every completed beat at the root, so a display can draw
     * convergence even when it joined late or read a finished run. */
    printf(",\"sbp_beats\":[");
    for (i = 0; i < root->nsbp; i++) {
        if (i) putchar(',');
        jnum(root->sbp_log[i], "%.4f");
    }
    /* The beat in progress joins sbp_beats only when the next foot closes it,
     * but its peak is final as soon as its lockout is over -- which is when
     * dSBP starts reporting it. Sent separately, so the display can show that
     * beat too, and never shows a peak that is still rising. */
    printf("],\"sbp_current\":");
    jnum(root->st == 0 && root->cycle >= 1 ? root->sbp : NAN, "%.4f");
    printf(",\"P\":[");

    for (i = 0; i < c->ndom; i++) {
        if (i) putchar(',');
        if (c->dom[i].nsamp) jnum(c->dom[i].P, "%.1f"); else fputs("null", stdout);
    }
    printf("],\"arrival_ms\":[");
    for (i = 0; i < c->ndom; i++)
        printf("%s%d", i ? "," : "", (int)wave_arrival_ms(&c->dom[i], root));
    printf("],\"front\":[");
    for (i = 0; i < c->ndom; i++)
        printf("%s%d", i ? "," : "", wave_front(&c->dom[i], dpmax));

    /* What arrived since the last line, as flat [t,P,t,P,...] per domain.
     * Capped at the ring size: after a from-start sweep that is the newest
     * two seconds, which is what a waveform view wants anyway. */
    printf("],\"hist\":[");
    for (i = 0; i < c->ndom; i++) {
        Dom *d = &c->dom[i];
        long n = d->hist_total - d->hist_sent, k;
        if (n > HIST_N) n = HIST_N;
        if (i) putchar(',');
        putchar('[');
        for (k = 0; k < n; k++) {
            int s = (int)(((long)d->hist_head - n + k + HIST_N) % HIST_N);
            printf("%s%.3f,%.1f", k ? "," : "", d->hist_t[s], d->hist_p[s]);
        }
        putchar(']');
        d->hist_sent = d->hist_total;
    }
    printf("]}\n");

    /* With SIGPIPE ignored, a reader that has gone away shows up here as a
     * failed flush. Report it, so the monitor stops instead of reading 116
     * files forever for nobody. */
    if (fflush(stdout) == EOF || ferror(stdout)) return -1;
    return 0;
}

void stream_exit(const Ctx *c, int code)
{
    printf("{\"type\":\"exit\",\"code\":%d,\"verdict\":\"%s\",\"note\":",
           code, wave_verdict_name(c->verdict));
    jstr(c->note);
    printf("}\n");
    fflush(stdout);
}
