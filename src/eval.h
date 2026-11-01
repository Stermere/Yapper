/* eval.h - the static evaluation. */
#ifndef EVAL_H
#define EVAL_H

#include "board.h"

/* In hundredths of a flat, from the side to move's point of view. Must stay well inside
 * SCORE_WIN_BOUND. `threats` is board_road_placements() for the side not to move: every
 * caller needs it for its own reasons, and it is the most expensive input there is. */
int evaluate(const Board *b, u64 threats);

#endif
