/* perft.h - exact tree counts, the correctness gate for everything in the board. */
#ifndef PERFT_H
#define PERFT_H

#include "board.h"
#include "types.h"

typedef struct PerftOptions {
    /* Counts at depth 1 are read off the generator, without making the moves. */
    bool bulk;
    /* A game that ended before `depth` counts as one leaf, rather than as none. Both
     * conventions are in use, and each has published numbers. */
    bool ended_is_leaf;
} PerftOptions;

/* Leaves of the legal move tree under `b`. */
u64 perft(Board *b, int depth, PerftOptions options);

/* Prints each root move's subtree, then the total. */
void perft_divide(Board *b, int depth);

/* Checks every line of a suite in tests/perft/ and returns the number of failures. */
int perft_run_suite(const char *path, int max_depth);

#endif
