/* policyfit.h - fits the move prior to the moves the search chose in self-play games. */
#ifndef POLICYFIT_H
#define POLICYFIT_H

/* Fits on one game in `keep` of `paths` for `epochs` on `threads`, holding one game in 16 out,
 * then scores a quarter of the held-out games with the engine's own prior and writes the header
 * `out`. `rate` 0 is the default. Non-zero on failure. */
int policyfit_run(const char *out, int keep, int epochs, int threads, double rate,
                  unsigned long long seed, char **paths, int path_count);

#endif
