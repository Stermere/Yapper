/* nntrain.h - fits a network for one board size to self-play games. */
#ifndef NNTRAIN_H
#define NNTRAIN_H

/* Trains on one position in `keep` of the games in `paths`, aiming at a blend of the search's
 * score, weighted `lambda`, and the game's result, then writes the header `out`. Non-zero on
 * failure. */
int nntrain_run(int size, const char *out, int keep, double lambda, int epochs, int threads,
                char **paths, int path_count);

#endif
