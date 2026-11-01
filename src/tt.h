/* tt.h - the transposition table. */
#ifndef TT_H
#define TT_H

#include "types.h"

/* A few seconds of search fill less than this, so a short match does not replace. */
#define TT_DEFAULT_MB 64

typedef enum { BOUND_NONE, BOUND_UPPER, BOUND_LOWER, BOUND_EXACT } Bound;

typedef struct TTEntry {
    u16 key;
    Move move;
    i16 score;
    i16 eval;
    u8 depth;
    u8 gen_bound; /* generation << 2 | bound */
} TTEntry;

/* Rounds down to a power of two. Returns false if the allocation failed, in which case
 * the previous table is still in place. */
bool tt_resize(int megabytes);
void tt_clear(void);

/* What tt_resize() last allocated. */
int tt_size_mb(void);

/* Call once per search, so entries from older searches can be replaced first. */
void tt_new_search(void);

/* True on a hit, which fills *out. */
bool tt_probe(u64 key, TTEntry *out);

void tt_store(u64 key, Move move, int score, int eval, int depth, Bound bound);

/* Permille of a sample of the table written by the current search. */
int tt_hashfull(void);

static inline Bound tt_bound(const TTEntry *e) { return (Bound)(e->gen_bound & 3); }

#endif
