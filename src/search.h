/* search.h - iterative deepening principal variation search. */
#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include "types.h"

#define MAX_PLY 128

#define SCORE_WIN       30000
#define SCORE_WIN_BOUND (SCORE_WIN - MAX_PLY)

/* A game long enough to fill this is adjudicated long before: racetrack stops at 1000
 * moves. */
#define MAX_GAME_PLIES 4096

typedef struct SearchLimits {
    int depth;      /* 0 for none */
    u64 nodes;      /* 0 for none */
    u64 soft_nodes; /* begin no iteration past this, 0 for none */
    i64 movetime;   /* milliseconds, 0 for none */
    bool clock;     /* time[] and inc[] are set */
    i64 time[2];    /* milliseconds left */
    i64 inc[2];
    bool infinite;
} SearchLimits;

typedef struct SearchReport {
    Move best;
    int score;
    int depth;
    u64 nodes;
} SearchReport;

void search_init(void);

/* Rebuilds what search_init() works out from the tunables, after one has changed. */
void search_retune(void);

/* Forgets everything carried between searches: table, history, killers. Results must not
 * depend on what was searched before. */
void search_clear(void);

/* Searches on a worker thread and prints `bestmove` when done, so the TEI loop can keep
 * answering `isready` and `stop`. `history` holds the keys of earlier positions. */
void search_start(const Board *root, const u64 *history, int history_count,
                  const SearchLimits *limits);

/* Asks a running search to finish. Safe when none is running. */
void search_stop(void);

/* Blocks until no search is running. */
void search_wait(void);

/* Searches on this thread and returns. Used by bench. */
SearchReport search_run(const Board *root, const u64 *history, int history_count,
                        const SearchLimits *limits, bool verbose);

#endif
