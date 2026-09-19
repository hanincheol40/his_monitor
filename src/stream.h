#ifndef STREAM_H
#define STREAM_H

#include "monitor.h"

/* --stream: the display as newline-delimited JSON on stdout, for a front end
 * in another process -- the Qt GUI in gui/, possibly at the far end of ssh.
 *
 *   {"type":"init", ...}   once: the tree -- names, regions, final time
 *   {"type":"tick", ...}   once per display period
 *   {"type":"exit", ...}   last, with the exit code
 *
 * Every number the terminal shows is in a tick, computed by the same code. */
#define STREAM_HZ 200            /* waveform history rate: 1 kHz in, 200 Hz out */

void stream_sample(Dom *d);      /* reader threads: keep the decimated history  */
void stream_init  (const Ctx *c, const char *base, int period_ms);
int  stream_tick  (Ctx *c);      /* main thread, after the tick barrier.
                                    -1 once the reader has gone away           */
void stream_exit  (const Ctx *c, int code);

#endif /* STREAM_H */
