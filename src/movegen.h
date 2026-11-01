/* movegen.h - legal move generation. Every generated move is legal: Tak has no check. */
#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"
#include "types.h"

/* Must run once before generating. */
void movegen_init(void);

/* Writes every legal move and returns how many. `list` needs MAX_MOVES entries. The
 * position must not be over. */
int movegen_all(const Board *b, Move *list);

/* What movegen_all() would return, without writing the moves. */
int movegen_count(const Board *b);

/* Is `m` legal here? For moves that did not come from the generator: a transposition
 * table entry can be stale or collided, and making one unchecked corrupts the board. */
bool movegen_is_legal(const Board *b, Move m);

#endif
