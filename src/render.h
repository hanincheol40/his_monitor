#ifndef RENDER_H
#define RENDER_H

#include "monitor.h"

void jet      (double u, int *r, int *g, int *b);
void term_size(int *cols, int *rows);
void render   (const Ctx *c, double target_pwv);

#endif /* RENDER_H */
