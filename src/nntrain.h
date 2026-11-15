/* nntrain.h - fits the network every size shares to self-play games. */
#ifndef NNTRAIN_H
#define NNTRAIN_H

#include <stdbool.h>

/* Millions of positions made at once: a block of 6x6 takes about 6 GB. */
#define NT_BLOCK_DEFAULT 64.0

/* Trains on one position in `keep` of the games in `paths`, aiming at a blend of the search's
 * score, weighted `lambda` by size, and the game's result, then writes the header `out`. An NVIDIA
 * GPU does the fitting, and without one it fails; `threads` read the games, make their positions,
 * lay out the card's batches and score the held-out positions. A path `@file` stands for the paths
 * in that file, one a line. `seed` picks the first weights and the order of the positions. `share`,
 * by size, is the part of a size's positions that each epoch sees, or NULL for all of them. `rate`
 * is the first step's learning rate, or 0 for the usual one. With `warm`, the fit starts from the
 * network this build plays rather than from random weights, and with `from`, from that net.h.
 * `scale`, by size, is what the loss reads a score through, 0 or NULL for NN_SCALE; the engine's
 * units do not change with it. With `smooth` above 1, the search's score in the target is the mean
 * of that many scored plies' from the position on. The games stay in memory and their positions
 * are made `block` million at a time. Progress goes to the JSON file `status` for
 * tools/progress.py, or with NULL to one named after `out` in external/games/live. Non-zero on
 * failure. */
int nntrain_run(const char *out, int keep, const double *lambda, int epochs, int threads,
                unsigned long long seed, double rate, bool warm, const char *from,
                const double *share, const double *scale, int smooth, double block,
                const char *status, char **paths, int path_count);

#endif
