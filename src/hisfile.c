/*
 * hisfile.c — the two Nektar1D file formats.
 *
 *   sim_N.in        a sectioned text file: parameter block, mesh block (one
 *                   entry per element, geometry given as formula strings),
 *                   boundary conditions, initial conditions, history points.
 *                   Undocumented, so it is read defensively: anything that
 *                   does not match a known shape is skipped rather than
 *                   treated as an error.
 *
 *   sim_N_<d>.his   one file per domain, appended to every HISSTEP steps
 *                   while the solver runs. Column layout is declared in a
 *                   header comment and depends on which options the run used,
 *                   so it is read from the file instead of assumed.
 */
/* strtok_r is POSIX, not ISO C: under -std=c11 the standard headers hide it,
 * the compiler falls back to an implicit int-returning declaration, and the
 * returned pointer is truncated to 32 bits -- a crash, not a warning at run
 * time. The Makefile passes this too; it is repeated here so the file still
 * compiles on its own. */
#define _POSIX_C_SOURCE 200809L
#include "hisfile.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ------------------------------------------------------------------ sim.in */

int in_param(const char *path, const char *key, double *out)
{
    FILE *f = fopen(path, "r");
    char line[1024], name[64];
    double v;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lf %63s", &v, name) == 2 && !strcmp(name, key)) {
            *out = v; fclose(f); return 1;
        }
        if (strstr(line, "Mesh --")) break;        /* parameter block is over */
    }
    fclose(f);
    return 0;
}

/* Anatomical grouping, by segment number in the 116-artery model. */
static int region_of(int s)
{
    if (s >= 42 && s <= 55)  return 3;
    if (s >= 97 && s <= 116) return 2;
    if (s == 4 || s == 7 || s == 8 || s == 9 || s == 10 || s == 11 ||
        s == 19 || s == 21 || s == 22 || s == 23 || s == 24 || s == 25) return 2;
    if (s >= 56 && s <= 96)  return 1;
    if (s == 3 || s == 5 || s == 6 || s == 12 || s == 13 ||
        s == 15 || s == 16 || s == 17 || s == 20) return 1;
    return 0;
}

int in_mesh(const char *path, Dom *dom, int maxdom)
{
    FILE *f = fopen(path, "r");
    char line[4096];
    int ndom = 0, cur = 0, i, d, npts, in_hist = 0, e1, e2;
    double a, b;
    if (!f) return 0;

    while (fgets(line, sizeof line, f)) {
        char *p = strstr(line, "Ndomains");
        if (p && (p = strchr(p, '='))) {
            ndom = atoi(p + 1);
            if (ndom > maxdom) ndom = maxdom;
            for (i = 0; i < ndom; i++) {
                dom[i].id = i + 1;
                dom[i].watch = -1;
                dom[i].region = region_of(i + 1);
            }
            continue;
        }
        if (strstr(line, "nel domain")) {
            if (sscanf(strstr(line, "nel domain") + 10, "%d", &d) == 1 &&
                d >= 1 && d <= ndom) cur = d;
            continue;
        }
        if (!strncmp(line, "History Pts", 11)) { in_hist = 1; cur = 0; continue; }

        if (in_hist) {
            /* "<npts> <domain>" then a line of that many x positions. Lines
             * that are really positions parse their first field as 0, which
             * the npts > 0 test rejects. */
            if (sscanf(line, "%d %d", &npts, &d) == 2 &&
                d >= 1 && d <= ndom && npts > 0 && npts <= MAXPTS) {
                char *s, *e;
                if (!fgets(line, sizeof line, f)) break;
                /* Only the count is kept. The positions themselves are read
                 * and discarded because a truncated position line means the
                 * count on the line above is a lie, and watching a point the
                 * solver never writes leaves the domain permanently blank. */
                for (i = 0, s = line; i < npts; i++, s = e) {
                    (void)strtod(s, &e);
                    if (e == s) break;
                }
                dom[d-1].npts = i;
            }
            continue;
        }
        if (cur && sscanf(line, "%lf %lf %d %d", &a, &b, &e1, &e2) == 4 &&
            strstr(line, "x_prox")) {
            (void)e1; (void)e2;
            if (b > dom[cur-1].len) dom[cur-1].len = b;   /* domain length */
        }
    }
    fclose(f);

    for (i = 0; i < ndom; i++)
        if (dom[i].watch < 0) dom[i].watch = dom[i].npts ? dom[i].npts - 1 : 0;
    return ndom;
}

/* ------------------------------------------------------------- artery names */

/* "Right Superior Middle Cerebral Artery (M2)" does not fit a cell; shorten it
 * without losing which vessel it is. */
void abbreviate(const char *in, char *out, size_t n)
{
    static const char *from[] = {
        "Right ", "Left ", "Superior ", "Inferior ", "Anterior ", "Posterior ",
        "Internal ", "External ", "Descending ", "Abdominal ", "Common ",
        "Superficial ", "Ascending ", "Cerebral", "Interosseous",
        "Communicating", "Mesenteric", "Mensenteric", "Brachiocephalic",
        "Subclavian", "Thoracic", " Artery", " Arteries", "Palmar",
        "Vertebral", "Carotid", "Femoral", "Digital", "Maxillary",
        "Temporal", "Opthalmic", "Ophthalmic", "Basilar", "Choroidal", NULL };
    static const char *to[] = {
        "R.", "L.", "Sup.", "Inf.", "Ant.", "Post.",
        "Int.", "Ext.", "Desc.", "Abd.", "Com.",
        "Superf.", "Asc.", "Cereb", "Inteross",
        "Comm", "Mesent", "Mesent", "Brachioceph",
        "Subclav", "Thor", "", "", "Palm",
        "Vert", "Car", "Fem", "Dig", "Maxill",
        "Temp", "Ophth", "Ophth", "Basil", "Choroid" };
    char buf[160];
    size_t i;
    int k;

    /* from[] and to[] are indexed by the same k, so a replacement added to one
     * and not the other reads past the end of to[]. Make that a build failure
     * rather than a garbage cell label. from[] carries the extra NULL. */
    _Static_assert(sizeof from / sizeof from[0] == sizeof to / sizeof to[0] + 1,
                   "abbreviate: from[] and to[] are out of step");

    strncpy(buf, in, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (k = 0; from[k]; k++) {
        char *hit;
        while ((hit = strstr(buf, from[k]))) {
            size_t fl = strlen(from[k]), tl = strlen(to[k]);
            memmove(hit + tl, hit + fl, strlen(hit + fl) + 1);
            memcpy(hit, to[k], tl);
        }
    }
    for (i = 0; i + 1 < n && buf[i]; i++) out[i] = buf[i];
    out[i] = 0;
}

void load_names(const char *path, Dom *dom, int ndom)
{
    FILE *f = fopen(path, "r");
    char line[2048];
    if (!f) return;
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }   /* header row */
    while (fgets(line, sizeof line, f)) {
        char *tok, *save = NULL, *last = NULL;
        int seg;
        if (sscanf(line, "%d", &seg) != 1 || seg < 1 || seg > ndom) continue;
        line[strcspn(line, "\r\n")] = 0;
        for (tok = strtok_r(line, "\t", &save); tok;
             tok = strtok_r(NULL, "\t", &save)) last = tok;
        if (!last) continue;
        strncpy(dom[seg-1].name, last, sizeof dom[0].name - 1);
        dom[seg-1].name[sizeof dom[0].name - 1] = 0;
        abbreviate(dom[seg-1].name, dom[seg-1].shrt, sizeof dom[0].shrt);
    }
    fclose(f);
}

/* ----------------------------------------------------------------- sim.his */

/* The header reads
 *   "# t, P(x,t), Pe(x,t), U(x,t), Q(x,t), A(x,t), # point"
 * Two traps live in that one line. The set of columns depends on the run's
 * options, so it has to be read rather than hardcoded; and each name carries
 * an argument list whose comma would split the field in two, putting every
 * later column index off by the number of parentheses seen so far. */
void parse_header(Dom *d, char *line)
{
    /* strtok_r, not strtok: eight reader threads run this concurrently and
     * strtok keeps its cursor in one process-wide static, so two threads
     * parsing headers at the same moment walk each other's buffers. That
     * silently drops whole domains -- the header line appears once per file,
     * so a domain that loses the race stays blank for the entire run. */
    char buf[1024], *tok, *save = NULL, *rd, *wr;
    int col = 0, depth = 0;

    if (!strstr(line, "# t")) return;
    strncpy(buf, line, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    buf[strcspn(buf, "\r\n")] = 0;

    for (rd = wr = buf; *rd; rd++) {              /* drop "(x,t)" */
        if (*rd == '(') { depth++; continue; }
        if (*rd == ')') { if (depth) depth--; continue; }
        if (!depth) *wr++ = *rd;
    }
    *wr = 0;

    d->cP = d->cU = d->cA = d->cPt = -1;
    for (tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save), col++) {
        char nm[32];
        char *s = tok;
        int k = 0;
        while (*s && (isspace((unsigned char)*s) || *s == '#')) s++;
        while (*s && !isspace((unsigned char)*s) && k < 31) nm[k++] = *s++;
        nm[k] = 0;
        if      (!strcmp(nm, "P"))     d->cP  = col;
        else if (!strcmp(nm, "U"))     d->cU  = col;
        else if (!strcmp(nm, "A"))     d->cA  = col;
        else if (!strcmp(nm, "point")) d->cPt = col;
    }
    d->ncol = col;
    /* Bound the indices as well as require them: a header wider than MAXCOL
     * would otherwise index past the value array. */
    d->hdr  = (d->cP >= 0 && d->cP < MAXCOL &&
               d->cU >= 0 && d->cU < MAXCOL &&
               d->cA >= 0 && d->cA < MAXCOL &&
               d->ncol > 0 && d->ncol <= MAXCOL);
}

/*
 * Learn the column layout, then jump to the end of the file.
 *
 * This exists because the header is written once, at the top of the file, and
 * every data row is discarded until it has been read (`if (!d->hdr) continue`
 * below). Opening in follow mode with a plain seek to the end therefore never
 * learns the layout, and the tool reads the whole run without accepting a
 * single sample -- while still counting the bytes, so it looks busy.
 *
 * That was the default mode. It stayed hidden because every screenshot and
 * every measurement here was taken with --from-start, which reads from
 * position 0 and picks the header up on the way past. The symptom in follow
 * mode was not a crash but "no samples after 20 s - wrong base name", blaming
 * the user for a correct command.
 */
void his_prime(Dom *d)
{
    char line[8192];

    if (!d->fp) return;
    for (;;) {
        size_t len;
        int np;
        if (!fgets(line, sizeof line, d->fp)) { clearerr(d->fp); break; }
        len = strlen(line);
        /* Only trust a line that ends in a newline, for the same reason
         * his_read_new does. A comment block being written right now can hand
         * back a truncated header -- "# t, P(x,t), Pe(x,t), U(x,t), Q(x,t),
         * A(x,t), # po" -- which parses happily, sets hdr, and leaves cPt at
         * -1. That silently disables the history-point filter for the rest of
         * the run, so a domain with three points folds all three waveforms
         * into one cell and the foot detector runs on the interleaving. */
        if (len == 0 || line[len - 1] != '\n') break;
        if (line[0] != '#') break;
        if (sscanf(line, "# History points: %d", &np) == 1 && np > 0) {
            d->npts = np;
            if (d->watch > np - 1) d->watch = np - 1;
        }
        if (!d->hdr) parse_header(d, line);
        /* Read the whole comment block rather than stopping at the header:
         * the point count may be declared after it, and his_read_new accepts
         * either order. */
    }

    fseek(d->fp, 0, SEEK_END);
}

/* Consume every complete line that appeared since the last poll.
 *
 * The solver is writing to this file at the same time, so the last line is
 * routinely half written. Reading it would produce a truncated row that parses
 * as valid numbers, so the position is remembered before each read and
 * restored when the line does not end in a newline. clearerr() is needed as
 * well: once fgets has seen EOF the stream latches it and every later read
 * returns nothing even after the file has grown. */
int his_read_new(Dom *d, void (*on_sample)(Dom *))
{
    char line[8192];
    int got = 0;
    if (!d->fp) return 0;
    for (;;) {
        long pos = ftell(d->fp);
        size_t len;
        if (!fgets(line, sizeof line, d->fp)) {
            /* clearerr() wipes the error flag as well as the EOF flag, so a
             * genuine read failure -- a stale NFS handle after the file server
             * blinks, a disk giving up -- would be indistinguishable from
             * "nothing new yet", and this domain would poll a dead stream in
             * silence for the rest of the run. Separate the two: close on a
             * real error so ensure_open() retries the file next tick, which
             * recovers from the transient case and keeps counting the rest. */
            if (ferror(d->fp)) {
                d->ioerr++;
                d->resume = pos;        /* pick up here, not at 0 and not at
                                           EOF: neither would keep the byte
                                           count or the waveform continuous */
                fclose(d->fp);
                d->fp = NULL;
                return got;
            }
            clearerr(d->fp);
            break;
        }
        len = strlen(line);
        /* A line longer than the buffer also comes back without a newline.
         * Treating that as "still being written" would rewind onto the same
         * bytes forever and freeze this domain silently, so skip past it. */
        if (len == sizeof line - 1 && line[len - 1] != '\n') {
            int ch;
            while ((ch = fgetc(d->fp)) != EOF && ch != '\n') ;
            if (ch == EOF) { clearerr(d->fp); fseek(d->fp, pos, SEEK_SET); break; }
            continue;
        }
        /* An incomplete tail: either a zero-filled hole (len == 0, the size
         * metadata landing before the data on ext4 and on NFS, which polling
         * a file under active append is exactly when you see) or a record the
         * writer is still in the middle of. Both rewind and wait.
         *
         * Rewinding assumes it fills in. If it never does -- a file truncated
         * by a full disk, or a writer that died mid-record -- this domain
         * re-reads the same bytes and rewinds forever, silently, for the rest
         * of the run.
         *
         * So both shapes are counted, not just the hole. Guarding only
         * `len == 0` would have missed the very case the comment named: a
         * writer dying mid-record leaves "0.003 11000 0.1" with no newline,
         * which is len > 0 and took the unbounded path. The counter clears on
         * any complete line below, so in normal operation -- where every poll
         * reads some rows and then stops on the half-written last one -- it
         * never accumulates. It only climbs when a poll reads nothing at all,
         * which is the freeze itself. */
        if (len == 0 || line[len - 1] != '\n') {
            if (++d->holes < 64) { fseek(d->fp, pos, SEEK_SET); break; }
            /* Give up on this record. Skip to the next line boundary rather
             * than one byte at a time: a large hole would otherwise be walked
             * byte by byte, and doing it inside this loop would consume
             * megabytes in a single poll. */
            d->ioerr++;
            d->holes = 0;
            fseek(d->fp, pos, SEEK_SET);
            { int ch; while ((ch = fgetc(d->fp)) != EOF && ch != '\n') ; }
            clearerr(d->fp);
            break;
        }
        d->holes = 0;
        d->bytes += (long)len;
        if (line[0] == '#') {
            int np;
            /* The .in says how many history points were asked for; the .his
             * says how many the solver could actually place. A point that
             * falls outside every element is dropped silently, so trusting
             * the .in leaves us watching a point number that is never
             * written and the domain reads as permanently empty. */
            if (sscanf(line, "# History points: %d", &np) == 1 && np > 0) {
                d->npts = np;
                if (d->watch > np - 1) d->watch = np - 1;
            }
            if (!d->hdr) parse_header(d, line);
            continue;
        }
        if (!d->hdr) continue;
        {
            double v[MAXCOL];
            char *s = line, *e;
            int n = 0;
            while (n < MAXCOL) {
                double x = strtod(s, &e);
                if (e == s) break;
                v[n++] = x; s = e;
            }
            if (n < d->ncol) continue;
            /* Use the column the header named, not "the last one". The header
             * exists precisely because the column set varies with the run's
             * options, and point is not always last. */
            /* A diverging solver keeps writing, but writes nan. Every
             * comparison against nan is false, so letting one through freezes
             * the foot detector and leaves dSBP at exactly 0.00 -- which reads
             * as CONVERGED. Count them instead and let wave_verdict say so.
             *
             * The time column is checked with the rest. It used to be taken on
             * trust, and a nan there is worse than one in P: t feeds the foot
             * times, and every downstream guard is a comparison, so a nan foot
             * passes each one and comes out the far end as a nan arrival time
             * and an INT_MIN in the display. Column 0 gets the same treatment
             * as the others, and before anything is cast. */
            if (!isfinite(v[0])       || !isfinite(v[d->cP]) ||
                !isfinite(v[d->cU])   || !isfinite(v[d->cA]) ||
                (d->cPt >= 0 && d->cPt < n && !isfinite(v[d->cPt]))) {
                d->nonfinite++;
                continue;
            }
            if (d->cPt >= 0 && d->cPt < n &&
                (int)v[d->cPt] != d->watch + 1) continue;    /* another point */
            d->prevP = d->P; d->prevT = d->t;
            d->t = v[0];
            d->P = v[d->cP] * PA2MMHG;
            d->U = v[d->cU];
            d->A = v[d->cA];
            /* Peak velocity is the cheapest early warning there is. A run whose
             * radii were scaled without scaling the terminal resistances keeps
             * the same flow through a smaller area, so U climbs long before the
             * pressure waveform looks obviously wrong. See wave_verdict(). */
            if (fabs(d->U) > d->umax) d->umax = fabs(d->U);
            if (d->nsamp && d->t > d->prevT)
                d->dPdt = (d->P - d->prevP) / (d->t - d->prevT);
            d->nsamp++;
            if (on_sample) on_sample(d);
            got++;
        }
    }
    return got;
}
