#ifndef WAVE_H
#define WAVE_H

#include "monitor.h"

void   wave_sample (Dom *d);                       /* called per new sample  */
double wave_cf_pwv (const Dom *dom, int ndom);     /* m/s, 0 until ready     */
double wave_arrival_ms(const Dom *d, const Dom *root);  /* foot delay from root */
void   wave_verdict(Ctx *c, double target_pwv, double tol_pct,
                    double stall_s, double umax_lim, double now);

#endif /* WAVE_H */
