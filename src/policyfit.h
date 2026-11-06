/* policyfit.h - fits the move prior to the moves the search chose in self-play games. */
#ifndef POLICYFIT_H
#define POLICYFIT_H

/* Fits on one game in `keep` of `paths` for `epochs`, holding one game in 16 out, then writes the
 * header `out` and scores the held-out games with the engine's own prior, beside the hand-set one
 * the search had before. Non-zero on failure. */
int policyfit_run(const char *out, int keep, int epochs, char **paths, int path_count);

#endif
