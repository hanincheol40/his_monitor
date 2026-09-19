#ifndef WAVE_H
#define WAVE_H

#include "monitor.h"

void   wave_sample (Dom *d);                       /* called per new sample  */
double wave_cf_pwv (const Dom *dom, int ndom);     /* m/s, 0 until ready     */
double wave_arrival_ms(const Dom *d, const Dom *root);  /* foot delay from root */
void   wave_verdict(Ctx *c, double target_pwv, double tol_pct,
                    double stall_s, double umax_lim, double now);

/* One definition each, shared by the terminal and --stream. */
const char *wave_verdict_name(int v);
double wave_tnow (const Dom *dom, int ndom);      /* latest simulated time   */
double wave_dpmax(const Dom *dom, int ndom);      /* fastest rise, this frame */
int    wave_front(const Dom *d, double dpmax);    /* on the wave front now   */
double wave_eta  (const Ctx *c, double tnow);     /* wall seconds left, or 0 */
double wave_dsbp (const Dom *d);                  /* settled beat-to-beat dSBP,
                                                     NAN until two beats settle */

#endif /* WAVE_H */
