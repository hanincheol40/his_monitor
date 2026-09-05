#ifndef HISFILE_H
#define HISFILE_H

#include "monitor.h"
#include <stddef.h>

/* sim_N.in */
int  in_param (const char *path, const char *key, double *out);
int  in_mesh  (const char *path, Dom *dom, int maxdom);   /* returns Ndomains */

/* artery names (116_artery_model.txt) */
void abbreviate(const char *in, char *out, size_t n);
void load_names(const char *path, Dom *dom, int ndom);

/* sim_N_<d>.his */
void parse_header (Dom *d, char *line);
void his_prime    (Dom *d);   /* learn the header, then seek to the end   */
int  his_read_new (Dom *d, void (*on_sample)(Dom *));     /* rows consumed */

#endif /* HISFILE_H */
