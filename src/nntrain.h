/* nntrain.h - fits a network for one board size to self-play games. */
#ifndef NNTRAIN_H
#define NNTRAIN_H

#include <stdbool.h>

/* Trains on one position in `keep` of the games in `paths`, aiming at a blend of the search's
 * score, weighted `lambda`, and the game's result, then writes the header `out`. With `gpu`, a
 * GPU does the fitting if the machine has one, and `threads` only lay out its batches. Non-zero
 * on failure. */
int nntrain_run(int size, const char *out, int keep, double lambda, int epochs, int threads,
                bool gpu, char **paths, int path_count);

#endif
