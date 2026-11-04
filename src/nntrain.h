/* nntrain.h - fits the network every size shares to self-play games. */
#ifndef NNTRAIN_H
#define NNTRAIN_H

#include <stdbool.h>

/* Trains on one position in `keep` of the games in `paths`, aiming at a blend of the search's
 * score, weighted `lambda` by size, and the game's result, then writes the header `out`. With
 * `gpu`, a GPU does the fitting if the machine has one, and `threads` only lay out its batches.
 * `seed` picks the first weights and the order of the positions. `share`, by size, is the part of a
 * size's positions that each epoch sees, or NULL for all of them. Non-zero on failure. */
int nntrain_run(const char *out, int keep, const double *lambda, int epochs, int threads, bool gpu,
                unsigned long long seed, const double *share, char **paths, int path_count);

#endif
